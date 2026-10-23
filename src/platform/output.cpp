#include "platform/output.h"
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QtMath>
#include <QClipboard>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSaveFile>
#include <qt_windows.h>

template <typename Set>
static bool setClipboardWithRetry(Set set)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QElapsedTimer retry;
    retry.start();
    do {
        const DWORD sequence = GetClipboardSequenceNumber();
        set(clipboard);
        if (clipboard->ownsClipboard() && GetClipboardSequenceNumber() != sequence) return true;
        QEventLoop wait;
        QTimer::singleShot(15, &wait, &QEventLoop::quit);
        wait.exec(QEventLoop::ExcludeUserInputEvents); // Clipboard and IME owners need system events to release contention.
    } while (retry.elapsed() < 300);
    return false;
}

bool copyImageToClipboard(const QImage &image)
{
    return !image.isNull() && setClipboardWithRetry([&](QClipboard *c) { c->setImage(image); });
}

bool copyTextToClipboard(const QString &text)
{
    return !text.isEmpty() && setClipboardWithRetry([&](QClipboard *c) { c->setText(text); });
}

bool saveImageAtomic(const QImage &image, const QString &path, const ImageSaveOptions &options)
{
    if (image.isNull()) return false;
    QByteArray format = options.format.trimmed().toLower().toLatin1();
    if (format.isEmpty()) format = QFileInfo(path).suffix().toLower().toLatin1();
    if (format == "jpeg") format = "jpg";
    if (format.isEmpty()) return false;

    QImage output = image;
    const int scale = qBound(10, options.scalePercent, 100);
    if (scale < 100) {
        const int width = qMax(1, qRound(output.width() * scale / 100.0));
        const int height = qMax(1, qRound(output.height() * double(width) / output.width()));
        output = output.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    if (format == "jpg") {
        // JPEG has no alpha; rounded captures must not turn transparent corners black.
        if (output.hasAlphaChannel()) {
            QImage flattened(output.size(), QImage::Format_RGB32);
            flattened.fill(Qt::white);
            QPainter painter(&flattened);
            painter.drawImage(0, 0, output);
            painter.end();
            output = flattened;
        }
    } else if (format == "png" && output.hasAlphaChannel()) {
        // Fully opaque captures drop the alpha channel, matching the smaller PNG
        // most screenshot tools produce; rounded captures keep it.
        QImage rgba = output.convertToFormat(QImage::Format_ARGB32);
        bool translucent = false;
        for (int y = 0; y < rgba.height() && !translucent; ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(rgba.constScanLine(y));
            for (int x = 0; x < rgba.width(); ++x) {
                if (qAlpha(line[x]) != 255) {
                    translucent = true;
                    break;
                }
            }
        }
        if (!translucent) output = rgba.convertToFormat(QImage::Format_RGB32);
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    return output.save(&file, format.constData(), options.quality) && file.commit();
}

QStringList writableImageFormats()
{
    QStringList supported;
    for (const QByteArray &format : QImageWriter::supportedImageFormats())
        supported.append(QString::fromLatin1(format).toLower());
    QStringList result;
    for (const char *name : {"png", "jpg", "webp", "bmp"}) {
        const QString format = QString::fromLatin1(name);
        if (supported.contains(format) || (format == QLatin1String("jpg") && supported.contains(QStringLiteral("jpeg"))))
            result.append(format);
    }
    return result;
}
