#include "text/engines/barcode_engine.h"
#include "capture/capture_overlay.h"
#include "longshot/long_capture.h"
#include "text/cards/ocr_dialog.h"
#include "tests/test_support.h"
#include "shared/widgets.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QThreadPool>
#include <QToolButton>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

namespace {

const char *const kQrPng =
    "iVBORw0KGgoAAAANSUhEUgAAAJQAAACUAQAAAABdRz15AAABO0lEQVR4nO1WQWrEQAyTh73LP9j/Pys/kF+g4sleuqVQ6jmuIcQEIiayJCeM96r14xHweYbfOIiIBJAVr3bN+KONyBKR3c65j4SvZ4G7PTLL4sWE/vXut3rct2QZF87gGSkweLczvDACzdyuAqiRPx59pk1gIV/eWwM82CRp07BN2RrigRYE0aTMOV7DNSwNYo4nwhQI48j3mt7HlEBpeL4FREEtO1K3ctbsfLoptKw9FQ31jNY0cWEbmOO8L0fwAlN4Yuq31WEAFa9qtH2toT/Epq2VR075g3sKHcu3Cg/oj0TLeOcCfUDP3jkAt4Onflv3fhPD1TuuI3/N8GijgkWiXXxkvz0FZqO2pNcQD+hVVPVaHziAl73iklUa+gMdV/d4O2E6HDT1bwQy3clw5YF9+V71+XfEn/n7Alf1yGlCqVzEAAAAAElFTkSuQmCC";

const char *const kQr2Png =
    "iVBORw0KGgoAAAANSUhEUgAAAIQAAACEAQAAAAB5P74KAAAA+ElEQVR4nO2WMW4DQQwDKeN66gf+/7P8A+4LGHDtIk6qrNIlwgHH22IgiSvhyniPdftyAPzxk6rqVegtBhzaaJe2mGRYDS2uLU451/PFEvsXOlZ6USYcC4Jri1NOGQVw6wVQh/f5AmLSYp4fVfEeN6C7GwU2eh1zYNsykRYRto45gBgGaXHAoS1R5CSfC0CnKC739u28P0xChhU5qYupx3Jm9pBzAx71cBcXmufzDpuxy2Fw5FcCMd/5Oq+rqtriHc3zOcW2K6WRTnKD+1NEZ28UiprtMWUnQtJwrzaX2PcAR/2xchPj2WTekXn3k3nIqf//hE/xvT8fX2OdrO+CfZYAAAAASUVORK5CYII=";

const char *const kCode128Png =
    "iVBORw0KGgoAAAANSUhEUgAAA24AAADICAIAAAAfniVeAAAEPklEQVR4nO3dSW6DQBRAQRPl/lfubC0WX+0XcCSnamt6AE9PbDjWWg8AAHjdVxgDAABSEgCAzl1JAAAiKQkAQCQlAQCIpCQAAJGUBAAgkpIAAERSEgCASEoCABB9Pz7OcRzDq6cHRT4fPD9Dcv/IvLeT51X2Bw4n+Jsl9k/5NM8lA+c588XZ38wwz1VH7g+84yxemueOvV11UsOcw8CbvjXDErPh12b/ur3hkzmvmPe2OclV72leIs9z097+9j2dB+5PcsmnKH/dZpf8K61PfFq1u5IAAERSEgCASEoCABBJSQAAIikJAEAkJQEAiKQkAACRlAQAIJKSAABEUhIAgEhKAgAQSUkAACIpCQBAJCUBAIikJAAAkZQEACCSkgAARFISAIBISgIAEElJAAAiKQkAQCQlAQCIpCQAAJGUBAAgkpIAAERSEgCASEoCABBJSQAAIikJAEAkJQEAiKQkAACRlAQAIJKSAABEUhIAgEhKAgAQSUkAACIpCQBAJCUBAIikJAAAkZQEACCSkgAARFISAIBISgIAEElJAAAiKQkAQCQlAQCIpCQAAJGUBAAgkpIAAERSEgCASEoCABBJSQAAIikJAEAkJQEAiKQkAACRlAQAIJKSAABEUhIAgEhKAgAQSUkAACIpCQBAJCUBAIikJAAAkZQEACCSkgAARFISAIBISgIAEElJAAAiKQkAQCQlAQCIpCQAAJGUBAAgkpIAAERSEgCASEoCACAlAQB4L3clAQCIpCQAAJGUBAAgkpIAAERSEgCASEoCABBJSQAAIikJAEAkJQEAiKQkAACRlAQAIJKSAABEUhIAgEhKAgAQSUkAACIpCQBAJCUBAIikJAAAkZQEACCSkgAARFISAIBISgIAEElJAAAiKQkAQCQlAQCIpCQAAJGUBAAgkpIAAERSEgCASEoCABBJSQAAIikJAEAkJQEAiKQkAACRlAQAIJKSAABEUhIAgEhKAgAQSUkAACIpCQBAJCUBAIikJAAAkZQEACCSkgAARFISAIBISgIAEElJAAAiKQkAQCQlAQCIpCQAAJGUBAAgkpIAAERSEgCASEoCABBJSQAAIikJAEAkJQEAiKQkAACRlAQAIJKSAABEUhIAgEhKAgAQSUkAACIpCQBAJCUBAIikJAAAkZQEACCSkgAARFISAIBISgIAEElJAAAiKQkAQCQlAQCIpCQAAJGUBAAgkpIAAERSEgCASEoCABBJSQAAIikJAEAkJQEAiKQkAACRlAQAIJKSAABEUhIAgEhKAgAQSUkAACIpCQBAJCUBAIikJAAAkZQEACCSkgAARFISAIBISgIAEElJAAAiKQkAQCQlAQCIpCQAAJGUBAAgkpIAAERSEgCA6Fhr1bEAAPxr7koCABBJSQAAIikJAEAkJQEAiKQkAACRlAQAIJKSAABEUhIAgEhKAgAQSUkAAB7ND3UXiIu9RwqdAAAAAElFTkSuQmCC";

QImage fixture(const char *base64)
{
    QImage image = QImage::fromData(QByteArray::fromBase64(base64), "PNG");
    if (image.isNull()) {
        std::fprintf(stderr, "FAIL: embedded barcode fixture did not decode\n");
        std::exit(1);
    }
    image.setDevicePixelRatio(1);
    return image;
}

void waitFor(const std::function<bool()> &done, int timeoutMs = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QThread::msleep(10);
    }
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

} // namespace

