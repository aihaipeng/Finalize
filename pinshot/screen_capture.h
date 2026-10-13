#pragma once

#define PINSHOT_WDA_EXCLUDEFROMCAPTURE 0x00000011

#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QRect>
#include <QString>
#include <QScreen>
#include <QtMath>
#include <qt_windows.h>

// ---------- Screen capture ----------
// One desktop BitBlt with CAPTUREBLT also captures layered windows; falls back
// to the per-screen Qt path when the GDI call fails.
QImage grabNative(const QRect &native);

// Qt 6 names screens by monitor model instead of the GDI device name, so display
// queries must enumerate real devices; attached desktop entries carry \\.\DISPLAYn.
struct GdiDisplay { QString device; QRect native; };

QVector<GdiDisplay> gdiDisplays();

// Windows screen origins remain native even when Qt scales the screen sizes.
QRect nativeRectForScreen(const QRect &logical, const QVector<GdiDisplay> &displays);
QRect nativeRectForScreen(const QScreen *s, const QVector<GdiDisplay> &displays);

// Map a logical rect onto native pixels via the screen at its center; exact for
// uniform DPI, approximate across mixed-DPI boundaries (known limitation).
QRect nativeFromLogical(const QRect &logical);

QPixmap grabQtScreen(const QRect &r);
QPixmap grabScreen(const QRect &rect);
