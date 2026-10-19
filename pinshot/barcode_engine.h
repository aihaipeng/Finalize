#pragma once

#include <QImage>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

// One decoded symbol in capture-image pixels. The quad follows the symbol's
// corners, which may be rotated or perspective-skewed.
struct BarcodeItem {
    QString text;
    QString format;
    QPolygonF quad;
    QRectF box;
};

struct BarcodeResult {
    QVector<BarcodeItem> items;
    QString error;
    bool cancelled = false;
    qint64 elapsed = 0;
};

using BarcodeCancel = std::shared_ptr<std::atomic_bool>;

class BarcodeEngine
{
public:
    // Decodes QR, matrix and 1D symbols from a QImage without network access.
    static BarcodeResult recognize(QImage image, const BarcodeCancel &cancel = {});
};