int OcrDialog::testBarcode(const QString &previewPath)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    const BarcodeCancel cancel = std::make_shared<std::atomic_bool>(false);
    const QImage qr = fixture(kQrPng);
    const QImage qr2 = fixture(kQr2Png);
    const QImage code128 = fixture(kCode128Png);
    // Embedded fixture images were generated with these payloads; keep them stable.
    const QString qrText = QStringLiteral("https://example.com/pinshot");
    const QString qr2Text = QStringLiteral("PINSHOT-123");
    const QString code128Text = QStringLiteral("PINSHOT-123");

    const BarcodeResult qrResult = BarcodeEngine::recognize(qr, cancel);
    check(qrResult.error.isEmpty() && qrResult.items.size() == 1
              && qrResult.items.first().text == qrText
              && qrResult.items.first().format == QStringLiteral("QR Code"),
          "QR fixture decodes to its exact payload and format");
    check(qrResult.items.first().quad.size() == 4 && !qrResult.items.first().box.isEmpty(),
          "QR result carries a four-corner position and bounding box");
    const BarcodeResult linearResult = BarcodeEngine::recognize(code128, cancel);
    check(linearResult.error.isEmpty() && linearResult.items.size() == 1
              && linearResult.items.first().text == code128Text
              && linearResult.items.first().format == QStringLiteral("Code 128"),
          "Code 128 fixture decodes to its exact payload");
    QImage blank(320, 180, QImage::Format_RGB32);
    blank.fill(Qt::white);
    const BarcodeResult empty = BarcodeEngine::recognize(blank, cancel);
    check(empty.error.isEmpty() && empty.items.isEmpty(), "blank captures produce no symbols without an error");
    cancel->store(true);
    check(BarcodeEngine::recognize(qr, cancel).cancelled, "pre-cancelled work does not decode");
    cancel->store(false);

    QImage pair(400, 220, QImage::Format_RGB32);
    pair.fill(Qt::white);
    {
        QPainter painter(&pair);
        painter.drawImage(QPoint(20, 20), qr);
        painter.drawImage(QPoint(220, 20), qr2);
    }
    const BarcodeResult multiple = BarcodeEngine::recognize(pair, cancel);
    check(multiple.error.isEmpty() && multiple.items.size() >= 2,
          "a capture containing two symbols returns both");
    bool foundQr = false, foundQr2 = false;
    for (const BarcodeItem &item : multiple.items) {
        foundQr = foundQr || item.text == qrText;
        foundQr2 = foundQr2 || item.text == qr2Text;
    }
    check(foundQr && foundQr2, "multi-symbol results retain both payloads");

    const QString oldClipboard = QGuiApplication::clipboard()->text();
    OcrDialog card(qr, QRect(QPoint(0, 0), qr.size()), false, true);
    card.setAttribute(Qt::WA_DeleteOnClose, false);
    card.show();
    auto *text = card.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    auto *retry = card.findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    auto *copy = card.findChild<QToolButton *>(QStringLiteral("ocrTextCopy"));
    auto *open = card.findChild<QToolButton *>(QStringLiteral("barcodeOpen"));
    auto *picker = card.findChild<QComboBox *>(QStringLiteral("barcodePicker"));
    auto *kind = card.findChild<QLabel *>(QStringLiteral("barcodeKind"));
    check(text && retry && copy && open && picker && kind, "barcode card exposes its result controls");
    check(dynamic_cast<PickerCombo *>(picker) != nullptr,
          "barcode picker uses the shared chevron combo");
    check(card.findChild<QToolButton *>(QStringLiteral("ocrSearch")) == nullptr,
          "barcode card has no stray search button in its header");
    waitFor([&] { return retry->isEnabled() && !text->toPlainText().isEmpty(); });
    check(card.windowTitle() == QStringLiteral("LensSnip 二维码/条形码"),
          "barcode card title names both symbol families");
    check(text->toPlainText() == qrText && kind->text() == QStringLiteral("QR Code"),
          "single result shows payload and format");
    check(picker->isHidden() && open->isVisible() && card.barcodeRegionCount() == 1
              && card.barcodeSelected() == 0,
          "single URL result hides the picker, offers open-link and marks one region");
    copy->click();
    check(QGuiApplication::clipboard()->text() == qrText, "copy action copies the selected payload");
    QGuiApplication::clipboard()->setText(oldClipboard);
    if (!previewPath.isEmpty()) card.grab().save(previewPath + QStringLiteral("-barcode-single.png"));
    auto *back = card.findChild<QToolButton *>(QStringLiteral("ocrBack"));
    check(back != nullptr, "barcode card keeps the back button");
    back->click();
    QApplication::processEvents();
    check(card.returnedToSource() && !card.isVisible(), "back marks the barcode card as returning to its source");

    OcrDialog multi(pair, QRect(QPoint(0, 0), pair.size()), false, true);
    multi.setAttribute(Qt::WA_DeleteOnClose, false);
    multi.show();
    auto *multiText = multi.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    auto *multiRetry = multi.findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    auto *multiPicker = multi.findChild<QComboBox *>(QStringLiteral("barcodePicker"));
    waitFor([&] { return multiRetry->isEnabled() && !multiText->toPlainText().isEmpty(); });
    check(multi.barcodeRegionCount() >= 2 && multiPicker->isVisible()
              && multiPicker->count() >= 2,
          "multiple results expose a picker and one region per symbol");
    const int secondIndex = multiPicker->findText(qr2Text, Qt::MatchContains);
    check(secondIndex >= 0, "picker labels include each payload");
    multiPicker->setCurrentIndex(secondIndex);
    QApplication::processEvents();
    check(multiText->toPlainText() == qr2Text && multi.barcodeSelected() == secondIndex,
          "choosing a picker row swaps the payload and the marked region");
    if (!previewPath.isEmpty()) multi.grab().save(previewPath + QStringLiteral("-barcode-multi.png"));
    multi.reject();
    QApplication::processEvents();

    OcrDialog none(blank, QRect(QPoint(0, 0), blank.size()), false, true);
    none.setAttribute(Qt::WA_DeleteOnClose, false);
    none.show();
    auto *noneText = none.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    auto *noneRetry = none.findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    auto *noneOpen = none.findChild<QToolButton *>(QStringLiteral("barcodeOpen"));
    waitFor([&] { return noneRetry->isEnabled() && none.m_emptyRetry->isVisible(); });
    check(noneText->toPlainText().isEmpty() && none.m_busy->text().contains(QStringLiteral("未识别到二维码或条形码"))
              && !none.findChild<QToolButton *>(QStringLiteral("ocrTextCopy"))->isEnabled()
              && none.barcodeRegionCount() == 0 && noneOpen->isHidden(),
          "an empty result explains itself, marks nothing and hides open-link");
    if(!previewPath.isEmpty())none.grab().save(previewPath+QStringLiteral("-barcode-empty.png"));
    none.reject();
    QApplication::processEvents();
    QThreadPool::globalInstance()->waitForDone();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    std::puts("PASS: zxing QR/Code128, multi-symbol card, picker, copy, link and back");
    return 0;
}

