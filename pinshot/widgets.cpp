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
#include <QSvgRenderer>
#include <qt_windows.h>

QIcon makeIcon(Icon icon, int size)
{
    static const char *const names[] = {
        "save", "copy", "pin", "long_shot", "ocr", "cancel", "rect", "ellipse", "arrow", "pen", "marker",
        "text", "number", "mosaic", "eraser", "undo", "redo", "bold", "italic", "minimize", "maximize",
        "restore", "refresh", "fill", "dashed", "icons8-line-width", "marker_rect"};
    QSvgRenderer svg(QStringLiteral(":/icons/%1.svg").arg(QLatin1String(names[int(icon)])));
    QIcon result;
    // Light bar language: dark glyphs, grey when disabled, white on the checked theme fill.
    const struct { QColor color; QIcon::State state; QList<QIcon::Mode> modes; } variants[] = {
        {QColor(0x32, 0x32, 0x32), QIcon::Off, {QIcon::Normal, QIcon::Active, QIcon::Selected}},
        {QColor(0x7b, 0x7b, 0x7b), QIcon::Off, {QIcon::Disabled}},
        {QColor(0x7b, 0x7b, 0x7b), QIcon::On, {QIcon::Disabled}},
        {QColor(Qt::white), QIcon::On, {QIcon::Normal, QIcon::Active, QIcon::Selected}},
    };
    for (const auto &variant : variants)
    for (int px : {size, size * 3 / 2, size * 2}) {   // 1x / 1.5x / 2x variants of one logical size.
        QPixmap pm(px, px);
        pm.setDevicePixelRatio(px / qreal(size));
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        svg.render(&p, QRectF(0, 0, size, size));
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);   // Recolor the glyph, keep its alpha.
        p.fillRect(QRectF(0, 0, size, size), variant.color);
        p.end();
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
