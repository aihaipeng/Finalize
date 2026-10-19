#include "text/engines/barcode_engine.h"

#include "Barcode.h"
#include "BarcodeFormat.h"
#include "ImageView.h"
#include "ReadBarcode.h"
#include <QElapsedTimer>
#include <QImage>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <algorithm>
#include <exception>

BarcodeResult BarcodeEngine::recognize(QImage image, const BarcodeCancel &cancel)
{
    BarcodeResult result;
    QElapsedTimer timer;
    timer.start();
    try {
        if (cancel && cancel->load()) {
            result.cancelled = true;
            return result;
        }
        if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
            result.error = tr("识别失败：截图区域为空");
            return result;
        }
        // zxing reads luminance rows directly; QImage's row padding is passed
        // explicitly so grayscale rows are not interpreted as packed pixels.
        QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
        gray.setDevicePixelRatio(1);
        const ZXing::ImageView view(gray.constBits(), gray.width(), gray.height(),
                                    ZXing::ImageFormat::Lum, gray.bytesPerLine());
        ZXing::ReaderOptions options;
        options.formats(ZXing::BarcodeFormat::All)
            .tryHarder(true)
            .tryRotate(true)
            .tryInvert(true)
            .tryDownscale(true)
            .maxNumberOfSymbols(32);
        const ZXing::Barcodes barcodes = ZXing::ReadBarcodes(view, options);
        for (const ZXing::Barcode &barcode : barcodes) {
            if (!barcode.isValid()) continue;
            BarcodeItem item;
            item.text = QString::fromStdString(barcode.text());
            item.format = QString::fromStdString(ZXing::ToString(barcode.format()));
            const ZXing::Position &position = barcode.position();
            item.quad = QPolygonF{
                QPointF(position.topLeft().x, position.topLeft().y),
                QPointF(position.topRight().x, position.topRight().y),
                QPointF(position.bottomRight().x, position.bottomRight().y),
                QPointF(position.bottomLeft().x, position.bottomLeft().y)};
            item.box = item.quad.boundingRect();
            // A linear symbol's position is a line; keep a visible selection band.
            if (item.box.height() < 4) item.box.adjust(0, -6, 0, 6);
            if (!item.text.isEmpty()) result.items.append(item);
        }
        result.cancelled = cancel && cancel->load();
        if (result.cancelled) result.items.clear();
    } catch (const std::exception &error) {
        result.items.clear();
        result.error = tr("识别失败：%1").arg(QString::fromUtf8(error.what()));
    }
    result.elapsed = timer.elapsed();
    return result;
}