int CaptureOverlay::testBarcode(const QString &previewPath)
{
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    const QImage qr = fixture(kQrPng);
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    const ScreenFrame frame{QRect(screen.topLeft(), qr.size()), QRect(QPoint(), qr.size()), qr};
    CaptureOverlay overlay({frame});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    overlay.m_mode = Adjusting;
    overlay.m_sel = overlay.localScreenRect(frame.logicalRect);
    overlay.placeToolbar();
    overlay.showToolBars();
    auto *button = overlay.m_tools->findChild<QToolButton *>(QStringLiteral("barcodeButton"));
    check(button && button->isEnabled() && button->toolTip() == QStringLiteral("识别二维码/条形码"),
          "capture toolbar exposes a usable barcode action");
    if (!previewPath.isEmpty()) overlay.grab().save(previewPath + QStringLiteral("-barcode-toolbar.png"));
    button->click();
    QPointer<OcrDialog> card = overlay.m_ocrDialog;
    check(card && card->windowTitle() == QStringLiteral("LensSnip 二维码/条形码"),
          "barcode action opens the barcode card");
    auto *retry = card ? card->findChild<QToolButton *>(QStringLiteral("ocrRetry")) : nullptr;
    waitFor([&] { return card && retry && retry->isEnabled() && card->barcodeRegionCount() > 0; });
    check(card && card->barcodeRegionCount() == 1, "capture selection reaches the decoder and marks the QR");
    auto *back = card ? card->findChild<QToolButton *>(QStringLiteral("ocrBack")) : nullptr;
    check(back != nullptr, "barcode card has a back action");
    back->click();
    QApplication::processEvents();
    check(overlay.isVisible() && overlay.m_tools->isVisible() && !overlay.m_ocrDialog,
          "back restores the live selection and toolbar after barcode recognition");
    overlay.close();
    std::puts("PASS: capture toolbar opens barcode card and back restores the selection");
    return 0;
}
