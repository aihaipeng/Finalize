#pragma once

#include <QImage>
#include <QString>

// ---------- Offline content-aware erase for the mosaic tool's smart mode. ----------
// Patch-based texture synthesis: the hole is refilled with pixels borrowed from
// a context band around it, matched by their already-known neighbours.
// image: any RGB format; mask: grayscale, 255 keeps pixels, 0 marks the hole.
namespace SmartErase {
// Pure function, safe on worker threads. Returns the full-size result with only
// hole pixels replaced, or a null image (with *error) when nothing can be borrowed.
QImage erase(const QImage &image, const QImage &mask, QString *error = nullptr);
}
