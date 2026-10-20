#include "capture/capture_overlay.h"
#include "annotate/smart_erase.h"
#include "platform/output.h"
#include "tests/test_support.h"
#include "shared/widgets.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QDir>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QFile>
#include <QEventLoop>
#include <QMouseEvent>
#include <QToolButton>
#include <QSlider>
#include <QtMath>
#include <cstdio>
#include <cstdlib>

// Covers the mosaic tool's smart-erase mode: asset gating, mask polarity and
// geometry, live inference (deterministic, hole-only changes, boundary-driven
// fill), the option-strip states, undo/redo re-runs and the failure fallback.
int CaptureOverlay::testSmartErase(const QString &previewPath)
{
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };

    if (const QString bench = qEnvironmentVariable("LENSSNIP_ERASE_BENCH"); !bench.isEmpty()) {
        // Offline comparison: <name>-dirty.png + <name>-mask.png (255 hole) -> <name>-pin.png.
        for (const QString &file : QDir(bench).entryList({QStringLiteral("*-dirty.png")})) {
            const QString stem = bench + QLatin1Char('/') + file.chopped(10);
            QImage hole = QImage(stem + QStringLiteral("-mask.png")).convertToFormat(QImage::Format_Grayscale8);
            hole.invertPixels();
            QElapsedTimer t;
            t.start();
            const QImage out = SmartErase::erase(QImage(stem + QStringLiteral("-dirty.png")), hole);
            std::fprintf(stderr, "bench %s %lld ms\n", qPrintable(file), t.elapsed());
            out.save(stem + QStringLiteral("-pin.png"));
        }
    }

    {   // Mask polarity, geometry and device scaling for both drawing tools.
        for (const qreal dpr : {1.0, 1.5}) {
            Annotation rect;
            rect.tool = ATool::Mosaic;
            rect.mosaicBrush = false;
            rect.rect = QRect(10, 12, 30, 24);
            const QSize device(qRound(200 * dpr), qRound(120 * dpr));
            const QImage mask = smartEraseMask(rect, device, dpr);
            const auto gray = [](const QImage &image, int x, int y) {
                return image.constScanLine(y)[x];
            };
            check(mask.size() == device && mask.format() == QImage::Format_Grayscale8,
                  "the erase mask matches the device size in 8-bit grayscale");
            check(gray(mask, 0, 0) == 255 && gray(mask, device.width() - 1, device.height() - 1) == 255,
                  "the mask keeps unselected pixels");
            const QPoint inside(qRound(25 * dpr), qRound(24 * dpr));
            const QPoint outside(qRound(8 * dpr), qRound(8 * dpr));
            check(gray(mask, inside.x(), inside.y()) == 0 && gray(mask, outside.x(), outside.y()) == 255,
                  "the rectangle hole is zero inside the device-scaled rect");
            Annotation brush;
            brush.tool = ATool::Mosaic;
            brush.mosaicBrush = true;
            brush.pen = 20;
            brush.stroke = {QPointF(60, 60), QPointF(140, 60)};
            const QImage strokeMask = smartEraseMask(brush, device, dpr);
            check(gray(strokeMask, qRound(100 * dpr), qRound(60 * dpr)) == 0
                      && gray(strokeMask, qRound(100 * dpr), qRound(100 * dpr)) == 255,
                  "the brush hole follows the stroked line only");
            check(smartEraseMask(brush, QSize(), dpr).isNull(),
                  "an empty device size yields no mask");
        }
    }

    {   // Synthesis on synthetic frames.
        QImage solid(400, 300, QImage::Format_RGB32);
        solid.fill(QColor(40, 160, 80));
        QPainter painter(&solid);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawRect(QRect(180, 120, 40, 40));
        painter.end();
        QImage mask(400, 300, QImage::Format_Grayscale8);
        mask.fill(255);
        QPainter holes(&mask);
        holes.setPen(Qt::NoPen);
        holes.setBrush(Qt::black);
        holes.drawRect(QRect(180, 120, 40, 40));
        holes.end();
        QString error;
        const QImage erased = [&] {
            QElapsedTimer t;
            t.start();
            const QImage result = SmartErase::erase(solid, mask, &error);
            std::fprintf(stderr, "erase: %lld ms\n", t.elapsed());
            return result;
        }();
        check(error.isEmpty() && erased.size() == solid.size(), "the synthesizer erases the frame");
        const QImage again = SmartErase::erase(solid, mask, &error);
        check(again == erased, "the synthesis is deterministic for identical inputs");
        if (!previewPath.isEmpty()) {
            solid.save(previewPath + QStringLiteral(".smart-before.png"));
            erased.save(previewPath + QStringLiteral(".smart-after.png"));
        }
        int changed = 0, drifted = 0;
        for (int y = 120; y < 160; ++y)
            for (int x = 180; x < 220; ++x)
                if (erased.pixel(x, y) != solid.pixel(x, y)) ++changed;
        std::fprintf(stderr, "changed=%d drifted=%d\n", changed, drifted);
        for (int y = 195; y < 240; ++y)
            for (int x = 195; x < 240; ++x) {
                const QColor c = erased.pixelColor(x, y);
                if (qAbs(c.red() - 40) > 24 || qAbs(c.green() - 160) > 24 || qAbs(c.blue() - 80) > 24) ++drifted;
            }
        check(changed > 400, "the hole content is replaced");
        check(drifted == 0 && erased.pixel(5, 5) == solid.pixel(5, 5)
                  && erased.pixel(394, 294) == solid.pixel(394, 294),
              "pixels outside the hole stay untouched");
        int background = 0;
        for (int y = 135; y < 145; ++y)
            for (int x = 195; x < 205; ++x) {
                const QColor c = erased.pixelColor(x, y);
                if (qAbs(c.red() - 40) <= 40 && qAbs(c.green() - 160) <= 40 && qAbs(c.blue() - 80) <= 40) ++background;
            }
        check(background > 60, "a hole in a solid background is filled with the boundary color");
        const QString mismatch = SmartErase::erase(solid, QImage(10, 10, QImage::Format_Grayscale8), &error).isNull()
                                     ? QStringLiteral("null")
                                     : QString();
        check(mismatch == QLatin1String("null") && !error.isEmpty(), "a mismatched mask is rejected with an error");
        if (!previewPath.isEmpty()) {
            solid.save(previewPath + QStringLiteral(".smart-before.png"));
            erased.save(previewPath + QStringLiteral(".smart-after.png"));
        }
    }

    {   // The option strip and the end-to-end annotation flow.
        for (const qreal dpr : {1.0, 2.0}) {
            const QRect screen = QGuiApplication::primaryScreen()->geometry();
            QImage image(qRound(600 * dpr), qRound(360 * dpr), QImage::Format_RGB32);
            for (int y = 0; y < image.height(); ++y) {
                QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
                for (int x = 0; x < image.width(); ++x)
                    row[x] = qRgb((x * 13 + y * 7) & 255, (x * 3 + y * 11) & 255, (x * 17 + y * 5) & 255);
            }
            CaptureOverlay overlay({ScreenFrame{QRect(screen.topLeft(), QSize(600, 360)),
                                                QRect(0, 0, image.width(), image.height()), image}});
            const QPoint origin = overlay.mapFromGlobal(screen.topLeft());
            overlay.setAttribute(Qt::WA_DeleteOnClose, false);
            overlay.m_mode = Adjusting;
            overlay.m_sel = QRect(origin + QPoint(40, 40), QSize(500, 280));
            overlay.placeToolBars();
            overlay.showToolBars();
            auto mouse = [&overlay, origin](QEvent::Type type, const QPoint &at) {
                const QPoint pos = origin + at;
                QMouseEvent event(type, pos, overlay.mapToGlobal(pos),
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(&overlay, &event);
            };
            const QImage original = overlay.cropped().toImage();
            overlay.m_toolBtn[int(ATool::Mosaic)]->click();
            auto *preview = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("mosaicEffectPreview"));
            check(preview && overlay.m_toolOptions->findChild<QSlider *>(QStringLiteral("mosaicStrength")),
                  "the mosaic strip starts in pixelate mode with a strength slider");
            preview->click();
            check(overlay.mosaicBlur() && !overlay.mosaicSmart(), "the first click switches pixelate to blur");
            preview->click();
            const QSlider *strength = overlay.m_toolOptions->findChild<QSlider *>(QStringLiteral("mosaicStrength"));
            check(overlay.mosaicSmart() && !overlay.mosaicBlur() && strength && !strength->isVisible(),
                  "the second click reaches the smart mode and hides the strength slider");
            overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("mosaicRectangle"))->click();
            check(!overlay.mosaicBrush(), "the rectangle drawing tool arms for the smart mode");
            const int before = overlay.m_mosaicLevels[0];
            overlay.setMosaicStrength(before + 4);
            check(overlay.m_mosaicLevels[0] == before, "the smart mode ignores strength changes");
            mouse(QEvent::MouseButtonPress, QPoint(150, 100));
            mouse(QEvent::MouseMove, QPoint(250, 180));
            mouse(QEvent::MouseButtonRelease, QPoint(250, 180));
            check(overlay.m_anns.size() == 1 && overlay.m_anns[0].mosaicSmart,
                  "a released rectangle commits a smart annotation");
            {   // Until the fill arrives the extent shows as a marker-style wash in the current colour.
                const QImage pending = overlay.cropped().toImage();
                const QPoint c(qRound((200 - 40) * dpr), qRound((140 - 40) * dpr));
                QColor wash = overlay.m_anns[0].color;
                wash.setAlpha(110);
                QImage expect = original.convertToFormat(QImage::Format_ARGB32);
                QPainter(&expect).fillRect(QRect(c, QSize(1, 1)), wash);
                const QRgb got = pending.pixel(c), want = expect.pixel(c);
                check(overlay.m_anns[0].smartPending && qAbs(qRed(got) - qRed(want)) <= 2
                          && qAbs(qGreen(got) - qGreen(want)) <= 2 && qAbs(qBlue(got) - qBlue(want)) <= 2,
                      "a pending erase shows the translucent wash");
            }
            auto settle = [&overlay, check] {
                QElapsedTimer guard;
                guard.start();
                overlay.cropped();       // Hidden widgets get no paint event; drive scheduling once.
                while (overlay.m_anns.size() == 1 && overlay.m_anns[0].smartPending) {
                    QApplication::processEvents(QEventLoop::AllEvents, 50);
                    overlay.cropped();
                    if (guard.elapsed() > 120000) break;
                }
                check(overlay.m_anns.size() == 1 && !overlay.m_anns[0].smartPending
                          && !overlay.m_anns[0].smartFailed && !overlay.m_anns[0].smartResult.isNull(),
                      "the offline model answers the committed rectangle");
            };
            settle();
            const QImage erased = overlay.cropped().toImage();
            const QRect hole(QPoint(qRound((150 - 40) * dpr), qRound((100 - 40) * dpr)),
                             QPoint(qRound((250 - 40) * dpr), qRound((180 - 40) * dpr)));
            int changed = 0;
            for (int y = hole.top() + 4; y < hole.bottom() - 4; ++y)
                for (int x = hole.left() + 4; x < hole.right() - 4; ++x)
                    if (erased.pixel(x, y) != original.pixel(x, y)) ++changed;
            check(changed * 5 > hole.width() * hole.height(),
                  "the erased region changes inside the committed rectangle");
            check(erased.pixel(2, 2) == original.pixel(2, 2)
                      && erased.pixel(erased.width() - 3, erased.height() - 3)
                          == original.pixel(erased.width() - 3, erased.height() - 3),
                  "the output keeps pixels outside the rectangle");
            if (!previewPath.isEmpty() && dpr == 1.0) {
                original.save(previewPath + QStringLiteral(".overlay-before.png"));
                erased.save(previewPath + QStringLiteral(".overlay-after.png"));
            }
            overlay.undoAnnotation();
            check(overlay.m_anns.isEmpty() && overlay.cropped().toImage() == original,
                  "undo restores the untouched frame");
            overlay.redoAnnotation();
            settle();
            check(overlay.cropped().toImage() == erased, "redo reproduces the erased pixels");
            overlay.m_anns[0].smartFailed = true;   // A failed erase must fall back to black.
            overlay.m_anns[0].smartResult = {};
            const QImage failed = overlay.cropped().toImage();
            check(failed.pixel(hole.center().x(), hole.center().y()) == qRgb(0, 0, 0)
                      && failed.pixel(2, 2) == original.pixel(2, 2),
                  "a failed erase renders black inside the hole only");
        }
    }

    std::fprintf(stderr, "PASS: smart erase\n");
    return 0;
}
