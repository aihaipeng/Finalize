#include "pin_window.h"
#include "ocr_dialog.h"
#include "output.h"
#include "test_support.h"
#include "widgets.h"
#include "long_capture.h"
#include <QMenu>
#include <QAction>
#include <QPixmap>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QSlider>
#include <QPainter>
#include <QFont>
#include <QThreadPool>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QSizePolicy>
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

    // Test context menu actions and image transformations
    QMenu *menu = longPin.createContextMenu();
    check(menu != nullptr, "pin creates a context menu");
    QList<QAction *> actions = menu->actions();
    check(!actions.isEmpty(), "context menu has actions");
    // Verify menu items structure
    check(actions.at(0)->text() == QStringLiteral("文本可选择") && actions.at(0)->isCheckable(),
          "first action is checkable text selection");
    actions.at(0)->trigger(); // Toggle on text selection
    check(longPin.m_textSelectable, "toggling text selection enables selectable mode");
    check(longPin.cursor().shape() == Qt::IBeamCursor, "enabling text selection sets IBeamCursor");
    actions.at(0)->trigger(); // Toggle off
    check(!longPin.m_textSelectable && longPin.cursor().shape() == Qt::SizeAllCursor,
          "disabling text selection restores SizeAllCursor");

    check(actions.at(1)->text() == QStringLiteral("翻译为") && !actions.at(1)->isEnabled(),
          "second action is disabled translation placeholder");
    // Submenu: Image processing
    QMenu *sub = actions.at(3)->menu();
    check(sub != nullptr && sub->title() == QStringLiteral("图像处理"), "submenu is 图像处理");
    QList<QAction *> subActions = sub->actions();
    check(subActions.size() >= 8, "submenu has transform actions and sliders");
    // Trigger rotate right (90 deg)
    const QSize origSize = longPin.size();
    subActions.at(1)->trigger(); // Rotate right
    check(longPin.width() == origSize.height() && longPin.height() == origSize.width(),
          "rotating 90 degrees swaps width and height");
    // Trigger reset
    subActions.last()->trigger(); // Reset all processing
    check(longPin.size() == origSize, "reset restores original size and orientation");

    // Opacity/brightness open aligned flyout sub-columns on hover
    QMenu *opacityMenu = nullptr;
    QMenu *brightnessMenu = nullptr;
    int flyouts = 0;
    for (QAction *a : subActions) {
        if (!a->menu()) continue;
        ++flyouts;
        if (a->text() == QStringLiteral("不透明度")) opacityMenu = a->menu();
        if (a->text() == QStringLiteral("亮度")) brightnessMenu = a->menu();
    }
    check(flyouts == 2 && opacityMenu && brightnessMenu,
          "only opacity and brightness have hover flyout submenus");
    if (opacityMenu && brightnessMenu) {
        const QList<QSlider *> osliders = opacityMenu->findChildren<QSlider *>();
        const QList<QSlider *> bsliders = brightnessMenu->findChildren<QSlider *>();
        check(osliders.size() == 1 && bsliders.size() == 1, "each flyout embeds one slider");
        check(opacityMenu->findChildren<QLabel *>().size() == 1
                  && opacityMenu->findChildren<QLabel *>().first()->text().endsWith(QLatin1Char('%')),
              "slider flyout shows only the percentage, not a title");
        check(osliders.first()->styleSheet() == lightSliderQss && osliders.first()->height() == 22,
              "pin slider uses the mosaic slider style and stays vertically centered");
        check(bsliders.first()->maximum() == 200, "brightness slider keeps its 0..200 percent range");
        // Menus are never shown raw in tests because popups outside exec() leave
        // dangling popup state that crashed later event processing.
        sub->move(400, 300);
        sub->resize(sub->sizeHint());
        opacityMenu->move(sub->geometry().right() - 6, sub->geometry().top() + 40);
        opacityMenu->resize(opacityMenu->sizeHint());
        const int parentRow = sub->actionGeometry(opacityMenu->menuAction()).height();
        separateFlyoutEdge(opacityMenu);
        check(!opacityMenu->geometry().intersects(sub->geometry()),
              "shown slider flyout no longer overlaps its parent column");
        check(opacityMenu->geometry().left() > sub->geometry().right(),
              "slider flyout opens to the right of the parent column");
        check(opacityMenu->height() == parentRow
                  && opacityMenu->geometry().top()
                      == sub->mapToGlobal(sub->actionGeometry(opacityMenu->menuAction()).topLeft()).y(),
              "slider flyout matches its parent row height and top edge");
        separateFlyoutEdge(opacityMenu);
        QWidget *centered = osliders.first()->parentWidget();
        const QRect sliderBox = osliders.first()->geometry();
        const QRect labelBox = opacityMenu->findChildren<QLabel *>().first()->geometry();
        const int contentTop = qMin(sliderBox.top(), labelBox.top());
        const int contentBottom = qMax(sliderBox.bottom(), labelBox.bottom());
        check(centered->height() == opacityMenu->height()
                  && qAbs((contentTop) - (centered->height() - 1 - contentBottom)) <= 1,
              "slider and percentage are centered between the flyout's top and bottom");
        sub->move(QGuiApplication::primaryScreen()->availableGeometry().right()
                      - sub->width() + 1, 300);
        separateFlyoutEdge(opacityMenu);
        check(opacityMenu->geometry().right() <= sub->geometry().left(),
              "slider flyout flips left when the parent column is at the screen edge");
        check(!opacityMenu->geometry().intersects(sub->geometry()),
              "flipped slider flyout does not overlap its parent column");
        QWidget *row = osliders.first()->parentWidget();
        QWheelEvent lower(QPointF(60, 5), QPointF(1060, 705), QPoint(), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(sub, &lower);
        check(osliders.first()->value() == 100, "wheel on an inactive row does not move the slider");
        sub->setActiveAction(opacityMenu->menuAction());
        QApplication::sendEvent(sub, &lower);
        check(osliders.first()->value() < 100, "wheel over the highlighted row lowers opacity");
        // Windows quantizes window opacity to 255 layered-window steps, so allow 1%.
        check(qAbs(longPin.windowOpacity() - double(osliders.first()->value()) / 100.0) < 0.01,
              "opacity flyout applies the percentage to the pin window");
        QWheelEvent raise(QPointF(60, 5), QPointF(1060, 705), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(sub, &raise);
        check(osliders.first()->value() > osliders.first()->minimum()
                  && osliders.first()->value() <= 100, "wheel up keeps opacity inside its range");
    }
    // Verify shadow toggle
    QAction *shadowAction = actions.last();
    check(shadowAction->text() == QStringLiteral("窗口阴影") && shadowAction->isCheckable(),
          "shadow action is checkable");
    // Verify text box hit test and selection
    longPin.m_textSelectable = true;
    const double sx = double(longPin.m_pix.width()) / longPin.width();
    const double sy = double(longPin.m_pix.height()) / longPin.height();
    PinWindow::CharBox cb1;
    cb1.ch = QLatin1Char('A');
    cb1.rect = QRectF(0, 0, 50 * sx, 50 * sy);
    cb1.lineIdx = 0;
    PinWindow::CharBox cb2;
    cb2.ch = QLatin1Char('B');
    cb2.rect = QRectF(50 * sx, 0, 50 * sx, 50 * sy);
    cb2.lineIdx = 0;
    longPin.m_charBoxes = {cb1, cb2};
    check(longPin.charIndexAt(QPoint(10, 10)) == 0, "first char hit test matches");
    check(longPin.charIndexAt(QPoint(60, 10)) == 1, "second char hit test matches");
    check(longPin.charIndexAt(QPoint(500, 500)) == -1, "far outside point returns -1");
    longPin.m_selAnchor = 0;
    longPin.m_selLead = 1;
    check(longPin.selectedText() == QStringLiteral("AB"), "selection extracts combined text");

    // Verify toolbar toggle
    QAction *toolbarAction = actions.at(actions.size() - 2);
    check(toolbarAction->text() == QStringLiteral("显示工具条") && toolbarAction->isCheckable(),
          "toolbar action is checkable");
    toolbarAction->trigger(); // Enable toolbar
    check(longPin.m_toolbarOn && longPin.m_toolbar != nullptr && longPin.m_toolbar->isVisible(),
          "enabling toolbar creates and shows floating toolbar widget");
    toolbarAction->trigger(); // Disable toolbar
    check(!longPin.m_toolbarOn && !longPin.m_toolbar->isVisible(),
          "disabling toolbar hides floating toolbar widget");

    delete menu;
    longPin.close();

    // Live end-to-end: drawn text on a pin must become selectable through the real OCR chain.
    if (OcrEngine::assetError(QCoreApplication::applicationDirPath()).isEmpty()) {
        QImage textImg(440, 140, QImage::Format_RGB32);
        textImg.fill(Qt::white);
        QPainter tp(&textImg);
        QFont font = tp.font();
        font.setPointSize(28);
        font.setBold(true);
        tp.setFont(font);
        tp.setPen(Qt::black);
        tp.drawText(textImg.rect().adjusted(20, 20, -20, -20), Qt::AlignCenter, QStringLiteral("HelloOCR2026"));
        tp.end();
        PinWindow textPin(QPixmap::fromImage(textImg), QRect(120, 700, 440, 140));
        textPin.setAttribute(Qt::WA_DeleteOnClose, false);
        textPin.toggleTextSelectable(true);
        for (int i = 0; i < 400 && (textPin.m_ocrRunning || textPin.m_charBoxes.isEmpty()); ++i) {
            QThreadPool::globalInstance()->waitForDone(50);
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        check(!textPin.m_charBoxes.isEmpty() && !textPin.m_ocrImgSize.isEmpty(),
              "pin OCR builds selectable character boxes from a real capture");
        if (!textPin.m_charBoxes.isEmpty()) {
            const auto viewPos = [&textPin](int idx) {
                const QRectF r = textPin.m_charBoxes.at(idx).rect;
                return QPointF(r.center().x() * textPin.width() / textPin.m_ocrImgSize.width(),
                               r.center().y() * textPin.height() / textPin.m_ocrImgSize.height()).toPoint();
            };
            const int last = textPin.m_charBoxes.size() - 1;
            const QPoint p0 = viewPos(0), p1 = viewPos(last);
            QMouseEvent press(QEvent::MouseButtonPress, p0, textPin.mapToGlobal(p0),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&textPin, &press);
            QMouseEvent move(QEvent::MouseMove, p1, textPin.mapToGlobal(p1),
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&textPin, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, p1, textPin.mapToGlobal(p1),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&textPin, &release);
            const QString picked = textPin.selectedText();
            check(!picked.isEmpty(), "dragging across the pin selects recognized text");
            std::printf("PIN OCR PICKED: [%s]\n", picked.toStdString().c_str());
            std::fflush(stdout);
        }
        textPin.toggleTextSelectable(false);
        textPin.close();
    } else {
        std::puts("SKIP: pin live OCR selection (models unavailable)");
    }

    std::puts("PASS: original-position pins, proportional long-capture fitting, DPI, wheel zoom, and context menu transforms");
    std::fflush(stdout);
    return 0;
}
