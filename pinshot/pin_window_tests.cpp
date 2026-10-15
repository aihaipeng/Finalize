#include "pin_window.h"
#include "ocr_dialog.h"
#include "output.h"
#include "test_support.h"
#include "widgets.h"
#include "long_capture.h"
#include <QPixmap>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtMath>
#include <qt_windows.h>
#include <cstdio>
#include <cstdlib>

int PinWindow::testPlacement()
{
    auto check = [](bool ok, const char *message) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    };
    const QRect available(0, 0, 1920, 1040);
    const QVector<GdiDisplay> displays{{QRect(-1920, 0, 1920, 1080)}, {QRect(0, 0, 1920, 1080)},
                                      {QRect(1920, 0, 1920, 1080)}};
    for (qreal dpr : {1.0, 1.5, 2.0})
        for (const GdiDisplay &display : displays)
            check(nativeRectForScreen(QRect(display.native.topLeft(),
                      QSize(qRound(1920 / dpr), qRound(1080 / dpr))), displays) == display.native,
                  "screen mapping keeps native origins at uniform and mixed DPI, including negative screens");
    const QRect source(300, 200, 600, 400);
    check(fittedGeometry(QSize(600, 400), source, available) == source,
          "a short capture keeps its original position and size");
    check(fittedGeometry(QSize(900, 600), source, available) == source,
          "native capture pixels map back to the original logical size at 150 percent DPI");
    check(fittedGeometry(QSize(600, 800), source, available) == QRect(300, 200, 600, 800),
          "a long capture that fits is not reduced or centered");
    const QRect tall = fittedGeometry(QSize(600, 2400), source, available);
    check(tall == QRect(300, 200, 210, 840) && available.contains(tall),
          "an oversized long capture uses the maximum height while retaining its origin");
    check(fittedGeometry(QSize(900, 3600), source, available) == tall,
          "oversized placement is independent of the native capture DPI");
    check(fittedGeometry(QSize(1200, 600), QRect(1500, 200, 1200, 300), available)
              == QRect(1500, 200, 420, 210),
          "width overflow shrinks proportionally to the remaining screen width");
    const QPoint offset(-1920, -1080);
    check(fittedGeometry(QSize(600, 2400), source.translated(offset), available.translated(offset))
              == tall.translated(offset),
          "secondary screens with negative coordinates retain the capture origin");
    check(fittedGeometry(QSize(600, 2400), QRect(1919, 1039, 600, 400), available)
              == QRect(1919, 1039, 1, 1),
          "edge captures keep a nonempty display inside the screen");
    check(fittedGeometry(QSize(), source, available).isEmpty(), "empty images are rejected");
    QPixmap pixels(900, 600);
    pixels.fill(Qt::green);
    PinWindow pin(pixels, source);
    pin.setAttribute(Qt::WA_DeleteOnClose, false);
    check(pin.geometry() == source && pin.m_pix.size() == pixels.size()
              && qAbs(pin.m_scale - 2.0 / 3.0) < 0.000001,
          "ordinary pins keep the source geometry and original pixels without automatic fitting");
    const QPoint anchor = source.center();
    QWheelEvent wheel(QPointF(anchor - source.topLeft()), QPointF(anchor), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&pin, &wheel);
    check(pin.size() == QSize(660, 440)
              && qAbs(double(anchor.x() - pin.x()) / pin.width()
                       - double(anchor.x() - source.x()) / source.width()) < 0.003,
          "wheel zoom starts from the displayed DPI scale and retains the cursor anchor");
    pin.close();
    QPixmap longPixels(600, 20000);
    longPixels.fill(Qt::green);
    PinWindow longPin(longPixels, fittedGeometry(longPixels.size(), source, available));
    longPin.setAttribute(Qt::WA_DeleteOnClose, false);
    const QSize fitted = longPin.size();
    const QPoint longAnchor = longPin.geometry().center();
    QWheelEvent shrink(QPointF(longAnchor - longPin.pos()), QPointF(longAnchor), QPoint(), QPoint(0, -120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&longPin, &shrink);
    check(longPin.width() < fitted.width() && longPin.height() < fitted.height(),
          "shrinking a long pin below five percent never enlarges it");
    for (int i = 0; i < 100; ++i) QApplication::sendEvent(&longPin, &shrink);
    check(longPin.width() >= 1 && longPin.height() >= 1,
          "very small pins retain nonzero dimensions");
    QWheelEvent horizontal(QPointF(), QPointF(longPin.pos()), QPoint(), QPoint(120, 0),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    const QRect beforeHorizontal = longPin.geometry();
    QApplication::sendEvent(&longPin, &horizontal);
    check(longPin.geometry() == beforeHorizontal, "horizontal wheel input does not resize pins");
    QMouseEvent rightPress(QEvent::MouseButtonPress, QPoint(3, 3), longPin.pos() + QPoint(3, 3),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(&longPin, &rightPress);
    QMouseEvent rightMove(QEvent::MouseMove, QPoint(9, 9), longPin.pos() + QPoint(9, 9),
                         Qt::NoButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(&longPin, &rightMove);
    check(longPin.geometry() == beforeHorizontal, "right-button movement cannot drag a pin");
    longPin.close();
    std::puts("PASS: original-position pins, proportional long-capture fitting, DPI, and wheel zoom");
    return 0;
}
