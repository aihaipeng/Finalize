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
#include <QWindow>
#include <QWheelEvent>
#include <QSlider>
#include <QClipboard>
#include <QPainter>
#include <QFont>
#include <QThreadPool>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QSizePolicy>
#include <QDeadlineTimer>
#include <QtMath>
#include <qt_windows.h>
#include <cstdio>
#include <cstdlib>

int PinWindow::testPlacement()
{
    const bool markup = qApp->arguments().contains(QStringLiteral("--markup"));
    QMenu *menu = nullptr;
    QList<QAction *> actions;
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
    check(pin.imageGeometry() == source && pin.m_pix.size() == pixels.size()
              && qAbs(pin.m_scale - 2.0 / 3.0) < 0.000001,
          "ordinary pins keep the source geometry and original pixels without automatic fitting");
    const QPoint anchor = source.center();
    QWheelEvent wheel(QPointF(anchor - source.topLeft()), QPointF(anchor), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&pin, &wheel);
    check(pin.imageRect().size() == QSize(660, 440)
              && qAbs(double(anchor.x() - pin.imageGeometry().x()) / pin.imageGeometry().width()
                       - double(anchor.x() - source.x()) / source.width()) < 0.003,
          "wheel zoom starts from the displayed DPI scale and retains the cursor anchor");
    pin.close();
    QPixmap longPixels(600, 20000);
    longPixels.fill(Qt::green);
    PinWindow longPin(longPixels, fittedGeometry(longPixels.size(), source, available));
    longPin.setAttribute(Qt::WA_DeleteOnClose, false);
    const QSize fitted = longPin.imageRect().size();
    const QPoint longAnchor = longPin.geometry().center();
    QWheelEvent shrink(QPointF(longAnchor - longPin.pos()), QPointF(longAnchor), QPoint(), QPoint(0, -120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&longPin, &shrink);
    check(longPin.imageRect().width() < fitted.width() && longPin.imageRect().height() < fitted.height(),
          "shrinking a long pin below five percent never enlarges it");
    for (int i = 0; i < 100; ++i) QApplication::sendEvent(&longPin, &shrink);
    check(longPin.imageRect().width() >= 1 && longPin.imageRect().height() >= 1,
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
    menu = longPin.createContextMenu();
    check(menu != nullptr, "pin creates a context menu");
    actions = menu->actions();
    check(!actions.isEmpty(), "context menu has actions");
    // Verify menu items structure
    check(actions.at(0)->text() == QStringLiteral("文本可选择") && actions.at(0)->isCheckable(),
          "first action is checkable text selection");
    check(actions.at(0)->isChecked() && longPin.m_textSelectable,
          "new pins start with text selection on");
    check(longPin.cursor().shape() == Qt::ArrowCursor,
          "text selection mode keeps the native cursor until the pointer is over text");
    actions.at(0)->trigger(); // Toggle off
    check(!longPin.m_textSelectable && longPin.cursor().shape() == Qt::SizeAllCursor,
          "disabling text selection restores SizeAllCursor");
    actions.at(0)->trigger(); // Toggle back on
    check(longPin.m_textSelectable, "toggling text selection re-enables selectable mode");

    // Submenu: Image processing
    QMenu *sub = actions.at(2)->menu();
    check(sub != nullptr && sub->title() == QStringLiteral("图像处理"), "submenu is 图像处理");
    QList<QAction *> subActions = sub->actions();
    check(subActions.size() >= 8, "submenu has transform actions and sliders");
    // Trigger rotate right (90 deg)
    const QSize origSize = longPin.imageRect().size();
    subActions.at(1)->trigger(); // Rotate right
    check(longPin.imageRect().width() == origSize.height() && longPin.imageRect().height() == origSize.width(),
          "rotating 90 degrees swaps width and height");
    // Trigger reset
    subActions.last()->trigger(); // Reset all processing
    check(longPin.imageRect().size() == origSize, "reset restores original size and orientation");

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
    check(actions.last()->text() == QStringLiteral("窗口阴影") && actions.last()->isCheckable(),
          "shadow action is checkable");
    {   // The shadow is painted into a transparent band around the image, never a system shadow.
        const int m = PinWindow::kShadowMargin;
        QPixmap gray(320, 200);
        gray.fill(Qt::gray);
        PinWindow shadowPin(gray, QRect(400, 300, 320, 200));
        shadowPin.setAttribute(Qt::WA_DeleteOnClose, false);
        QMenu *shadowMenu = shadowPin.createContextMenu();
        QAction *shadowAction = shadowMenu->actions().last();
        check(shadowAction->text() == QStringLiteral("窗口阴影") && shadowAction->isChecked(),
              "a new pin starts with its shadow on");
        check(shadowPin.testAttribute(Qt::WA_TranslucentBackground)
                  && shadowPin.geometry() == QRect(400, 300, 320, 200).marginsAdded(QMargins(m, m, m, m))
                  && !(GetClassLongPtrW(reinterpret_cast<HWND>(shadowPin.winId()), GCL_STYLE) & CS_DROPSHADOW),
              "the pin window is the image plus a 12px painted shadow band");
        const auto bandAlpha = [&](int d) {   // d px outside the left image edge, mid height.
            const QImage shot = shadowPin.grab().toImage();
            const qreal dpr = shot.devicePixelRatio();
            return qAlpha(shot.pixel(int((m - d + 0.5) * dpr), int(shadowPin.height() / 2.0 * dpr)));
        };
        const QRect g = shadowPin.geometry();
        const HWND hwndBefore = reinterpret_cast<HWND>(shadowPin.winId());
        const QString shotPrefix = qEnvironmentVariable("PIN_SHADOW_SHOT");
        const auto screenShot = [&](const QString &tag) {   // The composited desktop, as a user sees it.
            if (shotPrefix.isEmpty()) return;
            const QDeadlineTimer settle(400);
            while (!settle.hasExpired()) QApplication::processEvents(QEventLoop::AllEvents, 20);
            shadowPin.screen()->grabWindow(0, g.x() - 8, g.y() - 8, g.width() + 16, g.height() + 16)
                .save(shotPrefix + tag + QStringLiteral(".png"));
        };
        screenShot(QStringLiteral("-inactive"));
        if (!shotPrefix.isEmpty()) {
            shadowPin.activateWindow();
            screenShot(QStringLiteral("-active"));
        }
        check(bandAlpha(1) > 80 && bandAlpha(1) < 255 && bandAlpha(4) > 0 && bandAlpha(4) < bandAlpha(1)
                  && bandAlpha(12) <= 2,
              "the shadow fades out across the band");
        shadowAction->trigger();
        QApplication::processEvents();
        check(!shadowPin.m_shadowOn && bandAlpha(1) == 0 && shadowPin.isVisible() && shadowPin.geometry() == g
                  && reinterpret_cast<HWND>(shadowPin.winId()) == hwndBefore,
              "turning the shadow off clears the band by repainting; the pin is not moved or re-shown");
        screenShot(QStringLiteral("-off"));
        shadowAction->trigger();
        QApplication::processEvents();
        check(shadowPin.m_shadowOn && bandAlpha(1) > 80 && shadowPin.geometry() == g,
              "turning the shadow back on paints it again");
        delete shadowMenu;
        shadowPin.close();
    }
    // Verify text box hit test and selection
    longPin.m_textSelectable = true;
    const double sx = double(longPin.m_pix.width()) / longPin.imageRect().width();
    const double sy = double(longPin.m_pix.height()) / longPin.imageRect().height();
    const QPoint io = longPin.imageRect().topLeft();   // Event positions are window coordinates.
    PinWindow::CharBox cb1;
    cb1.ch = QLatin1Char('A');
    cb1.rect = QRectF(0, 0, 50 * sx, 50 * sy);
    cb1.lineIdx = 0;
    PinWindow::CharBox cb2;
    cb2.ch = QLatin1Char('B');
    cb2.rect = QRectF(50 * sx, 0, 50 * sx, 50 * sy);
    cb2.lineIdx = 0;
    longPin.m_charBoxes = {cb1, cb2};
    check(longPin.charIndexAt(io + QPoint(10, 10)) == 0, "first char hit test matches");
    check(longPin.charIndexAt(io + QPoint(60, 10)) == 1, "second char hit test matches");
    check(longPin.charIndexAt(io + QPoint(500, 500)) == -1, "far outside point returns -1");
    // Cursor gating: IBeam only over copyable text, native cursor elsewhere.
    QMouseEvent hoverText(QEvent::MouseMove, io + QPoint(10, 10), longPin.mapToGlobal(io + QPoint(10, 10)),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&longPin, &hoverText);
    check(longPin.cursor().shape() == Qt::IBeamCursor,
          "hovering copyable text shows the I-beam cursor");
    QMouseEvent hoverBlank(QEvent::MouseMove, io + QPoint(500, 500), longPin.mapToGlobal(io + QPoint(500, 500)),
                           Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&longPin, &hoverBlank);
    check(longPin.cursor().shape() == Qt::ArrowCursor,
          "hovering away from text restores the native cursor");
    longPin.m_selAnchor = 0;
    longPin.m_selLead = 1;
    check(longPin.selectedText() == QStringLiteral("AB"), "selection extracts combined text");
    if (markup) {
    // Runs only in its own process: a pin created before the live OCR section
    // crashes that recognition intermittently.
    QImage markImg(240, 120, QImage::Format_RGB32);
    markImg.fill(Qt::white);
    PinWindow markPin(QPixmap::fromImage(markImg), QRect(240, 120, 240, 120));
    markPin.setAttribute(Qt::WA_DeleteOnClose, false);
    menu = markPin.createContextMenu();
    actions = menu->actions();
    markPin.m_textSelectable = true;
    markPin.m_ocrImgSize = QSizeF(240, 120);
    PinWindow::CharBox mA;
    mA.ch = QLatin1Char('A');
    mA.rect = QRectF(20, 40, 80, 40);
    mA.lineIdx = 0;
    PinWindow::CharBox mB;
    mB.ch = QLatin1Char('B');
    mB.rect = QRectF(100, 40, 80, 40);
    mB.lineIdx = 0;
    markPin.m_charBoxes = {mA, mB};
    markPin.m_selAnchor = 0;
    markPin.m_selLead = 1;
    markPin.m_selecting = true;
    markPin.updateSelToolbar();
    check(!markPin.m_selToolbar || !markPin.m_selToolbar->isVisible(),
          "the toolbar stays hidden while the drag is still in progress");
    markPin.m_selecting = false;
    markPin.m_selBarOpen = true;
    markPin.updateSelToolbar();
    check(markPin.m_selToolbar && markPin.m_selToolbar->isVisible(),
          "releasing the selection opens the markup toolbar");
    QRectF sel;
    for (const PinWindow::CharBox &box : markPin.m_charBoxes) sel = sel.united(box.rect);
    const QRect mig = markPin.imageGeometry();
    const double scaleY = double(mig.height()) / markPin.m_ocrImgSize.height();
    const int selTop = mig.y() + qRound(sel.top() * scaleY);
    const int selBottom = mig.y() + qRound(sel.bottom() * scaleY);
    const QRect bar = markPin.m_selToolbar->geometry();
    check(!bar.intersects(QRect(mig.x(), selTop, mig.width(), selBottom - selTop))
              && (bar.top() >= selBottom || bar.bottom() <= selTop),
          "markup toolbar sits clear of the selection, below it or above it");
    const auto selButton = [&markPin](const QString &tip) -> QToolButton * {
        const QList<QToolButton *> btns = markPin.m_selToolbar->findChildren<QToolButton *>();
        for (QToolButton *b : btns)
            if (b->toolTip() == tip) return b;
        return nullptr;
    };
    QToolButton *selCopy = selButton(QStringLiteral("复制"));
    QToolButton *selHighlight = selButton(QStringLiteral("荧光笔"));
    QToolButton *selWavy = selButton(QStringLiteral("波浪线"));
    QToolButton *selLine = selButton(QStringLiteral("直线"));
    QToolButton *selStrike = selButton(QStringLiteral("删除线"));
    check(selCopy && selHighlight && selWavy && selLine && selStrike,
          "markup toolbar offers copy and the four markup tools");
    QToolButton *swatch2 = selButton(QStringLiteral("#b89fff"));
    QToolButton *clearMark = selButton(QStringLiteral("清除标注"));
    QToolButton *closeBar = selButton(QStringLiteral("关闭工具栏"));
    check(swatch2 && swatch2->isVisibleTo(markPin.m_selToolbar) && clearMark
              && markPin.m_selToolbar->findChildren<QToolButton *>().size() == 12,
          "five palette swatches and a clear button stay visible");
    const int barWidth = markPin.m_selToolbar->width();
    selHighlight->click();
    check(markPin.m_decoHighlight.size() == 2
              && markPin.m_decoHighlight.value(0) == QColor("#ff909c")
              && markPin.m_decoHighlight.value(1) == QColor("#ff909c"),
          "checking a markup tool draws immediately in its remembered color");
    check(!selHighlight->isCheckable(),
          "tool buttons light up only while pressed, never stay checked");
    swatch2->click();
    check(markPin.m_decoHighlight.value(1) == QColor("#b89fff"),
          "picking a color recolors the selected characters");
    swatch2->click();
    check(markPin.m_decoHighlight.size() == 2,
          "picking a color again never toggles the markup off");
    selHighlight->click();
    check(markPin.m_decoHighlight.isEmpty(),
          "clicking the tool again on a uniform mark toggles it off");
    selStrike->click();
    check(markPin.m_decoLine.size() == 2
              && markPin.m_decoLine.value(1).first == PinWindow::DecoStrike
              && markPin.m_decoLine.value(1).second == QColor("#ff909c"),
          "each tool remembers its own color, here the first palette entry");
    selHighlight->click();
    check(markPin.m_decoHighlight.size() == 2 && markPin.m_decoLine.size() == 2,
          "highlight keeps its own slot beside a line mark");
    selLine->click();
    check(markPin.m_decoLine.size() == 2
              && markPin.m_decoLine.value(0).first == PinWindow::DecoLine,
          "switching line marks replaces the previous one, highlight stays");
    check(markPin.m_selToolbar->width() == barWidth,
          "switching tools keeps the toolbar width unchanged");
    markPin.m_selAnchor = 0;
    markPin.m_selLead = 0;   // Select fewer characters than were marked.
    clearMark->click();
    check(markPin.m_decoHighlight.size() == 1 && markPin.m_decoHighlight.contains(1)
              && markPin.m_decoLine.size() == 1 && markPin.m_decoLine.contains(1),
          "clearing an under-selection removes just the selected part of every mark");
    markPin.m_selLead = 1;   // Select more than the remaining marks.
    clearMark->click();
    check(markPin.m_decoHighlight.isEmpty() && markPin.m_decoLine.isEmpty(),
          "clearing an over-selection erases every mark it covers");
    QToolButton *swatch3 = selButton(QStringLiteral("#74b4ff"));
    markPin.m_selAnchor = 1;
    markPin.m_selLead = 1;   // Recolor only the still-marked character.
    swatch3->click();   // Current tool is still the straight line.
    check(markPin.m_decoLine.size() == 1
              && markPin.m_decoLine.value(1).second == QColor("#74b4ff"),
          "recoloring after a partial clear marks only the selected characters");
    markPin.m_selAnchor = 0;
    markPin.m_selLead = 1;
    selStrike->click();
    check(markPin.m_decoLine.size() == 2
              && markPin.m_decoLine.value(0).first == PinWindow::DecoStrike
              && markPin.m_decoLine.value(1).first == PinWindow::DecoStrike,
          "clicking a tool on a partially marked selection applies it to all of it");
    selCopy->click();
    check(QApplication::clipboard()->text() == QStringLiteral("AB")
              && !markPin.m_selToolbar->isVisible() && markPin.m_selAnchor < 0,
          "copy copies the text, closes the toolbar and clears the selection");
    markPin.m_selBarOpen = true;
    markPin.m_selAnchor = 0;
    markPin.m_selLead = 1;
    markPin.updateSelToolbar();
    check(markPin.m_selToolbar->isVisible(), "the next finished selection shows the toolbar again");
    QMouseEvent outside(QEvent::MouseButtonPress, QPointF(-500, -500), QPointF(-500, -500),
                        QPointF(-5000, -5000), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&markPin, &outside);
    check(!markPin.m_selToolbar->isVisible(), "a press outside the toolbar closes it");
    markPin.m_selBarOpen = true;
    markPin.updateSelToolbar();
    closeBar->click();
    check(!markPin.m_selToolbar->isVisible(), "the close button hides the toolbar");
    markPin.m_selBarOpen = true;
    markPin.updateSelToolbar();
    markPin.m_selAnchor = -1;
    markPin.updateSelToolbar();
    check(!markPin.m_selToolbar->isVisible(), "clearing the selection hides the markup toolbar");
    markPin.m_selAnchor = 0;
    markPin.m_selLead = 1;
    markPin.drawTextDeco(PinWindow::DecoHighlight, QColor("#ff909c"));
    const QImage marked = markPin.grab().toImage();
    const QColor hl = marked.pixelColor(QPoint(120, 60) * marked.devicePixelRatio() + markPin.imageRect().topLeft() * marked.devicePixelRatio());   // Inside the char boxes.
    check(hl != Qt::white && hl != Qt::black,
          "the multiply highlight tints the marked line while text stays visible");
    // A "——" often comes back as its own OCR segment on the same visual row;
    // row clustering must keep the row whole instead of cutting the selection.
    markPin.m_decoHighlight.clear();
    markPin.m_decoLine.clear();
    markPin.m_ocrLines = {
        { QStringLiteral("你好"), QPolygonF(QRectF(10, 40, 40, 20)) },
        { QStringLiteral("——"), QPolygonF(QRectF(55, 42, 30, 16)) },
        { QStringLiteral("世界"), QPolygonF(QRectF(10, 70, 40, 20)) },
    };
    markPin.rebuildCharBoxes();
    check(markPin.m_charBoxes.size() == 6
              && markPin.m_charBoxes.at(2).ch == QChar(0x2014)
              && markPin.m_charBoxes.at(3).ch == QChar(0x2014)
              && markPin.m_charBoxes.at(3).lineIdx == 0
              && markPin.m_charBoxes.at(4).lineIdx == 1,
          "a dash segment on the same visual row merges into that row");
    markPin.m_selAnchor = 0;
    markPin.m_selLead = 3;
    check(markPin.selectedText() == QStringLiteral("你好——"),
          "selecting across a merged dash segment copies unbroken text");
    markPin.m_selLead = 5;
    check(markPin.selectedText() == QStringLiteral("你好——\n世界"),
          "a real row break still becomes a newline in the copied text");
    markPin.toggleTextSelectable(false);
    if (!qgetenv("PIN_TOOLBAR_SHOT").isEmpty())
        check(markPin.m_selToolbar->grab().save(QString::fromLocal8Bit(qgetenv("PIN_TOOLBAR_SHOT"))),
              "save the markup toolbar preview");
    markPin.close();
    QApplication::processEvents();
    return 0;
    }
    QAction *toolbarAction = actions.at(actions.size() - 2);
    check(toolbarAction->text() == QStringLiteral("显示工具条") && toolbarAction->isCheckable(),
          "toolbar action is checkable");
    toolbarAction->trigger(); // Enable toolbar
    check(longPin.m_toolbarOn && longPin.m_toolbar != nullptr && longPin.m_toolbar->isVisible(),
          "enabling toolbar creates and shows floating toolbar widget");
    toolbarAction->trigger(); // Disable toolbar
    check(!longPin.m_toolbarOn && !longPin.m_toolbar->isVisible(),
          "disabling toolbar hides floating toolbar widget");

    // The pin toolbar carries the full annotation tool set; a tool arms an
    // in-place canvas inside the pin: no session window, the pin and its
    // toolbar stay as they are, and the option strip opens under the toolbar.
    QPixmap flat(240, 160);
    flat.fill(Qt::white);
    PinWindow editPin(flat, QRect(200, 200, 240, 160));
    editPin.setAttribute(Qt::WA_DeleteOnClose, false);
    editPin.toggleToolbar(true);
    const auto barButtons = editPin.m_toolbar->findChildren<QToolButton *>();
    check(barButtons.size() == 14, "pin toolbar shows eight tools, undo/redo, three actions and done");
    QToolButton *shapeBtn = nullptr;
    for (QToolButton *b : barButtons)
        if (b->toolTip() == QStringLiteral("形状：矩形/椭圆")) shapeBtn = b;
    check(shapeBtn != nullptr && shapeBtn->isCheckable(), "the shape tool sits on the pin toolbar");
    check(!editPin.m_undoBtn->isEnabled() && !editPin.m_redoBtn->isEnabled(),
          "undo/redo start disabled");
    const QRect pinBefore = editPin.imageGeometry();
    const QRect barBefore = editPin.m_toolbar->geometry();
    const int topLevelsBefore = QApplication::topLevelWidgets().size();
    const QImage lookBefore = editPin.grab().toImage();
    check(editPin.m_toolbar->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus),
          "the pin toolbar never takes keyboard focus from the pin");
    shapeBtn->click();
    QApplication::processEvents();
    check(editPin.grab().toImage() == lookBefore,
          "arming a tool leaves every pin pixel unchanged, border included");
    check(editPin.m_canvas && editPin.m_canvas->parentWidget() == &editPin && editPin.m_canvas->isVisible(),
          "a tool click arms a child canvas inside the pin");
    check(editPin.isVisible() && editPin.imageGeometry() == pinBefore
              && editPin.m_toolbar->isVisible() && editPin.m_toolbar->geometry() == barBefore,
          "the pin and its toolbar do not move, hide or change");
    {
        bool overlayWindow = false;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<CaptureOverlay *>(w) && w->isVisible()) overlayWindow = true;
        check(!overlayWindow, "no capture session window opens");
    }
    QWidget *strip = editPin.m_canvas->optionsStrip();
    check(strip && strip->isVisible() && strip->isWindow()
              && strip->x() == editPin.m_toolbar->x()
              && strip->y() >= editPin.m_toolbar->geometry().bottom(),
          "the option strip opens left-aligned directly below the pin toolbar");
    check(QApplication::topLevelWidgets().size() == topLevelsBefore + 1,
          "the strip is the only new top-level widget");
    check(shapeBtn->isChecked(), "the armed tool button shows as checked");

    QWidget *canvas = editPin.m_canvas.data();
    const auto canvasMouse = [canvas](QEvent::Type type, const QPoint &local) {
        QMouseEvent event(type, QPointF(local), QPointF(canvas->mapToGlobal(local)),
                          Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    canvasMouse(QEvent::MouseButtonPress, QPoint(30, 30));
    canvasMouse(QEvent::MouseMove, QPoint(180, 120));
    canvasMouse(QEvent::MouseButtonRelease, QPoint(180, 120));
    QApplication::processEvents();
    check(editPin.m_canvas->hasAnnotations() && editPin.m_undoBtn->isEnabled(),
          "drawing on the pin creates a live annotation and enables undo");
    check(editPin.imageGeometry() == pinBefore, "drawing does not move the pin");
    {
        const QImage shown = editPin.grab(editPin.imageRect()).toImage();
        check(QColor(shown.pixel(105, 30)) != Qt::white && QColor(shown.pixel(10, 10)) == Qt::white,
              "the rectangle shows on the pin in place");
        const QImage out = editPin.annotatedPix().toImage();
        check(QColor(out.pixel(105, 30)) != Qt::white, "copy/save/OCR pixels carry the marks");
    }
    {   // The fresh mark stays editable; a right click locks it in and opens no menu.
        const QImage withHandles = editPin.grab(editPin.imageRect()).toImage();
        const QPoint at(200, 140);
        QMouseEvent rpress(QEvent::MouseButtonPress, QPointF(at), QPointF(canvas->mapToGlobal(at)),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &rpress);
        QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, at, canvas->mapToGlobal(at));
        menuEvent.ignore();
        QApplication::sendEvent(canvas, &menuEvent);
        check(withHandles != editPin.grab(editPin.imageRect()).toImage() && menuEvent.isAccepted(),
              "first right click hides the fresh mark's handles and opens no menu");
        const QImage locked = editPin.grab(editPin.imageRect()).toImage();
        shapeBtn->click();   // Disarm, then click the old mark: it stays part of the image.
        QApplication::processEvents();
        canvasMouse(QEvent::MouseButtonPress, QPoint(105, 30));
        canvasMouse(QEvent::MouseButtonRelease, QPoint(105, 30));
        check(editPin.grab(editPin.imageRect()).toImage() == locked, "a locked mark cannot be picked again");
        struct MenuCatcher : QObject {   // Stands in for the pin's modal menu.
            bool reached = false;
            bool eventFilter(QObject *, QEvent *ev) override {
                if (ev->type() != QEvent::ContextMenu) return false;
                reached = true;
                return true;
            }
        } catcher;
        editPin.installEventFilter(&catcher);
        QApplication::sendEvent(canvas, &rpress);
        QContextMenuEvent menuEvent2(QContextMenuEvent::Mouse, at, canvas->mapToGlobal(at));
        QApplication::sendEvent(canvas, &menuEvent2);
        editPin.removeEventFilter(&catcher);
        check(catcher.reached, "with nothing to finish the right click reaches the pin menu");
        shapeBtn->click();
        QApplication::processEvents();
    }
    editPin.m_undoBtn->click();
    QApplication::processEvents();
    check(!editPin.m_canvas->hasAnnotations() && editPin.m_redoBtn->isEnabled()
              && QColor(editPin.grab(editPin.imageRect()).toImage().pixel(105, 30)) == Qt::white,
          "pin undo removes the mark");
    editPin.m_redoBtn->click();
    QApplication::processEvents();
    check(editPin.m_canvas->hasAnnotations() && QColor(editPin.grab(editPin.imageRect()).toImage().pixel(105, 30)) != Qt::white,
          "pin redo restores the mark");

    {   // Disarming with the cursor over the pin maps it through the child canvas, never a null window.
        const QPoint restore = QCursor::pos();
        QCursor::setPos(canvas->mapToGlobal(QPoint(60, 60)));
        QApplication::processEvents();
        shapeBtn->click();
        QApplication::processEvents();
        check(editPin.isVisible() && editPin.m_canvas->armedTool() == CaptureOverlay::ATool::None,
              "disarming over the pin is safe");
        QCursor::setPos(restore);
        shapeBtn->click();
        QApplication::processEvents();
    }
    QKeyEvent esc1(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    // First Esc, even from the detached strip: the bar and strip hide, the
    // tool disarms, the pin and its marks stay.
    QApplication::sendEvent(strip, &esc1);
    QApplication::processEvents();
    check(editPin.m_canvas->armedTool() == CaptureOverlay::ATool::None && !strip->isVisible()
              && !editPin.m_toolbar->isVisible() && !shapeBtn->isChecked()
              && editPin.m_canvas->hasAnnotations() && editPin.isVisible(),
          "first Esc hides the toolbar and strip and disarms; marks stay on the pin");
    editPin.toggleToolbar(true);
    QApplication::processEvents();
    QToolButton *doneBtn = nullptr;
    for (QToolButton *b : editPin.m_toolbar->findChildren<QToolButton *>())
        if (b->toolTip() == QStringLiteral("完成")) doneBtn = b;
    check(doneBtn && editPin.m_toolbar->layout()->indexOf(doneBtn) == editPin.m_toolbar->layout()->count() - 1,
          "the done button closes the pin toolbar row");
    shapeBtn->click();
    QApplication::processEvents();
    doneBtn->click();
    QApplication::processEvents();
    check(!editPin.m_toolbar->isVisible() && !strip->isVisible()
              && editPin.m_canvas->armedTool() == CaptureOverlay::ATool::None
              && editPin.m_canvas->hasAnnotations() && editPin.isVisible(),
          "done hides the toolbar and strip, disarms, keeps the pin and marks");
    editPin.toggleToolbar(true);
    QApplication::processEvents();
    shapeBtn->click();
    QApplication::processEvents();
    shapeBtn->click();   // Reclicking the armed tool disarms it.
    QApplication::processEvents();
    check(!shapeBtn->isChecked() && !strip->isVisible(), "reclicking the armed tool closes the strip");

    QWheelEvent zoom(QPointF(120, 80), QPointF(canvas->mapToGlobal(QPoint(120, 80))), QPoint(),
                     QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas, &zoom);   // Nothing armed: the wheel zooms the pin.
    QApplication::processEvents();
    check(editPin.imageRect().width() > pinBefore.width() && !editPin.m_canvas->hasAnnotations()
              && editPin.m_canvas->geometry() == editPin.imageRect(),
          "zoom bakes the marks and resizes the canvas with the pin");
    {
        const QImage baked = editPin.m_pix.toImage();
        const int bx = qRound(105.0 * baked.width() / pinBefore.width());
        const int by = qRound(30.0 * baked.height() / pinBefore.height());
        check(QColor(baked.pixel(bx, by)) != Qt::white, "baked pixels keep the rectangle");
    }
    shapeBtn->click();
    QApplication::processEvents();
    {   // The enlarged pin accepts marks beyond its old size.
        const QPoint a(canvas->width() - 30, canvas->height() - 30), b(canvas->width() - 6, canvas->height() - 6);
        canvasMouse(QEvent::MouseButtonPress, a);
        canvasMouse(QEvent::MouseMove, b);
        canvasMouse(QEvent::MouseButtonRelease, b);
        QApplication::processEvents();
        check(editPin.m_canvas->hasAnnotations()
                  && QColor(editPin.grab(editPin.imageRect()).toImage().pixel(a.x() + 12, a.y())) != Qt::white,
              "after zoom the canvas clips marks to the whole pin, not its old size");
    }
    {   // Esc ladder end to end: bar first, then the pin.
        QKeyEvent escA(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(canvas, &escA);
        QApplication::processEvents();
        check(editPin.isVisible() && !editPin.m_toolbar->isVisible(), "Esc with the bar open only hides the bar");
        QKeyEvent escB(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(canvas, &escB);
        QApplication::processEvents();
        check(!editPin.isVisible(), "the next Esc closes the pin");
    }
    editPin.close();
    QApplication::processEvents();

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
                return QPointF(r.center().x() * textPin.imageRect().width() / textPin.m_ocrImgSize.width(),
                               r.center().y() * textPin.imageRect().height() / textPin.m_ocrImgSize.height()).toPoint()
                       + textPin.imageRect().topLeft();
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
            check(textPin.m_selToolbar && textPin.m_selToolbar->isVisible(),
                  "the live selection opens the markup toolbar");
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
