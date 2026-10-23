#pragma once

#include "platform/screen_capture.h"
#include <QCoreApplication>
#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QColor>
#include <QPaintEvent>
#include <QWidget>
#include <QDialog>
#include <QtMath>
#include <qt_windows.h>

class QLabel;
class QKeyEvent;

class PixelInspector : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(PixelInspector)
public:
    explicit PixelInspector(QWidget *parent);
    bool setSample(const ScreenFrame &frame, const QPoint &globalPixel,
                   const QRectF &selectionPixels = {});
    QString colorText() const;
    QString coordinateText() const;
    QColor color() const { return m_color; }
    QPoint position() const { return m_position; }
    bool relativeCoordinates() const { return m_relative; }
    void toggleFormat();
    void toggleCoordinates();
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QImage m_patch;
    QColor m_color;
    QPoint m_position;
    QPoint m_relativeOrigin;
    bool m_hex = false;
    bool m_relative = false;
};

// Full-screen modal sampler over the frozen capture frames. The host hides its
// color dialog while this runs and applies pickedColor() on acceptance.
class ScreenColorPicker : public QDialog
{
    Q_OBJECT
public:
    explicit ScreenColorPicker(const QVector<ScreenFrame> &frames, QWidget *parent = nullptr);
    QColor pickedColor() const { return m_color; }
    static QColor sample(const QVector<ScreenFrame> &frames, const QPoint &logicalPixel);
protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    void track(const QPoint &position);
    QVector<ScreenFrame> m_frames;
    PixelInspector *m_inspector = nullptr;
    QLabel *m_hint = nullptr;
    QColor m_color;
};
