#include "capture_overlay.h"
#include <QClipboard>
#include <QWindow>
#include "output.h"
#include "screen_capture.h"
#include <QAbstractItemView>
#include "test_support.h"
#include "widgets.h"
#include "pin_window.h"
#include "long_capture.h"
#include "ocr_dialog.h"
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
    failures += testCustomSize();
    overlay.resetSelection();
    check(overlay.m_sel.isEmpty() && overlay.m_mode==Selecting,"clearing the selection still permits a new capture");
    overlay.close();
    QCursor::setPos(savedCursor);
    failures += testSelectionBorders();
    if (!failures) std::printf("PASS: eight-direction outside adjustment, cursor/region keys, screen toggle and crop output\n");
    return failures ? 1 : 0;
}

int CaptureOverlay::testCustomSize()
{
    int failures = 0;
    const auto check = [&failures](bool ok, const char *message) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
    };
    QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
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
    check(overlay.m_sizeTip->text() == QStringLiteral("240 × 120 px"),
          "the size label converts logical pixels to the selected px unit");
    overlay.m_sizeUnit = SizeUnit::Dp;
    overlay.updateTip();
    check(overlay.m_sizeTip->text() == QStringLiteral("120 × 60 dp"),
          "the size label switches to logical dp without moving the region");

    overlay.m_sizeUnit = SizeUnit::Px;
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
    auto *unit = overlay.findChild<QComboBox *>(QStringLiteral("customUnit"));
    auto *lock = overlay.findChild<QCheckBox *>(QStringLiteral("customLockAspect"));
    auto *preset = overlay.findChild<QComboBox *>(QStringLiteral("customPreset"));
    auto *save = overlay.findChild<QPushButton *>(QStringLiteral("customPresetSave"));
    auto *popup = overlay.m_customSizePopup.data();
    check(popup && x && y && width && height && unit && lock && preset && save
              && x->value() == 200 && y->value() == 200
              && width->value() == 240 && height->value() == 120
              && unit->currentText() == QStringLiteral("px"),
          "clicking the size label opens an independent panel with live position and px size");
    if (popup && QDir(QStringLiteral("debug/publish-checks")).exists()) {
        popup->grab().save(QStringLiteral("debug/publish-checks/custom-size-panel.png"));
        overlay.m_sizeTip->grab().save(QStringLiteral("debug/publish-checks/custom-size-label.png"));
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
              && !flags.testFlag(Qt::FramelessWindowHint)
              && popup->windowTitle() == QStringLiteral("自定义截图"),
          "the editor is a standalone titled window rather than an anchored frameless popup");
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

    unit->setCurrentIndex(1);
    QApplication::processEvents();
    check(unit->currentText() == QStringLiteral("dp") && x->value() == 150 && y->value() == 100
              && width->value() == 200 && height->value() == 80,
          "switching the unit converts both live position and size fields");
    width->setValue(300);
    QApplication::processEvents();
    check(overlay.m_sel.size() == QSize(300, 120) && height->value() == 120
              && popup->pos() == panelPos,
          "live dp size edits remain locked and keep the panel in place");

    overlay.saveSizePreset(QStringLiteral("测试预设"));
    check(overlay.m_sizePresets.size() == 1 && preset->currentText() == QStringLiteral("测试预设")
              && settings.value(QStringLiteral("Capture/presets")).toStringList().size() == 1,
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
    check(preset->currentIndex() == 0 && overlay.m_sel.size() == QSize(200, 150),
          "editing after a preset returns the preset selector to the custom entry");
    const int savedIndex = preset->findText(QStringLiteral("测试预设"));
    check(savedIndex > 0, "saved presets remain in the preset list");
    preset->setCurrentIndex(savedIndex);
    QApplication::processEvents();
    check(overlay.m_sel.topLeft() == origin + QPoint(150, 100)
              && overlay.m_sel.size() == QSize(300, 120)
              && x->value() == 150 && width->value() == 300 && popup->pos() == panelPos,
          "selecting a saved preset updates the selection and fields without moving the panel");

    popup->close();
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
        check(dot.pixelColor(qRound(20 * dpr), qRound(20 * dpr)) == QColor(0x20, 0x80, 0xf0)
                  && dot.pixelColor(qRound(16 * dpr), qRound(16 * dpr)).alpha() == 0
                  && dot.pixelColor(qRound(26 * dpr), qRound(20 * dpr)).alpha() == 0,
              "handles are theme-blue dots with a light ring and transparent corners at every DPR");
    }
    QVector<ScreenFrame> frames = freezeScreens();
    check(!frames.isEmpty(), "border fixture captures screen geometry");
    if (frames.isEmpty()) return failures;
    for (ScreenFrame &frame : frames) frame.image.fill(QColor(120, 130, 140));
    CaptureOverlay overlay(frames, {}, DetectionOptions(), false);
    overlay.m_mode = Adjusting;
    overlay.m_sel = overlay.rect().adjusted(20, 20, -20, -20);
    overlay.m_hover = {};
    overlay.m_inspector->hide();
    overlay.m_sizeTip->hide();
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
        ann.tool = tool == ATool::Eraser ? ATool::Rect : tool;
        ann.rect = QRect(80, 80, 180, 100);
        ann.a = ann.rect.topLeft(); ann.b = ann.rect.bottomRight();
        ann.stroke = QPolygonF{QPointF(90, 100), QPointF(240, 140)};
        ann.text = QStringLiteral("Text");
        ann.color = QColor("#ff3b30");
        if (tool == ATool::Text) fitTextAnnotation(ann);
        overlay.m_anns = {ann};
        overlay.m_atool = tool;
        overlay.m_selAnn = -1;
        const QImage plain = render();
        const QImage output = overlay.cropped().toImage();
        overlay.m_selAnn = 0;
        const QImage active = render();
        check(active != plain, "the selected item keeps its controls while its drawing tool stays armed");
        if (previews) active.save(prefix + QString::number(t) + QStringLiteral("-drawing.png"));
        overlay.m_atool = ATool::None;
        const QImage selected = render();
        check(selected != plain, "explicit object selection reveals editing controls for every annotation tool");
        check(overlay.cropped().toImage() == output, "editing borders and nodes never enter exported pixels");
        if (previews) selected.save(prefix + QString::number(t) + QStringLiteral("-editing.png"));
        if (tool == ATool::Arrow) {
            const QPoint probe = annotationRect(ann).topLeft() + QPoint(40, -3);
            check(selected.pixelColor(qRound(probe.x() * dpr), qRound(probe.y() * dpr))
                      == plain.pixelColor(qRound(probe.x() * dpr), qRound(probe.y() * dpr)),
                  "arrow editing has endpoint nodes without an unnecessary rectangular border");
        }
        for (const QPoint &anchor : overlay.annAnchors(ann))
            check(overlay.annHandleAt(ann, mapFromLocal(ann, anchor)) != H_None,
                  "smaller visual circles retain the original resize hit targets");
        overlay.m_atool = tool;
        overlay.m_spaceHeld = true;
        check(render() != plain, "space-based object editing retains its editing feedback");
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
    const QImage selection = render();
    for (const QRect &target : handleRects(overlay.m_sel)) {
        const QPoint center = target.center() + QPoint(1, 1);
        check(selection.pixelColor(qRound(center.x() * dpr), qRound(center.y() * dpr)) == QColor(0x20, 0x80, 0xf0)
                  && overlay.hitHandle(center) != H_None,
              "all eight capture resize handles are theme-blue dots and remain draggable");
    }
    if (!failures) std::puts("PASS: per-tool editing borders, circular nodes, hit targets and clean exports");
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
        overlay.placeToolbar();
        overlay.showToolBars();
        const QPoint pressPoint(ink.rect.center().x(), ink.rect.top());
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
    overlay.show();
    overlay.m_sel = validSelection;
    overlay.m_sizeTip->hide();
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
