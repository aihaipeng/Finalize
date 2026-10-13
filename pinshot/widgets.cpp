#include "widgets.h"
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QIcon>
#include <QColor>
#include <QPaintEvent>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QtMath>
#include <QToolButton>
#include <QStylePainter>
#include <QStyleOptionComboBox>
#include <qt_windows.h>

void buildIconPath(QPainterPath &path, Icon icon)
{
    switch (icon) {
    case Icon::Save:                                  // Floppy disk with label slot.
        path.moveTo(16, 3); path.lineTo(5, 3); path.lineTo(3, 5); path.lineTo(3, 19);
        path.lineTo(5, 21); path.lineTo(19, 21); path.lineTo(21, 19); path.lineTo(21, 8);
        path.lineTo(16, 3);
        path.moveTo(7, 21); path.lineTo(7, 13); path.lineTo(17, 13); path.lineTo(17, 21);
        path.moveTo(7, 3); path.lineTo(7, 8); path.lineTo(15, 8); path.lineTo(15, 3);
        break;
    case Icon::Copy:                                  // Two stacked sheets.
        path.addRoundedRect(9, 9, 12, 12, 2, 2);
        path.moveTo(6, 15); path.lineTo(5, 15); path.quadTo(3, 15, 3, 13); path.lineTo(3, 5);
        path.quadTo(3, 3, 5, 3); path.lineTo(13, 3); path.quadTo(15, 3, 15, 5); path.lineTo(15, 6);
        break;
    case Icon::Pin:                                   // Thumbtack: head plus needle.
        path.addRoundedRect(7, 3, 10, 9, 3, 3);
        path.moveTo(12, 12); path.lineTo(12, 21);
        break;
    case Icon::LongShot:                              // Chevrons pointing down: scrolling capture.
        path.moveTo(7, 6); path.lineTo(12, 11); path.lineTo(17, 6);
        path.moveTo(7, 13); path.lineTo(12, 18); path.lineTo(17, 13);
        break;
    case Icon::Cancel:                                // Cross.
        path.moveTo(6, 6); path.lineTo(18, 18);
        path.moveTo(18, 6); path.lineTo(6, 18);
        break;
    case Icon::Rect:                                  // Annotation shapes.
        path.addRect(4, 4, 16, 16);
        break;
    case Icon::Ellipse:
        path.addEllipse(4, 4, 16, 16);
        break;
    case Icon::Arrow:                                 // Diagonal with arrowhead.
        path.moveTo(7, 17); path.lineTo(17, 7);
        path.moveTo(8, 7); path.lineTo(17, 7); path.lineTo(17, 16);
        break;
    case Icon::ArrowBoth:                             // Arrowheads on both ends.
        path.moveTo(8, 16); path.lineTo(16, 8);
        path.moveTo(8, 16); path.lineTo(8, 11); path.moveTo(8, 16); path.lineTo(13, 16);
        path.moveTo(16, 8); path.lineTo(11, 8); path.moveTo(16, 8); path.lineTo(16, 13);
        break;
    case Icon::ArrowLine:                             // Plain line, no heads.
        path.moveTo(5, 17); path.lineTo(17, 5);
        break;
    case Icon::ArrowHollow:                           // Open V head, unfilled.
        path.moveTo(7, 17); path.lineTo(17, 7);
        path.moveTo(8, 7); path.lineTo(17, 7); path.lineTo(17, 16);
        path.lineTo(8, 7);
        break;
    case Icon::Pen:                                   // Diagonal pencil with nib.
        path.moveTo(17, 3); path.lineTo(21, 7); path.lineTo(8, 20); path.lineTo(3, 21);
        path.lineTo(4, 16); path.closeSubpath();
        break;
    case Icon::Mosaic:                                // Filled checkerboard inside a square outline.
        path.addRect(4, 4, 8, 8);
        path.addRect(12, 12, 8, 8);
        break;
    case Icon::Marker:                                // Highlighter: slanted body plus tip.
        path.moveTo(9, 11); path.lineTo(3, 17); path.lineTo(3, 20);
        path.lineTo(12, 20); path.lineTo(15, 17);
        path.moveTo(14, 4); path.lineTo(22, 12); path.lineTo(17, 17);
        path.lineTo(9, 9); path.closeSubpath();
        break;
    case Icon::Text:                                  // Letter T.
        path.moveTo(5, 6); path.lineTo(5, 4); path.lineTo(19, 4); path.lineTo(19, 6);
        path.moveTo(12, 4); path.lineTo(12, 20);
        path.moveTo(9, 20); path.lineTo(15, 20);
        break;
    case Icon::Bold:
        path.moveTo(7, 4); path.lineTo(7, 20); path.lineTo(14, 20);
        path.cubicTo(21, 20, 21, 12, 14, 12); path.lineTo(7, 12);
        path.moveTo(7, 4); path.lineTo(13, 4); path.cubicTo(20, 4, 20, 12, 13, 12);
        break;
    case Icon::Minimize:
        path.moveTo(5, 12); path.lineTo(19, 12);
        break;
    case Icon::Maximize:
        path.addRect(5, 5, 14, 14);
        break;
    case Icon::Restore:
        path.addRect(5, 8, 11, 11);
        path.moveTo(8, 5); path.lineTo(19, 5); path.lineTo(19, 16);
        break;
    case Icon::Refresh:
        path.moveTo(19, 9); path.cubicTo(18, 5, 14, 3, 10, 4);
        path.cubicTo(5, 5, 3, 9, 4, 14); path.cubicTo(5, 19, 11, 22, 16, 19);
        path.lineTo(19, 16); path.moveTo(19, 4); path.lineTo(19, 9); path.lineTo(14, 9);
        break;
    case Icon::Ocr:
        path.moveTo(8, 4); path.lineTo(4, 4); path.lineTo(4, 8);
        path.moveTo(16, 4); path.lineTo(20, 4); path.lineTo(20, 8);
        path.moveTo(4, 16); path.lineTo(4, 20); path.lineTo(8, 20);
        path.moveTo(20, 16); path.lineTo(20, 20); path.lineTo(16, 20);
        path.moveTo(8, 8); path.lineTo(16, 8);
        path.moveTo(12, 8); path.lineTo(12, 17);
        break;
    case Icon::Italic:
        path.moveTo(10, 4); path.lineTo(18, 4);
        path.moveTo(14, 4); path.lineTo(10, 20);
        path.moveTo(6, 20); path.lineTo(14, 20);
        break;
    case Icon::Number:                                // Circled digit one.
        path.addEllipse(2, 2, 20, 20);
        path.moveTo(13, 7); path.lineTo(10, 9);
        path.moveTo(13, 7); path.lineTo(13, 17);
        break;
    case Icon::Eraser:                                // Slanted eraser block.
        path.moveTo(7, 21); path.lineTo(2.7, 16.7); path.lineTo(2.7, 13.3);
        path.lineTo(12.3, 3.7); path.lineTo(15.7, 3.7); path.lineTo(21.3, 9.3);
        path.lineTo(21.3, 12.7); path.lineTo(13, 21); path.closeSubpath();
        path.moveTo(22, 21); path.lineTo(7, 21);
        path.moveTo(5, 11); path.lineTo(14, 20);
        break;
    case Icon::Undo:                                  // Hooked arrow turning back.
        path.moveTo(9, 14); path.lineTo(4, 9); path.lineTo(9, 4);
        path.moveTo(4, 9); path.lineTo(15, 9); path.quadTo(20, 9, 20, 14); path.lineTo(20, 20);
        break;
    case Icon::Redo:                                  // Hooked arrow turning forward.
        path.moveTo(15, 14); path.lineTo(20, 9); path.lineTo(15, 4);
        path.moveTo(20, 9); path.lineTo(9, 9); path.quadTo(4, 9, 4, 14); path.lineTo(4, 20);
        break;
    case Icon::Palette:
        path.moveTo(12, 3);
        path.cubicTo(7, 3, 3, 7, 3, 12); path.cubicTo(3, 17, 7, 21, 12, 21);
        path.cubicTo(15, 21, 15, 18, 13.5, 17); path.cubicTo(12, 15, 14, 13, 16, 14);
        path.cubicTo(19, 15, 21, 14, 21, 11); path.cubicTo(21, 6, 17, 3, 12, 3);
        path.closeSubpath();
        for (const QPointF &dot : {QPointF(7, 10), QPointF(10, 6.5), QPointF(15, 7), QPointF(6.5, 14)})
            path.addEllipse(dot, 0.55, 0.55);
        break;
    }
}

