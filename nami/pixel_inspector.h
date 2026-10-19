#pragma once

#include "window_detection.h"
#include <QCoreApplication>
#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QColor>
#include <QPaintEvent>
#include <QWidget>
#include <QTextDocument>
#include <QtMath>
#include <qt_windows.h>

class PixelInspector : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(PixelInspector)
public:
    explicit PixelInspector(QWidget *parent);
    bool setSample(const ScreenFrame &frame, const QPoint &globalPixel,
                   const QRectF &selectionPixels = {});
    QString colorText() const;
    void toggleFormat();
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QImage m_patch;
    QColor m_color;
    QPoint m_position;
    bool m_hex = false;
    QTextDocument m_tips;
};
