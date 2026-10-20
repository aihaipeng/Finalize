#include "longshot/long_capture.h"
#include <QClipboard>
#include "platform/output.h"
#include "tests/test_support.h"
#include "shared/widgets.h"
#include "pin/pin_window.h"
#include "text/cards/ocr_dialog.h"
#include <QGuiApplication>
#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QByteArray>
#include <QColor>
#include <QPaintEvent>
#include <QWidget>
#include <QPainter>
#include <QLabel>
#include <QtMath>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QBuffer>
#include <QToolButton>
#include <QComboBox>
#include <QStringList>
#include <QWheelEvent>
#include <QApplication>
#include <QCheckBox>
#include "shared/settings.h"
#include <QSettings>
#include <qt_windows.h>
#include <cstdio>
#include <cstdlib>

int LongCapture::testStitching()
{
    auto check = [](bool ok, const char *message) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    };
    const int W = 960, H = 360;
    for (const QColor background : {QColor("#0b1016"), QColor(Qt::white)}) {
        QImage page(W, 2400, QImage::Format_RGB32);
        page.fill(background);
        QPainter p(&page);
        p.setPen(background == QColor(Qt::white) ? Qt::black : Qt::white);
        for (int y = 0; y < page.height(); y += 65) {
            p.drawText(30, y + 22, QStringLiteral("test(mcp): align completion checks with provider readiness"));
            p.drawText(30, y + 45, QStringLiteral("author committed 2 hours ago"));
            p.drawText(810, y + 22, QStringLiteral("%1").arg(y * 7919, 6, 16, QLatin1Char('0')));
        }
        p.end();
        LongCapture lc{QRect()};
        const int initial = 360;
        int first = initial, last = initial + H;
        check(lc.addFrame(page.copy(0, initial, W, H)), "initial frame");
        // Force buffer growth to check append/prepend copies pixel by pixel.
        lc.m_buf = lc.toImage();
        lc.m_tileMaxRows = 64;
        lc.m_tailPad = 0;
        for (int pos : {360, 361, 361, 430, 550, 730, 610, 500, 320, 200,
                        50, 0, 150, 350, 570, 780, 980, 1180, 1300}) {
            const bool aligned = lc.addFrame(page.copy(0, pos, W, H));
            if (!aligned) std::fprintf(stderr, "SCROLL position=%d reference=%d\n", pos, lc.m_frameRow);
            check(aligned, "scroll frame must align");
            check(lc.m_frameRow == pos - initial, "frame must retain its absolute position");
            first = qMin(first, pos);
            last = qMax(last, pos + H);
            check(lc.toImage() == page.copy(0, first, W, last - first),
                  "stationary/down/up/revisit must preserve content and order");
        }
        const QImage before = lc.toImage();
        check(!lc.addFrame(page.copy(0, 2000, W, H)), "non-overlapping jump must be rejected");
        check(lc.toImage() == before, "rejected jump must not corrupt output");
        check(lc.addFrame(page.copy(0, 1400, W, H)), "return to overlap must recover");
        check(lc.toImage() == page.copy(0, first, W, 1400 + H - first), "recovery must not leave a gap");
        check(!lc.addFrame(QImage()), "empty frame must be rejected");
        check(!lc.addFrame(page.copy(0, 1400, W - 1, H)), "resized frame must be rejected");
        check(!lc.addFrame(page.copy(0, 1400, W, H - 1)), "height change must be rejected");
        QImage changed = page.copy(0, 1400, W, H);
        changed.fill(Qt::red);
        check(!lc.addFrame(changed), "unrelated content must be rejected");
        check(lc.addFrame(page.copy(0, 1500, W, H)), "bad frames must not replace the alignment reference");
        check(lc.toImage() == page.copy(0, first, W, 1500 + H - first), "bad frames must not remove content");
    }
    QImage repeated(W, H * 2, QImage::Format_RGB32);
    repeated.fill(Qt::white);
    QPainter p(&repeated);
    p.setPen(Qt::black);
    for (int y = 0; y < repeated.height(); y += 40)
        p.drawText(30, y + 22, QStringLiteral("identical rows without a unique identifier"));
    p.end();
    LongCapture ambiguous{QRect()};
    check(ambiguous.addFrame(repeated.copy(0, 0, W, H)), "repeated initial frame");
    const QImage before = ambiguous.toImage();
    check(!ambiguous.addFrame(repeated.copy(0, 17, W, H)), "ambiguous repeated content must be rejected");
    check(ambiguous.toImage() == before, "ambiguous frame must not grow the output");
    QImage blank(W, H, QImage::Format_RGB32);
    blank.fill(Qt::white);
    LongCapture emptyPage{QRect()};
    check(emptyPage.addFrame(blank) && emptyPage.addFrame(blank), "stationary blank frame");
    check(emptyPage.toImage() == blank, "blank frames must not invent content");
    int failures = 0;
    auto regressionCheck = [&failures](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    QImage motionPage(W, 3600, QImage::Format_RGB32);
    motionPage.fill(Qt::white);
    QPainter motionPainter(&motionPage);
    motionPainter.setPen(Qt::black);
    for (int y = 0; y < motionPage.height(); y += 48) {
        motionPainter.drawText(24, y + 20, QStringLiteral("Scrolling capture: stable text and distinct rows"));
        motionPainter.drawText(790, y + 20, QString::number(y * 7919, 16));
        motionPainter.drawText(24, y + 40, QStringLiteral("Page row %1 with content that must stay in order").arg(y));
    }
    motionPainter.end();
    LongCapture fast{QRect()};
    regressionCheck(fast.addFrame(motionPage.copy(0, 0, W, H)), "fast-scroll initial frame");
    int firstFast = 0, lastFast = H;
    for (int pos : {281, 570, 865, 600, 300, 40, 0}) {
        const bool accepted = fast.addFrame(motionPage.copy(0, pos, W, H));
        regressionCheck(accepted && fast.m_frameRow == pos,
                        "fast scrolling with narrow overlap keeps its exact displacement");
        if (accepted) {
            firstFast = qMin(firstFast, pos);
            lastFast = qMax(lastFast, pos + H);
            regressionCheck(fast.toImage() == motionPage.copy(0, firstFast, W, lastFast - firstFast),
                            "fast scrolling never skips, repeats, or reorders image rows");
        }
    }
    for (double dpr : {1.5, 2.0}) {
        const QImage nativePage = motionPage.scaled(qRound(W * dpr), qRound(motionPage.height() * dpr),
                                                   Qt::IgnoreAspectRatio, Qt::FastTransformation);
        LongCapture nativeCapture{QRect(0, 0, W, H)};
        int nativeEnd = qRound(H * dpr);
        for (int pos : {0, 281, 570, 865, 600, 300, 40, 0}) {
            const int y = qRound(pos * dpr);
            const bool accepted = nativeCapture.addFrame(nativePage.copy(0, y, nativePage.width(), qRound(H * dpr)));
            if (!accepted || nativeCapture.m_frameRow != y)
                std::fprintf(stderr, "DPI %.1f position=%d accepted=%d actual=%d\n", dpr, y, int(accepted), nativeCapture.m_frameRow);
            regressionCheck(accepted && nativeCapture.m_frameRow == y,
                            "native 150/200 percent DPI frames retain exact fast-scroll positions");
            if (accepted) {
                nativeEnd = qMax(nativeEnd, y + qRound(H * dpr));
                regressionCheck(nativeCapture.toImage() == nativePage.copy(0, 0, nativePage.width(), nativeEnd),
                                "native high-DPI scrolling preserves every original output pixel");
            }
        }
        const QImage beforeJump = nativeCapture.toImage();
        regressionCheck(!nativeCapture.addFrame(nativePage.copy(0, qRound(3000 * dpr), nativePage.width(), qRound(H * dpr)))
                            && nativeCapture.toImage() == beforeJump,
                        "native high-DPI non-overlapping jumps cannot create plausible but incorrect splices");
    }
    auto animatedFrame = [&motionPage](int pos, int phase) {
        QImage frame = motionPage.copy(0, pos, W, H);
        for (int y = 72; y < 224; ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(frame.scanLine(y));
            for (int x = 250; x < 730; ++x)
                row[x] = ((x / 7 + y / 9 + phase) % 3) ? qRgb(225, 35, 55) : qRgb(20, 60, 235);
        }
        return frame;
    };
    LongCapture animated{QRect()};
    regressionCheck(animated.addFrame(animatedFrame(0, 0)), "animated initial frame");
    regressionCheck(animated.addFrame(animatedFrame(0, 1)) && animated.m_frameRow == 0
                        && animated.m_rowEnd == H,
                    "stationary local animation must not lose alignment or extend the output");
    int phase = 2;
    for (int pos : {80, 160, 300, 590, 390, 150, 0}) {
        regressionCheck(animated.addFrame(animatedFrame(pos, phase++)) && animated.m_frameRow == pos,
                        "local animation is excluded while stable content establishes the scroll offset");
    }
    regressionCheck(animated.toImage().copy(0, 0, 200, animated.m_rowEnd) == motionPage.copy(0, 0, 200, animated.m_rowEnd),
                    "animation handling preserves every stable output pixel during down/up/revisit");
    QImage unrelated(W, H, QImage::Format_RGB32);
    for (int y = 0; y < H; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(unrelated.scanLine(y));
        for (int x = 0; x < W; ++x)
            row[x] = qRgb((x * 73 + y * 17) % 256, (x * 11 + y * 67) % 256, (x * 53 + y * 31) % 256);
    }
    const QImage animatedBefore = animated.toImage();
    regressionCheck(!animated.addFrame(unrelated) && animated.toImage() == animatedBefore,
                    "an unrelated animated frame never replaces the reference or changes the output");
    for (bool footer : {false, true}) {
        const auto fixedFrame = [&](int pos) {
            QImage frame = motionPage.copy(0, pos, W, H);
            QPainter paint(&frame);
            paint.drawImage(0, footer ? H - 144 : 0, unrelated.copy(0, 0, W, 144));
            return frame;
        };
        LongCapture fixedOverlay{QRect(0, 0, W, H)};
        const QImage first = fixedFrame(0);
        regressionCheck(fixedOverlay.addFrame(first), "fixed-overlay initial frame");
        regressionCheck(!fixedOverlay.addFrame(fixedFrame(80)) && fixedOverlay.m_frameRow == 0
                            && fixedOverlay.toImage() == first && fixedOverlay.m_lastFrame == first,
                        "fixed headers and footers cannot hide scrolling or pollute the alignment reference");
        regressionCheck(fixedOverlay.addFrame(first), "returning to the last accepted fixed-overlay frame recovers");
    }
    QImage longRows(16, 70000, QImage::Format_RGB32);
    for (int y = 0; y < longRows.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(longRows.scanLine(y));
        for (int x = 0; x < longRows.width(); ++x)
            row[x] = qRgb(y % 256, (y / 256) % 256, x * 13);
    }
    LongCapture large{QRect()};
    large.initTiles(longRows.width());
    large.m_tileMaxRows = 4096;
    for (int from = 0; from < longRows.height(); from += 2048)
        large.appendRows(longRows, from, qMin(2048, longRows.height() - from));
    regressionCheck(large.m_rowEnd == 70000 && large.toImage() == longRows,
                    "buffer growth preserves every pixel beyond 32767 and 65000 rows");
    QImage prefix(16, 1024, QImage::Format_RGB32);
    prefix.fill(Qt::green);
    large.prependRows(prefix, 0, prefix.height());
    const QImage extended = large.toImage();
    regressionCheck(extended.copy(0, 0, 16, prefix.height()) == prefix
                        && extended.copy(0, prefix.height(), 16, longRows.height()) == longRows,
                    "prepending an extra head preserves the complete very long image");
    QByteArray png;
    QBuffer pngBuffer(&png);
    regressionCheck(pngBuffer.open(QIODevice::WriteOnly) && extended.save(&pngBuffer, "PNG") && QImage::fromData(png, "PNG") == extended,
                    "PNG export beyond 65000 rows round-trips every pixel");
    regressionCheck(!large.reserveRows(1 << 25, 0) && large.m_storageFull && large.toImage() == extended,
                    "memory budget rejection leaves the complete capture intact");
    {
        // The preview shows a bounded slice ending at the current frame, without copying the whole result.
        const QRect screen = QGuiApplication::primaryScreen()->geometry();
        large.m_region = QRect(screen.left() + 40, screen.top() + 200, 16, 300);
        large.m_storageFull = false;
        large.m_frameRow = large.m_rowEnd - 300;
        large.m_lastFrame = longRows.copy(0, longRows.height() - 300, 16, 300);
        large.updatePreview();
        LongPreview *pv = large.m_preview;
        regressionCheck(pv && !pv->image.isNull() && pv->atEnd && !pv->atStart && pv->showFrame
                            && pv->frameRect.bottom() == pv->imageRect.bottom()
                            && pv->imageRect.bottom() + 1 == large.m_region.bottom() + 1 - 4 - pv->geometry().top(),
                        "very long preview slices the tail, frames the current frame, and bottom-aligns to the selection");
        large.m_alignmentLost = true;
        large.updatePreview();
        regressionCheck(pv && !pv->showFrame, "a failed match drops the match frame");
        large.m_alignmentLost = false;
        delete large.m_preview;
        large.m_preview = nullptr;
    }
    auto *exportCapture = new LongCapture{QRect()};
    exportCapture->initTiles(16);
    regressionCheck(exportCapture->appendRows(extended, 0, extended.height()),
                    "a first frame larger than the growth step reserves enough storage");
    QPixmap exported;
    QObject::connect(exportCapture, &LongCapture::finished, [&exported](const QPixmap &pix, bool) { exported = pix; });
    exportCapture->finish();
    regressionCheck(exported.toImage() == extended, "very long pinned output retains the original pixels");
    LongCapture exportFailure{QRect()};
    exportFailure.finish();
    regressionCheck(!exportFailure.m_done, "failed export keeps the capture session open");
    // Horizontal capture: frames are transposed onto the vertical stitcher and transposed back on export.
    {
        const QImage wide = motionPage.copy(0, 0, W, 1400);
        const QImage page = transposed(wide);               // 1400 wide, W tall: content scrolls right.
        regressionCheck(transposed(page) == wide, "transposing twice restores every pixel");
        LongCapture horizontal{QRect(0, 0, H, W)};
        horizontal.m_vertical = false;
        bool aligned = true;
        int right = H;
        for (int pos : {0, 200, 450, 700, 450, 650, 900}) {
            aligned = horizontal.ingest(page.copy(pos, 0, H, W)) && horizontal.m_frameRow == pos && aligned;
            right = qMax(right, pos + H);
        }
        regressionCheck(aligned && horizontal.exportImage() == page.copy(0, 0, right, W),
                        "horizontal scrolling stitches left to right and exports in screen orientation");
    }
    regressionCheck(autoScrollStep(299) == 25 && autoScrollStep(300) == 35 && autoScrollStep(499) == 35
                        && autoScrollStep(500) == 45 && autoScrollStep(700) == 55 && autoScrollStep(899) == 55
                        && autoScrollStep(900) == 65 && autoScrollStep(4000) == 65,
                    "auto-scroll wheel step follows the selection extent table");
    {
        QImage text(200, 5000, QImage::Format_RGB32);
        text.fill(Qt::white);
        QPainter tp(&text);
        tp.setPen(Qt::black);
        for (int y = 0; y < text.height(); y += 30) tp.drawText(10, y + 20, QStringLiteral("line %1").arg(y));
        tp.end();
        const QVector<int> ends = ocrCuts(text, 1440);
        bool blankCuts = ends.last() == text.height();
        int top = 0;
        for (int end : ends) {
            blankCuts = blankCuts && end - top <= 1440 && end > top;
            if (end != text.height()) {
                const QRgb *row = reinterpret_cast<const QRgb *>(text.constScanLine(end - 1));
                for (int x = 0; x < text.width(); ++x) blankCuts = blankCuts && row[x] == row[0];
            }
            top = end;
        }
        regressionCheck(ends.size() == 4 && blankCuts,
                        "tall recognition input splits into bounded chunks on blank rows");
        regressionCheck(ocrCuts(text.copy(0, 0, 200, 900), 1440) == QVector<int>{900},
                        "short recognition input stays whole");
    }
    QImage performancePage(1280, 3600, QImage::Format_RGB32);
    performancePage.fill(Qt::white);
    QPainter performancePainter(&performancePage);
    performancePainter.setPen(Qt::black);
    for (int y = 0; y < performancePage.height(); y += 36) {
        performancePainter.drawText(24, y + 24, QStringLiteral("Row %1").arg(y * 7919));
        performancePainter.drawText(1100, y + 24, QString::number(y * 271, 16));
    }
    performancePainter.end();
    LongCapture denseAnimation{QRect()};
    qint64 maxMatchNs = 0;
    for (int pos : {0, 250, 500, 750, 1000}) {
        QImage frame = performancePage.copy(0, pos, 1280, 720);
        for (int y = 100; y < 620; ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(frame.scanLine(y));
            for (int x = 260; x < 1000; ++x) {
                quint32 value = quint32(x) * 747796405u ^ quint32(y) * 2891336453u ^ quint32(pos + 1) * 277803737u;
                value ^= value >> 16;
                value *= 2246822519u;
                row[x] = qRgb(value & 255, (value >> 8) & 255, (value >> 16) & 255);
            }
        }
        QElapsedTimer timer;
        timer.start();
        const bool accepted = denseAnimation.addFrame(frame);
        if (pos) maxMatchNs = qMax(maxMatchNs, timer.nsecsElapsed());
        regressionCheck(accepted && denseAnimation.m_frameRow == pos,
                        "dense video-like animation does not dominate the static texture evidence");
    }
    std::printf("PERF: 1280x720 dense-animation match maximum %.2f ms\n", double(maxMatchNs) / 1000000.0);
    check(failures == 0, "fast-scroll, animation, and very-long-buffer regression checks");
    std::puts("PASS: long-capture stitching regression checks");
    return 0;
}

