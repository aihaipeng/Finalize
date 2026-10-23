#include "capture/capture_overlay.h"
#include "shared/settings.h"
#include "tests/test_support.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QImage>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

int CaptureOverlay::testWatermark(const QString &previewPath)
{
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    const qint64 stamp = QDateTime(QDate(2026, 5, 14), QTime(9, 8, 7)).toMSecsSinceEpoch();
    check(watermarkString(QStringLiteral("A$yyyy-MM-dd$B"), stamp) == QStringLiteral("A2026-05-14B"),
          "watermark time tokens use the stored capture time");
    check(watermarkString(QStringLiteral("A$yyyy"), stamp) == QStringLiteral("A$yyyy"),
          "an unclosed dollar token leaves the whole watermark literal");
    check(watermarkString(QStringLiteral("A$$B"), stamp) == QStringLiteral("A$B"),
          "a doubled dollar renders one literal dollar");
    check(watermarkString(QStringLiteral("$HH:mm:ss$"), stamp) == QStringLiteral("09:08:07"),
          "watermark supports time-only tokens");

    // The widget callbacks persist the last-used style; restore the real settings afterwards.
    LensSnipSettings settings;
    const QStringList keys = {QStringLiteral("Watermark/Text"), QStringLiteral("Watermark/Color"),
        QStringLiteral("Watermark/FontFamily"), QStringLiteral("Watermark/FontSize"),
        QStringLiteral("Watermark/Opacity"), QStringLiteral("Watermark/Gap"),
        QStringLiteral("Watermark/Position"), QStringLiteral("Watermark/Angle"),
        QStringLiteral("Watermark/AutoAdd"), QStringLiteral("Watermark/LastTimeFormat")};
    QHash<QString, QVariant> saved;
    for (const QString &key : keys) saved.insert(key, settings.value(key));
    struct Restore {
        LensSnipSettings &s; const QStringList &keys; const QHash<QString, QVariant> &values;
        ~Restore() { for (const QString &key : keys) s.setValue(key, values.value(key)); }
    } restore{settings, keys, saved};

    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    QImage image(720, 480, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#252a33")));
    CaptureOverlay overlay({ScreenFrame{QRect(screen.topLeft(), QSize(720, 480)),
                                       QRect(0, 0, image.width(), image.height()), image}});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    check(overlay.m_toolSide.value(QStringLiteral("watermark")) == QStringLiteral("hidden"),
          "watermark starts in the secondary toolbar area");
    const QPoint origin = overlay.mapFromGlobal(screen.topLeft());
    overlay.m_mode = Adjusting;
    overlay.m_sel = QRect(origin + QPoint(40, 40), QSize(560, 360));
    const QImage base = overlay.cropped().toImage();

    overlay.m_wmText = QStringLiteral("内部资料 $yyyy-MM-dd$");
    overlay.m_wmSize = 22;
    overlay.m_wmOpacity = 100;
    overlay.m_wmGap = 0;
    overlay.m_wmPosition = 0;
    Annotation *wm = overlay.ensureWatermarkItem();
    check(wm != nullptr && wm->tool == ATool::Watermark, "watermark creates one text overlay item");
    wm->text = overlay.m_wmText;
    wm->font = 22;
    wm->watermarkOpacity = 100;
    wm->watermarkGap = 0;
    wm->watermarkPosition = 0;
    wm->createdAt = stamp;
    auto changed = [](const QImage &a, const QImage &b, const QRect &area = QRect()) {
        const QRect r = area.isNull() ? a.rect() : area;
        int count = 0;
        for (int y = r.top(); y <= r.bottom(); ++y)
            for (int x = r.left(); x <= r.right(); ++x)
                if (a.pixel(x, y) != b.pixel(x, y)) ++count;
        return count;
    };
    QImage marked = overlay.cropped().toImage();
    check(changed(base, marked) > 500, "tile watermark paints text into the output");
    wm->watermarkOpacity = 0;
    check(changed(base, overlay.cropped().toImage()) == 0, "zero opacity leaves the output untouched");
    wm->watermarkOpacity = 100;
    wm->watermarkPosition = 4;                   // Top-left: one tile, no repeat in the center.
    marked = overlay.cropped().toImage();
    check(changed(base, marked, QRect(0, 0, 180, 80)) > 100,
          "top-left watermark paints one tile at the anchor");
    check(changed(base, marked, QRect(210, 140, 140, 80)) == 0,
          "anchored watermark does not repeat across the image");
    wm->watermarkPosition = 7;                   // Center.
    marked = overlay.cropped().toImage();
    check(changed(base, marked, QRect(220, 150, 120, 60)) > 50,
          "center watermark paints at the selection center");

    overlay.selectTool(ATool::Watermark);
    auto *edit = overlay.m_toolOptions->findChild<QLineEdit *>(QStringLiteral("watermarkText"));
    auto *position = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("watermarkPosition"));
    auto *style = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("watermarkStyle"));
    auto *addTime = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("watermarkAddTime"));
    auto *font = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("watermarkFont"));
    check(edit && position && style && addTime && font,
          "watermark strip exposes content, position, style, time and font controls");
    edit->setText(QStringLiteral("HELLO $yyyy$"));
    check(overlay.watermark() && overlay.watermark()->text == QStringLiteral("HELLO $yyyy$"),
          "typing in the watermark field updates the overlay");
    QMetaObject::invokeMethod(position, "activated", Q_ARG(int, 4));
    check(overlay.watermark()->watermarkPosition == 4, "position menu updates the watermark anchor");
    style->click();
    check(overlay.m_watermarkStylePopup != nullptr, "watermark style button opens the floating panel");
    auto sliders = overlay.m_watermarkStylePopup->findChildren<QSlider *>();
    check(sliders.size() >= 3, "watermark style panel has opacity, size and gap sliders");
    auto *gapRow = overlay.m_watermarkStylePopup->findChild<QWidget *>(QStringLiteral("watermarkGapRow"));
    check(gapRow && !gapRow->isVisible(), "gap control is hidden for an anchored watermark");
    QMetaObject::invokeMethod(position, "activated", Q_ARG(int, 0));
    check(gapRow && gapRow->isVisible(), "gap control appears when switching to tile mode");
    sliders.first()->setValue(55);
    check(overlay.watermark()->watermarkOpacity == 55, "opacity slider updates the watermark");
    if (!previewPath.isEmpty())
        check(overlay.m_watermarkStylePopup->grab().save(previewPath + QStringLiteral(".style.png")),
              "save watermark style popup preview");
    overlay.m_watermarkStylePopup->close();
    edit->setCursorPosition(edit->text().size());
    overlay.insertWatermarkTime(QStringLiteral("HH:mm:ss"));
    check(edit->text().contains(QStringLiteral("$HH:mm:ss$")), "add time inserts a QDateTime token at the cursor");
    QTimer::singleShot(80, [previewPath] {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QDialog *>(w);
            if (!dialog || !dialog->isVisible()) continue;
            auto *combo = dialog->findChild<QComboBox *>(QStringLiteral("watermarkTimeFormat"));
            if (!combo) continue;
            if (!previewPath.isEmpty())
                dialog->grab().save(previewPath + QStringLiteral(".time-dialog.png"));
            combo->setCurrentIndex(2);
            if (auto *buttons = dialog->findChild<QDialogButtonBox *>()) {
                if (QPushButton *ok = buttons->button(QDialogButtonBox::Ok)) ok->click();
            }
            return;
        }
    });
    overlay.showWatermarkTimeDialog();
    check(edit->text().contains(QStringLiteral("$dd/MM/yyyy HH:mm:ss$")),
          "time dialog inserts the chosen format at the cursor");
    edit->clear();
    check(overlay.watermark() && overlay.watermark()->text.isEmpty(),
          "clearing the field hides the watermark text");
    check(changed(base, overlay.cropped().toImage()) == 0, "cleared watermark leaves no painted pixels");

    // A frame whose native pixels are twice its logical size exercises the
    // texture-DPR path used by real 150%/200% captures.
    CaptureOverlay hidpi({ScreenFrame{QRect(screen.topLeft(), QSize(360, 240)),
                                      QRect(0, 0, 720, 480), image}});
    hidpi.setAttribute(Qt::WA_DeleteOnClose, false);
    const QPoint hdOrigin = hidpi.mapFromGlobal(screen.topLeft());
    hidpi.m_mode = Adjusting;
    hidpi.m_sel = QRect(hdOrigin + QPoint(20, 20), QSize(280, 180));
    hidpi.m_wmText = QStringLiteral("HD $yyyy$");
    hidpi.m_wmSize = 20;
    hidpi.m_wmOpacity = 100;
    hidpi.m_wmPosition = 7;
    Annotation *hd = hidpi.ensureWatermarkItem();
    hd->text = hidpi.m_wmText; hd->font = 20; hd->watermarkOpacity = 100;
    hd->watermarkPosition = 7; hd->createdAt = stamp;
    const QImage hdOut = hidpi.cropped().toImage();
    check(hdOut.size() == QSize(560, 360), "high-DPI watermark crop keeps native pixels");
    int hdRed = 0;
    for (int y = 0; y < hdOut.height(); ++y)
        for (int x = 0; x < hdOut.width(); ++x) {
            const QColor c = hdOut.pixelColor(x, y);
            if (c.red() > c.green() + 10 && c.red() > c.blue() + 10) ++hdRed;
        }
    check(hdRed > 500, "high-DPI watermark rasterizes visible text");
    if (!previewPath.isEmpty())
        check(hdOut.save(previewPath + QStringLiteral(".hidpi.png")),
              "save high-DPI watermark preview");
    hd->watermarkPosition = 0;
    const QImage hdTile = hidpi.cropped().toImage();
    int hdTileRed = 0;
    for (int y = 0; y < hdTile.height(); ++y)
        for (int x = 0; x < hdTile.width(); ++x) {
            const QColor c = hdTile.pixelColor(x, y);
            if (c.red() > c.green() + 10 && c.red() > c.blue() + 10) ++hdTileRed;
        }
    check(hdTileRed > 1000, "high-DPI tile watermark repeats across the crop");
    if (!previewPath.isEmpty())
        check(hdTile.save(previewPath + QStringLiteral(".hidpi-tile.png")),
              "save high-DPI tile preview");

    overlay.m_autoWatermark = true;
    overlay.m_autoWatermarkAdded = false;
    overlay.m_anns.clear();
    overlay.m_wmText = QStringLiteral("AUTO");
    overlay.m_wmOpacity = 30;
    overlay.m_wmSize = 20;
    overlay.ensureAutoWatermark();
    check(overlay.watermark() && overlay.watermark()->text == QStringLiteral("AUTO"),
          "auto watermark appends one overlay on a fresh selection");
    overlay.ensureAutoWatermark();
    check(overlay.watermarkIndex() >= 0
              && std::count_if(overlay.m_anns.cbegin(), overlay.m_anns.cend(),
                               [](const Annotation &a) { return a.tool == ATool::Watermark; }) == 1,
          "auto watermark is idempotent within one capture");
    overlay.m_anns.clear();
    overlay.pushUndo();
    overlay.ensureWatermarkItem()->text = QStringLiteral("UNDO");
    overlay.undoAnnotation();
    check(overlay.watermarkIndex() < 0, "undo removes the watermark item");
    overlay.redoAnnotation();
    check(overlay.watermarkIndex() >= 0, "redo restores the watermark item");

    if (!previewPath.isEmpty()) {
        overlay.m_anns.clear();
        overlay.m_wmText = QStringLiteral("$yyyy-MM-dd HH:mm:ss$");
        overlay.m_wmSize = 20;
        overlay.m_wmOpacity = 30;
        overlay.m_wmGap = 0;
        overlay.m_wmPosition = 0;
        wm = overlay.ensureWatermarkItem();
        wm->text = overlay.m_wmText; wm->font = 20; wm->watermarkOpacity = 30;
        wm->watermarkGap = 0; wm->watermarkPosition = 0; wm->createdAt = stamp;
        check(overlay.cropped().save(previewPath + QStringLiteral(".tile.png")),
              "save tile watermark preview");
        wm->watermarkGap = 32;
        check(overlay.cropped().save(previewPath + QStringLiteral(".tile-gap32.png")),
              "save tile gap watermark preview");
        wm->watermarkGap = 0;
        for (int anchor = 1; anchor <= 7; ++anchor) {
            wm->watermarkPosition = anchor;
            check(overlay.cropped().save(previewPath + QStringLiteral(".anchor%1.png").arg(anchor)),
                  "save anchored watermark preview");
        }
        wm->watermarkPosition = 7;
        check(overlay.cropped().save(previewPath + QStringLiteral(".center.png")),
              "save center watermark preview");
        overlay.selectTool(ATool::Watermark);
        check(overlay.grab(overlay.m_tools->geometry().united(overlay.m_toolOptions->geometry()))
                  .save(previewPath + QStringLiteral(".strip.png")),
              "save watermark strip preview");
    }
    std::puts("PASS: watermark time tokens, tile/anchor rendering, opacity, controls, and auto add");
    return 0;
}
