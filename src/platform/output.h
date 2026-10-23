#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QtMath>
#include <qt_windows.h>

bool copyImageToClipboard(const QImage &image);
bool copyTextToClipboard(const QString &text);

// Encoding options for exports. Default values keep the previous behavior.
struct ImageSaveOptions
{
    QString format;          // Empty uses the destination suffix.
    int quality = -1;        // -1 keeps the encoder default; 0..100 for lossy formats.
    int scalePercent = 100;  // Clamped to 10..100; aspect ratio is preserved.
};

// Formats this build can actually write, in display order (PNG, JPG, WebP, BMP).
QStringList writableImageFormats();

// Save through a temporary file so failed replacement never truncates the destination.
bool saveImageAtomic(const QImage &image, const QString &path, const ImageSaveOptions &options = {});
