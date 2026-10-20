#pragma once

#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QIcon>
#include <QColor>
#include <QPaintEvent>
#include <QWidget>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <qt_windows.h>

// ---------- Shared icon set for all buttons. ----------
// Sharp-cornered SVG glyphs compiled in from icons/icons.qrc; the enum order matches
// the file-name table in makeIcon.

enum class Icon { Save, Copy, Pin, LongShot, Ocr, Cancel, Rect, Ellipse, Arrow, Pen, Marker, Text,
                  Number, Mosaic, Eraser, Undo, Redo, Bold, Italic, Minimize, Maximize, Restore, Refresh,
                  Fill, Dashed, LineWidth, MarkerRect, FitView, Search, TextFormat, SplitColumns, SplitRows,
                  ChevronUp, ChevronDown, ChevronRight, WavyLine, Underline, Strikethrough, Check, Translate, View,
                  CardFit, CardTranslate, CardReload, CardCopy, CardZoomIn,
                  CardZoomOut, CardActual, CardPin, CardMinimize, CardMaximize, CardRestore, CardClose, CardSearch,
                  CardUp, CardDown, CardRight, CardFormat, Back, Crop, CropStart, CropEnd, Barcode, CardLink,
                  RoundRect, CornerSquare, CornerRounded, AspectLock, Warning, Sliders };

// dark: glyphs for a dark surface (light ink) instead of the light bars.
QIcon makeIcon(Icon icon, int size = 16, bool dark = false);

// Text-card glyphs: muted ink, accent when hovered (QIcon::Active) or checked, dim when disabled.
QIcon cardIcon(Icon icon, int size = 16);

QToolButton *iconButton(Icon icon, const QString &tip);

// Shared dark chrome tokens: one surface, ink, accent and radius scale for every floating bar and menu.
namespace Bar {
inline const QColor background{"#101114"}, surface{"#18191d"}, content{"#0c0d10"},
    field{"#111216"}, ink{"#f4f5f8"}, muted{"#9c9da1"}, dim{"#73757f"},
    accent{"#5e6ad2"}, divider{"#2b2d33"}, hover{"#222326"}, selection{"#32345b"},
    focusRing{"#828fff"}, primaryFill{"#e5e5e6"}, primaryInk{"#09090a"};
constexpr int button = 28, icon = 18, radius = 8, gap = 8;
}
// Capture bar glyphs: light ink, accent when checked, dim when disabled.
QIcon barIcon(Icon icon, int size = Bar::icon);
QToolButton *barButton(Icon icon, const QString &tip);
// Pill combo for the dark capture bars: flat popup rows and a thin chevron.
QComboBox *flatComboBox();
// Rounded track slider for the dark capture bars.
extern const QString darkSliderQss;

// A toolbar button with a solid color chip; checkable by the caller.
QToolButton *colorButton(const QColor &color, const QString &tip, int size = 32);

// Dark menu used by the tray, pin and selection menus.
extern const QString darkMenuQss;

// 1px vertical divider between toolbar groups, styled by the bar stylesheet.
QWidget *barSeparator(bool dark = false);

// Rounded top-level tool surfaces: keep the opaque sheet background and clip the window corners.
void applyRoundedMask(QWidget *widget, int radius = 8);

// ---------- Compact color strip with the complete palette available on demand. ----------
class ColorBoard : public QWidget
{
    Q_OBJECT
public:
    explicit ColorBoard(const QColor &current, QWidget *parent = nullptr);
    void setCurrentColor(const QColor &color);
    QColor currentColor() const { return m_current; }
    QRect currentRect() const { return QRect(1, 3, 24, 24); }
    static QRect swatchRect(int index);
    static const QColor &preset(int index);
    static constexpr int PresetCount = 20, CommonCount = 8;
    static int testRendering();
signals:
    void presetPicked(const QColor &color);
    void customRequested();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override { e->accept(); }   // Keep presses off the canvas below.
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;
private:
    int swatchAt(const QPoint &pos) const;
    const QColor &swatchColor(int index) const;
    QColor m_current;
    int m_hover = -1;
};
