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
    case Icon::Dashed:                                // Three horizontal dashes on the pixel grid, so they stay full contrast.
        path.moveTo(2.5, 8.5); path.lineTo(3.5, 8.5);
        path.moveTo(7.5, 8.5); path.lineTo(8.5, 8.5);
        path.moveTo(12.5, 8.5); path.lineTo(13.5, 8.5);
        break;
    case Icon::Ellipse:
        path.addEllipse(QPointF(8, 8), 5.5, 5.5);
        break;
    case Icon::Arrow:                                 // Thin diagonal shaft with a swept-back solid head.
        path.moveTo(2.5, 13.5); path.lineTo(10, 6);
        fill.moveTo(13.5, 2.5); fill.lineTo(12, 9); fill.lineTo(10.2, 5.8); fill.lineTo(7, 4); fill.closeSubpath();
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

// The classic 20-color paint palette, dark row first.
static const QColor kPresets[ColorBoard::PresetCount] = {
    "#000000", "#7f7f7f", "#880015", "#ed1c24", "#ff7f27", "#fff200", "#22b14c", "#00a2e8", "#3f48cc", "#a349a4",
    "#ffffff", "#c3c3c3", "#b97a57", "#ffaec9", "#ffc90e", "#efe4b0", "#b5e61d", "#99d9ea", "#7092be", "#c8bfe7"};

const QColor &ColorBoard::preset(int index) { return kPresets[qBound(0, index, PresetCount - 1)]; }

ColorBoard::ColorBoard(const QColor &current, QWidget *parent) : QWidget(parent), m_current(current)
{
    setObjectName(QStringLiteral("colorBoard"));
    setFixedSize(158, 26);                             // 24px current color, 3px gap, 10 x 13px cells, 1px hover margin.
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(QStringLiteral("颜色"));
}

QRect ColorBoard::swatchRect(int index)
{
    return QRect(28 + 13 * (index % 10), 1 + 13 * (index / 10), 11, 11);
}

void ColorBoard::setCurrentColor(const QColor &color)
{
    if (!color.isValid() || color == m_current) return;
    m_current = color;
    update(currentRect());
}

int ColorBoard::swatchAt(const QPoint &pos) const
{
    for (int i = 0; i < PresetCount; ++i)
        if (swatchRect(i).contains(pos)) return i;   // Gaps between cells pick nothing.
    return -1;
}

void ColorBoard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRect big = currentRect();
    p.fillRect(big, QColor(0x22, 0x22, 0x22));
    p.fillRect(big.adjusted(1, 1, -1, -1), m_current);
    // Rainbow corner marks the button that opens the full color dialog.
    for (int y = big.bottom() - 8; y < big.bottom(); ++y)
        for (int x = big.right() - 1 - (y - (big.bottom() - 8)); x < big.right(); ++x) {
            const bool edge = x + y == big.right() + big.bottom() - 9;
            int hue = qRound(180 + 20.5 * ((x - big.left()) - (y - big.top())));
            hue = ((hue % 360) + 360) % 360;
            p.fillRect(QRect(x, y, 1, 1), QColor::fromHsv(hue, 249, edge ? 125 : 249));
        }
    for (int i = 0; i < PresetCount; ++i) {
        const QColor &c = kPresets[i];
        QRect r = swatchRect(i);
        if (i == m_hover) r.adjust(-1, -1, 1, 1);
        // Light top/left frame, color-tinted dark bottom/right edge.
        const QColor shade(34 + qRound(c.red() * 0.325), 34 + qRound(c.green() * 0.325), 34 + qRound(c.blue() * 0.325));
        p.fillRect(r, i == m_hover ? QColor(0x40, 0x40, 0x40) : QColor(0x71, 0x71, 0x71));
        p.fillRect(r.adjusted(1, 1, 0, 0), shade);
        p.fillRect(r.adjusted(1, 1, -1, -1), c);
    }
}

void ColorBoard::mouseMoveEvent(QMouseEvent *e)
{
    const int hover = swatchAt(e->pos());
    if (hover == m_hover) return;
    m_hover = hover;
    update();
}

void ColorBoard::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    const int index = swatchAt(e->pos());
    if (index >= 0) emit presetPicked(kPresets[index]);
    else if (currentRect().contains(e->pos())) emit customRequested();
}

void ColorBoard::leaveEvent(QEvent *)
{
    if (m_hover < 0) return;
    m_hover = -1;
    update();
}
