#include "capture/capture_overlay.h"
#include <QClipboard>
#include <QWindow>
#include "platform/output.h"
#include "platform/screen_capture.h"
#include <QAbstractItemView>
#include "tests/test_support.h"
#include "shared/widgets.h"
#include "pin/pin_window.h"
#include "longshot/long_capture.h"
#include "text/cards/ocr_dialog.h"
#include <QGuiApplication>
#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QColor>
#include <QPainter>
#include <QDir>
#include <QApplication>
#include <QScreen>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QtMath>
#include <QCursor>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QWheelEvent>
#include "shared/settings.h"
#include <QSettings>
#include <qt_windows.h>
#include <cstdio>

int CaptureOverlay::testSelection()
{
    int failures = 0;
    const auto check = [&failures](bool ok,const char *message) {
        if (!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); }
    };
    const QPoint savedCursor = QCursor::pos();
    QVector<ScreenFrame> frames = freezeScreens();
    for (ScreenFrame &frame:frames) frame.image.fill(QColor(160,180,200));
    CaptureOverlay overlay(frames);
    overlay.setAttribute(Qt::WA_DeleteOnClose,false);
    overlay.m_roundEnabled = false;
    overlay.m_roundRadius = 0;
    overlay.raise(); overlay.activateWindow(); overlay.setFocus();
    QApplication::processEvents();
    const QPoint origin = overlay.localScreenRect(QGuiApplication::primaryScreen()->geometry()).topLeft();
    const QRect base(origin+QPoint(233,173),QSize(171,113));
    const auto mouse = [&overlay](QEvent::Type type,const QPoint &point,Qt::MouseButton button,Qt::MouseButtons buttons) {
        const QPoint global = overlay.windowHandle()->mapToGlobal(QPointF(point)).toPoint();
        QMouseEvent event(type,point,global,button,buttons,Qt::NoModifier);
        QApplication::sendEvent(&overlay,&event);
    };
    const auto key = [&overlay](int code,Qt::KeyboardModifiers modifiers=Qt::NoModifier,bool repeat=false) {
        QKeyEvent event(QEvent::KeyPress,code,modifiers,QString(),repeat);
        QApplication::sendEvent(&overlay,&event);
    };
    Annotation ann;
    ann.tool = ATool::Rect; ann.rect = QRect(base.center()-QPoint(12,12),QSize(24,24));
    overlay.m_anns.append(ann); overlay.pushUndo();
    const int undoCount = overlay.m_undoStack.size();
    for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x) {
        if (!x && !y) continue;
        overlay.m_sel=base; overlay.m_mode=Adjusting; overlay.m_drag=H_None;
        const QPoint outside(x<0 ? base.left()-28 : x>0 ? base.right()+24 : base.center().x(),
                             y<0 ? base.top()-22 : y>0 ? base.bottom()+24 : base.center().y());
        QRect expected=base;
        if (x<0) expected.setLeft(outside.x());
        if (x>0) expected.setRight(outside.x());
        if (y<0) expected.setTop(outside.y());
        if (y>0) expected.setBottom(outside.y());
        mouse(QEvent::MouseButtonPress,outside,Qt::LeftButton,Qt::LeftButton);
        check(overlay.m_sel==expected && overlay.m_mode==Adjusting && overlay.m_drag!=H_New
                  && !overlay.m_tools->isVisible(),"every outside direction immediately extends the original selection on press");
        check(overlay.m_anns.size()==1 && overlay.m_anns.first().rect==ann.rect
                  && overlay.m_undoStack.size()==undoCount,"outside expansion preserves annotations and undo history");
        mouse(QEvent::MouseButtonRelease,outside,Qt::LeftButton,Qt::NoButton);
        check(overlay.m_sel==expected && overlay.m_drag==H_None && overlay.m_tools->isVisible(),
              "outside release retains the expanded region and restores the toolbar");
        check(overlay.m_tools->geometry().top() > overlay.m_sel.bottom(),
              "restored toolbar is positioned around the committed selection");
        overlay.m_sel=base;
        mouse(QEvent::MouseButtonPress,outside,Qt::LeftButton,Qt::LeftButton);
        const QPoint inside = base.center();
        mouse(QEvent::MouseMove,inside,Qt::NoButton,Qt::LeftButton);
        QRect smaller=base;
        if (x<0) smaller.setLeft(inside.x());
        if (x>0) smaller.setRight(inside.x());
        if (y<0) smaller.setTop(inside.y());
        if (y>0) smaller.setBottom(inside.y());
        check(overlay.m_sel==smaller,"an outside press continues resizing against the original opposite edge or corner");
        mouse(QEvent::MouseButtonRelease,inside,Qt::LeftButton,Qt::NoButton);
    }
    overlay.m_sel=base;
    const QPoint outside(base.left()-28,base.center().y());
    mouse(QEvent::MouseButtonPress,outside,Qt::LeftButton,Qt::LeftButton);
    const QPoint across(base.right()+24,base.center().y());
    mouse(QEvent::MouseMove,across,Qt::NoButton,Qt::LeftButton);
    if (overlay.m_sel!=QRect(QPoint(base.right(),base.top()),QPoint(across.x(),base.bottom())))
        std::fprintf(stderr,"CROSS: actual=(%d,%d %dx%d) fixed-right=%d pointer=%d\n",overlay.m_sel.x(),overlay.m_sel.y(),overlay.m_sel.width(),overlay.m_sel.height(),base.right(),across.x());
    check(overlay.m_sel==QRect(QPoint(base.right(),base.top()),QPoint(across.x(),base.bottom())),
          "resizing across the fixed edge normalizes rather than moving or restarting the capture");
    mouse(QEvent::MouseButtonRelease,across,Qt::LeftButton,Qt::NoButton);
    overlay.m_sel=base;
    const QPoint moveStart=base.center()+QPoint(40,20);
    mouse(QEvent::MouseButtonPress,moveStart,Qt::LeftButton,Qt::LeftButton);
    mouse(QEvent::MouseMove,moveStart+QPoint(17,13),Qt::NoButton,Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease,moveStart+QPoint(17,13),Qt::LeftButton,Qt::NoButton);
    check(overlay.m_sel==base.translated(17,13),"dragging the empty interior still moves the region without resizing");
    overlay.m_sel=base; overlay.m_selAnn=-1;
    for (int code:{Qt::Key_Left,Qt::Key_Up,Qt::Key_Right,Qt::Key_Down}) {
        const QRect before=overlay.m_sel;
        key(code);
        check(overlay.m_sel==before.translated(keyDelta(code)),"ordinary arrows translate the region by exactly one coordinate without snapping");
        const QRect once=overlay.m_sel;
        key(code,Qt::NoModifier,true);
        check(overlay.m_sel==once.translated(keyDelta(code)),"holding an arrow repeats region movement");
    }
    overlay.m_sel=QRect(QPoint(),QSize(20,20));
    key(Qt::Key_Left); key(Qt::Key_Up);
    check(overlay.m_sel==QRect(0,0,20,20),"region movement clamps at the virtual desktop origin");
    overlay.m_sel=QRect(overlay.width()-20,overlay.height()-20,20,20);
    const QRect lowerEdge=overlay.m_sel;
    key(Qt::Key_Right); key(Qt::Key_Down);
    check(overlay.m_sel==lowerEdge,"region movement clamps at the virtual desktop end");
    overlay.m_sel=base;
    key(Qt::Key_Left,Qt::KeypadModifier);
    check(overlay.m_sel==base.translated(-1,0),"numeric-keypad arrows retain the same one-step movement semantics");
    overlay.m_sel=base;
    QCursor::setPos(overlay.windowHandle()->mapToGlobal(QPointF(base.center())).toPoint());
    QApplication::processEvents();
    for (int code:{Qt::Key_W,Qt::Key_A,Qt::Key_S,Qt::Key_D}) {
        const QPoint before=QCursor::pos();
        key(code);
        check(QCursor::pos()==before+keyDelta(code) && overlay.m_sel==base && overlay.m_atool==ATool::None,
              "WASD moves the actual cursor one coordinate and never arms an annotation tool or changes the idle region");
    }
    const QPoint beforeRepeat=QCursor::pos();
    key(Qt::Key_D,Qt::NoModifier,true);
    check(QCursor::pos()==beforeRepeat+QPoint(1,0),"held WASD repeats cursor movement");
    for (const QScreen *screen:QGuiApplication::screens()) {
        QCursor::setPos(screen->geometry().center());
        QApplication::processEvents();
        overlay.m_sel=base; overlay.m_mode=Adjusting;
        const QRect current=overlay.localScreenRect(screen->geometry());
        key(Qt::Key_A,Qt::ControlModifier);
        check(overlay.m_sel==current && overlay.m_hover.isEmpty() && !overlay.m_queryPending,
              "Ctrl+A selects the cursor screen including its taskbar without re-detecting windows");
        key(Qt::Key_A,Qt::ControlModifier);
        check(overlay.m_sel==overlay.rect(),"a second Ctrl+A selects the entire virtual desktop");
        key(Qt::Key_A,Qt::ControlModifier);
        check(overlay.m_sel==current,"Ctrl+A can return from the virtual desktop to the cursor screen");
        overlay.m_mode=Selecting; overlay.m_sel={};
        key(Qt::Key_A,Qt::ControlModifier);
        check(overlay.m_sel==current && overlay.m_mode==Adjusting,"Ctrl+A works before any manual selection exists");
    }
    check(overlay.m_anns.size()==1 && overlay.m_anns.first().rect==ann.rect
              && overlay.m_undoStack.size()==undoCount,"keyboard region changes preserve the old annotation and its undo stack");
    overlay.m_sel=base; overlay.m_selAnn=0;
    key(Qt::Key_Left);
    check(overlay.m_sel==base && overlay.m_anns.first().rect==ann.rect.translated(-1,0),
          "a selected annotation retains priority over region nudging");
    overlay.m_anns.clear(); overlay.m_selAnn=-1;
    overlay.beginTextEdit(base.center());
    overlay.m_editor->setPlainText(QStringLiteral("校对文字"));
    key(Qt::Key_A,Qt::ControlModifier);
    check(overlay.m_sel==base && overlay.editorActive() && overlay.m_editor->toPlainText()==QStringLiteral("校对文字"),
          "text editing cannot turn Ctrl+A into a fullscreen selection");
    overlay.m_editor->setPlainText(QString()); overlay.commitTextEdit(); overlay.setToolNone();
    overlay.m_sel=base;
    key(Qt::Key_Right);
    const qreal scale=overlay.m_shot.devicePixelRatio();
    QPixmap expected=overlay.m_shot.copy(QRect(qRound(overlay.m_sel.x()*scale),qRound(overlay.m_sel.y()*scale),
                                              qRound(overlay.m_sel.width()*scale),qRound(overlay.m_sel.height()*scale)));
    expected.setDevicePixelRatio(1);
    check(overlay.cropped().toImage()==expected.toImage(),"output crops the adjusted region at the frozen image's native pixel scale");
    {
        // A mixed-DPI desktop stores each screen image at its own native origin.
        // A selection on the low-DPI screen must not be upscaled by the combined shot.
        const QPoint origin = QGuiApplication::primaryScreen()->geometry().topLeft();
        QImage high(200, 200, QImage::Format_RGB32);
        high.fill(Qt::red);
        high.setDevicePixelRatio(2);
        QImage low(100, 100, QImage::Format_RGB32);
        low.fill(Qt::blue);
        ScreenFrame highFrame = {QRect(origin, QSize(100, 100)), QRect(0, 0, 200, 200), high};
        ScreenFrame lowFrame = {QRect(origin + QPoint(100, 0), QSize(100, 100)),
                                QRect(200, 0, 100, 100), low};
        CaptureOverlay mixed({highFrame, lowFrame});
        mixed.setAttribute(Qt::WA_DeleteOnClose, false);
        mixed.m_sel = QRect(QPoint(100, 0), QSize(100, 100));
        const QImage mixedOut = mixed.cropped().toImage();
        check(mixedOut.size() == QSize(100, 100)
                  && mixedOut.pixelColor(50, 50) == QColor(Qt::blue),
              "mixed-DPI crop uses the selected screen's local native pixels");
    }
    failures += testCustomSize();
    overlay.m_toolSide[QStringLiteral("arrow")] = QStringLiteral("top");
    overlay.m_toolSide[QStringLiteral("ocr")] = QStringLiteral("left");
    overlay.m_toolSide[QStringLiteral("pen")] = QStringLiteral("right");
    overlay.relayoutToolButtons();
    overlay.m_atool = ATool::None;
    if (overlay.m_toolOptions) overlay.m_toolOptions->hide();
    const QRect fullBounds = overlay.rect();
    const QRect screenBounds = overlay.localScreenRect(
        QGuiApplication::primaryScreen()->availableGeometry());
    const QSize edgeSize(200, 160);
    const QVector<QRect> edgeSelections{
        QRect(QPoint(screenBounds.center().x() - edgeSize.width() / 2, screenBounds.top()), edgeSize),
        QRect(QPoint(screenBounds.center().x() - edgeSize.width() / 2,
                     screenBounds.bottom() + 1 - edgeSize.height()), edgeSize),
        QRect(QPoint(screenBounds.left(), screenBounds.center().y() - edgeSize.height() / 2), edgeSize),
        QRect(QPoint(screenBounds.right() + 1 - edgeSize.width(),
                     screenBounds.center().y() - edgeSize.height() / 2), edgeSize)
    };
    for (const QRect &edge : edgeSelections) {
        overlay.m_sel = edge;
        overlay.m_hover = {};
        overlay.placeToolBars();
        overlay.updateTip();
        bool chromeOverlap = false;
        bool barOverlapsSelection = false;
        for (QWidget *bar : {overlay.m_tools, overlay.m_toolsTop,
                             overlay.m_toolsLeft, overlay.m_toolsRight}) {
            if (!bar || !bar->isVisible()) continue;
            if (bar != overlay.m_toolsTop
                && overlay.m_sizeBar->geometry().intersects(bar->geometry()))
                chromeOverlap = true;
            if (bar->layout() && bar->layout()->count() > 0
                && bar->geometry().intersects(edge))
                barOverlapsSelection = true;
        }
        const bool chromeOk = overlay.m_sizeBar->isVisible()
            && overlay.m_toolsTop->isVisible()
            && fullBounds.contains(overlay.m_sizeBar->geometry())
            && fullBounds.contains(overlay.m_toolsTop->geometry())
            && !chromeOverlap
            && !barOverlapsSelection
            && !overlay.m_sizeBar->geometry().intersects(edge)
            && !overlay.m_sizeBar->geometry().intersects(overlay.m_toolsTop->geometry());
        if (!chromeOk)
            std::fprintf(stderr, "EDGE sel=(%d,%d %dx%d) size=(%d,%d %dx%d) top=(%d,%d %dx%d) left=(%d,%d %dx%d) right=(%d,%d %dx%d) bottom=(%d,%d %dx%d) barsSel=%d chrome=%d\n",
                         edge.x(), edge.y(), edge.width(), edge.height(),
                         overlay.m_sizeBar->x(), overlay.m_sizeBar->y(),
                         overlay.m_sizeBar->width(), overlay.m_sizeBar->height(),
                         overlay.m_toolsTop->x(), overlay.m_toolsTop->y(),
                         overlay.m_toolsTop->width(), overlay.m_toolsTop->height(),
                         overlay.m_toolsLeft->x(), overlay.m_toolsLeft->y(),
                         overlay.m_toolsLeft->width(), overlay.m_toolsLeft->height(),
                         overlay.m_toolsRight->x(), overlay.m_toolsRight->y(),
                         overlay.m_toolsRight->width(), overlay.m_toolsRight->height(),
                         overlay.m_tools->x(), overlay.m_tools->y(),
                         overlay.m_tools->width(), overlay.m_tools->height(),
                         barOverlapsSelection ? 1 : 0, chromeOverlap ? 1 : 0);
        check(chromeOk, "chrome stays outside the selection without overlapping at every edge");
    }
    const QString visualDir = QStringLiteral("debug/strict-ui/audit/boundary");
    if (QDir().mkpath(visualDir)) {
        const auto stage = [&overlay, &screenBounds, &visualDir](const QString &side,
                                                                  const QString &name) {
            overlay.m_moreButton->setChecked(false);
            overlay.m_hover = {};
            for (const QString &id : overlay.m_toolOrder) {
                const bool hidden = id == QStringLiteral("pen")
                    || id == QStringLiteral("marker") || id == QStringLiteral("mosaic");
                overlay.m_toolSide[id] = hidden ? QStringLiteral("hidden") : side;
            }
            overlay.relayoutToolButtons();
            const QSize size(300, 200);
            if (side == QStringLiteral("top"))
                overlay.m_sel = QRect(QPoint(screenBounds.center().x() - size.width() / 2,
                                             screenBounds.top()), size);
            else if (side == QStringLiteral("bottom"))
                overlay.m_sel = QRect(QPoint(screenBounds.center().x() - size.width() / 2,
                                             screenBounds.bottom() + 1 - size.height()), size);
            else if (side == QStringLiteral("left"))
                overlay.m_sel = QRect(QPoint(screenBounds.left(),
                                             screenBounds.center().y() - size.height() / 2), size);
            else
                overlay.m_sel = QRect(QPoint(screenBounds.right() + 1 - size.width(),
                                             screenBounds.center().y() - size.height() / 2), size);
            overlay.m_atool = ATool::None;
            if (overlay.m_toolOptions) overlay.m_toolOptions->hide();
            overlay.placeToolBars();
            overlay.m_moreButton->click();
            QApplication::processEvents();
            overlay.grab(screenBounds).save(visualDir + QLatin1Char('/') + name + QStringLiteral(".png"));
            overlay.m_moreButton->setChecked(false);
            if (overlay.m_morePanel) overlay.m_morePanel->hide();
        };
        stage(QStringLiteral("top"), QStringLiteral("top"));
        stage(QStringLiteral("bottom"), QStringLiteral("bottom"));
        stage(QStringLiteral("left"), QStringLiteral("left"));
        stage(QStringLiteral("right"), QStringLiteral("right"));
    }
    {   // The frame and everything outside it resize the region even with a tool armed.
        overlay.m_anns.clear();
        overlay.m_undoStack.clear();
        overlay.m_redoStack.clear();
        overlay.m_sel = base;
        overlay.m_selAnn = -1;
        overlay.selectTool(ATool::Rect);
        const QPoint outside = base.bottomRight() + QPoint(40, 40);
        mouse(QEvent::MouseButtonPress, outside, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
        check(overlay.m_sel == QRect(base.topLeft(), outside) && overlay.m_anns.isEmpty()
                  && overlay.m_atool == ATool::Rect,
              "an armed tool does not draw outside the frame: the press resizes the region");
        overlay.m_sel = base;
        const QPoint edge(base.right(), base.center().y() + 30);
        mouse(QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, edge + QPoint(20, 0), Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, edge + QPoint(20, 0), Qt::LeftButton, Qt::NoButton);
        check(overlay.m_sel == QRect(base.topLeft(), QPoint(base.right() + 20, base.bottom()))
                  && overlay.m_anns.isEmpty() && overlay.m_atool == ATool::Rect,
              "the frame itself resizes the region instead of starting a mark");
        overlay.m_sel = base;
        const QPoint inner(base.right() - 2, base.center().y() + 30);
        mouse(QEvent::MouseButtonPress, inner, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, inner + QPoint(20, 10), Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, inner + QPoint(20, 10), Qt::LeftButton, Qt::NoButton);
        check(overlay.m_anns.size() == 1 && overlay.m_atool == ATool::Rect,
              "two pixels inside the frame still starts a mark");
        overlay.undoAnnotation();
        overlay.m_anns.clear();
        overlay.m_selAnn = -1;
        Annotation outsideMark;
        outsideMark.tool = ATool::Rect;
        outsideMark.color = QColor(255, 0, 255);
        outsideMark.pen = 2;
        outsideMark.fill = true;
        outsideMark.rect = QRect(base.center(), QSize(180, 80));
        overlay.m_anns.append(outsideMark);
        for (QWidget *bar : {overlay.m_toolsTop, overlay.m_toolsLeft, overlay.m_toolsRight,
                             overlay.m_tools, overlay.m_toolOptions})
            if (bar) bar->hide();
        overlay.update();
        QApplication::processEvents();
        const QPixmap shown = overlay.grab();
        const qreal shownDpr = shown.devicePixelRatio();
        const QPoint probe(base.right() + 24, base.center().y() + 8);
        check(shown.toImage().pixelColor(QPoint(qRound(probe.x() * shownDpr),
                                                qRound(probe.y() * shownDpr))) == outsideMark.color,
              "mark content remains visible outside the capture frame");
        check(overlay.cropped().width() == qRound(base.width() * overlay.m_shot.devicePixelRatioF()),
              "capture output still clips marks to the selection");
        overlay.m_anns.clear();
        overlay.m_undoStack.clear();
        overlay.setToolNone();
        overlay.m_sel = base;
    }
    overlay.resetSelection();
    check(overlay.m_sel.isEmpty() && overlay.m_mode==Selecting,"clearing the selection still permits a new capture");
    overlay.close();
    QCursor::setPos(savedCursor);
    failures += testSelectionBorders();
    if (!failures) std::printf("PASS: eight-direction outside adjustment, cursor/region keys, screen toggle and crop output\n");
    return failures ? 1 : 0;
}