int LongCapture::testOutput()
{
    int failures = 0;
    const auto check = [&](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(QColor("#123abc"));
    LongCapture capture{QRect()};
    check(capture.addFrame(image), "output fixture accepts its first frame");
    {
        ClipboardBlocker blocker;
        check(blocker.locked(), "native clipboard contention fixture acquired the lock");
        const DWORD sequence = GetClipboardSequenceNumber();
        capture.copyCapture();
        check(!capture.m_done && capture.toImage() == image && GetClipboardSequenceNumber() == sequence,
              "failed long-capture copy preserves the session, image, and existing clipboard");
    }
    capture.copyCapture();
    check(capture.m_done && QGuiApplication::clipboard()->image() == image,
          "long-capture copy succeeds after the clipboard lock is released");
    LongCapture fullScreen(QGuiApplication::primaryScreen()->availableGeometry());
    fullScreen.buildUi();
    fullScreen.m_barAffinity = false;
    fullScreen.setUiVisible(false);
    check(!fullScreen.m_bar->isVisible(), "a bar that is not capture-excluded hides while it overlaps the grab");
    fullScreen.setUiVisible(true);
    check(fullScreen.m_bar->isVisible(), "toolbar returns after capture");
    fullScreen.m_barAffinity = true;
    fullScreen.setUiVisible(false);
    check(fullScreen.m_bar->isVisible(), "a capture-excluded bar stays up during the grab");
    fullScreen.setUiVisible(true);
    fullScreen.cleanupUi();

    const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    LongCapture session(QRect(screen.left() + 100, screen.top() + 100, 400, 300));
    session.buildUi();
    QStringList tips;
    for (QToolButton *b : session.m_bar->findChildren<QToolButton *>()) tips << b->toolTip();
    const QString ocrTip = session.m_ocrMissing.isEmpty() ? QStringLiteral("文字识别 (OCR)") : session.m_ocrMissing;
    const QString translateTip = session.m_ocrMissing.isEmpty() ? QStringLiteral("识别并翻译文字") : session.m_ocrMissing;
    check(tips == QStringList{QStringLiteral("切换纵向/横向截图"),
                              QStringLiteral("自动向上滚动截图"), QStringLiteral("自动向下滚动截图"),
                              QStringLiteral("裁剪"), QStringLiteral("编辑"), ocrTip, translateTip,
                              QStringLiteral("识别二维码/条形码"),
                              QStringLiteral("贴图"), QStringLiteral("保存"), QStringLiteral("关闭"), QStringLiteral("复制并关闭")},
          "long-capture bar lists the direction toggle, both auto-scroll directions, crop, edit, OCR, translate, barcode, pin, save, close and copy-and-close in order");
    check(session.m_bar->findChildren<QComboBox *>().isEmpty()
              && session.m_autoUp->icon().cacheKey() != session.m_auto->icon().cacheKey(),
          "the direction control has no dropdown and the two auto-scroll icons point in opposite directions");
    const QPoint cursorBeforeAuto = QCursor::pos();
    session.m_auto->click();
    check(session.m_autoScroll == 1 && session.m_auto->isChecked() && !session.m_autoUp->isChecked()
              && session.m_auto->isEnabled() && !session.m_autoUp->isEnabled()
              && QCursor::pos() == cursorBeforeAuto,
          "scrolling down keeps the down button active, greys out the up button and leaves the pointer in place");
    session.m_auto->click();
    check(session.m_autoScroll == 0 && !session.m_auto->isChecked() && session.m_autoUp->isEnabled(),
          "clicking the active direction stops and enables the other");
    session.m_autoUp->click();
    check(session.m_autoScroll == -1 && session.m_autoUp->isChecked() && !session.m_auto->isChecked()
              && session.m_autoUp->isEnabled() && !session.m_auto->isEnabled(),
          "scrolling up greys out the down button");
    session.m_autoUp->click();
    check(session.m_autoScroll == 0 && !session.m_autoUp->isChecked() && session.m_auto->isEnabled(),
          "clicking the up direction again stops the capture");
    check(!session.m_crop->isEnabled() && session.m_cropStart->toolTip() == QStringLiteral("裁剪顶部")
              && session.m_cropEnd->toolTip() == QStringLiteral("裁剪底部"),
          "crop waits for a captured image and its panel crops top and bottom");
    check(session.m_label->text() == QStringLiteral("400 × 300") && session.m_dir->text() == QStringLiteral("纵向"),
          "bar starts with the selection size and vertical direction");
    const QString sizeLabel = session.m_label->text();
    session.fail(QStringLiteral("保存失败，请重试"));
    check(session.m_label->text() == sizeLabel && session.m_feedback->isVisible()
              && session.m_feedback->text().contains(QStringLiteral("保存失败")),
          "a save/copy error stays in the feedback strip instead of replacing the size readout");
    const QImage feedback = session.m_feedback->grab().toImage();
    check(feedback.pixelColor(4, feedback.height() / 2) == QColor(QStringLiteral("#18191d")),
          "the feedback strip paints its opaque dark surface");
    session.m_errorText.clear();
    session.m_errorClock.invalidate();
    session.updateFeedback();
    bool allDisabled = true;
    for (QToolButton *b : session.m_gated) allDisabled = allDisabled && !b->isEnabled();
    check(allDisabled, "edit, OCR, translate, barcode, pin and copy wait for a captured image");
    QImage frame(400, 300, QImage::Format_RGB32);
    frame.fill(Qt::white);
    session.addFrame(frame);
    session.updateButtons();
    session.updateLabel();
    check(session.m_gated.at(0)->isEnabled() && session.m_gated.at(3)->isEnabled()
              && session.m_gated.at(4)->isEnabled() && session.m_gated.at(5)->isEnabled()
              && (session.m_ocrMissing.isEmpty()
                      ? session.m_gated.at(1)->isEnabled() && session.m_gated.at(2)->isEnabled()
                      : !session.m_gated.at(1)->isEnabled() && !session.m_gated.at(2)->isEnabled())
              && session.m_label->text() == QStringLiteral("400 × 300"),
          "captured rows enable the bounded outputs and the label shows the stitched size");
    session.m_rowEnd = session.m_rowStart + 29001;          // Length gate only; rows are never read here.
    session.updateButtons();
    bool gatedOff = true;
    for (QToolButton *b : session.m_gated) gatedOff = gatedOff && !b->isEnabled();
    check(gatedOff && session.m_save->isEnabled(),
          "images of 29001 rows or more keep only save");
    session.m_rowEnd = session.m_rowStart + 300;
    const QPoint barBefore = session.m_bar->pos();
    const QPoint moved = session.moveRegion(QPoint(30, 40));
    check(moved == QPoint(0, 40) && session.m_region.topLeft() == QPoint(screen.left() + 100, screen.top() + 140)
              && session.m_bar->pos() == barBefore + QPoint(0, 40),
          "the move control shifts the selection and bar along the scroll axis only");
    check(session.moveRegion(QPoint(0, 5000)).isNull(), "a jump beyond a third of the screen is ignored");
    session.m_dir->click();
    check(!session.m_vertical && session.m_dir->text() == QStringLiteral("横向")
              && session.logicalLength() == 0 && session.m_autoScroll == 0
              && session.m_label->text() == QStringLiteral("400 × 300"),
          "clicking the direction toggle switches and restarts the capture");
    check(session.m_autoUp->icon().cacheKey() != session.m_auto->icon().cacheKey(),
          "left and right auto-scroll icons remain distinct");
    check(session.moveRegion(QPoint(25, 25)) == QPoint(25, 0), "horizontal sessions move along x");
    check(session.m_cropStart->toolTip() == QStringLiteral("裁剪左侧") && session.m_cropEnd->toolTip() == QStringLiteral("裁剪右侧"),
          "horizontal sessions crop left and right");
    check(session.m_autoUp->toolTip() == QStringLiteral("自动向左滚动截图")
              && session.m_auto->toolTip() == QStringLiteral("自动向右滚动截图"),
          "horizontal sessions scroll left and right");
    {
        // Hovering the crop button opens its panel under the bar; leaving both closes it after a short delay.
        session.addFrame(transposed(frame));
        session.updateButtons();
        QEvent enter(QEvent::Enter), leave(QEvent::Leave);
        QApplication::sendEvent(session.m_crop, &enter);
        QElapsedTimer t;
        t.start();
        while (!session.m_cropPanel->isVisible() && t.elapsed() < 1000) QApplication::processEvents();
        check(session.m_cropPanel->isVisible() && session.m_cropPanel->y() >= session.m_bar->geometry().bottom(),
              "hovering crop opens the panel below the bar");
        QApplication::sendEvent(session.m_crop, &leave);
        QApplication::sendEvent(session.m_cropPanel, &enter);
        t.restart();
        while (t.elapsed() < 350) QApplication::processEvents();
        check(session.m_cropPanel->isVisible(), "moving onto the panel keeps it open");
        QApplication::sendEvent(session.m_cropPanel, &leave);
        t.restart();
        while (session.m_cropPanel->isVisible() && t.elapsed() < 1000) QApplication::processEvents();
        check(!session.m_cropPanel->isVisible(), "leaving the panel closes it");
    }
    {
        // Back from the editor: the session hides and holds still, then returns as it was.
        const int rows = session.logicalLength();
        session.suspend();
        check(session.suspended() && !session.m_bar->isVisible() && !session.m_mask->isVisible(),
              "editing hides the session without ending it");
        session.resume();
        check(!session.suspended() && session.m_bar->isVisible() && session.m_mask->isVisible()
                  && session.logicalLength() == rows,
              "back restores the session with its stitched rows");
    }
    session.cleanupUi();
    if (session.m_ocrMissing.isEmpty()) {
        // Both text cards use the same dialog as a normal capture and end the session when closed.
        for (bool translate : {false, true}) {
            auto *card = new LongCapture(QRect(screen.left() + 100, screen.top() + 100, 400, 300));
            card->buildUi();
            QImage page(400, 300, QImage::Format_RGB32);
            page.fill(Qt::white);
            card->addFrame(page);
            card->updateButtons();
            QPointer<LongCapture> alive(card);
            (translate ? card->m_translate : card->m_ocr)->click();
            check(alive && alive->m_ocrDialog && alive->m_ocrDialog->isVisible()
                      && alive->m_ocrDialog->windowTitle()
                          == (translate ? QStringLiteral("LensSnip 翻译") : QStringLiteral("LensSnip OCR"))
                      && alive->suspended() && !alive->m_bar->isVisible(),
                  translate ? "long capture translate opens the same translate card and pauses the session"
                            : "long capture OCR opens the same OCR card and pauses the session");
            alive->m_ocrDialog->findChild<QToolButton *>(QStringLiteral("ocrBack"))->click();
            QElapsedTimer backWait;
            backWait.start();
            while (alive && alive->m_ocrDialog && backWait.elapsed() < 2000) QApplication::processEvents();
            check(alive && !alive->m_ocrDialog && !alive->suspended() && alive->m_bar->isVisible(),
                  translate ? "back from translate resumes the long capture session"
                            : "back from OCR resumes the long capture session");
            (translate ? card->m_translate : card->m_ocr)->click();
            QApplication::processEvents();
            check(alive && alive->m_ocrDialog && alive->m_ocrDialog->isVisible(),
                  "the long capture card can be opened again after back");
            alive->m_ocrDialog->close();
            QElapsedTimer wait;
            wait.start();
            while (alive && wait.elapsed() < 2000) QApplication::processEvents();
            check(!alive, translate ? "closing the translate card ends the long capture"
                                    : "closing the OCR card ends the long capture");
        }
    }
    {
        // Barcode recognition follows the same suspend/back/close lifecycle, without OCR assets.
        auto *card = new LongCapture(QRect(screen.left() + 100, screen.top() + 100, 400, 300));
        card->buildUi();
        QImage page(400, 300, QImage::Format_RGB32);
        page.fill(Qt::white);
        card->addFrame(page);
        card->updateButtons();
        QPointer<LongCapture> alive(card);
        card->m_barcode->click();
        check(alive && alive->m_ocrDialog && alive->m_ocrDialog->isVisible()
                  && alive->m_ocrDialog->windowTitle() == QStringLiteral("LensSnip 二维码/条形码")
                  && alive->suspended() && !alive->m_bar->isVisible(),
              "long capture barcode opens its card and pauses the session");
        alive->m_ocrDialog->findChild<QToolButton *>(QStringLiteral("ocrBack"))->click();
        QElapsedTimer backWait;
        backWait.start();
        while (alive && alive->m_ocrDialog && backWait.elapsed() < 2000) QApplication::processEvents();
        check(alive && !alive->m_ocrDialog && !alive->suspended() && alive->m_bar->isVisible(),
              "back from barcode resumes the long capture session");
        card->m_barcode->click();
        QApplication::processEvents();
        check(alive && alive->m_ocrDialog && alive->m_ocrDialog->isVisible(),
              "the long capture barcode card can be reopened after back");
        alive->m_ocrDialog->close();
        QElapsedTimer wait;
        wait.start();
        while (alive && wait.elapsed() < 2000) QApplication::processEvents();
        check(!alive, "closing the barcode card ends the long capture");
    }
    {
        // Auto scroll no longer stops when the pointer leaves: the wheel goes to the page under the selection.
        class WheelPage : public QWidget {
        public:
            int wheels = 0, delta = 0;
            void wheelEvent(QWheelEvent *e) override { ++wheels; delta = e->angleDelta().y(); }
        } wheelPage;
        wheelPage.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        wheelPage.setGeometry(QRect(screen.center() - QPoint(100, 80), QSize(200, 160)));
        wheelPage.show();
        wheelPage.raise();
        QApplication::processEvents();
        LongCapture wheel{QRect(wheelPage.geometry().topLeft(), wheelPage.size())};
        QCursor::setPos(screen.topLeft() + QPoint(2, 2));
        wheel.m_autoScroll = 1;
        wheel.scrollOnce();
        QElapsedTimer wait;
        wait.start();
        while (wheelPage.wheels < 1 && wait.elapsed() < 1000) QApplication::processEvents();
        check(wheelPage.wheels == 1 && wheelPage.delta < 0, "auto scroll down reaches the page with the pointer outside");
        wheel.m_autoScroll = -1;
        wheel.scrollOnce();
        wait.restart();
        while (wheelPage.wheels < 2 && wait.elapsed() < 1000) QApplication::processEvents();
        check(wheelPage.wheels == 2 && wheelPage.delta > 0, "auto scroll up sends the opposite wheel delta");
    }
    {
        // Crop keeps the rows from the current frame to one end, in place.
        QImage page(40, 1000, QImage::Format_RGB32);
        for (int y = 0; y < page.height(); ++y)
            for (int x = 0; x < page.width(); ++x) page.setPixel(x, y, qRgb(y % 256, y / 256, x));
        LongCapture c{QRect()};
        c.initTiles(40);
        c.appendRows(page, 0, page.height());
        c.m_lastFrame = page.copy(0, 300, 40, 200);
        c.m_frameRow = 300;
        c.cropBefore();
        check(c.toImage() == page.copy(0, 300, 40, 700) && c.m_frameRow == c.m_rowStart,
              "crop top keeps the current frame and everything after it");
        c.m_frameRow = c.m_rowStart + 100;                 // Frame now at page row 400.
        c.cropAfter();
        check(c.toImage() == page.copy(0, 300, 40, 300), "crop bottom keeps everything up to the current frame's end");
        c.m_frameRow = c.m_rowStart;
        c.cropBefore();
        check(c.toImage() == page.copy(0, 400, 40, 200) && c.m_lastFrame == c.toImage(),
              "crop top at the start keeps the last frame's worth and re-anchors on it");
        const QImage kept = c.toImage();
        c.m_frameRow = c.m_rowStart;
        c.cropAfter();
        check(c.toImage() == kept, "nothing to crop leaves the image unchanged");
        QImage tail(40, 200, QImage::Format_RGB32);
        tail.fill(Qt::red);
        c.m_frameRow = c.m_rowEnd - 200;
        check(c.appendRows(tail, 0, 200) && c.toImage().copy(0, 200, 40, 200) == tail
                  && c.toImage().copy(0, 0, 40, 200) == kept,
              "capture continues to grow after a crop");
    }
    {
        // Auto crop: once scrolling turns back from the direction it grew in, rows past the frame are dropped.
        LongCapture c{QRect()};
        c.m_autoCrop = true;
        c.initTiles(40);
        QImage rows(40, 2000, QImage::Format_RGB32);
        for (int y = 0; y < rows.height(); ++y) rows.scanLine(y)[0] = uchar(y);
        c.appendRows(rows, 0, 200);
        c.m_lastFrame = rows.copy(0, 0, 40, 200);
        c.m_frameRow = 0;
        for (int top : {100, 200, 300, 400}) {               // Scroll down: the tail grows.
            c.appendRows(rows, c.m_rowEnd, top + 200 - c.m_rowEnd);
            c.m_frameRow = top;
            c.autoCrop(1);
        }
        check(c.m_cropState == 1 && c.logicalLength() == 600, "growing toward the tail sets the crop direction");
        c.m_frameRow = 250;                                    // Scroll back up inside the coverage.
        c.autoCrop(0);
        check(c.logicalLength() == 450 && c.m_rowEnd == 450, "scrolling back up drops the rows below the frame");
        c.m_frameRow = 200;
        c.autoCrop(0);
        check(c.logicalLength() == 400, "continuing up keeps cropping to the frame");
        LongCapture off{QRect()};
        off.initTiles(40);
        off.appendRows(rows, 0, 600);
        off.m_lastFrame = rows.copy(0, 0, 40, 200);
        off.m_frameRow = 400;
        off.autoCrop(1);
        off.m_frameRow = 100;
        off.autoCrop(0);
        check(off.logicalLength() == 600, "auto crop off leaves turning back alone");
    }
    {
        // The editor shows the native size when it fits and scales down to fit otherwise, centered.
        const QRect av(0, 0, 1920, 1040);
        const QRect small = editorGeometry(QSize(600, 400), 1.0, av);
        check(small.size() == QSize(600, 400) && qAbs(small.center().x() - av.center().x()) <= 1
                  && qAbs(small.center().y() - av.center().y()) <= 1,
              "a capture that fits opens at its native size in the screen center");
        check(editorGeometry(QSize(1200, 800), 2.0, av).size() == QSize(600, 400),
              "native size is in device pixels, shown at the screen's scale");
        const QRect tall = editorGeometry(QSize(600, 6000), 1.0, av);
        check(tall.height() <= av.height() - 2 * PinWindow::kShadowMargin - 96 && qAbs(tall.width() - 600.0 * tall.height() / 6000) <= 1
                  && qAbs(tall.center().x() - av.center().x()) <= 1 && qAbs(tall.center().y() - av.center().y()) <= 1
                  && tall.top() >= av.top() + PinWindow::kShadowMargin,
              "a tall capture scales down proportionally, stays exactly centered, and keeps room for the bar");
        const QRect wide = editorGeometry(QSize(8000, 300), 1.0, av);
        check(wide.width() <= av.width() - 2 * PinWindow::kShadowMargin && wide.left() >= av.left()
                  && qAbs(wide.center().x() - av.center().x()) <= 1 && qAbs(wide.center().y() - av.center().y()) <= 1,
              "a wide capture scales down to fit the width and stays centered");
    }
    return failures ? 1 : 0;
}

int LongCapture::testLiveCapture(const QString &previewPath)
{
    // The live row-count expectation assumes auto crop is off; keep the user's setting untouched.
    LensSnipSettings settings;
    const QVariant savedAutoCrop = settings.value(QStringLiteral("LongShot/AutoCrop"));
    settings.setValue(QStringLiteral("LongShot/AutoCrop"), false);
    settings.sync();
    class Page : public QWidget {
    public:
        QImage image;
        int offset = 0;
        void paintEvent(QPaintEvent *) override {
            QPainter painter(this);
            painter.drawImage(rect(), image.copy(0, offset, image.width(), height()));
        }
    } page;
    page.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    page.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(80, 80), QSize(960, 360)));
    page.image = QImage(960, 1600, QImage::Format_RGB32);
    page.image.fill(Qt::white);
    QPainter painter(&page.image);
    painter.setPen(Qt::black);
    for (int y = 0; y < page.image.height(); y += 48) {
        painter.drawText(24, y + 20, QStringLiteral("Live capture row %1: keep every row in order").arg(y * 7919));
        painter.drawText(790, y + 20, QString::number(y * 271, 16));
    }
    painter.end();
    page.show();
    QEventLoop settle;
    QTimer::singleShot(200, &settle, &QEventLoop::quit);
    settle.exec();
    const QImage direct = grabNative(nativeFromLogical(page.geometry()));
    const QPixmap fallback = grabQtScreen(page.geometry());
    const bool fallbackPassed = !direct.isNull() && fallback.devicePixelRatio() == 1
        && fallback.toImage().convertToFormat(QImage::Format_RGB32) == direct;
    if (!fallbackPassed) std::fprintf(stderr, "FAIL: Qt fallback differs from native pixels or capture dimensions\n");
    LongCapture capture(page.geometry());
    capture.start();
    const qreal scale = qreal(capture.m_lastFrame.height()) / page.height();
    bool passed = !capture.m_lastFrame.isNull() && fallbackPassed;
    for (int pos : {80, 180, 280, 180, 80, 0}) {
        page.offset = pos;
        page.repaint();
        QElapsedTimer budget;
        budget.start();
        const int expected = qRound(pos * scale);
        while (capture.m_frameRow != expected && budget.elapsed() < 2500) {
            QEventLoop wait;
            QTimer::singleShot(20, &wait, &QEventLoop::quit);
            wait.exec();
        }
        if (capture.m_frameRow != expected || capture.m_alignmentLost) {
            std::fprintf(stderr, "FAIL: live scrolling position=%d actual=%d expected=%d\n", pos, capture.m_frameRow, expected);
            passed = false;
            break;
        }
    }
    if (!previewPath.isEmpty()) {
        capture.updateUi();
        passed = capture.m_preview->grab().save(previewPath) && passed;
    }
    if (capture.m_rowEnd != qRound((page.height() + 280) * scale)) {
        std::fprintf(stderr, "FAIL: live capture accumulated an incorrect row count (actual=%d expected=%d scale=%.3f)\n",
                     capture.m_rowEnd, qRound((page.height() + 280) * scale), double(scale));
        passed = false;
    }
    capture.cancel();
    page.close();
    if (savedAutoCrop.isValid()) settings.setValue(QStringLiteral("LongShot/AutoCrop"), savedAutoCrop);
    else settings.remove(QStringLiteral("LongShot/AutoCrop"));
    settings.sync();
    if (passed) std::printf("PASS: live GDI scrolling capture, preview, down/up/revisit, DPR %.2f\n", double(scale));
    return passed ? 0 : 1;
}
