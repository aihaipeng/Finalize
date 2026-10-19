#include "pixel_inspector.h"
#include "widgets.h"
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
    m_tips.setDefaultFont(QFont(QStringLiteral("Microsoft YaHei")));
    QFont tipsFont = m_tips.defaultFont();
    tipsFont.setPixelSize(13);
    tipsFont.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
    m_tips.setDefaultFont(tipsFont);
    m_tips.setDocumentMargin(0);
    m_tips.setHtml(QStringLiteral("<div align='center' style='color:white'>按 <b>C</b> 复制颜色值<br>按 <b>Shift</b> 切换 RGB/HEX</div>"));
    setFixedSize(qMax(179, qCeil(m_tips.idealWidth()) + 4), 217);
    m_tips.setTextWidth(width());
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
    p.fillRect(rect(), QColor(0, 0, 0, 188));
    const QRect view((width() - 177) / 2, 1, 177, 121);
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
    const int line = fontMetrics().height(), y = 127;
    const QString x = QStringLiteral("(%1").arg(m_position.x());
    const QString coordinateY = QStringLiteral("%1)").arg(m_position.y());
    const int xWidth = fontMetrics().horizontalAdvance(x);
    const int commaWidth = fontMetrics().horizontalAdvance(QLatin1Char(','));
    const int yWidth = fontMetrics().horizontalAdvance(coordinateY);
    const int coordinateLeft = (width() - xWidth - commaWidth - yWidth - 12) / 2;
    p.drawText(QRect(coordinateLeft, y, xWidth, line), Qt::AlignVCenter, x);
    p.drawText(QRect(coordinateLeft + xWidth + 6, y, commaWidth, line), Qt::AlignVCenter, QStringLiteral(","));
    p.drawText(QRect(coordinateLeft + xWidth + commaWidth + 12, y, yWidth, line), Qt::AlignVCenter, coordinateY);
    const QString text = m_hex ? colorText()
        : QStringLiteral("%1, %2, %3").arg(m_color.red(), 3).arg(m_color.green(), 3).arg(m_color.blue(), 3);
    const int left = (width() - fontMetrics().horizontalAdvance(text) - 20) / 2;
    const QRect swatch(left, 153 + (line - 14) / 2, 14, 14);
    p.fillRect(swatch, m_color);
    p.drawRect(swatch.adjusted(0, 0, -1, -1));
    p.drawText(QRect(left + 20, 153, width() - left - 20, line), Qt::AlignVCenter, text);
    p.translate(0, 179);
    m_tips.drawContents(&p);
}
