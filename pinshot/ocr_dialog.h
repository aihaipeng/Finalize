#pragma once

#include "ocr_engine.h"
#include "widgets.h"
#include "output.h"
#include <QObject>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QResizeEvent>
#include <QWidget>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QLabel>
#include <QGraphicsView>
#include <QtMath>
#include <QToolButton>
#include <QDialog>
#include <QTimer>
#include <QPlainTextEdit>
#include <QFutureWatcher>
#include <qt_windows.h>

class OcrImageView : public QGraphicsView
{
public:
    explicit OcrImageView(const QImage &image, QWidget *parent = nullptr);
protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
private:
    bool m_zoomed = false;
};

class OcrDialog : public QDialog
{
    Q_OBJECT
public:
    explicit OcrDialog(const QImage &image, const QRect &captureRect = {});
    ~OcrDialog() override;
    static QRect placement(const QRect &capture, const QRect &available);
    void start();
    static int testLifetime();
protected:
    bool eventFilter(QObject *object,QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    Qt::Edges resizeEdges(const QPoint &point) const;
    QImage m_image;
    QPlainTextEdit *m_text = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_retry = nullptr, *m_copy = nullptr, *m_maximize = nullptr;
    OcrCancel m_cancel;
    QFutureWatcher<OcrResult> m_watcher;
    QTimer m_timer;
    int m_ticks = 0;
    bool m_closed = false;
};

OcrDialog *showOcr(const QImage &image, QWidget *source, const QRect &captureRect);
