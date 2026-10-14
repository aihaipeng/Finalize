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
#include <QFrame>
#include <qt_windows.h>

// One 16px grid, glyphs inside a 2.5..13.5 box, straight strokes on half pixels so they
// stay crisp at 100%; `fill` holds the few solid accents.
static void buildIconPath(QPainterPath &path, QPainterPath &fill, Icon icon)
{
    switch (icon) {
    case Icon::Save:                                  // Floppy disk with shutter and label.
        path.moveTo(2.5, 2.5); path.lineTo(11, 2.5); path.lineTo(13.5, 5); path.lineTo(13.5, 13.5);
        path.lineTo(2.5, 13.5); path.closeSubpath();
        path.moveTo(5.5, 2.5); path.lineTo(5.5, 5.5); path.lineTo(10.5, 5.5); path.lineTo(10.5, 2.5);
        path.moveTo(4.5, 13.5); path.lineTo(4.5, 9.5); path.lineTo(11.5, 9.5); path.lineTo(11.5, 13.5);
        break;
    case Icon::Copy:                                  // Front sheet over a partly hidden back sheet.
        path.addRoundedRect(5.5, 5.5, 8, 8, 1.5, 1.5);
        path.moveTo(10.5, 4); path.lineTo(10.5, 3.5); path.quadTo(10.5, 2.5, 9.5, 2.5);
        path.lineTo(3.5, 2.5); path.quadTo(2.5, 2.5, 2.5, 3.5); path.lineTo(2.5, 9.5);
        path.quadTo(2.5, 10.5, 3.5, 10.5); path.lineTo(4, 10.5);
        break;
    case Icon::Pin:                                   // Thumbtack: cap, flared body, needle.
        path.moveTo(5, 2.5); path.lineTo(11, 2.5);
        path.moveTo(6.5, 2.5); path.lineTo(6.5, 6.5); path.lineTo(4.5, 9.5); path.lineTo(11.5, 9.5);
        path.lineTo(9.5, 6.5); path.lineTo(9.5, 2.5);
        path.moveTo(8, 9.5); path.lineTo(8, 13.5);
        break;
    case Icon::LongShot:                              // Page top with a downward arrow: scrolling capture.
        path.moveTo(2.5, 5.5); path.lineTo(2.5, 2.5); path.lineTo(13.5, 2.5); path.lineTo(13.5, 5.5);
        path.moveTo(8, 5.5); path.lineTo(8, 13);
        path.moveTo(5, 10); path.lineTo(8, 13); path.lineTo(11, 10);
        break;
    case Icon::Cancel:                                // Cross.
        path.moveTo(3.5, 3.5); path.lineTo(12.5, 12.5);
        path.moveTo(12.5, 3.5); path.lineTo(3.5, 12.5);
        break;
    case Icon::Rect:                                  // Plain square frame, square corners.
        path.addRect(2.5, 3.5, 11, 9);
        break;
    case Icon::Fill:                                  // Solid block inside a frame: filled shape.
        path.addRect(2.5, 3.5, 11, 9);
        fill.addRect(4, 5, 8, 6);
        break;
    case Icon::Dashed:                                // Three dashes on a diagonal.
        path.moveTo(2.5, 13.5); path.lineTo(4.5, 11.5);
        path.moveTo(7, 9); path.lineTo(9, 7);
        path.moveTo(11.5, 4.5); path.lineTo(13.5, 2.5);
        break;
    case Icon::Ellipse:
        path.addEllipse(QPointF(8, 8), 5.5, 5.5);
        break;
    case Icon::Arrow:                                 // Thin diagonal shaft with a swept-back solid head.
        path.moveTo(2.5, 13.5); path.lineTo(10, 6);
        fill.moveTo(13.5, 2.5); fill.lineTo(12, 9); fill.lineTo(10.2, 5.8); fill.lineTo(7, 4); fill.closeSubpath();
        break;
    case Icon::ArrowBoth:                             // Solid heads on both ends.
        path.moveTo(6, 10); path.lineTo(10, 6);
        fill.moveTo(13.5, 2.5); fill.lineTo(13.5, 7.5); fill.lineTo(8.5, 2.5); fill.closeSubpath();
        fill.moveTo(2.5, 13.5); fill.lineTo(2.5, 8.5); fill.lineTo(7.5, 13.5); fill.closeSubpath();
        break;
    case Icon::ArrowLine:                             // Plain line, no heads.
        path.moveTo(3, 13); path.lineTo(13, 3);
        break;
    case Icon::ArrowHollow:                           // Outline head, open inside.
        path.moveTo(3, 13); path.lineTo(9, 7);
        path.moveTo(13, 3); path.lineTo(13, 8.5); path.lineTo(7.5, 3); path.closeSubpath();
        break;
    case Icon::ArrowHollowBoth:                       // Outline heads on both ends.
        path.moveTo(6.5, 9.5); path.lineTo(9.5, 6.5);
        path.moveTo(13, 3); path.lineTo(13, 8); path.lineTo(8, 3); path.closeSubpath();
        path.moveTo(3, 13); path.lineTo(3, 8); path.lineTo(8, 13); path.closeSubpath();
        break;
    case Icon::ArrowCurve:                            // Curved shaft with a solid head.
        path.moveTo(2.5, 13); path.quadTo(3.5, 5, 9.5, 5);
        fill.moveTo(14, 5); fill.lineTo(9, 2); fill.lineTo(9, 8); fill.closeSubpath();
        break;
    case Icon::LineDashDense:                         // Dashed line, short period.
        for (qreal x = 2; x < 14; x += 3) { path.moveTo(x, 8); path.lineTo(x + 1.5, 8); }
        break;
    case Icon::LineDashSparse:                        // Dashed line, long period.
        path.moveTo(2, 8); path.lineTo(6, 8);
        path.moveTo(10, 8); path.lineTo(14, 8);
        break;
    case Icon::Pen:                                   // Slim pencil: body, ferrule line, nib.
        path.moveTo(11, 2.5); path.lineTo(13.5, 5); path.lineTo(5, 13.5); path.lineTo(2.5, 13.5);
        path.lineTo(2.5, 11); path.closeSubpath();
        path.moveTo(9.5, 4); path.lineTo(12, 6.5);
        break;
    case Icon::Mosaic:                                // Checkerboard: two solid cells in an outline.
        path.addRect(2.5, 2.5, 11, 11);
        fill.addRect(3, 3, 5, 5);
        fill.addRect(8, 8, 5, 5);
        break;
    case Icon::Marker:                                // Highlighter: wide body, chisel tip, ink base.
        path.moveTo(9, 2.5); path.lineTo(13.5, 7); path.lineTo(8.5, 12); path.lineTo(4, 7.5);
        path.closeSubpath();
        path.moveTo(5.5, 9); path.lineTo(3.5, 11); path.lineTo(5, 12.5); path.lineTo(7, 10.5);
        fill.addRect(2.5, 13, 11, 1);
        break;
    case Icon::Text:                                  // Letter T.
        path.moveTo(3.5, 4.5); path.lineTo(3.5, 2.5); path.lineTo(12.5, 2.5); path.lineTo(12.5, 4.5);
        path.moveTo(8, 2.5); path.lineTo(8, 13.5);
        path.moveTo(6, 13.5); path.lineTo(10, 13.5);
        break;
    case Icon::Bold:
        path.moveTo(4.5, 2.5); path.lineTo(9, 2.5); path.cubicTo(12.5, 2.5, 12.5, 7.5, 9, 7.5);
        path.lineTo(4.5, 7.5);
        path.moveTo(4.5, 2.5); path.lineTo(4.5, 13.5); path.lineTo(9.5, 13.5);
        path.cubicTo(13.5, 13.5, 13.5, 7.5, 9.5, 7.5);
        break;
    case Icon::Minimize:
        path.moveTo(3, 8.5); path.lineTo(13, 8.5);
        break;
    case Icon::Maximize:
        path.addRect(3.5, 3.5, 9, 9);
        break;
    case Icon::Restore:
        path.addRect(3.5, 5.5, 7, 7);
        path.moveTo(5.5, 3.5); path.lineTo(12.5, 3.5); path.lineTo(12.5, 10.5);
        break;
    case Icon::Refresh:                               // Open ring with an arrowhead at its end.
        path.moveTo(13, 8); path.arcTo(QRectF(3, 3, 10, 10), 0, 300);
        path.moveTo(13.5, 3.5); path.lineTo(13.5, 7.5); path.lineTo(9.5, 7.5);
        break;
    case Icon::Ocr:                                   // Scan corners around a letter T.
        path.moveTo(2.5, 5.5); path.lineTo(2.5, 2.5); path.lineTo(5.5, 2.5);
        path.moveTo(10.5, 2.5); path.lineTo(13.5, 2.5); path.lineTo(13.5, 5.5);
        path.moveTo(2.5, 10.5); path.lineTo(2.5, 13.5); path.lineTo(5.5, 13.5);
        path.moveTo(13.5, 10.5); path.lineTo(13.5, 13.5); path.lineTo(10.5, 13.5);
        path.moveTo(5.5, 5.5); path.lineTo(10.5, 5.5);
        path.moveTo(8, 5.5); path.lineTo(8, 10.5);
        break;
    case Icon::Italic:
        path.moveTo(7, 2.5); path.lineTo(12, 2.5);
        path.moveTo(4, 13.5); path.lineTo(9, 13.5);
        path.moveTo(9.5, 2.5); path.lineTo(6.5, 13.5);
        break;
    case Icon::Number:                                // Circled digit one.
        path.addEllipse(QPointF(8, 8), 5.5, 5.5);
        path.moveTo(6.5, 6.5); path.lineTo(8.5, 5); path.lineTo(8.5, 11);
        break;
    case Icon::Eraser:                                // Slanted block with a dividing band, on a base line.
        path.moveTo(9, 2.5); path.lineTo(13.5, 7); path.lineTo(8, 12.5); path.lineTo(5.5, 12.5);
        path.lineTo(2.5, 9.5); path.closeSubpath();
        path.moveTo(5.5, 6); path.lineTo(10, 10.5);
        path.moveTo(8, 13.5); path.lineTo(13.5, 13.5);
        fill.moveTo(9, 2.5); fill.lineTo(13.5, 7); fill.lineTo(10, 10.5); fill.lineTo(5.5, 6);
        fill.closeSubpath();
        break;
    case Icon::Undo:                                  // Hooked arrow turning back.
        path.moveTo(6, 3.5); path.lineTo(3, 6.5); path.lineTo(6, 9.5);
        path.moveTo(3, 6.5); path.lineTo(10, 6.5); path.quadTo(13.5, 6.5, 13.5, 10);
        path.quadTo(13.5, 13.5, 10, 13.5); path.lineTo(7, 13.5);
        break;
    case Icon::Redo:                                  // Hooked arrow turning forward.
        path.moveTo(10, 3.5); path.lineTo(13, 6.5); path.lineTo(10, 9.5);
        path.moveTo(13, 6.5); path.lineTo(6, 6.5); path.quadTo(2.5, 6.5, 2.5, 10);
        path.quadTo(2.5, 13.5, 6, 13.5); path.lineTo(9, 13.5);
        break;
    case Icon::Palette:                               // Paint palette with a thumb notch and colour wells.
        path.moveTo(8, 2.5);
        path.cubicTo(4.5, 2.5, 2.5, 5, 2.5, 8); path.cubicTo(2.5, 11.5, 5, 13.5, 8, 13.5);
        path.cubicTo(9.5, 13.5, 9.5, 11.5, 8.8, 11); path.cubicTo(8, 10, 9, 9, 10.5, 9.5);
        path.cubicTo(12.5, 10, 13.5, 9, 13.5, 7.5); path.cubicTo(13.5, 4.5, 11, 2.5, 8, 2.5);
        path.closeSubpath();
        for (const QPointF &dot : {QPointF(5, 7), QPointF(7, 4.8), QPointF(10, 5), QPointF(5, 10)})
            fill.addEllipse(dot, 0.9, 0.9);
        break;
    }
}

