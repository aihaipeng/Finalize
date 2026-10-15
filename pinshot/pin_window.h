#pragma once

#include "output.h"
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QPaintEvent>
#include <QContextMenuEvent>
#include <QWidget>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QtMath>
#include <qt_windows.h>

// ---------- Pinned image window ----------
class PinWindow : public QWidget
{
public:
    explicit PinWindow(const QPixmap &pix, const QRect &displayRect);
    static QRect fittedGeometry(const QSize &imageSize, const QRect &source, const QRect &available);
    static int testPlacement();
protected:
    void paintEvent(QPaintEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
private:
    QPixmap m_pix;
    double m_scale;
    QPoint m_dragOff;
    bool m_borderOn = true;
};
