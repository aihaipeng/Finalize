#include "onnxruntime_c_api.h"
#include <QClipboard>
// pinshot — screenshot capture, pinned images, scrolling capture, and offline OCR (Qt 6 / MinGW).
// shortcut: Screen captures run serially; use concurrent streamers if synchronized frames are required.

#include "controller.h"
#include "long_capture.h"
#include "ocr_engine.h"
#include "capture_overlay.h"
#include "pin_window.h"
#include "widgets.h"
#include "test_support.h"
#include <QGuiApplication>
#include <QVector>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QApplication>
#include <QtMath>
#include <QMessageBox>
#include <QMimeData>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QTranslator>
#include <QStyleHints>
#include <qt_windows.h>
#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);   // Standard dialogs match the light toolbars.
    QTranslator qtTranslator;                   // Chinese labels for standard Qt dialogs.
    if (qtTranslator.load(QStringLiteral("qtbase_zh_CN"), QCoreApplication::applicationDirPath() + QStringLiteral("/translations")))
        app.installTranslator(&qtTranslator);
    if (app.arguments().contains(QStringLiteral("--test-ocr-lifetime")))
        return OcrDialog::testLifetime();
    if (app.arguments().contains(QStringLiteral("--test-translate")))
        return OcrDialog::testTranslate();
    if (app.arguments().contains(QStringLiteral("--test-ocr-languages")))
        return OcrEngine::testLanguages(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-ocr-languages"))+1));
    if (app.arguments().contains(QStringLiteral("--test-ocr")))
        return CaptureOverlay::testOcr(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-ocr"))+1));
    if (app.arguments().contains(QStringLiteral("--test-ocr-loader"))) {
        auto &engine = OcrEngine::instance();
        const QString root = app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-ocr-loader"))+1,
                                                  QCoreApplication::applicationDirPath());
        const QString missing = OcrEngine::assetError(root);
        if (!missing.isEmpty()) { std::printf("UNAVAILABLE: %s\n", qPrintable(missing)); return 0; }
        const QString error = engine.load(root);
        if (!error.isEmpty()) { std::fprintf(stderr,"FAIL: %s\n",qPrintable(error)); return 1; }
        std::printf("PASS: ORT %s / API %d / recognition dictionary %d entries\n",qPrintable(engine.version()),ORT_API_VERSION,engine.dictionarySize());
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--test-mosaic")))
        return CaptureOverlay::testMosaic(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-mosaic")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-output"))) {
        auto *saved = new QMimeData;
        if (const QMimeData *data = QGuiApplication::clipboard()->mimeData())
            for (const QString &format : data->formats()) saved->setData(format, data->data(format));
        const int longResult = LongCapture::testOutput();
        const int captureResult = CaptureOverlay::testOutput();
        QGuiApplication::clipboard()->setMimeData(saved);
        return longResult || captureResult ? 1 : 0;
    }
    if (app.arguments().contains(QStringLiteral("--test-stitching")))
        return LongCapture::testStitching();
    if (app.arguments().contains(QStringLiteral("--test-live-capture")))
        return LongCapture::testLiveCapture(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-live-capture")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-pin-placement")))
        return PinWindow::testPlacement();
    if (app.arguments().contains(QStringLiteral("--test-color-picker")))
        return CaptureOverlay::testColorPicker(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-color-picker")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-selection")))
        return CaptureOverlay::testSelection();
    if (app.arguments().contains(QStringLiteral("--test-window-detection")))
        return CaptureOverlay::testWindowDetection();
    if (app.arguments().contains(QStringLiteral("--test-text-annotation")))
        return CaptureOverlay::testTextAnnotation();
    if (app.arguments().contains(QStringLiteral("--test-color-wheel")))
        return ColorBoard::testRendering();
    const int probe = app.arguments().indexOf(QStringLiteral("--probe-uia"));
    if (probe > 0 && probe + 2 < app.arguments().size()) {
        // Diagnostic: run the production element engines at one native-pixel screen point.
        const QPoint point(app.arguments().at(probe + 1).toInt(), app.arguments().at(probe + 2).toInt());
        const HRESULT com = CoInitialize(nullptr);
        UiaAutomation *automation = nullptr;
        if (SUCCEEDED(com))
            CoCreateInstance(kUiaClassId, nullptr, CLSCTX_INPROC_SERVER, kUiaIid,
                             reinterpret_cast<void **>(&automation));
        QElapsedTimer t;
        t.start();
        const WindowTargets targets = detectWindowRects();
        const qint64 enumMs = t.elapsed();
        std::printf("detectWindowRects %lldms (%lld windows)\n", enumMs, (long long)targets.size());
        for (const DetectedWindow &win : targets) {
            if (!win.native.contains(point)) continue;
            QStringList slow;
            QElapsedTimer total, t;
            total.start();
            t.start();
            const QRect uia = uiaRectAt(automation, win.hwnd, point, win.native, QRect(), &slow, win.program);
            const qint64 uiaMs = t.restart();
            const QRect msaa = SUCCEEDED(com) ? accessibleAt(win.hwnd, point, win.native) : QRect();
            const qint64 msaaMs = t.restart();
            const QRect child = childAt(win, point);
            const qint64 childMs = t.elapsed();
            std::printf("window (%d,%d %dx%d) program=%s\nuia    (%d,%d %dx%d) %lldms\nmsaa   (%d,%d %dx%d) %lldms\nchild  (%d,%d %dx%d) %lldms\ntotal query %lldms\n",
                        win.native.x(), win.native.y(), win.native.width(), win.native.height(),
                        qPrintable(win.program),
                        uia.x(), uia.y(), uia.width(), uia.height(), uiaMs,
                        msaa.x(), msaa.y(), msaa.width(), msaa.height(), msaaMs,
                        child.x(), child.y(), child.width(), child.height(), childMs,
                        total.elapsed());
            break;
        }
        if (automation) automation->Release();
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--dump-windows"))) {
        // Diagnostic: print detected targets in global logical coordinates.
        const QVector<ScreenFrame> frames = freezeScreens();
        if (frames.isEmpty()) { std::puts("no frames"); return 1; }
        for (const DetectedWindow &w : detectWindowRects()) {
            const QRect top = logicalFromNative(frames, w.native);
            std::printf("top  (%d,%d %dx%d)", top.x(), top.y(), top.width(), top.height());
            for (const QRect &c : w.nativeChildren) {
                const QRect lc = logicalFromNative(frames, c);
                std::printf("  child (%d,%d %dx%d)", lc.x(), lc.y(), lc.width(), lc.height());
            }
            std::printf("\n");
        }
        return 0;
    }
    app.setQuitOnLastWindowClosed(false);            // Keep the tray active after all pins are closed.
    app.setApplicationName(QStringLiteral("pinshot"));

    Controller ctrl;
    HotkeyFilter hf;
    hf.onHotkey = [&ctrl] { ctrl.startCapture(); };
    app.installNativeEventFilter(&hf);
    if (!RegisterHotKey(nullptr, 1, MOD_NOREPEAT, VK_F1))
        QMessageBox::warning(nullptr, QStringLiteral("pinshot"),
                             QStringLiteral("注册 F1 热键失败（可能被其他截图工具占用）"));

    const int exitCode = app.exec();
    QThreadPool::globalInstance()->waitForDone();
    return exitCode;
}