QIcon makeIcon(Icon icon)
{
    QPainterPath path;
    buildIconPath(path, icon);
    QIcon result;
    for (int px : {16, 24, 32}) {                     // 1x / 1.5x / 2x variants of one logical size.
        QPixmap pm(px, px);
        pm.setDevicePixelRatio(px / 16.0);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(16.0 / 24.0, 16.0 / 24.0);
        QPen pen(Qt::white, 1.75 * 24.0 / 16.0);   // Renders 1.75px at the 16px size.
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        if (icon == Icon::Mosaic) {
            p.fillPath(path, Qt::white);
            p.drawRect(QRectF(4, 4, 16, 16));
        }
        else p.drawPath(path);
        for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active, QIcon::Selected})
            for (QIcon::State state : {QIcon::Off, QIcon::On}) result.addPixmap(pm, mode, state);
    }
    return result;
}

QToolButton *iconButton(Icon icon, const QString &tip)
{
    auto *button = new QToolButton;
    button->setIcon(makeIcon(icon));
    button->setIconSize(QSize(16, 16));
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

FontComboBox::FontComboBox() : m_icon(makeIcon(Icon::Text)) {}

void FontComboBox::paintEvent(QPaintEvent *)
{
    QStylePainter painter(this);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    option.currentText.clear();
    option.currentIcon = m_icon;
    option.iconSize = QSize(16, 16);
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    painter.drawControl(QStyle::CE_ComboBoxLabel, option);
}

ColorWheel::ColorWheel(const QColor &color, QWidget *parent) : QWidget(parent)
{
    setFixedSize(220, 220);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setAccessibleName(QStringLiteral("圆形颜色选择器"));
    setToolTip(QStringLiteral("点击或拖动选色；左右调整色相，上下调整饱和度"));
    setCurrentColor(color);
}

QRectF ColorWheel::wheelRect() const
{
    const qreal side = qMin(width(), height()) - 20;
    return QRectF((width() - side) / 2, (height() - side) / 2, side, side);
}

QColor ColorWheel::currentColor() const { return m_color; }

QColor ColorWheel::brightColor() const { return QColor::fromHsvF(m_hue, m_saturation, 1).toRgb(); }

int ColorWheel::brightness() const { return qRound(m_value * 255); }

void ColorWheel::setCurrentColor(const QColor &color)
{
    if (!color.isValid() || color.toRgb() == m_color) return;
    m_color = color.toRgb();
    if (color.hsvHueF() >= 0) m_hue = color.hsvHueF();
    if (color.valueF() > 0) m_saturation = color.hsvSaturationF(); // Black keeps the last wheel position.
    m_value = color.valueF();
    update();
    emit colorChanged(m_color);
}

void ColorWheel::setBrightness(int value)
{
    const qreal next = qBound(0, value, 255) / 255.0;
    if (next == m_value) return;
    m_value = next;
    updateColor();
}

void ColorWheel::paintEvent(QPaintEvent *)
{
    const QRectF circle = wheelRect();
    const int side = qRound(circle.width() * devicePixelRatioF());
    if (m_disc.width() != side) {                 // Cache physical pixels; pointer drags only move the marker.
        m_disc = QImage(side, side, QImage::Format_ARGB32);
        m_disc.fill(Qt::transparent);
        const qreal radius = side / 2.0;
        for (int y = 0; y < side; ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(m_disc.scanLine(y));
            for (int x = 0; x < side; ++x) {
                const qreal dx = x + 0.5 - radius, dy = radius - y - 0.5;
                const qreal distance = qSqrt(dx * dx + dy * dy);
                const qreal coverage = qBound(qreal(0), radius - distance + 0.5, qreal(1));
                if (coverage <= 0) continue;
                qreal hue = qAtan2(dy, dx) / (2 * M_PI);
                if (hue < 0) hue += 1;
                QColor color = QColor::fromHsvF(hue, qMin(qreal(1), distance / radius), 1);
                color.setAlpha(qRound(coverage * 255));
                row[x] = color.rgba();
            }
        }
    }
    QPainter p(this);
    p.fillRect(rect(), QColor(29, 31, 35));
    p.setRenderHint(QPainter::Antialiasing);
    p.drawImage(circle, m_disc);
    p.setPen(QPen(QColor("#626468"), 1));
    p.setBrush(QColor(0, 0, 0, qRound((1 - m_value) * 255)));
    p.drawEllipse(circle);
    const qreal angle = m_hue * 2 * M_PI;
    const qreal radius = circle.width() / 2 * m_saturation;
    const QPointF marker = circle.center() + QPointF(qCos(angle) * radius, -qSin(angle) * radius);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 3));
    p.drawEllipse(marker, 5, 5);
    p.setPen(QPen(Qt::white, 1.5));
    p.drawEllipse(marker, 5, 5);
}

