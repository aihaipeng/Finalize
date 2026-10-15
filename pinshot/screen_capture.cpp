#include "screen_capture.h"
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QScreen>
#include <QPainter>
#include <QtMath>
#include <qt_windows.h>

QImage grabNative(const QRect &native)
{
    if (native.width() <= 0 || native.height() <= 0)
        return {};
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, native.width(), native.height());
    QImage img;
    if (bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        if (BitBlt(mem, 0, 0, native.width(), native.height(), screen,
                   native.x(), native.y(), SRCCOPY | CAPTUREBLT)) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = native.width();
            info.bmiHeader.biHeight = -native.height();  // Top-down rows.
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            img = QImage(native.width(), native.height(), QImage::Format_RGB32);
            if (!GetDIBits(mem, bmp, 0, native.height(), img.bits(), &info, DIB_RGB_COLORS))
                img = {};
        }
        if (old)
            SelectObject(mem, old);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

QVector<GdiDisplay> gdiDisplays()
{
    QVector<GdiDisplay> out;
    for (DWORD i = 0; ; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0))
            break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
            continue;
        out.append({QRect(mode.dmPosition.x, mode.dmPosition.y, mode.dmPelsWidth, mode.dmPelsHeight)});
    }
    return out;
}

QRect nativeRectForScreen(const QRect &logical, const QVector<GdiDisplay> &displays)
{
    for (const GdiDisplay &d : displays)
        if (d.native.topLeft() == logical.topLeft())
            return d.native;
    return {};
}

QRect nativeRectForScreen(const QScreen *s, const QVector<GdiDisplay> &displays)
{
    return s ? nativeRectForScreen(s->geometry(), displays) : QRect();
}

QRect nativeFromLogical(const QRect &logical)
{
    const QScreen *s = QGuiApplication::screenAt(logical.center());
    if (!s)
        s = QGuiApplication::primaryScreen();
    const QRect native = nativeRectForScreen(s, gdiDisplays());
    if (native.isEmpty())
        return {};
    const QRect lg(s->geometry());
    const double sx = double(native.width()) / lg.width(), sy = double(native.height()) / lg.height();
    return QRect(qRound(native.left() + (logical.left() - lg.left()) * sx),
                 qRound(native.top() + (logical.top() - lg.top()) * sy),
                 qRound(logical.width() * sx), qRound(logical.height() * sy));
}

QPixmap grabQtScreen(const QRect &r)
{
    for (QScreen *s : QGuiApplication::screens()) {
        if (s->geometry().contains(r)) {
            const QPoint local = r.topLeft() - s->geometry().topLeft();
            QPixmap out = s->grabWindow(0, local.x(), local.y(), r.width(), r.height());
            out.setDevicePixelRatio(1);
            return out;
        }
    }
    qreal dpr = 1;
    for (QScreen *s : QGuiApplication::screens())
        if (s->geometry().intersects(r)) dpr = qMax(dpr, s->devicePixelRatio());
    QPixmap out(qRound(r.width() * dpr), qRound(r.height() * dpr));
    if (out.isNull()) return {};
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::black);
    QPainter p(&out);
    for (QScreen *s : QGuiApplication::screens()) {
        const QRect inter = s->geometry().intersected(r);
        if (!inter.isEmpty()) {
            const QPoint local = inter.topLeft() - s->geometry().topLeft();
            const QPixmap pixels = s->grabWindow(0, local.x(), local.y(), inter.width(), inter.height());
            if (pixels.isNull()) return {};
            p.drawPixmap(QRect(inter.topLeft() - r.topLeft(), inter.size()), pixels, pixels.rect());
        }
    }
    p.end();
    out.setDevicePixelRatio(1);
    return out;
}

QPixmap grabScreen(const QRect &rect)
{
    const QRect r = rect.isEmpty() ? QGuiApplication::primaryScreen()->virtualGeometry() : rect;
    QPixmap out = QPixmap::fromImage(grabNative(nativeFromLogical(r)));
    return out.isNull() ? grabQtScreen(r) : out;
}