QIcon makeIcon(Icon icon)
{
    QPainterPath path, fill;
    buildIconPath(path, fill, icon);
    QIcon result;
    // Light bar language: dark glyphs, grey when disabled, white on the checked theme fill.
    const struct { QColor color; QIcon::State state; QList<QIcon::Mode> modes; } variants[] = {
        {QColor(0x32, 0x32, 0x32), QIcon::Off, {QIcon::Normal, QIcon::Active, QIcon::Selected}},
        {QColor(0x7b, 0x7b, 0x7b), QIcon::Off, {QIcon::Disabled}},
        {QColor(0x7b, 0x7b, 0x7b), QIcon::On, {QIcon::Disabled}},
        {QColor(Qt::white), QIcon::On, {QIcon::Normal, QIcon::Active, QIcon::Selected}},
    };
    for (const auto &variant : variants)
    for (int px : {16, 24, 32}) {                     // 1x / 1.5x / 2x variants of one logical size.
        QPixmap pm(px, px);
        pm.setDevicePixelRatio(px / 16.0);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(variant.color, 1);                   // One uniform 1px line weight across the set.
        pen.setCapStyle(Qt::SquareCap);
        pen.setJoinStyle(Qt::MiterJoin);
        p.setPen(pen);
        p.fillPath(fill, variant.color);
        p.drawPath(path);
        for (QIcon::Mode mode : variant.modes) result.addPixmap(pm, mode, variant.state);
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

QWidget *barSeparator()
{
    auto *line = new QFrame;
    line->setObjectName(QStringLiteral("barSeparator"));
    line->setFixedSize(5, 14);                         // 1px line plus 2px margins each side.
    return line;
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
    p.fillRect(rect(), QColor(0xf2, 0xf2, 0xf2));
    p.setRenderHint(QPainter::Antialiasing);
    p.drawImage(circle, m_disc);
    p.setPen(QPen(QColor("#858585"), 1));
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
