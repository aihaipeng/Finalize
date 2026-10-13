#include "widgets.h"
#include "output.h"
#include "test_support.h"
#include "pin_window.h"
#include "long_capture.h"
#include "ocr_dialog.h"
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QColor>
#include <QApplication>
#include <QMouseEvent>
#include <QtMath>
#include <qt_windows.h>
#include <cstdio>

int ColorWheel::testRendering()
{
    ColorWheel wheel(Qt::cyan);
    wheel.show();
    QApplication::processEvents();
    const QImage image = wheel.grab().toImage();
    const qreal dpr = image.devicePixelRatio();
    const QRectF circle = wheel.wheelRect();
    const QColor center = image.pixelColor(qRound(circle.center().x() * dpr), qRound(circle.center().y() * dpr));
    const QColor rim = image.pixelColor(qRound((circle.right() - 6) * dpr), qRound(circle.center().y() * dpr));
    if (image.width() != qRound(wheel.width() * dpr) || image.pixelColor(0, 0) != QColor(29, 31, 35)
        || center.red() < 250 || center.green() < 250 || center.blue() < 250
        || rim.red() < 250 || rim.green() >= 32 || rim.blue() >= 32) {
        std::fprintf(stderr, "FAIL: circular color rendering at DPR %.2f\n", double(dpr));
        return 1;
    }
    const QPointF point = circle.center();
    for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, point, wheel.mapToGlobal(point.toPoint()), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&wheel, &event);
    }
    if (wheel.currentColor() != QColor(Qt::white)) {
        std::fprintf(stderr, "FAIL: logical color-wheel hit testing at DPR %.2f\n", double(dpr));
        return 1;
    }
    std::printf("PASS: circular color rendering and hit testing at DPR %.2f\n", double(dpr));
    return 0;
}
