#include "long_capture.h"
#include <QClipboard>
#include "output.h"
#include "test_support.h"
#include "widgets.h"
#include "pin_window.h"
#include "ocr_dialog.h"
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
    QLabel preview;
    preview.resize(120, 240);
    large.m_preview = &preview;
    large.updateUi();
    large.m_preview = nullptr;
    regressionCheck(!preview.pixmap(Qt::ReturnByValue).isNull() && preview.pixmap(Qt::ReturnByValue).toImage()
                        == extended.scaled(1, 240, Qt::IgnoreAspectRatio, Qt::FastTransformation),
                    "very long preview scales the buffer view without copying the whole result first");
    auto *exportCapture = new LongCapture{QRect()};
    exportCapture->initTiles(16);
    regressionCheck(exportCapture->appendRows(extended, 0, extended.height()),
                    "a first frame larger than the growth step reserves enough storage");
    QPixmap exported;
    QObject::connect(exportCapture, &LongCapture::finished, [&exported](const QPixmap &pix) { exported = pix; });
    exportCapture->finish();
    regressionCheck(exported.toImage() == extended, "very long pinned output retains the original pixels");
    LongCapture exportFailure{QRect()};
    exportFailure.finish();
    regressionCheck(!exportFailure.m_done, "failed export keeps the capture session open");
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
    fullScreen.m_affinityOk = true;
    fullScreen.setUiVisible(false);
    check(!fullScreen.m_bar->isVisible(), "toolbar hides independently of preview capture exclusion");
    fullScreen.setUiVisible(true);
    check(fullScreen.m_bar->isVisible(), "toolbar returns after capture");
    fullScreen.cleanupUi();
    return failures ? 1 : 0;
}

int LongCapture::testLiveCapture(const QString &previewPath)
{
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
        std::fprintf(stderr, "FAIL: live capture accumulated an incorrect row count\n");
        passed = false;
    }
    capture.cancel();
    page.close();
    if (passed) std::printf("PASS: live GDI scrolling capture, preview, down/up/revisit, DPR %.2f\n", double(scale));
    return passed ? 0 : 1;
}
