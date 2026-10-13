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
// Stroke glyphs on a 24-unit grid with 2-unit round strokes, following the free
// Feather/Lucide line-icon style: one vocabulary for every button in the app, painted
// in code so no resource files ship and any icon stays crisp at every scale.

enum class Icon { Save, Copy, Pin, LongShot, Ocr, Cancel, Rect, Ellipse, Arrow, Pen, Marker, Text,
                  Number, Mosaic, Eraser, Undo, Redo, ArrowBoth, ArrowLine, ArrowHollow, Palette, Bold, Italic,
                  Minimize, Maximize, Restore, Refresh };

void buildIconPath(QPainterPath &path, Icon icon);

QIcon makeIcon(Icon icon);

QToolButton *iconButton(Icon icon, const QString &tip);

// The closed font selector shows only its icon; the popup retains readable family names.
class FontComboBox : public QComboBox
{
public:
    FontComboBox();
protected:
    void paintEvent(QPaintEvent *) override;
private:
    const QIcon m_icon;
};

// ---------- Compact hue/saturation wheel; brightness is controlled by a slider. ----------
class ColorWheel : public QWidget
{
    Q_OBJECT
public:
    explicit ColorWheel(const QColor &color, QWidget *parent = nullptr);
    QRectF wheelRect() const;
    QColor currentColor() const;
    QColor brightColor() const;
    int brightness() const;
    void setCurrentColor(const QColor &color);
    void setBrightness(int value);
    static int testRendering();
signals:
    void colorChanged(const QColor &color);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
private:
    void selectAt(const QPoint &point);
    void updateColor();
    QImage m_disc;
    QColor m_color;
    qreal m_hue = 0, m_saturation = 1, m_value = 1;
    bool m_dragging = false;
};
