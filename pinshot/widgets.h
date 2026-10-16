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
                  ChevronUp, ChevronDown, ChevronRight };

QIcon makeIcon(Icon icon, int size = 16);

QToolButton *iconButton(Icon icon, const QString &tip);

// Grey groove, theme-blue fill, white ring handle. Shared by mosaic and pin sliders.
extern const QString lightSliderQss;

// 1px vertical divider between toolbar groups, styled by the bar stylesheet.
QWidget *barSeparator();

// ---------- Color strip: current-color button plus a 2x10 preset grid. ----------
class ColorBoard : public QWidget
{
    Q_OBJECT
public:
    explicit ColorBoard(const QColor &current, QWidget *parent = nullptr);
    void setCurrentColor(const QColor &color);
    QColor currentColor() const { return m_current; }
    QRect currentRect() const { return QRect(1, 1, 24, 24); }
    static QRect swatchRect(int index);        // Normal 11x11 cell; hover grows it by 1px.
    static const QColor &preset(int index);
    static constexpr int PresetCount = 20;
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
    QColor m_current;
    int m_hover = -1;
};