int CaptureOverlay::testToolbarPlacement(const QString &previewPath)
{
    int failures = 0;
    auto frames = freezeScreens();
    for (ScreenFrame &frame : frames) {
        frame.image.fill(QColor(110, 135, 160));
        QPainter painter(&frame.image);
        painter.setPen(QColor(85, 110, 135));
        for (int x = 0; x < frame.image.width(); x += 40)
            painter.drawLine(x, 0, x, frame.image.height());
        for (int y = 0; y < frame.image.height(); y += 40)
            painter.drawLine(0, y, frame.image.width(), y);
    }
    CaptureOverlay overlay(frames, {}, DetectionOptions(), false);
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    overlay.m_roundEnabled = false;
    overlay.m_toolOrder = QStringList{
        QStringLiteral("more"), QStringLiteral("save"), QStringLiteral("refresh"),
        QStringLiteral("pin"), QStringLiteral("barcode"), QStringLiteral("translate"),
        QStringLiteral("ocr"), QStringLiteral("scroll"), QStringLiteral("redo"),
        QStringLiteral("undo"), QStringLiteral("eraser"), QStringLiteral("number"),
        QStringLiteral("copy"), QStringLiteral("text"), QStringLiteral("shape"),
        QStringLiteral("arrow"), QStringLiteral("marker"), QStringLiteral("pen"),
        QStringLiteral("mosaic")};
    const QRect bounds = overlay.localScreenRect(
        QGuiApplication::primaryScreen()->availableGeometry());
    const QSize size(246, 116);
    const int midX = bounds.center().x() - size.width() / 2;
    const int midY = bounds.center().y() - size.height() / 2;
    const int right = bounds.right() + 1 - size.width();
    const int bottom = bounds.bottom() + 1 - size.height();
    const QVector<QPair<QString, QPoint>> positions{
        {QStringLiteral("top"), QPoint(midX, bounds.top())},
        {QStringLiteral("bottom"), QPoint(midX, bottom)},
        {QStringLiteral("left"), QPoint(bounds.left(), midY)},
        {QStringLiteral("right"), QPoint(right, midY)},
        {QStringLiteral("top-left"), bounds.topLeft()},
        {QStringLiteral("top-right"), QPoint(right, bounds.top() + 20)},
        {QStringLiteral("bottom-left"), QPoint(bounds.left(), bottom)},
        {QStringLiteral("bottom-right"), QPoint(right, bottom)}};
    if (!previewPath.isEmpty()) QDir().mkpath(previewPath);
    for (bool mirrored : {false, true}) {
        for (const QString &id : overlay.m_toolOrder) {
            const bool vertical = id == QStringLiteral("marker")
                || id == QStringLiteral("pen") || id == QStringLiteral("mosaic");
            overlay.m_toolSide[id] = id == QStringLiteral("more")
                ? (mirrored ? QStringLiteral("top") : QStringLiteral("bottom"))
                : vertical ? (mirrored ? QStringLiteral("right") : QStringLiteral("left"))
                           : (mirrored ? QStringLiteral("bottom") : QStringLiteral("top"));
        }
        overlay.relayoutToolButtons();
        for (const auto &position : positions) {
            const QString name = (mirrored ? QStringLiteral("mirrored-") : QStringLiteral("user-"))
                + position.first;
            overlay.m_sel = QRect(position.second, size);
            overlay.m_mode = Adjusting;
            overlay.m_hover = {};
            overlay.placeToolBars();
            overlay.showToolBars();
            QApplication::processEvents();
            overlay.m_inspector->hide();
            const QVector<QWidget *> chrome{overlay.m_toolsTop, overlay.m_toolsRight,
                overlay.m_tools, overlay.m_toolsLeft, overlay.m_sizeBar};
            if (!overlay.m_sizeBar->isVisible()) {
                ++failures;
                std::fprintf(stderr, "FAIL %s: size strip disappeared\n", qPrintable(name));
            }
            for (int i = 0; i < chrome.size(); ++i) {
                QWidget *a = chrome.at(i);
                if (!a->isVisible()) {
                    if (a != overlay.m_sizeBar && a->layout() && a->layout()->count() > 0) {
                        ++failures;
                        std::fprintf(stderr, "FAIL %s: nonempty chrome %d disappeared\n", qPrintable(name), i);
                    }
                    continue;
                }
                if (!bounds.contains(a->geometry())) {
                    ++failures;
                    std::fprintf(stderr, "FAIL %s: chrome %d outside screen\n", qPrintable(name), i);
                }
                for (int j = i + 1; j < chrome.size(); ++j) {
                    QWidget *b = chrome.at(j);
                    if (!b->isVisible()) continue;
                    if (a->geometry().adjusted(-3, -3, 3, 3).intersects(b->geometry())) {
                        ++failures;
                        std::fprintf(stderr, "FAIL %s: chrome %d (%d,%d %dx%d) overlaps %d (%d,%d %dx%d)\n",
                            qPrintable(name), i, a->x(), a->y(), a->width(), a->height(),
                            j, b->x(), b->y(), b->width(), b->height());
                    }
                }
            }
            QVector<QRect> firstPlacement;
            for (QWidget *widget : chrome) firstPlacement << widget->geometry();
            overlay.hideToolBars();
            overlay.placeToolBars();
            overlay.showToolBars();
            for (int i = 0; i < chrome.size(); ++i) {
                if (chrome.at(i)->geometry() != firstPlacement.at(i)) {
                    ++failures;
                    std::fprintf(stderr, "FAIL %s: chrome %d moved after hide/show\n", qPrintable(name), i);
                }
            }
            if (!previewPath.isEmpty()) {
                QRect view = overlay.m_sel.adjusted(-12, -12, 12, 12);
                for (QWidget *widget : chrome)
                    if (widget->isVisible()) view = view.united(widget->geometry().adjusted(-12, -12, 12, 12));
                view = view.intersected(bounds);
                const QString output = previewPath + QLatin1Char('/') + name;
                overlay.grab(view).save(output + QStringLiteral(".png"));
                SetWindowPos(reinterpret_cast<HWND>(overlay.winId()), HWND_TOPMOST,
                             0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                QApplication::processEvents();
                overlay.m_inspector->hide();
                overlay.repaint();
                QThread::msleep(60);
                const QRect global(overlay.mapToGlobal(view.topLeft()), view.size());
                grabNative(nativeFromLogical(global)).save(output + QStringLiteral("-desktop.png"));
            }
        }
    }
    overlay.close();
    std::printf("%s: mixed horizontal/vertical toolbar placement, 4 edges + 4 corners, both layouts\n",
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

int CaptureOverlay::testCustomSize()
{
    int failures = 0;
    const auto check = [&failures](bool ok, const char *message) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
    };
    LensSnipSettings settings;
    const bool hadUnit = settings.contains(QStringLiteral("Capture/sizeUnit"));
    const QVariant oldUnit = settings.value(QStringLiteral("Capture/sizeUnit"));
    const bool hadPresets = settings.contains(QStringLiteral("Capture/presets"));
    const QVariant oldPresets = settings.value(QStringLiteral("Capture/presets"));
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    const QRect frameRect(screen.topLeft(), QSize(800, 600));
    QImage image(1600, 1200, QImage::Format_RGB32);
    image.fill(QColor(120, 150, 180));
    CaptureOverlay overlay({ScreenFrame{frameRect, QRect(0, 0, 1600, 1200), image}});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    const QPoint origin = overlay.localScreenRect(frameRect).topLeft();
    overlay.m_mode = Adjusting;
    overlay.m_sel = QRect(origin + QPoint(100, 100), QSize(120, 60));
    overlay.m_sizeUnit = SizeUnit::Px;
    overlay.updateTip();
    check(overlay.m_sizeTip->text() == QStringLiteral("240 × 120")
              && overlay.m_sizeUnitButton->text() == QStringLiteral("px"),
          "the size strip shows the region in the selected px unit");
    overlay.m_sizeUnit = SizeUnit::Dp;
    overlay.updateTip();
    check(overlay.m_sizeTip->text() == QStringLiteral("120 × 60")
              && overlay.m_sizeUnitButton->text() == QStringLiteral("dip"),
          "the size strip switches to logical dip without moving the region");

    overlay.m_sizeUnit = SizeUnit::Px;
    overlay.updateTip();
    check(qAbs(overlay.m_sizeBar->geometry().center().x() - overlay.m_sel.center().x()) <= 1
              && overlay.m_sizeBar->geometry().bottom() < overlay.m_sel.top(),
          "the size strip is centered outside above the selection");
    const QRect savedSel = overlay.m_sel;
    overlay.m_sel = overlay.rect();
    overlay.updateTip();
    check(overlay.m_sizeBar->geometry().top() > overlay.m_sel.top()
              && overlay.m_sizeBar->geometry().bottom() <= overlay.m_sel.bottom()
              && qAbs(overlay.m_sizeBar->geometry().center().x() - overlay.m_sel.center().x()) <= 1,
          "a full-screen size strip is centered inside the top edge");
    overlay.m_sel = savedSel;
    overlay.updateTip();
    const auto clickSizeLabel = [&overlay] {
        const QPoint at(2, 2);
        QMouseEvent press(QEvent::MouseButtonPress, at, overlay.m_sizeTip->mapToGlobal(at),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(overlay.m_sizeTip, &press);
        QApplication::processEvents();
    };
    clickSizeLabel();
    auto *x = overlay.findChild<QSpinBox *>(QStringLiteral("customX"));
    auto *y = overlay.findChild<QSpinBox *>(QStringLiteral("customY"));
    auto *width = overlay.findChild<QSpinBox *>(QStringLiteral("customWidth"));
    auto *height = overlay.findChild<QSpinBox *>(QStringLiteral("customHeight"));
    auto *unit = overlay.findChild<QToolButton *>(QStringLiteral("sizeUnit"));
    auto *lock = overlay.findChild<QToolButton *>(QStringLiteral("sizeLockAspect"));
    auto *preset = overlay.findChild<QComboBox *>(QStringLiteral("customPreset"));
    auto *save = overlay.findChild<QPushButton *>(QStringLiteral("customPresetSave"));
    auto *popup = overlay.m_customSizePopup.data();
    check(popup && x && y && width && height && preset && save
              && x->value() == 200 && y->value() == 200
              && width->value() == 240 && height->value() == 120,
          "clicking the size label opens an independent panel with live position and size");
    check(preset && preset->currentText().isEmpty()
              && !preset->lineEdit() && preset->isEnabled(),
          "the preset combo has no placeholder and stays clickable");
    check(overlay.m_sizeBar && overlay.m_sizeBar->isVisible() && unit && lock
              && unit->text() == QStringLiteral("px") && !lock->isChecked(),
          "the size strip carries the unit switch and aspect lock");
    if (popup && QDir(QStringLiteral("debug/publish-checks")).exists()) {
        popup->grab().save(QStringLiteral("debug/publish-checks/custom-size-panel.png"));
        overlay.m_sizeBar->grab().save(QStringLiteral("debug/publish-checks/custom-size-bar.png"));
        const QImage framed = grabNative(nativeFromLogical(popup->frameGeometry()));
        if (!framed.isNull())
            framed.save(QStringLiteral("debug/publish-checks/custom-size-window.png"));
    }
    if (!popup || !x || !y || !width || !height || !unit || !lock || !preset || !save) {
        settings.remove(QStringLiteral("Capture/sizeUnit"));
        settings.remove(QStringLiteral("Capture/presets"));
        if (hadUnit) settings.setValue(QStringLiteral("Capture/sizeUnit"), oldUnit);
        if (hadPresets) settings.setValue(QStringLiteral("Capture/presets"), oldPresets);
        if (popup) popup->close();
        return failures;
    }
    const Qt::WindowFlags flags = popup->windowFlags();
    check((flags & Qt::WindowType_Mask) == Qt::Tool
              && flags.testFlag(Qt::FramelessWindowHint)
              && popup->windowTitle() == QStringLiteral("自定义截图")
              && popup->findChild<QToolButton *>(QStringLiteral("dialogClose")),
          "the editor remains an independent tool window with its designed title and close button");
    const QList<QWidget *> controls{x, y, width, height, preset, save};
    for (QWidget *control : controls) {
        const QRect bounds(control->mapTo(popup, QPoint()), control->size());
        QWidget *hit = popup->childAt(bounds.center());
        const bool usable = control->isVisible() && control->isEnabled()
                  && control->width() >= control->minimumSizeHint().width()
                  && control->height() >= 24 && popup->rect().contains(bounds)
                  && (hit == control || control->isAncestorOf(hit));
        if (!usable)
            std::fprintf(stderr, "DETAIL: %s visible=%d enabled=%d size=%dx%d min=%dx%d bounds=%d,%d %dx%d popup=%dx%d hit=%s\n",
                         qPrintable(control->objectName()), control->isVisible(), control->isEnabled(),
                         control->width(), control->height(), control->minimumSizeHint().width(),
                         control->minimumSizeHint().height(), bounds.x(), bounds.y(),
                         bounds.width(), bounds.height(), popup->width(), popup->height(),
                         hit ? qPrintable(hit->objectName()) : "null");
        check(usable,
              "every custom-size control is visible, usable, inside the panel and unobstructed");
        for (QWidget *other : controls) {
            if (control == other) continue;
            const QRect otherBounds(other->mapTo(popup, QPoint()), other->size());
            check(!bounds.intersects(otherBounds), "custom-size controls must not overlap");
        }
    }
    check(x->mapTo(popup, QPoint()).y() == y->mapTo(popup, QPoint()).y()
              && width->mapTo(popup, QPoint()).y() == height->mapTo(popup, QPoint()).y()
              && width->mapTo(popup, QPoint()).y() > x->mapTo(popup, QPoint()).y(),
          "position and size use separate aligned rows");
    auto *xLetter = popup->findChild<QLabel *>(QStringLiteral("customXLetter"));
    auto *yLetter = popup->findChild<QLabel *>(QStringLiteral("customYLetter"));
    auto *wLetter = popup->findChild<QLabel *>(QStringLiteral("customWidthLetter"));
    auto *hLetter = popup->findChild<QLabel *>(QStringLiteral("customHeightLetter"));
    const auto outsideField = [popup](QLabel *letter, QSpinBox *spin) {
        QWidget *field = spin ? spin->parentWidget() : nullptr;
        return letter && field
            && letter->mapTo(popup, QPoint(letter->width(), 0)).x()
                   <= field->mapTo(popup, QPoint()).x()
            && qAbs(letter->mapTo(popup, QPoint(0, letter->height() / 2)).y()
                    - field->mapTo(popup, QPoint(0, field->height() / 2)).y()) <= 1;
    };
    check(xLetter && yLetter && wLetter && hLetter
              && xLetter->text() == QStringLiteral("X") && yLetter->text() == QStringLiteral("Y")
              && wLetter->text() == QStringLiteral("W") && hLetter->text() == QStringLiteral("H")
              && outsideField(xLetter, x) && outsideField(yLetter, y)
              && outsideField(wLetter, width) && outsideField(hLetter, height)
              && (x->alignment() & Qt::AlignHCenter) && (y->alignment() & Qt::AlignHCenter)
              && (width->alignment() & Qt::AlignHCenter)
              && (height->alignment() & Qt::AlignHCenter),
          "X/Y/W/H sit outside their fields and the values are centered");
    auto *stepUp = width->parentWidget()->findChild<QToolButton *>(QStringLiteral("customStepUp"));
    const int beforeStep = width->value();
    if (stepUp) stepUp->click();
    QApplication::processEvents();
    check(stepUp && width->value() > beforeStep,
          "the field stepper adjusts the live region on click");
    const int beforeWheel = width->value();
    QWheelEvent wheel(QPointF(10, 10), QPointF(width->mapToGlobal(QPoint(10, 10))),
                      QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(width, &wheel);
    QApplication::processEvents();
    check(width->value() > beforeWheel,
          "the wheel adjusts the field and the live region");
    QWidget *heightField = height->parentWidget();
    check(heightField && save->mapTo(popup, QPoint(save->width(), 0)).x()
              == heightField->mapTo(popup, QPoint(heightField->width(), 0)).x(),
          "the preset row ends at the same right edge as the size fields");
    const QPoint panelPos = popup->pos();

    width->setValue(300);
    QApplication::processEvents();
    check(overlay.m_sel.size() == QSize(150, 60) && popup->pos() == panelPos,
          "editing width updates the selection immediately without moving the panel");

    x->setValue(300);
    QApplication::processEvents();
    check(overlay.m_sel.topLeft() == origin + QPoint(150, 100) && popup->pos() == panelPos,
          "editing position moves the selection immediately without moving the panel");
    check(overlay.m_sel.width() == 150 && overlay.m_sel.height() == 60,
          "editing position leaves the selection size unchanged");

    lock->setChecked(true);
    if (QDir(QStringLiteral("debug/publish-checks")).exists())
        popup->grab().save(QStringLiteral("debug/publish-checks/custom-size-panel-locked.png"));
    width->setValue(400);
    QApplication::processEvents();
    check(overlay.m_lockAspect && height->value() == 160
              && overlay.m_sel.size() == QSize(200, 80) && popup->pos() == panelPos,
          "locking the ratio keeps live width/height edits proportional and the panel fixed");

    unit->click();
    QApplication::processEvents();
    check(unit->text() == QStringLiteral("dip") && x->value() == 150 && y->value() == 100
              && width->value() == 200 && height->value() == 80,
          "switching the unit converts both live position and size fields");
    width->setValue(300);
    QApplication::processEvents();
    check(overlay.m_sel.size() == QSize(300, 120) && height->value() == 120
              && popup->pos() == panelPos,
          "live dip size edits remain locked and keep the panel in place");

    const int presetsBefore = overlay.m_sizePresets.size();
    const bool hadTestPreset = preset->findText(QStringLiteral("测试预设")) >= 0;
    overlay.saveSizePreset(QStringLiteral("测试预设"));
    const int expectedPresets = presetsBefore + (hadTestPreset ? 0 : 1);
    check(overlay.m_sizePresets.size() == expectedPresets
              && preset->currentText() == QStringLiteral("测试预设")
              && settings.value(QStringLiteral("Capture/presets")).toStringList().size() == expectedPresets,
          "saving a preset stores the current region and selects it");
    const int commonIndex = preset->findText(QStringLiteral("640 × 480 px"));
    check(commonIndex > 0, "the preset list includes common px sizes");
    if (QDir(QStringLiteral("debug/publish-checks")).exists()) {
        preset->showPopup();
        QApplication::processEvents();
        preset->view()->window()->grab().save(
            QStringLiteral("debug/publish-checks/custom-size-presets.png"));
        preset->hidePopup();
    }
    preset->setCurrentIndex(commonIndex);
    QApplication::processEvents();
    check(overlay.m_sel.topLeft() == origin + QPoint(150, 100)
              && overlay.m_sel.size() == QSize(320, 240)
              && width->value() == 320 && height->value() == 240 && popup->pos() == panelPos,
          "selecting a common size keeps the top-left and applies the px size");
    x->setValue(200);
    width->setValue(200);
    QApplication::processEvents();
    check(preset->currentIndex() == -1 && overlay.m_sel.size() == QSize(200, 150),
          "editing after a preset clears the preset selection");
    const int savedIndex = preset->findText(QStringLiteral("测试预设"));
    check(savedIndex > 0, "saved presets remain in the preset list");
    preset->setCurrentIndex(savedIndex);
    QApplication::processEvents();
    check(overlay.m_sel.topLeft() == origin + QPoint(150, 100)
              && overlay.m_sel.size() == QSize(300, 120)
              && x->value() == 150 && width->value() == 300 && popup->pos() == panelPos,
          "selecting a saved preset updates the selection and fields without moving the panel");

    popup->findChild<QToolButton *>(QStringLiteral("dialogClose"))->click();
    QApplication::processEvents();
    check(!popup->isVisible(), "the editor close button dismisses the panel");
    overlay.m_sel = QRect(origin + QPoint(100, 100), QSize(200, 100));
    lock->setChecked(false);
    lock->setChecked(true);
    check(overlay.m_lockAspect && qFuzzyCompare(overlay.m_aspect, 2.0),
          "the size-strip lock captures the live selection after the editor closes");
    clickSizeLabel();
    check(overlay.m_customSizePopup && overlay.m_customSizePopup->isVisible()
              && overlay.m_customSizePopup->pos() == panelPos,
          "reopening the editor keeps its last window position");

    overlay.m_sel = QRect(origin + QPoint(100, 100), QSize(200, 100));
    overlay.m_lockAspect = true;
    overlay.m_aspect = 2.0;
    overlay.m_pressSel = overlay.m_sel;
    overlay.m_pressPos = overlay.m_sel.topLeft();
    const QRect corner = overlay.resizeBy(H_BR, overlay.m_sel.topLeft() + QPoint(299, 149));
    check(corner.size() == QSize(300, 150), "a locked corner drag preserves the current ratio");
    const QRect edge = overlay.resizeBy(H_R, overlay.m_sel.topLeft() + QPoint(299, 0));
    check(edge.size() == QSize(300, 150) && edge.center().y() == overlay.m_sel.center().y(),
          "a locked edge drag grows the other dimension around the center");
    overlay.m_lockAspect = false;
    const QRect free = overlay.resizeBy(H_BR, overlay.m_sel.topLeft() + QPoint(299, 299));
    check(free.size() == QSize(300, 300), "unlocked handles resize each dimension independently");

    if (hadUnit) settings.setValue(QStringLiteral("Capture/sizeUnit"), oldUnit);
    else settings.remove(QStringLiteral("Capture/sizeUnit"));
    if (hadPresets) settings.setValue(QStringLiteral("Capture/presets"), oldPresets);
    else settings.remove(QStringLiteral("Capture/presets"));
    overlay.close();
    return failures;
}

int CaptureOverlay::testSelectionBorders()
{
    int failures = 0;
    const auto check = [&failures](bool ok, const char *message) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
    };
    for (qreal dpr : {1.0, 1.5, 2.0}) {
        QImage dot(qRound(40 * dpr), qRound(40 * dpr), QImage::Format_ARGB32);
        dot.setDevicePixelRatio(dpr);
        dot.fill(Qt::transparent);
        QPainter painter(&dot);
        paintHandle(painter, QPointF(20, 20));
        painter.end();
        check(dot.pixelColor(qRound(20 * dpr), qRound(20 * dpr)) == QColor(0x5e, 0x6a, 0xd2)
                  && dot.pixelColor(qRound(16 * dpr), qRound(16 * dpr)).alpha() == 0
                  && dot.pixelColor(qRound(26 * dpr), qRound(20 * dpr)).alpha() == 0,
              "handles are accent dots with a light ring and transparent corners at every DPR");
    }
    QVector<ScreenFrame> frames = freezeScreens();
    check(!frames.isEmpty(), "border fixture captures screen geometry");
    if (frames.isEmpty()) return failures;
    for (ScreenFrame &frame : frames) frame.image.fill(QColor(120, 130, 140));
    CaptureOverlay overlay(frames, {}, DetectionOptions(), false);
    overlay.m_roundEnabled = false;
    overlay.m_roundRadius = 0;
    overlay.m_mode = Adjusting;
    overlay.m_sel = overlay.rect().adjusted(20, 20, -20, -20);
    overlay.m_hover = {};
    overlay.m_inspector->hide();
    overlay.m_sizeBar->hide();
    overlay.hideToolBars();
    overlay.m_mosaicBrush = false;
    const qreal dpr = overlay.devicePixelRatioF();
    const auto render = [&] {
        QImage image(qRound(overlay.width() * dpr), qRound(overlay.height() * dpr), QImage::Format_RGB32);
        image.setDevicePixelRatio(dpr);
        overlay.render(&image);
        return image;
    };
    const QString prefix = QStringLiteral("debug/publish-checks/ui-borders-%1-").arg(dpr);
    const bool previews = QDir(QStringLiteral("debug/publish-checks")).exists();
    for (int t = int(ATool::Rect); t <= int(ATool::Eraser); ++t) {
        const ATool tool = ATool(t);
        Annotation ann;
        ann.tool = tool;
        ann.fill = tool == ATool::Eraser;
        ann.rect = QRect(80, 80, 180, 100);
        ann.a = ann.rect.topLeft(); ann.b = ann.rect.bottomRight();
        ann.stroke = QPolygonF{QPointF(90, 100), QPointF(240, 140)};
        ann.text = QStringLiteral("Text");
        ann.color = QColor("#ff3b30");
        if (tool == ATool::Text) fitTextAnnotation(ann);
        if (tool == ATool::Number) ann.rect.setSize(QSize(100, 100));
        overlay.m_anns = {ann};
        overlay.m_atool = tool;
        overlay.m_selAnn = -1;
        const QImage plain = render();
        const QImage output = overlay.cropped().toImage();
        overlay.m_selAnn = 0;
        const QImage active = render();
        check(tool == ATool::Pen ? active == plain : active != plain,
              "only adjustable geometric annotations retain controls with their tool armed");
        if (previews) active.save(prefix + QString::number(t) + QStringLiteral("-drawing.png"));
        overlay.m_atool = ATool::None;
        const QImage selected = render();
        check(tool == ATool::Pen ? selected == plain : selected != plain,
              "freehand pen marks never gain controls, even in object selection mode");
        check(overlay.cropped().toImage() == output, "editing borders and nodes never enter exported pixels");
        if (previews) selected.save(prefix + QString::number(t) + QStringLiteral("-editing.png"));
        const QVector<QPoint> anchors = overlay.annAnchors(ann);
        const QPoint knob = overlay.rotateKnobLocal(ann);
        const QRect box = annotationRect(ann);
        int extraPixels = 0;
        const QRect scan = box.adjusted(-8, -8, 8, 8).intersected(overlay.rect());
        for (int y = scan.top(); y <= scan.bottom(); ++y)
            for (int x = scan.left(); x <= scan.right(); ++x) {
                const QPoint point(x, y);
                bool control = false;
                for (const QPoint &anchor : anchors)
                    control |= QLineF(point, anchor).length() <= 7;
                if (tool != ATool::Mosaic && tool != ATool::Pen)
                    control |= QLineF(point, knob).length() <= 7
                        || (qAbs(x - box.center().x()) <= 2
                            && (knob.y() < box.top() ? y <= box.top() : y >= box.bottom()));
                if (!control && selected.pixelColor(qRound(x * dpr), qRound(y * dpr))
                        != plain.pixelColor(qRound(x * dpr), qRound(y * dpr))) ++extraPixels;
            }
        check(extraPixels == 0, "selection adds nodes and rotation only, never a surrounding rectangle");
        for (const QPoint &anchor : anchors)
            check(overlay.annHandleAt(ann, mapFromLocal(ann, anchor)) != H_None,
                  "smaller visual circles retain the original resize hit targets");
        if (tool != ATool::Pen && tool != ATool::Mosaic) {
            Annotation turned = ann;
            turned.rot = 30;
            for (const QPoint &anchor : overlay.annAnchors(turned))
                check(overlay.annHandleAt(turned, mapFromLocal(turned, anchor)) != H_None,
                      "contour-node hit targets follow the rotated annotation");
        }
        if (tool == ATool::Ellipse) {
            const QPointF center = QRectF(ann.rect.topLeft(), ann.rect.bottomRight()).center();
            for (const QPoint &anchor : anchors) {
                const QPointF normalized((anchor.x() - center.x()) / ((ann.rect.width() - 1) / 2.0),
                                         (anchor.y() - center.y()) / ((ann.rect.height() - 1) / 2.0));
                check(qAbs(QPointF::dotProduct(normalized, normalized) - 1) < 0.04,
                      "ellipse handles lie on the ellipse instead of its bounding-box corners");
            }
        }
        if (tool == ATool::Rect || tool == ATool::Ellipse || tool == ATool::Mosaic
            || tool == ATool::Eraser || tool == ATool::Number) {
            check(anchors.size() == (tool == ATool::Number ? 4 : 8), "area tools retain their contour resize nodes");
            static const Handle handles[] = {H_TL, H_TR, H_BL, H_BR, H_L, H_R, H_T, H_B};
            for (int i = 0; i < anchors.size(); ++i) {
                overlay.m_pressAnn = ann;
                overlay.m_itemDrag = handles[i];
                overlay.applyItemDrag(anchors[i]);
                check(overlay.m_anns[0].rect == ann.rect, "grabbing a contour node does not jump the shape");
                const QPoint delta = tool == ATool::Number
                    ? QPoint(i == 0 || i == 2 ? -20 : 20, i < 2 ? -20 : 20) : QPoint(20, 15);
                overlay.applyItemDrag(anchors[i] + delta);
                check(overlay.m_anns[0].rect != ann.rect, "each contour resize node changes the shape");
                const QPoint expected = anchors[i] + (i < 4 ? delta : i < 6 ? QPoint(20, 0) : QPoint(0, 15));
                check(QLineF(overlay.annAnchors(overlay.m_anns[0])[i], expected).length() <= 2,
                      "the dragged contour node follows the pointer");
            }
            overlay.m_anns[0] = ann;
            overlay.m_itemDrag = H_None;
        }
        overlay.m_atool = tool;
        overlay.m_spaceHeld = true;
        check(tool == ATool::Pen ? render() == plain : render() != plain,
              "space-based editing does not restore controls to a completed pen stroke");
        overlay.m_spaceHeld = false;
        if (tool == ATool::Mosaic) {
            overlay.m_anns[0].mosaicBrush = true;
            overlay.m_mosaicBrush = true;
            overlay.m_selAnn = -1;
            const QImage brush = render();
            overlay.m_selAnn = 0;
            check(render() == brush, "mosaic brush strokes never acquire editing frames");
            overlay.m_mosaicBrush = false;
        }
    }
    overlay.m_anns.clear(); overlay.m_selAnn = -1;
    overlay.selectTool(ATool::Pen);
    const auto mouse = [&](QEvent::Type type, const QPoint &point, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent event(type, point, overlay.mapToGlobal(point),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
        QApplication::sendEvent(&overlay, &event);
    };
    for (int i = 0; i < 2; ++i) {
        mouse(QEvent::MouseButtonPress, QPoint(120, 130));
        mouse(QEvent::MouseMove, QPoint(220, 170));
        mouse(QEvent::MouseButtonRelease, QPoint(220, 170));
        check(overlay.m_anns.size() == i + 1 && overlay.m_selAnn == -1,
              "a new pen stroke can start on an old stroke without selecting or moving it");
    }
    const QColor penColor = overlay.m_anns[0].color;
    const int penWidth = overlay.m_anns[0].pen;
    overlay.pickToolColor(Qt::green);
    overlay.setToolSize(penWidth + 1);
    check(overlay.m_anns[0].color == penColor && overlay.m_anns[0].pen == penWidth,
          "pen option changes only affect future strokes");
    overlay.undoAnnotation();
    check(overlay.m_anns.size() == 1 && overlay.m_selAnn == -1, "undo removes one pen stroke without selecting another");
    overlay.redoAnnotation();
    check(overlay.m_anns.size() == 2 && overlay.m_selAnn == -1, "redo restores pen ink without adjustment controls");
    overlay.selectTool(ATool::Marker);
    overlay.m_markerFill = false;
    mouse(QEvent::MouseButtonPress, QPoint(120, 240));
    mouse(QEvent::MouseMove, QPoint(180, 330));
    mouse(QEvent::MouseMove, QPoint(240, 180));
    mouse(QEvent::MouseMove, QPoint(300, 240));
    mouse(QEvent::MouseButtonRelease, QPoint(300, 240));
    const Annotation line = overlay.m_anns.last();
    check(line.a == QPoint(120, 240) && line.b == QPoint(300, 240) && line.stroke.isEmpty()
              && overlay.annAnchors(line) == QVector<QPoint>{line.a, line.b},
          "highlights use two endpoint controls and never retain freehand samples");
    check(overlay.annHandleAt(line, line.a) == H_TL && overlay.annHandleAt(line, line.b) == H_BR,
          "highlight endpoints have distinct start and end hit targets");
    overlay.updateAnnotationCursor(line.b);
    check(overlay.cursor().shape() == cursorForHandle(H_BR), "a highlight endpoint advertises its adjustment cursor");
    overlay.m_pressAnn = line;
    overlay.m_itemDrag = H_BR;
    overlay.applyItemDrag(line.b + QPoint(30, 10));
    check(overlay.m_anns.last().a == line.a && overlay.m_anns.last().b == line.b + QPoint(30, 10),
          "dragging a highlight endpoint changes only that endpoint");
    overlay.m_itemDrag = H_None;
    QImage linePixels(360, 360, QImage::Format_ARGB32);
    linePixels.fill(Qt::transparent);
    {
        QPainter painter(&linePixels);
        overlay.drawAnnotation(painter, line, QImage());
    }
    check(linePixels.pixelColor(200, 240).alpha() == 110 && linePixels.pixelColor(180, 330).alpha() == 0,
          "highlight output contains only the translucent straight segment, not the mouse trail");
    overlay.setToolSize(line.pen + 1);
    check(overlay.m_anns.last().pen == line.pen + 1, "straight highlights retain editable width");
    overlay.undoAnnotation();
    check(overlay.m_anns.last().pen == line.pen, "highlight width changes remain undoable");
    overlay.redoAnnotation();
    check(overlay.m_anns.last().pen == line.pen + 1, "highlight width changes remain redoable");
    overlay.toggleFill();
    mouse(QEvent::MouseButtonPress, QPoint(450, 240));
    mouse(QEvent::MouseMove, QPoint(550, 290));
    mouse(QEvent::MouseButtonRelease, QPoint(550, 290));
    check(overlay.m_anns.last().tool == ATool::Marker && overlay.m_anns.last().fill
              && overlay.m_anns.last().rect == QRect(QPoint(450, 240), QPoint(550, 290))
              && overlay.m_anns.last().stroke.isEmpty() && overlay.annAnchors(overlay.m_anns.last()).size() == 8,
          "rectangle highlights remain available with eight boundary nodes and no freehand path");
    overlay.toggleFill();
    overlay.m_anns.clear(); overlay.m_selAnn = -1;
    mouse(QEvent::MouseButtonPress, QPoint(120, 240));
    mouse(QEvent::MouseMove, QPoint(260, 265), Qt::ShiftModifier);
    mouse(QEvent::MouseButtonRelease, QPoint(260, 265), Qt::ShiftModifier);
    check(overlay.m_anns.last().a.y() == overlay.m_anns.last().b.y(), "Shift snaps straight highlights to 45-degree steps");
    overlay.m_selAnn = -1;
    overlay.m_atool = ATool::Pen;
    overlay.m_live = false;
    overlay.beginAnnotation(QPoint(360, 130));
    overlay.updateAnnotation(QPoint(420, 160), Qt::NoModifier);
    QMouseEvent finish(QEvent::MouseButtonPress, QPoint(420, 160), overlay.mapToGlobal(QPoint(420, 160)),
                       Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &finish);
    check(!overlay.m_live && overlay.m_selAnn == -1 && overlay.m_anns.last().tool == ATool::Pen,
          "right-click finishing also leaves completed pen ink without selection controls");
    overlay.m_anns.clear(); overlay.m_selAnn = -1;
    overlay.m_atool = ATool::None;
    overlay.hideToolBars();
    overlay.m_sizeBar->hide();
    overlay.m_inspector->hide();
    const QImage selection = render();
    for (const QRect &target : handleRects(overlay.m_sel)) {
        const QPoint center = target.center() + QPoint(1, 1);
        check(selection.pixelColor(qRound(center.x() * dpr), qRound(center.y() * dpr)) == QColor(0x5e, 0x6a, 0xd2)
                  && overlay.hitHandle(center) != H_None,
              "all eight capture resize handles are accent dots and remain draggable");
    }
    if (!failures) std::puts("PASS: contour controls without outer frames, finished pen ink, straight highlights, undo and clean exports");
    return failures;
}

int CaptureOverlay::testOutput()
{
    int failures = 0;
    const auto check = [&](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    QImage image(320, 240, QImage::Format_RGB32);
    image.fill(QColor("#243648"));
    CaptureOverlay overlay({ScreenFrame{QRect(screen.topLeft(), image.size()), QRect(0, 0, 320, 240), image}});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    int capturedCount = 0;
    const auto captureConnection = QObject::connect(&overlay, &CaptureOverlay::captured,
        [&](const QPixmap &, const QRect &) { ++capturedCount; });
    overlay.m_mode = Adjusting;
    overlay.m_sel = QRect(screen.topLeft() - overlay.pos() + QPoint(40, 40), QSize(200, 120));
    overlay.selectTool(ATool::Text);
    overlay.beginTextEdit(overlay.m_sel.topLeft() + QPoint(20, 20));
    overlay.m_editor->setPlainText(QStringLiteral("Retain this annotation"));
    overlay.commitTextEdit();
    const QImage expected = overlay.cropped().toImage();
    {
        ClipboardBlocker blocker;
        check(blocker.locked(), "ordinary-copy contention fixture acquired the lock");
        overlay.copyCapture();
        check(overlay.isVisible() && overlay.m_anns.size() == 1
                  && overlay.cropped().toImage() == expected && overlay.m_sizeTip->isVisible(),
              "failed ordinary copy keeps the selection, annotation, and failure hint");
        check(capturedCount == 0, "failed output does not enter history");
    }
    QTemporaryDir directory;
    check(directory.isValid(), "output regression has an isolated temporary directory");
    const QString protectedPath = directory.path() + QStringLiteral("/protected.png");
    QImage original(24, 18, QImage::Format_RGB32);
    original.fill(Qt::magenta);
    check(original.save(protectedPath), "create an existing image for replacement-failure regression");
    HANDLE heldFile = CreateFileW(reinterpret_cast<LPCWSTR>(protectedPath.utf16()), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    check(heldFile != INVALID_HANDLE_VALUE, "hold the destination without delete-sharing");
    if (heldFile != INVALID_HANDLE_VALUE) {
        check(!overlay.saveCaptureTo(protectedPath) && overlay.isVisible() && QImage(protectedPath) == original,
              "failed replacement preserves both the original file and the annotated capture");
        CloseHandle(heldFile);
        check(overlay.saveCaptureTo(protectedPath) && QImage(protectedPath) == expected,
              "retrying the same destination succeeds after the replacement lock is released");
        check(capturedCount == 1, "successful output reports one history capture");
    }
    overlay.show();
    for (int action : {Qt::Key_Delete, Qt::Key_Z, Qt::Key_Y, Qt::Key_Escape}) {
        Annotation ink;
        ink.tool = ATool::Rect;
        ink.color = Qt::red;
        ink.fill = true;
        ink.rect = QRect(overlay.m_sel.topLeft() + QPoint(20, 20), QSize(80, 50));
        overlay.m_anns = {ink};
        overlay.m_selAnn = 0;
        overlay.m_atool = ATool::None;
        overlay.m_itemDrag = H_None;
        overlay.m_dragStarted = false;
        overlay.m_undoStack = {overlay.m_anns};
        overlay.m_redoStack = {overlay.m_anns};
        overlay.placeToolBars();
        overlay.showToolBars();
        const QPoint pressPoint(ink.rect.left() + ink.rect.width() / 4, ink.rect.top());
        QMouseEvent press(QEvent::MouseButtonPress, pressPoint, overlay.mapToGlobal(pressPoint),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &press);
        check(overlay.m_itemDrag == H_Move, "pressing an existing annotation enters object drag");
        if (action != Qt::Key_Y) {
            const QPoint next = pressPoint + QPoint(20, 10);
            QMouseEvent drag(QEvent::MouseMove, next, overlay.mapToGlobal(next),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&overlay, &drag);
            check(overlay.m_dragStarted && overlay.m_tools->isVisible(), "active drag keeps selected-object options available before interruption");
        }
        QKeyEvent interrupt(QEvent::KeyPress, action,
            action == Qt::Key_Z || action == Qt::Key_Y ? Qt::ControlModifier : Qt::NoModifier);
        QApplication::sendEvent(&overlay, &interrupt);
        check(overlay.m_itemDrag == H_None && !overlay.m_dragStarted,
              "keyboard changes to selection or annotations terminate an active item drag");
        if (overlay.m_itemDrag == H_None) {
            const auto before = overlay.m_anns;
            QMouseEvent move(QEvent::MouseMove, overlay.m_sel.center(), overlay.mapToGlobal(overlay.m_sel.center()),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&overlay, &move);
            check(overlay.m_anns.size() == before.size(), "mouse movement after interrupted drag is safe");
            QMouseEvent release(QEvent::MouseButtonRelease, overlay.m_sel.center(), overlay.mapToGlobal(overlay.m_sel.center()),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&overlay, &release);
            check(overlay.m_tools->isVisible(), "interrupted drag restores the toolbar and safely accepts release");
        }
    }
    overlay.m_itemDrag = H_None;
    overlay.m_dragStarted = false;
    overlay.m_itemDrag = H_Move;
    overlay.m_dragStarted = true;
    overlay.selectTool(ATool::Rect);
    check(overlay.m_itemDrag == H_None && !overlay.m_dragStarted,
          "switching tools terminates an active object drag");
    overlay.m_anns.clear();
    overlay.m_undoStack.clear();
    overlay.m_redoStack.clear();
    overlay.m_selAnn = -1;
    overlay.selectTool(ATool::Text);
    overlay.beginTextEdit(overlay.m_sel.topLeft() + QPoint(20, 20));
    overlay.m_editor->setPlainText(QStringLiteral("Retain this annotation"));
    overlay.commitTextEdit();
    const QRect validSelection = overlay.m_sel;
    overlay.m_sel = {};
    check(overlay.cropped().isNull(), "empty selection cannot export the whole desktop");
    const QString emptyPath = directory.path() + QStringLiteral("/empty.png");
    check(!overlay.saveCaptureTo(emptyPath) && !QFileInfo::exists(emptyPath),
          "saving an empty selection cannot write a desktop image");
    overlay.show();
    bool emittedEmpty = false;
    const auto connection = QObject::connect(&overlay, &CaptureOverlay::grabbed, &overlay,
        [&](const QPixmap &, const QRect &) { emittedEmpty = true; });
    overlay.pinCapture();
    check(!emittedEmpty && overlay.isVisible(), "empty pin export preserves the capture without creating a pin");
    QObject::disconnect(connection);
    QObject::disconnect(captureConnection);
    overlay.show();
    overlay.m_sel = validSelection;
    overlay.m_sizeBar->hide();
    overlay.m_inspector->setSample(overlay.m_frames.first(), overlay.m_frames.first().pixelRect.center());
    overlay.m_inspector->show();
    QKeyEvent copyColor(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier);
    {
        ClipboardBlocker blocker;
        check(blocker.locked(), "color-copy contention fixture acquired the lock");
        const DWORD sequence = GetClipboardSequenceNumber();
        QApplication::sendEvent(&overlay, &copyColor);
        check(overlay.isVisible() && overlay.m_sizeTip->isVisible() && GetClipboardSequenceNumber() == sequence,
              "failed color copy provides feedback and preserves the clipboard and capture");
    }
    QApplication::sendEvent(&overlay, &copyColor);
    check(QGuiApplication::clipboard()->text() == overlay.m_inspector->colorText() && !overlay.m_sizeTip->isVisible(),
          "color copy succeeds after clipboard contention ends");
    overlay.m_inspector->hide();
    check(!overlay.saveCaptureTo(directory.path() + QStringLiteral("/missing/capture.png"))
              && overlay.isVisible() && overlay.cropped().toImage() == expected && overlay.m_anns.size() == 1,
          "failed image save keeps the same annotated capture available for retry");
    const QString path = directory.path() + QStringLiteral("/capture.png");
    check(overlay.saveCaptureTo(path) && !overlay.isVisible() && QImage(path) == expected,
          "successful save closes the capture only after every output pixel was written");
    overlay.show();
    overlay.copyCapture();
    const QImage copied = QGuiApplication::clipboard()->image();
    if (copied != expected || overlay.isVisible())
        std::fprintf(stderr, "ordinary copy visible=%d expected=%dx%d format=%d actual=%dx%d format=%d owns=%d\n",
                     int(overlay.isVisible()), expected.width(), expected.height(), int(expected.format()),
                     copied.width(), copied.height(), int(copied.format()), int(QGuiApplication::clipboard()->ownsClipboard()));
    check(!overlay.isVisible() && QGuiApplication::clipboard()->image() == expected,
          "ordinary copy succeeds after contention ends and retains the original pixels");
    if (!failures) std::puts("PASS: failed output preserves captures and annotations; save/copy retries succeed");
    return failures ? 1 : 0;
}