void ColorWheel::mousePressEvent(QMouseEvent *e)
{
    const QRectF circle = wheelRect();
    if (e->button() != Qt::LeftButton || QLineF(circle.center(), e->pos()).length() > circle.width() / 2) return;
    m_dragging = true;
    setFocus(Qt::MouseFocusReason);
    selectAt(e->pos());
}

void ColorWheel::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging && (e->buttons() & Qt::LeftButton)) selectAt(e->pos());
}

void ColorWheel::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) m_dragging = false;
}

void ColorWheel::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) {
        m_hue += e->key() == Qt::Key_Right ? 1.0 / 360 : -1.0 / 360;
        if (m_hue < 0) m_hue += 1;
        if (m_hue >= 1) m_hue -= 1;
    } else if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
        m_saturation = qBound(qreal(0), m_saturation + (e->key() == Qt::Key_Up ? 0.01 : -0.01), qreal(1));
    } else { QWidget::keyPressEvent(e); return; }
    updateColor();
}

void ColorWheel::selectAt(const QPoint &point)
{
    const QRectF circle = wheelRect();
    const QPointF delta = point - circle.center();
    const qreal distance = qSqrt(delta.x() * delta.x() + delta.y() * delta.y());
    if (distance > 0.01) {
        m_hue = qAtan2(-delta.y(), delta.x()) / (2 * M_PI);
        if (m_hue < 0) m_hue += 1;
    }
    m_saturation = qMin(qreal(1), distance / (circle.width() / 2));
    updateColor();
}

void ColorWheel::updateColor()
{
    m_color = QColor::fromHsvF(m_hue, m_saturation, m_value).toRgb();
    update();
    emit colorChanged(m_color);
}
