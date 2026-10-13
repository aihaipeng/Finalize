#include "capture_overlay.h"
#include <QWindow>
#include "output.h"
#include "test_support.h"
#include "widgets.h"
#include "pin_window.h"
#include "long_capture.h"
#include "ocr_dialog.h"
#include <QGuiApplication>
#include <QImage>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QIcon>
#include <QColor>
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QtMath>
#include <QToolButton>
#include <QLineEdit>
#include <QClipboard>
#include <QCursor>
#include <QTimer>
#include <QFontMetrics>
#include <QMimeData>
#include <QEventLoop>
#include <qt_windows.h>
#include <cstdio>
#include <cstdlib>

int CaptureOverlay::testColorPicker(const QString &previewPath)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    auto *saved = new QMimeData;
    if (const QMimeData *data = clipboard->mimeData())
        for (const QString &format : data->formats()) saved->setData(format, data->data(format));
    auto check = [clipboard, saved](bool ok, const char *message) {
        if (!ok) {
            clipboard->setMimeData(saved);
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    };
    QWidget calibration(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    calibration.setGeometry(QRect(QGuiApplication::primaryScreen()->geometry().center(), QSize(64, 64)));
    calibration.setStyleSheet("background:#010409;");
    calibration.show();
    QEventLoop settle;
    QTimer::singleShot(200, &settle, &QEventLoop::quit);
    settle.exec();
    const QVector<ScreenFrame> liveFrames = freezeScreens();
    POINT native{32, 32};
    check(ClientToScreen(reinterpret_cast<HWND>(calibration.winId()), &native), "native calibration coordinates");
    PixelInspector probe(nullptr);
    bool sampled = false;
    for (const ScreenFrame &live : liveFrames)
        if (probe.setSample(live, QPoint(native.x, native.y))) sampled = true;
    check(sampled && probe.colorText() == QStringLiteral("1, 4, 9"), "live desktop capture preserves native calibration pixels");
    {
        CaptureOverlay liveOverlay(liveFrames);
        liveOverlay.setAttribute(Qt::WA_DeleteOnClose, false);
        RECT actual{};
        GetWindowRect(reinterpret_cast<HWND>(liveOverlay.winId()), &actual);
        QRect desktop;
        for (const GdiDisplay &display : gdiDisplays()) desktop |= display.native;
        check(QRect(actual.left, actual.top, actual.right - actual.left, actual.bottom - actual.top) == desktop,
              "the visible capture overlay covers the entire native desktop at every scale");
        const QRect before = liveOverlay.geometry();
        const QImage frozen = liveOverlay.m_shot.toImage();
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        check(liveOverlay.geometry() == before && liveOverlay.m_shot.toImage() == frozen,
              "processing window events cannot move the overlay relative to the frozen pixels");
        for (const ScreenFrame &captured : liveFrames) {
            const QPointF local = liveOverlay.windowHandle()->mapFromGlobal(QPointF(captured.logicalRect.topLeft()));
            const qreal ratio = liveOverlay.m_shot.devicePixelRatioF();
            const QPoint pixel(qRound(local.x() * ratio), qRound(local.y() * ratio));
            check(liveOverlay.rect().contains(local.toPoint()) && pixel.x() >= 0 && pixel.y() >= 0,
                   "every captured screen starts inside the overlay, including negative-origin screens");
        }
        const QPoint savedCursor = QCursor::pos();
        const QPoint center = calibration.geometry().center();
        auto moveCursor = [&check](const QPoint &global) {
            DWORD foreground = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
            check(foreground == GetCurrentProcessId(), "native fixture input requires this process to own the foreground");
            QCursor::setPos(global);
        };
        auto mouse = [&liveOverlay, &moveCursor](QEvent::Type type, const QPoint &global) {
            moveCursor(global);
            const QPoint local = liveOverlay.mapFromGlobal(global);
            QMouseEvent event(type, local, global,
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(&liveOverlay, &event);
        };
        moveCursor(center);
        const QPoint local = liveOverlay.mapFromGlobal(center);
        liveOverlay.m_sel = QRect(local - QPoint(60, 40), QSize(180, 120));
        liveOverlay.m_mode = Adjusting;
        liveOverlay.updateInspector();
        check(liveOverlay.m_inspector->isVisible() && liveOverlay.m_inspector->colorText() == QStringLiteral("1, 4, 9"),
              "a locked selection immediately permits sampling the frozen native pixel");
        QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier);
        QApplication::sendEvent(&liveOverlay, &copy);
        check(clipboard->text() == QStringLiteral("1, 4, 9") && liveOverlay.isVisible(),
              "C copies a color after selection without copying or closing the image");
        mouse(QEvent::MouseButtonPress, center);
        check(!liveOverlay.m_inspector->isVisible(), "moving the entire region hides the card on press");
        mouse(QEvent::MouseMove, center + QPoint(20, 15));
        check(!liveOverlay.m_inspector->isVisible(), "a held region move keeps the card hidden, including while stationary");
        mouse(QEvent::MouseButtonRelease, center + QPoint(20, 15));
        check(liveOverlay.m_inspector->isVisible(), "region release restores the card without another mouse move");
        const QPoint corner = liveOverlay.mapToGlobal(liveOverlay.m_sel.bottomRight());
        mouse(QEvent::MouseButtonPress, corner);
        mouse(QEvent::MouseMove, corner + QPoint(30, 20));
        check(liveOverlay.m_inspector->isVisible(), "resizing keeps the magnifier available for pixel precision");
        mouse(QEvent::MouseButtonRelease, corner + QPoint(30, 20));
        check(liveOverlay.m_inspector->isVisible(), "resize release leaves the card visible");
        moveCursor(center + QPoint(400, 300));
        liveOverlay.updateInspector();
        check(!liveOverlay.m_inspector->isVisible(), "idle sampling outside the locked selection is hidden");
        moveCursor(center + QPoint(20, 15));
        liveOverlay.selectTool(ATool::Rect);
        check(!liveOverlay.m_inspector->isVisible(), "arming an annotation tool suppresses automatic sampling");
        moveCursor(center + QPoint(20, 15));
        check(liveOverlay.m_sel.contains(liveOverlay.mapFromGlobal(QCursor::pos())), "tool restoration starts with the cursor inside the region");
        liveOverlay.setToolNone();
        if (!liveOverlay.m_inspector->isVisible())
            std::fprintf(stderr, "inspector restore: cursor=%d,%d local=%d,%d selection=%d,%d,%d,%d drag=%d item=%d tool=%d selected=%d visible=%d\n",
                         QCursor::pos().x(), QCursor::pos().y(), liveOverlay.mapFromGlobal(QCursor::pos()).x(), liveOverlay.mapFromGlobal(QCursor::pos()).y(),
                         liveOverlay.m_sel.x(), liveOverlay.m_sel.y(), liveOverlay.m_sel.width(), liveOverlay.m_sel.height(),
                         int(liveOverlay.m_drag), int(liveOverlay.m_itemDrag), int(liveOverlay.m_atool), liveOverlay.m_selAnn, int(liveOverlay.isVisible()));
        check(liveOverlay.m_inspector->isVisible(), "leaving the annotation tool restores automatic sampling");
        check(liveOverlay.m_shot.toImage() == frozen, "sampling and region adjustments never change frozen pixels");
        moveCursor(savedCursor);
        liveOverlay.close();
    }
    calibration.hide();
    QImage image(240, 180, QImage::Format_RGB32);
    image.fill(QColor(1, 4, 9));
    image.setPixelColor(110, 83, QColor(90, 150, 210));
    ScreenFrame frame{QRect(QGuiApplication::primaryScreen()->geometry().topLeft(), QSize(160, 120)),
                      QRect(-320, -240, 240, 180), image};
    CaptureOverlay overlay({frame});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    PixelInspector *inspector = overlay.m_inspector;
    const QPoint sample = frame.pixelRect.topLeft() + QPoint(120, 90);
    check(inspector->setSample(frame, sample), "native pixel sample with negative screen origin");
    inspector->show();
    check(inspector->colorText() == QStringLiteral("1, 4, 9"), "RGB channel order and formatting");
    for (const QSize logical : {QSize(240, 180), QSize(160, 120), QSize(120, 90)}) {
        frame.logicalRect.setSize(logical);
        check(inspector->setSample(frame, sample) && inspector->colorText() == QStringLiteral("1, 4, 9"),
              "100/150/200 percent scaling must not resample native colors");
    }
    QImage panel(inspector->size(), QImage::Format_RGB32);
    inspector->render(&panel);
    const int left = (inspector->width() - 177) / 2;
    check(inspector->size() == QSize(179, 217), "the compact card uses a 25 by 17 sample at sevenfold magnification");
    check(panel.pixelColor(left + 88, 61) == QColor(1, 4, 9), "highlighted pixel retains its raw color");
    check(panel.pixelColor(left + 15, 9) == QColor(90, 150, 210), "nearest-neighbor magnifier preserves neighboring pixels");
    QImage translucent(inspector->size(), QImage::Format_ARGB32_Premultiplied);
    translucent.fill(Qt::transparent);
    inspector->render(&translucent, QPoint(), QRegion(), QWidget::DrawChildren);
    check(translucent.pixelColor(0, 170) == QColor(0, 0, 0, 188), "the dark surface has alpha 188 rather than fading text and pixels");
    check(translucent.pixelColor(left + 88, 61) == QColor(1, 4, 9), "the center color remains opaque on the translucent surface");
    check(inspector->setSample(frame, sample, QRectF(0, 0, 121, 91)), "a magnifier may preview the selection boundary");
    inspector->render(&panel);
    check(panel.pixelColor(left + 165, 100) != QColor(1, 4, 9)
              && inspector->colorText() == QStringLiteral("1, 4, 9"),
          "the magnified mask is visual only and never replaces the raw copied color");
    check(inspector->setSample(frame, sample), "restore the unmasked sample preview");
    inspector->render(&panel);
    if (!previewPath.isEmpty()) check(panel.save(previewPath), "save magnifier preview");
    const QImage frozen = overlay.m_shot.toImage();
    check(overlay.m_shot.devicePixelRatio() == 1.5, "frozen desktop preserves high-DPI capture resolution");
    const QPointF localOrigin = overlay.mapFromGlobal(QPointF(frame.logicalRect.topLeft()));
    const QPoint origin(qRound(localOrigin.x() * 1.5), qRound(localOrigin.y() * 1.5));
    check(frozen.pixelColor(origin + QPoint(110, 83)) == QColor(90, 150, 210), "frozen capture retains native pixel detail");
    QKeyEvent copyRgb(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &copyRgb);
    check(clipboard->text() == QStringLiteral("1, 4, 9") && overlay.isVisible(), "C copies RGB without closing capture");
    QKeyEvent shift(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
    QApplication::sendEvent(&overlay, &shift);
    check(inspector->colorText() == QStringLiteral("#010409"), "Shift switches to six-digit HEX");
    QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, QString(), true);
    QApplication::sendEvent(&overlay, &repeat);
    check(inspector->colorText() == QStringLiteral("#010409"), "holding Shift must not repeatedly toggle formats");
    QApplication::sendEvent(&overlay, &copyRgb);
    check(clipboard->text() == QStringLiteral("#010409"), "C copies the displayed HEX format");
    QApplication::sendEvent(&overlay, &shift);
    check(inspector->colorText() == QStringLiteral("1, 4, 9"), "Shift switches back to RGB");
    for (const QPoint pixel : {QPoint(0, 0), QPoint(239, 0), QPoint(0, 179), QPoint(239, 179)})
        check(inspector->setSample(frame, frame.pixelRect.topLeft() + pixel), "screen-edge samples remain valid");
    check(!inspector->setSample(frame, frame.pixelRect.topLeft() - QPoint(1, 0)), "outside-screen sample is rejected");
    check(!inspector->setSample(ScreenFrame{}, sample), "empty frame is rejected");
    ScreenFrame mismatch = frame;
    mismatch.pixelRect.setWidth(241);
    check(!inspector->setSample(mismatch, sample), "unexpected native size is rejected");
    const QRect bounds(-640, -100, 640, 480);
    for (const QPoint cursor : {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()}) {
        const QRect positioned(inspectorPosition(cursor, inspector->size(), bounds), inspector->size());
        check(bounds.contains(positioned) && !positioned.contains(cursor), "panel stays on screen and avoids the cursor");
    }
    check(overlay.m_shot.toImage() == frozen, "magnifier and format changes must not alter capture pixels");
    // Magnetic edge snapping while creating, moving, and resizing a selection.
    const QRect screenGeo = QGuiApplication::primaryScreen()->geometry();
    const QPoint screenOrigin = screenGeo.topLeft() - overlay.geometry().topLeft();
    const auto mouseMove = [&overlay](const QPoint &local) {
        QMouseEvent move(QEvent::MouseMove, local, local + overlay.geometry().topLeft(),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &move);
    };
    overlay.m_pressPos = screenOrigin + QPoint(40, 40);
    overlay.m_drag = H_New;
    overlay.m_mode = Selecting;
    mouseMove(screenOrigin + QPoint(screenGeo.width() - 6, 120));
    check(overlay.m_sel.right() + 1 == screenOrigin.x() + screenGeo.width() && overlay.m_snapX == screenOrigin.x() + screenGeo.width(),
          "new selection snaps its right edge to the screen border");
    overlay.m_sel = QRect(screenOrigin + QPoint(20, 20), QSize(60, 40));
    overlay.m_pressSel = overlay.m_sel;          // H_Move diffs against the press snapshot.
    overlay.m_pressPos = screenOrigin + QPoint(50, 40);
    overlay.m_drag = H_Move;
    overlay.m_mode = Adjusting;
    mouseMove(screenOrigin + QPoint(38, 40));   // Lands 8px from the screen border.
    check(overlay.m_sel.left() == screenOrigin.x() && overlay.m_snapX == screenOrigin.x(),
          "moving the selection snaps its left edge to the screen border");
    check(overlay.m_sel.size() == QSize(60, 40), "edge snapping preserves the selection size while moving");
    overlay.m_pressSel = overlay.m_sel;
    overlay.m_pressPos = screenOrigin + QPoint(38, 40);
    overlay.m_drag = H_R;
    mouseMove(screenOrigin + QPoint(screenGeo.width() - 3, 40));
    check(overlay.m_sel.right() + 1 == screenOrigin.x() + screenGeo.width(),
          "resizing snaps the dragged edge to the screen border");
    mouseMove(screenOrigin + QPoint(200, 40));
    check(overlay.m_sel.right() + 1 == screenOrigin.x() + 201 && overlay.m_snapX < 0,
          "no snap indicator appears away from anchor lines");
    // A fresh capture selects a detected window; dragging overrides that window hit.
    overlay.resetSelection();
    overlay.m_detectionOptions.elements = true;
    overlay.m_windowRects = { DetectedWindow{QRect(screenOrigin + QPoint(120, 60), QSize(80, 50)),
                                             {QRect(screenOrigin + QPoint(140, 70), QSize(30, 20))}} };
    const auto mouseClick = [&overlay](const QPoint &local) {
        QMouseEvent press(QEvent::MouseButtonPress, local, local + overlay.geometry().topLeft(),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, local + overlay.geometry().topLeft(),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &press);
        QApplication::sendEvent(&overlay, &release);
    };
    mouseClick(screenOrigin + QPoint(150, 80));
    check(overlay.m_sel == overlay.m_windowRects.first().nativeChildren.first() && overlay.m_mode == Adjusting,
          "clicking inside a detected child area selects the finer child rect");
    overlay.m_mode = Selecting;                  // Fresh state: a drag overrides the window hit.
    QMouseEvent dragPress(QEvent::MouseButtonPress, screenOrigin + QPoint(150, 80),
                          screenOrigin + QPoint(150, 80) + overlay.geometry().topLeft(),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &dragPress);
    mouseMove(screenOrigin + QPoint(165, 95));   // Drag before release overrides the click.
    QMouseEvent dragRelease(QEvent::MouseButtonRelease, screenOrigin + QPoint(165, 95),
                            screenOrigin + QPoint(165, 95) + overlay.geometry().topLeft(),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &dragRelease);
    check(overlay.m_sel == QRect(screenOrigin + QPoint(150, 80), QSize(16, 16)),
          "dragging from inside a window starts a new selection");
    overlay.m_sel = QRect(screenOrigin + QPoint(60, 60), QSize(40, 50));
    overlay.m_pressSel = overlay.m_sel;
    overlay.m_pressPos = screenOrigin + QPoint(100, 70);
    overlay.m_drag = H_R;
    overlay.m_mode = Adjusting;
    mouseMove(screenOrigin + QPoint(196, 70));   // 4px short of the window's right edge (200).
    check(overlay.m_sel.right() + 1 == screenOrigin.x() + 200,
          "resizing snaps the dragged edge to a window edge");
    // Hover: the mask hole follows the detected window before any selection exists.
    overlay.m_sel = QRect();
    overlay.m_mode = Selecting;
    overlay.m_drag = H_None;
    mouseMove(screenOrigin + QPoint(150, 80));   // Inside the child rect.
    check(overlay.m_hover == overlay.m_windowRects.first().nativeChildren.first() && overlay.m_sizeTip->isVisible(),
          "hovering a child area highlights the finer child rect and shows its size");
    mouseMove(screenOrigin + QPoint(125, 100));  // Inside the window, outside any child.
    check(overlay.m_hover == overlay.m_windowRects.first().native,
          "hover falls back to the whole window outside child areas");
    mouseMove(screenOrigin + QPoint(450, 300));  // Empty area away from the window.
    check(overlay.m_hover.isNull() && !overlay.m_sizeTip->isVisible(),
          "hover clears outside detected windows");
    // A committed selection stays put: hover re-detection waits until the selection is
    // cleared, so the highlight never chases the cursor after a click locks a target.
    overlay.m_sel = QRect(screenOrigin + QPoint(300, 200), QSize(50, 40));
    overlay.m_mode = Adjusting;
    overlay.m_drag = H_None;
    mouseMove(screenOrigin + QPoint(150, 80));   // Over the child, outside the selection.
    check(overlay.m_hover.isNull() && overlay.m_sel == QRect(screenOrigin + QPoint(300, 200), QSize(50, 40)),
          "a locked selection does not chase the cursor across detected areas");
    mouseMove(screenOrigin + QPoint(320, 210));  // Back inside the selection.
    check(overlay.m_hover.isNull(), "hover re-detection stays off inside the locked selection too");
    // Both rows stay horizontal and move above the selection when the screen bottom is full.
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry()
                           .translated(-overlay.geometry().topLeft());
    overlay.m_sel = QRect(avail.topLeft() + QPoint(60, 60), QSize(640, 120));
    overlay.m_mode = Adjusting;
    overlay.placeToolbar();
    check(overlay.m_tools->y() >= overlay.m_sel.bottom() + 1
              && overlay.m_tools->x() + overlay.m_tools->width() == overlay.m_sel.right() + 1
              && overlay.m_tools->width() > overlay.m_tools->height(),
          "toolbar docks below the selection, flush with its right edge");
    overlay.m_sel = QRect(avail.left() + 40, avail.bottom() + 1 - 150, 300, 150);
    overlay.placeToolbar();
    check(overlay.m_tools->y() + overlay.m_tools->height() <= overlay.m_sel.top()
              && overlay.m_tools->width() > overlay.m_tools->height(),
          "the horizontal toolbar moves above the selection at the screen bottom");
    // Annotation editing: shortcuts, shift constraints, selection, re-tune, delete,
    // undo/redo, marker, the text editor, sequence numbers, blur, and the eraser.
    auto mouse = [&overlay](QEvent::Type type, const QPoint &local,
                            Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent ev(type, local, local + overlay.geometry().topLeft(), Qt::LeftButton,
                       type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                       mods);
        QApplication::sendEvent(&overlay, &ev);
    };
    const QPoint annOrigin = screenOrigin + QPoint(20, 20);
    overlay.m_sel = QRect(annOrigin, QSize(110, 80));
    overlay.m_mode = Adjusting;
    QKeyEvent keyR(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &keyR);
    check(overlay.m_atool == ATool::Rect, "single-letter shortcuts arm annotation tools");
    const QPoint squareA = annOrigin + QPoint(10, 10);
    mouse(QEvent::MouseButtonPress, squareA);
    mouse(QEvent::MouseMove, squareA + QPoint(40, 20), Qt::ShiftModifier);
    mouse(QEvent::MouseButtonRelease, squareA + QPoint(40, 20), Qt::ShiftModifier);
    check(overlay.m_anns.size() == 1 && overlay.m_anns[0].rect.size() == QSize(41, 41),
          "shift constrains rectangles to squares");
    overlay.setToolNone();
    const QPoint edge(overlay.m_anns[0].rect.center().x(), overlay.m_anns[0].rect.top());
    mouse(QEvent::MouseButtonPress, edge);
    check(overlay.m_selAnn == 0 && overlay.m_itemDrag == H_Move,
          "clicking an annotation selects it");
    mouse(QEvent::MouseMove, edge + QPoint(12, 0));
    mouse(QEvent::MouseButtonRelease, edge + QPoint(12, 0));
    check(overlay.m_anns[0].rect == QRect(squareA + QPoint(12, 0), QSize(41, 41)),
          "dragging moves the selected annotation");
    const QPoint corner = overlay.m_anns[0].rect.bottomRight();
    mouse(QEvent::MouseButtonPress, corner);
    mouse(QEvent::MouseMove, corner + QPoint(25, 5));
    mouse(QEvent::MouseButtonRelease, corner + QPoint(25, 5));
    check(overlay.m_anns[0].rect.size() == QSize(66, 46),
          "corner handles resize the selected annotation");
    const QPoint wheelAt = overlay.m_sel.center();
    QWheelEvent tune(QPointF(wheelAt), QPointF(wheelAt + overlay.geometry().topLeft()),
                     QPoint(0, 8), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                     Qt::ScrollUpdate, false);
    QApplication::sendEvent(&overlay, &tune);
    check(overlay.m_anns[0].pen == 4, "the wheel retunes the selected annotation");
    const QColor originalShapeColor = overlay.m_anns[0].color;
    const int beforeColorUndo = overlay.m_undoStack.size();
    overlay.pickToolColor(ATool::Rect, QColor("#1c1c1e"));
    check(overlay.m_anns[0].color == originalShapeColor
              && overlay.m_undoStack.size() == beforeColorUndo
              && overlay.m_colorShape == QColor("#1c1c1e"),
          "color selection updates the tool default without recoloring a committed selection");
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &del);
    check(overlay.m_anns.isEmpty(), "delete removes the selected annotation");
    overlay.undoAnnotation();
    check(overlay.m_anns.size() == 1, "undo restores the deleted annotation");
    overlay.redoAnnotation();
    check(overlay.m_anns.isEmpty(), "redo replays the deletion");
    overlay.selectTool(ATool::Marker);
    const QPoint strokeAt = annOrigin + QPoint(12, 40);
    mouse(QEvent::MouseButtonPress, strokeAt);
    mouse(QEvent::MouseMove, strokeAt + QPoint(15, 5));
    mouse(QEvent::MouseMove, strokeAt + QPoint(18, 18));
    mouse(QEvent::MouseButtonRelease, strokeAt + QPoint(18, 18));
    check(overlay.m_anns.size() == 1 && overlay.m_anns[0].tool == ATool::Marker
              && overlay.m_anns[0].stroke.size() >= 3,
          "the marker commits a decimated translucent stroke");
    const QImage beforeText = overlay.cropped().toImage();
    overlay.selectTool(ATool::Text);
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(60, 20));
    check(overlay.editorActive(), "the text tool opens an inline editor");
    overlay.m_editor->setPlainText(QStringLiteral("标注"));
    overlay.commitTextEdit();
    check(overlay.m_anns.size() == 2 && overlay.m_anns[1].tool == ATool::Text
              && overlay.m_anns[1].text == QStringLiteral("标注")
              && overlay.m_anns[1].font == overlay.m_afont,
          "text commits into an annotation");
    check(overlay.cropped().toImage() != beforeText, "text composes into the capture");
    overlay.setToolNone();                       // Re-editing works in selection mode.
    mouse(QEvent::MouseButtonDblClick, overlay.m_anns[1].rect.center());
    check(overlay.editorActive()
              && overlay.m_editor->toPlainText() == QStringLiteral("标注"),
          "double-click reopens a text annotation for editing");
    overlay.m_editor->setPlainText(QStringLiteral("改"));
    overlay.commitTextEdit();
    check(overlay.m_anns.size() == 2 && overlay.m_anns[1].text == QStringLiteral("改"),
          "re-editing replaces the annotation in place");
    overlay.selectTool(ATool::Number);
    const QPoint n1 = annOrigin + QPoint(15, 15), n2 = annOrigin + QPoint(70, 50);
    mouse(QEvent::MouseButtonPress, n1);
    mouse(QEvent::MouseButtonRelease, n1);
    mouse(QEvent::MouseButtonPress, n2);
    mouse(QEvent::MouseButtonRelease, n2);
    check(overlay.m_anns.size() == 4 && overlay.m_anns[2].text == QStringLiteral("1")
              && overlay.m_anns[3].text == QStringLiteral("2"),
          "number clicks place incrementing sequence digits");
    overlay.setToolNone();
    mouse(QEvent::MouseButtonPress, n1);
    mouse(QEvent::MouseButtonRelease, n1);
    QApplication::sendEvent(&overlay, &del);
    check(overlay.m_anns.size() == 3 && overlay.m_anns[2].text == QStringLiteral("1"),
          "numbers renumber after a deletion");
    const QImage beforeMosaic = overlay.cropped().toImage();
    overlay.selectTool(ATool::Mosaic);
    overlay.selectMosaicMode(false);
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(50, 10));
    mouse(QEvent::MouseMove, annOrigin + QPoint(100, 60));
    mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(100, 60));
    check(overlay.m_anns.size() == 4 && overlay.m_anns[3].tool == ATool::Mosaic
              && overlay.cropped().toImage() != beforeMosaic,
          "mosaic pixelates the frozen frame in its rectangle");
    overlay.selectTool(ATool::Eraser);
    mouse(QEvent::MouseButtonPress, strokeAt + QPoint(9, 9));
    mouse(QEvent::MouseMove, strokeAt + QPoint(10, 10));
    mouse(QEvent::MouseButtonRelease, strokeAt + QPoint(10, 10));
    bool markerGone = true;
    for (const Annotation &a : overlay.m_anns)
        if (a.tool == ATool::Marker) markerGone = false;
    check(overlay.m_anns.size() == 3 && markerGone,
          "the eraser removes whole annotations under it");
    overlay.setToolNone();
    mouse(QEvent::MouseButtonPress, n2);
    mouse(QEvent::MouseButtonRelease, n2);
    QKeyEvent escSel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &escSel);
    check(overlay.m_selAnn == -1 && overlay.isVisible(),
          "escape deselects the annotation before closing");
    // Behavior parity round: right click finishes input, +/- width, WASD nudge,
    // rotation, and undo/redo button enable state.
    const int annCount = overlay.m_anns.size();
    overlay.selectTool(ATool::Marker);
    const QPoint fin = annOrigin + QPoint(16, 12);
    mouse(QEvent::MouseButtonPress, fin);
    mouse(QEvent::MouseMove, fin + QPoint(14, 6));
    QMouseEvent rightClick(QEvent::MouseButtonPress, fin + QPoint(14, 6),
                           fin + QPoint(14, 6) + overlay.geometry().topLeft(),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &rightClick);
    check(overlay.m_anns.size() == annCount + 1
              && overlay.m_anns.last().tool == ATool::Marker && !overlay.m_live,
          "right click finishes a live stroke instead of discarding it");
    QKeyEvent plus(QEvent::KeyPress, Qt::Key_Plus, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &plus);
    check(overlay.m_anns.last().pen == 4, "the plus key retunes the selected annotation");
    overlay.setToolNone();
    mouse(QEvent::MouseButtonPress, fin + QPoint(7, 3));   // Re-select the stroke.
    mouse(QEvent::MouseButtonRelease, fin + QPoint(7, 3));
    const QPointF beforeNudge = overlay.m_anns.last().stroke.first();
    QKeyEvent keyA(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &keyA);
    QKeyEvent keyW(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &keyW);
    check(overlay.m_anns.last().stroke.first() == beforeNudge + QPointF(-1, -1),
          "WASD keys nudge the selected annotation one pixel");
    const QPoint knob = CaptureOverlay::rotateAnchor(overlay.m_anns.last());
    mouse(QEvent::MouseButtonPress, knob);
    mouse(QEvent::MouseMove, knob + QPoint(30, 0));
    mouse(QEvent::MouseButtonRelease, knob + QPoint(30, 0));
    check(!qFuzzyIsNull(overlay.m_anns.last().rot), "the rotate handle spins the annotation");
    const QPoint inside = CaptureOverlay::annotationRect(overlay.m_anns.last()).center()
                          + QPoint(2, 3);
    mouse(QEvent::MouseButtonPress, inside);
    check(overlay.m_selAnn == overlay.m_anns.size() - 1,
          "rotated annotations stay selectable through the inverse transform");
    mouse(QEvent::MouseButtonRelease, inside);
    QKeyEvent keyDel(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &keyDel);
    while (!overlay.m_undoStack.isEmpty()) overlay.undoAnnotation();
    check(!overlay.m_undoButton->isEnabled(), "the undo button disables on an empty stack");
    overlay.m_redoStack.clear();
    overlay.syncUndoButtons();
    check(!overlay.m_redoButton->isEnabled(), "the redo button disables on an empty stack");
    overlay.selectTool(ATool::Rect);
    QKeyEvent fillKey(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &fillKey);
    check(overlay.m_afill, "the F key toggles fill mode for the shape tools");
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(60, 20));
    mouse(QEvent::MouseMove, annOrigin + QPoint(95, 55));
    mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(95, 55));
    check(overlay.m_anns.last().fill, "fill mode carries into fresh shapes");
    overlay.m_afill = false;
    // Per-tool option strip round: merged shape button, arrow styles, per-tool
    // colors, text weight, and eraser reach.
    overlay.setToolNone();
    overlay.selectTool(ATool::Rect);
    check(overlay.m_shapeButton->isChecked() && !overlay.m_shapeEllipse
              && overlay.m_acolor == overlay.m_colorShape
              && overlay.m_toolOptions->isVisible(),
          "the merged shape button arms the rectangle with its option strip");
    overlay.selectTool(ATool::Ellipse);
    check(overlay.m_shapeEllipse && overlay.m_shapeButton->isChecked(),
          "arming the ellipse flips the shared button's sub-shape");
    overlay.pickToolColor(ATool::Marker, QColor("#34c759"));
    overlay.selectTool(ATool::Marker);
    check(overlay.m_acolor == QColor("#34c759"), "each tool remembers its own color");
    overlay.selectTool(ATool::Arrow);
    overlay.pickArrowStyle(3);
    const QPoint a1 = annOrigin + QPoint(60, 5), a2 = annOrigin + QPoint(60, 55);
    mouse(QEvent::MouseButtonPress, a1);
    mouse(QEvent::MouseMove, a2);
    mouse(QEvent::MouseButtonRelease, a2);
    check(overlay.m_anns.last().tool == ATool::Arrow
              && overlay.m_anns.last().arrowStyle == 3,
          "the arrow style carries into fresh arrows");
    overlay.setToolNone();
    mouse(QEvent::MouseButtonPress, a1 + QPoint(0, 25));
    mouse(QEvent::MouseButtonRelease, a1 + QPoint(0, 25));
    check(overlay.m_selAnn == overlay.m_anns.size() - 1
              && overlay.m_anns[overlay.m_selAnn].tool == ATool::Arrow,
          "the arrow stays selectable");
    overlay.pickArrowStyle(1);
    check(overlay.m_anns[overlay.m_selAnn].arrowStyle == 1,
          "the options strip restyles the selected arrow");
    overlay.setTextBold(true);
    overlay.setTextItalic(true);
    overlay.selectTool(ATool::Text);
    check(overlay.m_abold && overlay.m_aitalic, "text toggles persist per tool");
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(30, 30));
    overlay.m_editor->setPlainText(QStringLiteral("粗"));
    overlay.commitTextEdit();
    check(overlay.m_anns.last().bold && overlay.m_anns.last().italic,
          "bold and italic commit into text annotations");
    overlay.selectTool(ATool::Eraser);
    overlay.m_apen = 12;                        // A fat eraser reaches farther.
    const int beforeErase = overlay.m_anns.size();
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(60, 75));
    mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(60, 75));
    check(overlay.m_anns.size() < beforeErase, "the eraser's reach tracks its thickness");
    overlay.m_anns.clear();
    overlay.m_selAnn = -1;
    // Every tool exposes an editable size, and clicking it again keeps its options open.
    QVector<QImage> toolbarPreviews;
    for (ATool tool : {ATool::Rect, ATool::Ellipse, ATool::Arrow, ATool::Pen, ATool::Marker,
                       ATool::Text, ATool::Number, ATool::Mosaic, ATool::Eraser}) {
        overlay.selectTool(tool);
        overlay.m_toolBtn[int(tool)]->click();
        overlay.m_toolBtn[int(tool)]->click();
        check(overlay.m_atool == tool && overlay.m_toolOptions->isVisible()
                  && (tool == ATool::Mosaic ? overlay.m_mosaicStrengthLabel != nullptr : overlay.m_sizeInput != nullptr),
              "repeated toolbar clicks keep the matching options and numeric control visible");
        const bool textSize = tool == ATool::Text || tool == ATool::Number;
        if (tool == ATool::Mosaic) {
            check(overlay.m_mosaicStrengthLabel->text().toInt() == overlay.mosaicStrength()
                      && !overlay.m_sizeInput && (overlay.mosaicBrush() ? overlay.m_mosaicWidthLabel
                          && overlay.m_mosaicWidthLabel->text().toInt() == overlay.toolSize() : !overlay.m_mosaicWidthLabel),
                  "mosaic exposes independent mode-specific width and strength controls");
        } else {
        check(overlay.m_sizeInput->width() == (textSize ? 62 : 40)
                  && overlay.m_sizeInput->lineEdit()->alignment() == Qt::AlignCenter,
              "numeric controls use a compact fixed width with centered values");
        check(overlay.m_sizeInput->lineEdit()->contentsRect().width()
                  >= QFontMetrics(overlay.m_sizeInput->lineEdit()->font()).horizontalAdvance(
                      textSize ? QStringLiteral("1637.5") : QStringLiteral("12")),
              "compact numeric controls still fit the complete supported values");
        check(overlay.m_toolOptions->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly).isEmpty(),
              "option strips contain no visible thickness labels or wheel instructions");
        }
        for (QWidget *bar : {overlay.m_tools, overlay.m_toolOptions})
            for (QToolButton *button : bar->findChildren<QToolButton *>())
                check(button->text().isEmpty(), "toolbar actions render icons or swatches rather than text");
        if (!previewPath.isEmpty()) {
            QImage bars = overlay.grab(overlay.m_tools->geometry().united(overlay.m_toolOptions->geometry())).toImage();
            bars.setDevicePixelRatio(1);
            toolbarPreviews.append(bars);
        }
        const bool colored = tool != ATool::Mosaic && tool != ATool::Eraser;
        const QList<QToolButton *> swatches = overlay.m_toolOptions->findChildren<QToolButton *>(QStringLiteral("presetColor"));
        check((overlay.m_colorButton != nullptr) == colored && swatches.size() == (colored ? 8 : 0),
              "colored tools expose eight common colors and a palette button, not a dense color grid");
        if (colored)
            check(overlay.m_colorButton->text().isEmpty() && !overlay.m_colorButton->icon().isNull()
                      && overlay.m_colorButton->toolButtonStyle() == Qt::ToolButtonIconOnly
                      && overlay.m_colorButton->styleSheet().isEmpty(),
                  "every palette button uses a white icon without a text label or custom border");
        for (QToolButton *swatch : swatches)
            check(swatch->y() == swatches.first()->y(), "common color presets stay in one spacious row");
    }
    if (!toolbarPreviews.isEmpty()) {
        int width = 0, height = 0;
        for (const QImage &bars : toolbarPreviews) { width = qMax(width, bars.width()); height += bars.height() + 8; }
        QImage sheet(width, height, QImage::Format_RGB32);
        sheet.fill(QColor(15, 17, 20));
        QPainter painter(&sheet);
        int y = 0;
        for (const QImage &bars : toolbarPreviews) { painter.drawImage(0, y, bars); y += bars.height() + 8; }
        painter.end();
        check(sheet.save(previewPath + QStringLiteral(".toolbars.png")), "save toolbar visual regression sheet");
    }
    check(parseAnnotationColor(QStringLiteral("rgb(0, 128,255)")) == QColor(0, 128, 255)
              && parseAnnotationColor(QStringLiteral("#A1b2C3")) == QColor("#a1b2c3")
              && !parseAnnotationColor(QStringLiteral("rgb(256,0,0)")).isValid()
              && !parseAnnotationColor(QStringLiteral("rgb(-1,0,0)")).isValid()
              && !parseAnnotationColor(QStringLiteral("#12xz56")).isValid()
              && !parseAnnotationColor(QStringLiteral("#12345678")).isValid(),
          "manual colors validate channel bounds and exactly six hexadecimal digits");
    overlay.selectTool(ATool::Pen);
    QLineEdit *sizeEdit = overlay.m_sizeInput->findChild<QLineEdit *>();
    sizeEdit->setText(QStringLiteral("6"));
    QKeyEvent sizeEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(sizeEdit, &sizeEnter);
    check(overlay.m_apen == 6 && overlay.m_sizeInput->currentText().toDouble() == 6 && overlay.isVisible(),
          "typing a width and pressing Enter changes the tool without pinning the capture");
    QWheelEvent sizeWheel(QPointF(10, 10), QPointF(sizeEdit->mapToGlobal(QPoint(10, 10))),
                         QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                         Qt::ScrollUpdate, false);
    QApplication::sendEvent(sizeEdit, &sizeWheel);
    check(overlay.m_apen == 7 && overlay.m_sizeInput->currentText().toDouble() == 7,
          "wheel input over the numeric field uses the same width and updates the displayed value");
    const QPoint sizeStroke = annOrigin + QPoint(15, 15);
    mouse(QEvent::MouseButtonPress, sizeStroke);
    overlay.tuneWidth(1, sizeStroke);
    mouse(QEvent::MouseMove, sizeStroke + QPoint(20, 10));
    overlay.pickToolColor(ATool::Pen, QColor("#007aff"));
    check(overlay.m_tools->isVisible() && overlay.m_toolOptions->isVisible()
              && overlay.m_sizeInput->currentText().toDouble() == 8 && overlay.m_ann.pen == 8,
          "drawing keeps both bars visible and wheel changes update the live stroke and number");
    check(overlay.m_ann.color == QColor("#007aff"), "color changes apply to the in-progress stroke");
    mouse(QEvent::MouseButtonRelease, sizeStroke + QPoint(20, 10));
    overlay.m_sizeInput->lineEdit()->setText(QStringLiteral("9"));
    QApplication::sendEvent(overlay.m_sizeInput->lineEdit(), &sizeEnter);
    check(overlay.m_anns.last().pen == 9 && overlay.m_sizeInput->currentText().toDouble() == 9,
          "the numeric field retunes the selected object");
    overlay.undoAnnotation();
    check(overlay.m_anns.last().pen == 8, "one undo restores the width before a numeric edit");
    overlay.redoAnnotation();
    check(overlay.m_anns.last().pen == 9, "redo reapplies the numeric width edit");
    overlay.selectTool(ATool::Eraser);
    overlay.setToolSize(99);
    check(overlay.toolSize() == 12 && overlay.m_sizeInput->currentText().toDouble() == 12,
          "width input stays within the supported range");
    overlay.setToolSize(0);
    check(overlay.toolSize() == 1 && overlay.m_sizeInput->currentText().toDouble() == 1,
          "width input cannot become zero or negative");
    overlay.placeToolbar();
    overlay.showToolBars();
    check(overlay.m_toolOptions->x() == overlay.m_tools->x()
              && overlay.m_toolOptions->y() == overlay.m_tools->geometry().bottom() + 1
              && overlay.m_toolOptions->width() == overlay.m_tools->width(),
          "the main toolbar and its options form aligned, gapless horizontal rows");
    for (QWidget *bar : {overlay.m_tools, overlay.m_toolOptions}) {
        const QImage painted = overlay.grab(bar->geometry()).toImage();
        const qreal dpr = painted.devicePixelRatio();
        const QColor background = painted.pixelColor(qRound(10 * dpr), qRound((bar->height() - 2) * dpr));
        if (background != QColor(29, 31, 35))
            std::fprintf(stderr, "bar background %d,%d,%d size=%dx%d dpr=%.2f main=%d\n",
                         background.red(), background.green(), background.blue(),
                         bar->width(), bar->height(), double(dpr), bar == overlay.m_tools);
        check(background == QColor(29, 31, 35),
              "main and option bars render the exact solid background color");
    }
    for (QWidget *widget : overlay.m_toolButtons) {
        const QIcon icon = static_cast<QToolButton *>(widget)->icon();
        for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled}) {
            const QImage pixels = icon.pixmap(QSize(16, 16), mode).toImage();
            bool white = true, visible = false;
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = 0; x < pixels.width(); ++x) {
                    const QColor color = pixels.pixelColor(x, y);
                    if (!color.alpha()) continue;
                    visible = true;
                    if (color.red() != 255 || color.green() != 255 || color.blue() != 255) white = false;
                }
            check(visible && white, "toolbar icons stay white in normal and disabled states");
        }
    }
    overlay.m_anns.clear();
    overlay.m_undoStack.clear();
    overlay.m_redoStack.clear();
    overlay.m_atool = ATool::None;
    overlay.m_selAnn = -1;
    overlay.syncUndoButtons();
    overlay.m_windowRects.clear();
    overlay.m_sel = QRect(2, 2, 30, 20);
    overlay.m_mode = Adjusting;
    const QImage cropped = overlay.cropped().toImage();
    check(cropped.size() == QSize(45, 30), "selection crop uses native capture coordinates");
    check(cropped.devicePixelRatio() == 1, "capture output uses raw pixels for pinning and clipboard consumers");
    QKeyEvent copyImage(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(&overlay, &copyImage);
    check(clipboard->image() == cropped && !overlay.isVisible(), "Ctrl+C still copies only the selection and closes capture");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    for (const Mode mode : {Selecting, Adjusting}) {
        overlay.m_mode = mode;
        overlay.show();
        inspector->show();
        overlay.m_tools->setVisible(mode == Adjusting);
        QApplication::sendEvent(&overlay, &escape);
        check(!overlay.isVisible() && !inspector->isVisible() && !overlay.m_tools->isVisible(),
              "Escape exits capture before and after selecting a region");
    }
    clipboard->setMimeData(saved);
    std::puts("PASS: pixel sampling, magnifier, color shortcuts, and capture regression checks");
    return 0;
}
