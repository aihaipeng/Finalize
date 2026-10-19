#include "capture/pixel_inspector.h"
#include "shared/widgets.h"
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QColor>
#include <QFont>
#include <QPaintEvent>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>
#include <qt_windows.h>

PixelInspector::PixelInspector(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont f(QStringLiteral("Consolas"));
    f.setPointSize(11);
    f.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
    setFont(f);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(230, 220);
    hide();
}

bool PixelInspector::setSample(const ScreenFrame &frame, const QPoint &globalPixel,
               const QRectF &selectionPixels)
{
    if (frame.image.isNull() || !frame.pixelRect.contains(globalPixel)
        || frame.image.size() != frame.pixelRect.size())
        return false;
    const QPoint pixel = globalPixel - frame.pixelRect.topLeft();
    m_color = frame.image.pixelColor(pixel);
    m_position = globalPixel;
    const QPoint origin = pixel - QPoint(12, 8);
    m_patch = frame.image.copy(QRect(origin, QSize(25, 17)));
    m_patch.setDevicePixelRatio(1);
    if (!selectionPixels.isEmpty()) {
        QPainter p(&m_patch);
        p.translate(-origin);
        QPainterPath mask;
        mask.addRect(QRectF(origin, m_patch.size()));
        mask.addRect(selectionPixels);
        p.fillPath(mask, QColor(0, 0, 0, 120));
        p.setPen(QPen(Bar::accent, 1));
        p.drawRect(selectionPixels.adjusted(0, 0, -1, -1));
    }
    update();
    return true;
}

QString PixelInspector::colorText() const
{
    if (!m_color.isValid())
        return QString();
    return m_hex ? m_color.name(QColor::HexRgb)
                 : QStringLiteral("%1, %2, %3").arg(m_color.red()).arg(m_color.green()).arg(m_color.blue());
}

void PixelInspector::toggleFormat() { m_hex = !m_hex; update(); }

void PixelInspector::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Bar::divider,1));p.setBrush(Bar::surface);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5),8,8);
    p.setBrush(Qt::NoBrush);
    const QRect view((width() - 177) / 2, 12, 177, 121);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.save();
    p.setClipRect(view);
    p.drawImage(view.adjusted(1, 1, -1, -1), m_patch);
    const QRect pixel(view.left() + 85, view.top() + 57, 7, 7);
    QColor crosshair = Bar::accent;
    crosshair.setAlpha(88);
    p.fillRect(QRect(pixel.left(), view.top() + 1, 7, 119), crosshair);
    p.fillRect(QRect(view.left() + 1, pixel.top(), 175, 7), crosshair);
    p.fillRect(pixel, m_color);
    p.setPen(Qt::black);
    p.drawRect(pixel.adjusted(-1, -1, 0, 0));
    p.setPen(Qt::white);
    p.drawRect(pixel.adjusted(0, 0, -1, -1));
    p.restore();
    p.setPen(Qt::white);
    p.drawRect(view.adjusted(0, 0, -1, -1));
    QFont valueFont=font();valueFont.setPixelSize(14);p.setFont(valueFont);
    p.setPen(Bar::ink);p.setBrush(m_color);p.drawRoundedRect(QRect(20,150,20,20),4,4);
    p.drawText(QRect(52,148,160,24),Qt::AlignVCenter,m_color.name(QColor::HexRgb).toUpper());
    QFont rgb=valueFont;rgb.setPixelSize(12);p.setFont(rgb);p.setPen(Bar::muted);
    p.drawText(QRect(20,180,190,20),Qt::AlignVCenter,
        QStringLiteral("RGB  %1, %2, %3").arg(m_color.red()).arg(m_color.green()).arg(m_color.blue()));
}
