#pragma once

#include <QImage>
#include <QString>
#include <QtMath>
#include <qt_windows.h>

bool copyImageToClipboard(const QImage &image);
bool copyTextToClipboard(const QString &text);

// Save through a temporary file so failed replacement never truncates the destination.
bool saveImageAtomic(const QImage &image, const QString &path);
