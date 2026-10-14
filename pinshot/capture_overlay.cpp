#include "capture_overlay.h"
#include <QWindow>
#include "output.h"
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
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QColor>
#include <QPaintEvent>
#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <QAbstractItemView>
#include <QDialog>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QStandardPaths>
#include <QCursor>
#include <QMessageBox>
#include <QTimer>
#include <QEventLoop>
#include <QThread>
#include <QSettings>
#include <QPointer>
#include <QCloseEvent>
#include <qt_windows.h>
#include <imm.h>

CaptureOverlay::CaptureOverlay(const QVector<ScreenFrame> &frames,
                        const WindowTargets &nativeWindows,
                        const DetectionOptions &options, bool liveDetection)
    : m_frames(frames), m_detectionOptions(options)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setGeometry(QGuiApplication::primaryScreen()->virtualGeometry());
    QRect nativeDesktop;
    for (const GdiDisplay &display : gdiDisplays()) nativeDesktop |= display.native;
    if (!nativeDesktop.isEmpty()) {
        // Position the native window before freezing its local coordinate system.
        if (!SetWindowPos(reinterpret_cast<HWND>(winId()), nullptr, nativeDesktop.x(), nativeDesktop.y(),
                          nativeDesktop.width(), nativeDesktop.height(), SWP_NOZORDER | SWP_NOACTIVATE)) {
            QMessageBox::warning(nullptr, QStringLiteral("pinshot"), QStringLiteral("无法定位截图覆盖层"));
            deleteLater();
            return;
        }
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    qreal scale = 1;
    for (const ScreenFrame &frame : m_frames)
        scale = qMax(scale, frame.image.devicePixelRatio() > 1 ? frame.image.devicePixelRatio()
                        : qreal(frame.image.width()) / frame.logicalRect.width());
    m_shot = QPixmap(qRound(width() * scale), qRound(height() * scale));
    m_shot.setDevicePixelRatio(scale);
    m_shot.fill(Qt::black);
    QPainter painter(&m_shot);
    for (const ScreenFrame &frame : m_frames) {
        const QPointF local = windowHandle()->mapFromGlobal(QPointF(frame.logicalRect.topLeft()));
        const QSizeF size = frame.image.devicePixelRatio() > 1
            ? frame.image.deviceIndependentSize() : QSizeF(frame.logicalRect.size());
        painter.drawImage(QRectF(local, size), frame.image);
    }
    painter.end();
    m_inspector = new PixelInspector(this);

    m_sizeTip = new QLabel(this);
    m_sizeTip->setStyleSheet(
        "background:#f2f2f2;color:#323232;padding:2px 6px;border:1px solid #2080f0;font-size:12px;");
    m_sizeTip->hide();

    // The overlay paints matching solid backgrounds for both toolbar containers.
    qApp->installEventFilter(this);                  // Swallows hover tooltips of every capture control.
    m_tools = new QWidget(this);
    m_tools->installEventFilter(this);
    m_tools->setCursor(Qt::ArrowCursor);
    // Light bar language: flat 32px buttons, grey hover, theme-blue checked fill, 1px separators.
    const QString barQss = QStringLiteral(
        "QToolButton{color:#323232;background:transparent;border:none;border-radius:0px;padding:4px;}"
        "QToolButton:hover{background-color:#d4d4d4;}"
        "QToolButton:checked{background-color:#2080f0;}"
        "QToolButton:pressed{padding:6px 2px 2px 6px;}"
        "QFrame#barSeparator{background-color:rgba(50,50,50,144);margin:0px 2px;border:none;}");
    m_tools->setStyleSheet(barQss);
    auto *save = iconButton(Icon::Save, QStringLiteral("保存到文件 (Ctrl+S)"));
    auto *copy = iconButton(Icon::Copy, QStringLiteral("复制到剪贴板 (Ctrl+C)"));
    auto *pin = iconButton(Icon::Pin, QStringLiteral("贴在屏幕上 (Enter / 双击)"));
    auto *scroll = iconButton(Icon::LongShot, QStringLiteral("长截图：手动滚动拼接长图"));
    auto *ocr = iconButton(Icon::Ocr, QStringLiteral("文字识别 (OCR)：截图与文本对照"));
    ocr->setObjectName(QStringLiteral("ocrButton"));
    ocr->setAccessibleName(QStringLiteral("文字识别 OCR"));
    const QString ocrMissing = OcrEngine::assetError(QCoreApplication::applicationDirPath());
    if (!ocrMissing.isEmpty()) { ocr->setEnabled(false); ocr->setToolTip(ocrMissing); }
    connect(save, &QToolButton::clicked, this, &CaptureOverlay::saveCapture);
    connect(copy, &QToolButton::clicked, this, &CaptureOverlay::copyCapture);
    connect(pin, &QToolButton::clicked, this, &CaptureOverlay::pinCapture);
    connect(scroll, &QToolButton::clicked, this, &CaptureOverlay::longCapture);
    connect(ocr, &QToolButton::clicked, this, &CaptureOverlay::ocrCapture);

    // Instant annotation tools: draw inside the committed selection; every capture
    // action composes them in. One shape button covers rect / ellipse; per-tool
    // options (sub-shape, arrow style, colors, text weight) live on the strip below.
    auto *undo = iconButton(Icon::Undo, QStringLiteral("撤销 (Ctrl+Z)"));
    connect(undo, &QToolButton::clicked, this, &CaptureOverlay::undoAnnotation);
    m_undoButton = undo;
    auto *redo = iconButton(Icon::Redo, QStringLiteral("重做 (Ctrl+Y)"));
    connect(redo, &QToolButton::clicked, this, &CaptureOverlay::redoAnnotation);
    m_redoButton = redo;
    m_shapeButton = new QToolButton;               // One button for both shapes.
    m_shapeButton->setIcon(makeIcon(Icon::Rect));
    m_shapeButton->setIconSize(QSize(16, 16));
    m_shapeButton->setToolTip(QStringLiteral("形状：矩形/椭圆 (R=矩形 / O=椭圆)"));
    m_shapeButton->setCheckable(true);
    m_shapeButton->setCursor(Qt::PointingHandCursor);
    connect(m_shapeButton, &QToolButton::clicked, this, [this] {
        selectTool(m_shapeEllipse ? ATool::Ellipse : ATool::Rect);
        setCursor(Qt::CrossCursor);
        update();
    });
    m_toolBtn.fill(nullptr, int(ATool::Eraser) + 1);
    m_toolBtn[int(ATool::Rect)] = m_toolBtn[int(ATool::Ellipse)] = m_shapeButton;
    struct { ATool tool; Icon icon; const char *tip; } const tools[] = {
        {ATool::Arrow, Icon::Arrow, "箭头"},
        {ATool::Pen, Icon::Pen, "画笔 (P)"},
        {ATool::Marker, Icon::Marker, "荧光笔 (H)"},
        {ATool::Text, Icon::Text, "文字 (T)"},
        {ATool::Number, Icon::Number, "序号 (N)"},
        {ATool::Mosaic, Icon::Mosaic, "马赛克 (M)"},
        {ATool::Eraser, Icon::Eraser, "橡皮 (E)"},
    };
    for (const auto &t : tools) {
        auto *button = iconButton(t.icon, QString::fromUtf8(t.tip));
        button->setCheckable(true);
        const ATool tool = t.tool;
        connect(button, &QToolButton::clicked, this, [this, tool] {
            selectTool(tool);
            update();
        });
        m_toolBtn[int(tool)] = button;
    }
    // Drawing tools | undo, redo | capture actions ending with pin, save, copy.
    m_toolButtons = {m_shapeButton, m_toolBtn[int(ATool::Arrow)], m_toolBtn[int(ATool::Pen)],
                     m_toolBtn[int(ATool::Marker)], m_toolBtn[int(ATool::Mosaic)], m_toolBtn[int(ATool::Text)],
                     m_toolBtn[int(ATool::Number)], m_toolBtn[int(ATool::Eraser)],
                     undo, redo, scroll, ocr, pin, save, copy};
    for (QWidget *button : m_toolButtons) button->setFocusPolicy(Qt::NoFocus);

    m_toolOptions = new QWidget(this);
    m_toolOptions->installEventFilter(this);
    m_toolOptions->setCursor(Qt::ArrowCursor);
    m_toolOptions->setStyleSheet(barQss + QStringLiteral(
        "QLabel{color:#323232;font-size:12px;border:none;}"
        "QComboBox{color:#323232;background:#fdfdfd;border:1px solid #d2d2d2;border-bottom-color:#bcbcbc;"
        "padding:1px 3px;font-size:12px;}"
        "QComboBox:hover{border-color:#a0a0a0;}"
        "QComboBox::drop-down{border:none;width:14px;background:transparent;}"
        "QComboBox::down-arrow{image:none;width:0px;height:0px;}"
        "QComboBox QAbstractItemView{outline:0px;color:#323232;background:#ffffff;border:1px solid #828790;"
        "selection-background-color:#e5f1fb;selection-color:#323232;}"
        "QLineEdit{color:#323232;background:transparent;border:none;"
        "selection-background-color:#2080f0;selection-color:#ffffff;}"));
    m_toolOptions->hide();
    buildToolLayout();
    m_tools->adjustSize();
    m_tools->hide();

    show();
    setWindowTargets(nativeWindows);
    // Claim foreground synchronously (hotkey handling grants activation rights)
    // and detach the IME so plain C/Shift always reach this window.
    HWND handle = reinterpret_cast<HWND>(winId());
    SetForegroundWindow(handle);
    SetFocus(handle);
    m_imeCtx = ImmAssociateContext(handle, nullptr);   // Detached; re-attached for text editing.
    setFocus();
    updateInspector();
    if (liveDetection) {
        m_detectionWatchdog.setSingleShot(true);
        connect(&m_detectionWatchdog, &QTimer::timeout, this, &CaptureOverlay::recoverDetector);
        startDetector();
        requestDetection(mapFromGlobal(QCursor::pos()));
    }
}

CaptureOverlay::~CaptureOverlay()  { if (m_detector) m_detector->stop(); }

void CaptureOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.drawPixmap(0, 0, m_shot);                 // Frozen frame covers the virtual desktop.
    // The mask hole tracks the detected target, falling back to the committed selection.
    const QRect hole = !m_hover.isNull() ? m_hover : m_sel;
    if (hole.isNull()) {
        p.fillRect(rect(), QColor(0, 0, 0, 60));
        return;
    }
    QPainterPath mask;                          // Dim outside the hole, leave the inside clear.
    mask.addRect(rect());
    mask.addRect(hole);
    p.fillPath(mask, QColor(0, 0, 0, 120));
    p.setPen(QPen(QColor("#2080f0"), 1));
    p.drawRect(QRectF(hole).adjusted(0.5, 0.5, -0.5, -0.5));
    const QColor snapColor(0, 229, 255);        // Magnetic-snap indicator lines.
    if (m_snapX >= 0) {
        p.setPen(QPen(snapColor, 1));
        p.drawLine(m_snapX, hole.top() - 10, m_snapX, hole.bottom() + 10);
    }
    if (m_snapY >= 0) {
        p.setPen(QPen(snapColor, 1));
        p.drawLine(hole.left() - 10, m_snapY, hole.right() + 10, m_snapY);
    }
    if (m_mode == Adjusting && !m_sel.isNull())   // Annotations clip to the selection.
        paintAnnotations(p, QPointF(0, 0), 1.0, QRectF(m_sel), false);
    if (m_live && m_ann.tool == ATool::Mosaic && !m_ann.mosaicBrush)
        paintMosaicBorder(p, m_ann, false);
    if (m_mode == Adjusting && !m_live
        && (editorActive() || (m_selAnn >= 0 && m_selAnn < m_anns.size()
                              && !m_anns[m_selAnn].mosaicBrush
                              && (m_atool == ATool::None || m_spaceHeld
                                  || (m_atool == ATool::Text && m_anns[m_selAnn].tool == ATool::Text)
                                  || (m_atool == ATool::Mosaic && !m_mosaicBrush
                                      && m_anns[m_selAnn].tool == ATool::Mosaic))))) {
        const Annotation ann = editorActive() ? editorAnnotation() : m_anns[m_selAnn];
        if (ann.tool == ATool::Mosaic) {
            paintMosaicBorder(p, ann, true);
        } else {
        p.save();
        if (!qFuzzyIsNull(ann.rot)) p.setTransform(annTransform(ann), true);
        const QRect box = annotationRect(ann);
        if (ann.tool == ATool::Text) {
            // Reference text frame: one hairline white rect at the bounds, nothing dashed.
            QPen frame(Qt::white, 0);
            p.setPen(frame);
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(box).adjusted(0, 0, 0.5, 0.5));
        } else {
            QPen border(QColor("#2080f0"), 1);
            border.setDashPattern({4, 3});
            p.setPen(border);
            p.setBrush(Qt::NoBrush);
            if (ann.tool != ATool::Arrow) p.drawRect(box.adjusted(-3, -3, 3, 3));
        }
        for (const QPoint &anchor : annAnchors(ann))
            paintHandle(p, anchor);
        if (ann.tool != ATool::Mosaic) {          // Mosaic regions move and resize without rotation.
            const QPoint base(box.center().x(), box.bottom() + 2);
            const QPoint knob(box.center().x(), box.bottom() + 22);
            p.setPen(QPen(QColor("#2080f0"), 1));
            p.drawLine(base, knob);
            paintHandle(p, knob);
        }
        p.restore();
        }
    }
    if (m_mode == Adjusting && !m_sel.isNull() && m_hover.isNull())  // Eight resize handles.
        for (const QRect &h : handleRects(m_sel))
            paintHandle(p, h.center() + QPoint(1, 1));
    for (QWidget *glass : {m_tools, m_toolOptions}) {   // Light fill inside a 1px theme-blue frame.
        if (!glass->isVisible()) continue;
        const QRect bar = glass->geometry();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x20, 0x80, 0xf0));
        p.drawRect(bar);
        p.setBrush(QColor(0xf2, 0xf2, 0xf2));
        p.drawRect(bar.adjusted(1, 1, -1, -1));
    }
}

void CaptureOverlay::mousePressEvent(QMouseEvent *e)
{
    m_snapX = m_snapY = -1;                     // Clear magnetic-snap indicators.
    cancelDetection();
    m_nudging = false;
    const bool wasEditing = editorActive();
    if (wasEditing) commitTextEdit();           // Canvas clicks finish input; controls keep it live.
    if (e->button() == Qt::RightButton) {       // Right click ends input, then the ladder.
        stopItemDrag();
        if (m_live) { m_live = false; commitAnnotation(); }
        else if (wasEditing) return;
        else if (m_atool != ATool::None) setToolNone();
        else if (m_selAnn >= 0) { m_selAnn = -1; updateToolOptions(); update(); }
        else if (m_mode == Adjusting) { resetSelection(); } else { close(); }
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (m_live && m_ann.tool == ATool::Arrow && m_ann.lineStyle == 2) {   // Polyline: each click fixes a vertex.
        m_ann.stroke.last() = e->pos();
        m_ann.stroke << e->pos();
        update();
        return;
    }
    setFocus();                                   // Any click restores keyboard handling.
    m_pressPos = e->pos();
    m_dragStarted = false;
    m_inspector->hide();
    if (m_mode == Adjusting && !m_spaceHeld && (m_atool == ATool::Mosaic || m_atool == ATool::None)) {
        const bool regionTool = m_atool == ATool::None || !m_mosaicBrush;
        if (regionTool && m_selAnn >= 0 && m_selAnn < m_anns.size()
            && m_anns[m_selAnn].tool == ATool::Mosaic && !m_anns[m_selAnn].mosaicBrush) {
            const Annotation &ann = m_anns[m_selAnn];
            const Handle handle = annHandleAt(ann, e->pos());
            if (handle != H_None || annotationHit(ann, e->pos())) {
                m_itemDrag = handle == H_None ? H_Move : handle;
                m_pressAnn = ann;
                updateAnnotationCursor(e->pos());
                return;
            }
        }
        const int hit = regionTool ? annotationAt(e->pos()) : -1;
        if (hit >= 0 && m_anns[hit].tool == ATool::Mosaic && !m_anns[hit].mosaicBrush) {
            m_selAnn = hit;
            m_pressAnn = m_anns[hit];
            const Handle handle = annHandleAt(m_pressAnn, e->pos());
            m_itemDrag = handle == H_None ? H_Move : handle;
            updateToolOptions();
            updateAnnotationCursor(e->pos());
            update();
            return;
        }
    }
    if (m_selAnn >= 0 && m_selAnn < m_anns.size() && m_anns[m_selAnn].tool == ATool::Mosaic) {
        m_selAnn = -1;
        updateToolOptions();
        update();
    }
    if (m_mode == Adjusting && m_atool == ATool::Text && !m_spaceHeld) {
        if (m_selAnn >= 0 && m_selAnn < m_anns.size()
            && m_anns[m_selAnn].tool == ATool::Text) {
            const Handle h = annHandleAt(m_anns[m_selAnn], e->pos());
            if (h != H_None) {
                m_itemDrag = h;
                m_pressAnn = m_anns[m_selAnn];
                return;
            }
        }
        const int hit = annotationAt(e->pos());
        if (hit >= 0 && m_anns[hit].tool == ATool::Text) {
            beginTextEdit(e->pos(), hit);
            return;
        }
    }
    if (m_mode == Adjusting && m_atool != ATool::None
        && !m_spaceHeld && hitHandle(e->pos()) == H_Move) { // A tool acts inside the selection.
        if (m_atool == ATool::Text) { beginTextEdit(e->pos()); return; }
        if (m_atool == ATool::Eraser) {          // Drag deletes whole annotations touched.
            m_erasing = true;
            m_eraseSnapshotted = false;
            eraseAt(e->pos());
            return;
        }
        if (m_atool == ATool::Number) {          // Click drops the next sequence number.
            pushUndo();
            Annotation ann;
            ann.tool = ATool::Number;
            ann.color = m_acolor;
            ann.font = m_afont;
            ann.numberStyle = m_numberStyle;
            ann.numberFormat = m_numberFormat;
            const int s = numberDiameter(ann);
            ann.rect = QRect(e->pos() - QPoint(s / 2, s / 2), QSize(s, s));
            m_anns.append(ann);
            normalizeNumbers();
            m_selAnn = m_anns.size() - 1;        // Auto-select fresh annotations.
            syncSizeInput();
            update(ann.rect.adjusted(-8, -8, 8, 8));
            return;
        }
        beginAnnotation(e->pos());
        return;
    }
    if (m_mode == Adjusting && (m_atool == ATool::None || m_spaceHeld)) {   // Editing mode.
        if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
            const Handle h = annHandleAt(m_anns[m_selAnn], e->pos());
            if (h != H_None) {                   // Corner/endpoint handles resize in place.
                m_itemDrag = h;
                m_pressAnn = m_anns[m_selAnn];
                return;
            }
        }
        const int hit = annotationAt(e->pos());
        if (hit >= 0) {                          // Click selects and drags the annotation.
            m_selAnn = hit;
            m_itemDrag = H_Move;
            m_pressAnn = m_anns[hit];
            updateToolOptions();
            update();
            return;
        }
        m_selAnn = -1;
        updateToolOptions();
    }
    if (m_mode == Adjusting) {
        m_drag = hitHandle(e->pos());
        m_pressSel = m_sel;
        if (m_drag != H_None && m_drag != H_Move && !m_sel.contains(e->pos())) {
            m_sel = resizeBy(m_drag, e->pos());  // Outside presses extend the existing edge or corner.
            applySnap(m_drag);
            hideToolBars();
            updateTip();
            update();
        } else if (m_drag == H_None) {
            resetSelection();
            m_drag = H_New;
        }
    } else {
        resetSelection();                       // A fresh press always starts from a clean state.
        m_drag = H_New;
    }
    m_pressWindow = m_drag == H_New ? windowAt(e->pos()) : QRect();
    if (m_drag == H_New) requestDetection(e->pos());
    updateInspector();
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent *e)
{
    updateInspector();
    const QPoint pos = e->pos();
    if (m_waitingClick) return;
    if (m_live && m_ann.tool == ATool::Mosaic && m_ann.mosaicBrush) updateAnnotationCursor(pos);
    if (m_live) {                               // Live annotation follows the cursor.
        const QRect dirty = annotationRect(m_ann);
        updateAnnotation(pos, e->modifiers());
        update(dirty.united(annotationRect(m_ann)).adjusted(-24, -24, 24, 24));
        m_sizeTip->hide();                      // Drawing shows no floating text.
        return;
    }
    if (m_erasing) {                             // The eraser removes whole annotations.
        eraseAt(pos);
        return;
    }
    if (m_itemDrag != H_None) {                  // Move or resize the selected annotation.
        if (m_selAnn < 0 || m_selAnn >= m_anns.size()) { stopItemDrag(); return; }
        if (!m_dragStarted) {
            if ((pos - m_pressPos).manhattanLength() < QApplication::startDragDistance())
                return;
            m_dragStarted = true;
            pushUndo();                          // Snapshot before the first modification.
            hideToolBars();
            m_inspector->hide();
        }
        const QRect dirty = rotatedBounds(m_anns[m_selAnn]).adjusted(-24, -24, 24, 24);
        applyItemDrag(pos, e->modifiers());
        update(dirty.united(rotatedBounds(m_anns[m_selAnn])).adjusted(-24, -24, 24, 24));
        return;
    }
    if (m_drag == H_None) {                     // Hover: cursor shape, size hint, window detection.
        if (m_mode == Adjusting) {
            updateAnnotationCursor(pos);
            if (hitHandle(pos) != H_None) {
                cancelDetection();
                clearHover();                   // Repaint only the retired hover rectangle.
            }
            // Hover re-detection only runs before a selection is committed: afterwards
            // the highlight would chase the cursor and visually unlock the selection.
        } else requestDetection(pos);
        updateTip();                            // Visual state changes only with the hover rect.
        return;
    }
    if (m_drag == H_New && !m_dragStarted
        && (pos - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
    m_dragStarted = true;
    cancelDetection();
    hideToolBars();                              // The status bar steps aside during drags.
    if (!m_hover.isNull())
        m_hover = QRect();                      // A drag replaces the hover preview with the selection.
    if (m_drag == H_New) {                      // New selection.
        if (m_mode == Selecting)
            m_mode = Adjusting;                 // Dragging enters adjustment mode.
        m_sel = QRect(m_pressPos, pos).normalized();
    } else {
        m_sel = resizeBy(m_drag, pos);
    }
    applySnap(m_drag);
    updateInspector();
    updateTip();
    update();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_live && m_ann.tool == ATool::Arrow && m_ann.lineStyle == 2) {
        // A drag on the first press lays the first segment; otherwise vertices come from clicks.
        if (m_ann.stroke.size() == 2
            && (e->pos() - m_ann.stroke.first().toPoint()).manhattanLength() >= QApplication::startDragDistance()) {
            m_ann.stroke.last() = e->pos();
            m_ann.stroke << e->pos();
        }
        return;
    }
    if (m_live) {                               // Commit the annotation; the tool stays active.
        m_live = false;
        commitAnnotation();
        return;
    }
    if (m_erasing) { m_erasing = false; return; }
    if (m_itemDrag != H_None) {                 // A click only selects; a drag edits.
        const bool moved = m_dragStarted;
        m_itemDrag = H_None;
        m_dragStarted = false;
        if (moved) {
            placeToolbar();
            showToolBars();
        }
        update();
        updateAnnotationCursor(e->pos());
        return;
    }
    if (m_drag == H_None)
        return;
    if (m_drag == H_New && !m_dragStarted) {
        m_drag = H_None;
        if (m_detector && m_queryPending) { m_waitingClick = true; return; }
        m_sel = m_pressWindow;
    }
    m_drag = H_None;
    m_snapX = m_snapY = -1;                     // Snap indicators only live during a drag.
    m_sel = m_sel.normalized();
    if (m_sel.width() < 4 || m_sel.height() < 4) {
        if (m_pressWindow.isEmpty()) {          // Plain click on empty space: nothing selected.
            resetSelection();
            return;
        }
        m_sel = m_pressWindow;                  // Click without drag: select the whole window.
    }
    m_mode = Adjusting;
    m_hover = {};
    setCursor(Qt::SizeAllCursor);
    placeToolbar();
    showToolBars();
    updateTip();
    updateInspector();
    update();
    if (m_pinAfterDetection) { m_pinAfterDetection = false; pinCapture(); }
}

void CaptureOverlay::wheelEvent(QWheelEvent *e)
{
    if (!e->angleDelta().y()) return;
    if (editorActive()) {                        // Wheel retunes the live text editor.
        editorFontStep(e->angleDelta().y() > 0 ? 1 : -1);
        return;
    }
    if (m_mode == Adjusting && m_drag == H_None && m_itemDrag == H_None) {
        tuneWidth(e->angleDelta().y() > 0 ? 1 : -1, e->position().toPoint());
        if (m_selAnn >= 0 || m_atool != ATool::None) return;
    }
    if (m_drag != H_None || m_mode != Selecting || !m_detectionOptions.elements) return;
    const QPoint point = e->position().toPoint();
    if (e->angleDelta().y() > 0) {
        if (m_hoverHistory.isEmpty()) m_hoverHistory.append(m_hover);
        requestDetection(point, m_hover);
    } else if (e->angleDelta().y() < 0) {
        cancelDetection();
        if (m_hoverHistory.size() > 1) {
            m_hoverHistory.removeLast();
            m_hover = m_hoverHistory.last();
            updateTip();
            update();
        } else requestDetection(point);
    }
}

void CaptureOverlay::closeEvent(QCloseEvent *e)
{
    if (m_colorDialog) m_colorDialog->close();
    cancelDetection();
    if (m_detector) m_detector->stop();
    QWidget::closeEvent(e);
}

void CaptureOverlay::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (editorActive()) return;                  // A text double-click never pins the capture.
    if (m_live && m_ann.tool == ATool::Arrow && m_ann.lineStyle == 2) {   // Double-click ends a polyline.
        m_live = false;
        commitAnnotation();
        return;
    }
    if (m_mode == Adjusting && (m_atool == ATool::None || m_atool == ATool::Text)) {
        const int hit = annotationAt(e->pos());  // Double-click re-opens a text annotation.
        if (hit >= 0 && m_anns[hit].tool == ATool::Text) {
            m_selAnn = hit;
            m_itemDrag = H_None;
            beginTextEdit(e->pos(), hit);
            return;
        }
    }
    if (m_queryPending && (m_waitingClick || m_drag == H_New))
        m_pinAfterDetection = true;
    else if (m_mode == Adjusting && m_atool == ATool::None && m_sel.contains(e->pos()))
        pinCapture();
}

void CaptureOverlay::keyPressEvent(QKeyEvent *e)
{
    if (editorActive()) {                        // Unhandled editing keys must not trigger capture actions.
        if (e->key() == Qt::Key_Escape) commitTextEdit();
        else if (e->modifiers() & Qt::ControlModifier) {
            if (e->key() == Qt::Key_S) saveCapture();
            else if (e->key() == Qt::Key_T) pinCapture();
            else if (e->key() == Qt::Key_Z) {
                if (e->modifiers() & Qt::ShiftModifier) redoAnnotation();
                else undoAnnotation();
            } else if (e->key() == Qt::Key_Y) redoAnnotation();
        }
        return;
    }
    if (!m_live && m_itemDrag == H_None && m_drag == H_None
        && e->key() == Qt::Key_A && e->modifiers() == Qt::ControlModifier) {
        const QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
        const QRect current = localScreenRect((screen ? screen : QGuiApplication::primaryScreen())->geometry());
        cancelDetection();
        m_sel = m_sel == current ? rect() : current;
        m_mode = Adjusting;
        m_hover = {};
        m_snapX = m_snapY = -1;
        placeToolbar(); showToolBars(); updateTip(); update();
        updateInspector();
        return;
    }
    const QPoint delta = keyDelta(e->key());
    const bool arrow = e->key() >= Qt::Key_Left && e->key() <= Qt::Key_Down;
    const Qt::KeyboardModifiers movementModifiers = e->modifiers() & ~Qt::KeypadModifier;
    if (!delta.isNull() && m_selAnn < 0 && !m_live && m_itemDrag == H_None) {
        if (movementModifiers == Qt::NoModifier && (!arrow || m_mode == Selecting)) {
            const QRect bounds = QGuiApplication::primaryScreen()->virtualGeometry();
            const QPoint next = QCursor::pos() + delta;
            QCursor::setPos(qBound(bounds.left(),next.x(),bounds.right()),
                            qBound(bounds.top(),next.y(),bounds.bottom()));
            if (m_drag != H_None) {
                const QPoint local = windowHandle()->mapFromGlobal(QPointF(QCursor::pos())).toPoint();
                QMouseEvent move(QEvent::MouseMove,local,QCursor::pos(),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
                mouseMoveEvent(&move);
            } else updateInspector();
            return;
        }
        if (arrow && m_mode == Adjusting && m_drag == H_None && !m_sel.isEmpty()
            && movementModifiers == Qt::NoModifier) {
            cancelDetection();
            m_sel.translate(delta);
            m_sel.moveTo(qBound(0,m_sel.x(),width()-m_sel.width()),qBound(0,m_sel.y(),height()-m_sel.height()));
            m_snapX = m_snapY = -1;
            placeToolbar(); showToolBars(); updateTip(); update();
            updateInspector();
            return;
        }
    }
    if ((e->modifiers() & Qt::ControlModifier)
        && !(e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier))) {
        if (e->key() == Qt::Key_T && m_mode == Adjusting && !m_live && !editorActive()) {
            pinCapture();                        // Pin to screen (Ctrl+T).
            return;
        }
    }
    switch (e->key()) {
    case Qt::Key_Escape:
        if (m_live) { m_live = false; endAnnotationDrag(); }
        else if (editorActive()) commitTextEdit();
        else if (m_atool != ATool::None) setToolNone();
        else if (m_selAnn >= 0) { stopItemDrag(); m_selAnn = -1; updateToolOptions(); update(); }
        else close();
        break;
    case Qt::Key_Space:
        if (!e->isAutoRepeat()) m_spaceHeld = true;   // Space turns drags into moves.
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        if (!m_live && !editorActive() && m_mode == Adjusting)
            tuneWidth(1, mapFromGlobal(QCursor::pos()));
        break;
    case Qt::Key_Minus:
        if (!m_live && !editorActive() && m_mode == Adjusting)
            tuneWidth(-1, mapFromGlobal(QCursor::pos()));
        break;
    case Qt::Key_F:                              // Fill mode for shapes and the selection.
        if (e->modifiers() == Qt::NoModifier && !e->isAutoRepeat()
            && !m_live && !editorActive() && m_mode == Adjusting) {
            const ATool tool = optionsTool();
            if (tool == ATool::Rect || tool == ATool::Ellipse || tool == ATool::Marker)
                toggleFill();                    // Same toggle as the strip's fill button.
        }
        break;
    case Qt::Key_Delete:
        if (m_selAnn >= 0) deleteSelected();
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_mode == Adjusting && !m_live) pinCapture();
        break;
    case Qt::Key_C:
        if (e->modifiers() & Qt::ControlModifier) {
            if (m_mode == Adjusting && !m_live) copyCapture();
        } else if (!(e->modifiers() & (Qt::AltModifier | Qt::MetaModifier))
                   && m_inspector->isVisible() && !m_inspector->colorText().isEmpty()) {
            if (!copyTextToClipboard(m_inspector->colorText()))
                showOutputError(QStringLiteral("复制失败，剪贴板可能被占用，请重试"));
            else m_sizeTip->hide();
        }
        break;
    case Qt::Key_Z:
        if (e->modifiers() & Qt::ControlModifier) {
            if (e->modifiers() & Qt::ShiftModifier) redoAnnotation();
            else undoAnnotation();
        }
        break;
    case Qt::Key_Y:
        if (e->modifiers() & Qt::ControlModifier) redoAnnotation();
        break;
    case Qt::Key_Shift:
        if (!e->isAutoRepeat()) m_inspector->toggleFormat();
        break;
    case Qt::Key_S:
        if (e->modifiers() & Qt::ControlModifier && !m_live) saveCapture();
        else if (e->modifiers() == Qt::NoModifier && !e->isAutoRepeat())
            nudgeSelected(Qt::Key_S);            // WASD nudges the selection.
        break;
    default:
        if (e->modifiers() == Qt::NoModifier && !e->isAutoRepeat()
            && !m_live && !editorActive()) {
            if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
                nudgeSelected(e->key());        // Arrows / WASD move the selected item.
                return;
            }
            const int key = e->key();           // Single letters arm annotation tools.
            const ATool tool = key == Qt::Key_R ? ATool::Rect
                : key == Qt::Key_O ? ATool::Ellipse
                : key == Qt::Key_A ? ATool::Arrow
                : key == Qt::Key_P ? ATool::Pen
                : key == Qt::Key_H ? ATool::Marker
                : key == Qt::Key_T ? ATool::Text
                : key == Qt::Key_N ? ATool::Number
                : key == Qt::Key_M ? ATool::Mosaic
                : key == Qt::Key_E ? ATool::Eraser : ATool::None;
            if (tool != ATool::None)
                selectTool(tool == m_atool ? ATool::None : tool);   // Same key toggles off.
        }
        break;
    }
}

void CaptureOverlay::keyReleaseEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Space) m_spaceHeld = false;
}

void CaptureOverlay::startDetector()
{
    qRegisterMetaType<WindowTargets>("WindowTargets");
    m_detectionWatchdog.stop();
    m_lastHoverId = m_queryId = 0;
    m_detector = WindowDetector::create(reinterpret_cast<HWND>(winId()), m_detectionOptions);
    if (!m_detector) {
        m_queryPending = false;
        setWindowTargets(detectWindowRects(reinterpret_cast<HWND>(winId()), m_detectionOptions.excludedPrograms));
        return;
    }
    const QPointer<WindowDetector> detector = m_detector;
    connect(detector, &QThread::finished, detector, &QObject::deleteLater);
    connect(detector, &WindowDetector::gotTargets, this, [this, detector](const WindowTargets &targets, quint64 id) {
        // Arrives in query order from one worker; the newest table lands last.
        Q_UNUSED(id);
        if (detector == m_detector && isVisible()) setWindowTargets(targets);
    }, Qt::QueuedConnection);
    connect(detector, &WindowDetector::gotRect, this, [this, detector](const QRect &rect, quint64 id) {
        if (detector == m_detector) {
            if (m_queryPending) m_detectionWatchdog.start(500);
            handleDetectedRect(rect, id);
        }
    }, Qt::QueuedConnection);
    detector->start(QThread::LowestPriority);
}

void CaptureOverlay::setWindowTargets(const WindowTargets &windows)
{
    m_windowRects.clear();
    for (const DetectedWindow &win : windows) {
        QRect logical = logicalFromNative(m_frames, win.native);
        const QScreen *center = QGuiApplication::screenAt(logical.center());
        bool crosses = false;
        for (const QScreen *screen : QGuiApplication::screens())
            if (screen != center && screen->geometry().intersects(logical)) crosses = true;
        if (center && (win.maximized || !crosses)) logical &= center->geometry();
        if (logical.isEmpty()) continue;
        DetectedWindow target(localScreenRect(logical));
        target.hwnd = win.hwnd;
        target.program = win.program;
        for (const QRect &child : win.nativeChildren) {
            const QRect clipped = logical.intersected(logicalFromNative(m_frames, child));
            if (!clipped.isEmpty()) target.nativeChildren.append(localScreenRect(clipped));
        }
        m_windowRects.append(target);
    }
}

QRect CaptureOverlay::localScreenRect(const QRect &global) const
{
    if (global.isEmpty()) return {};
    const QPointF top = windowHandle()->mapFromGlobal(QPointF(global.topLeft()));
    const QPointF end = windowHandle()->mapFromGlobal(QPointF(global.x() + global.width(), global.y() + global.height()));
    return QRect(QPoint(qRound(top.x()), qRound(top.y())),
                 QSize(qRound(end.x()) - qRound(top.x()), qRound(end.y()) - qRound(top.y())));
}

QRect CaptureOverlay::globalScreenRect(const QRect &local) const
{
    if (local.isEmpty()) return {};
    const QPointF top = windowHandle()->mapToGlobal(QPointF(local.topLeft()));
    const QPointF end = windowHandle()->mapToGlobal(QPointF(local.x() + local.width(), local.y() + local.height()));
    return QRect(QPoint(qRound(top.x()), qRound(top.y())),
                 QSize(qRound(end.x()) - qRound(top.x()), qRound(end.y()) - qRound(top.y())));
}

QRect CaptureOverlay::nativeDetectionRect(const QRect &local) const
{
    if (local.isEmpty()) return {};
    const QRect global = globalScreenRect(local);
    for (const ScreenFrame &frame : m_frames)
        if (frame.logicalRect.contains(global.center())) {
            const double sx = double(frame.pixelRect.width()) / frame.logicalRect.width();
            const double sy = double(frame.pixelRect.height()) / frame.logicalRect.height();
            return QRect(qRound(frame.pixelRect.x() + (global.x() - frame.logicalRect.x()) * sx),
                         qRound(frame.pixelRect.y() + (global.y() - frame.logicalRect.y()) * sy),
                         qRound(global.width() * sx), qRound(global.height() * sy));
        }
    return {};
}

void CaptureOverlay::cancelDetection()
{
    if (m_detector) m_detector->cancel();
    m_detectionWatchdog.stop();
    m_queryId = 0;
    m_queryPending = m_waitingClick = false;
    m_pinAfterDetection = false;
}

void CaptureOverlay::requestDetection(const QPoint &point, const QRect &previous)
{
    if (m_drag == H_None && point == m_detectionPoint && previous.isEmpty() && m_queryId != 0) return;
    if (previous.isEmpty()) m_hoverHistory.clear();
    m_detectionPoint = point;
    if (!m_detector) { m_hover = windowAt(point); updateTip(); return; }
    const QRect nativePoint = nativeDetectionRect(QRect(point, QSize(1, 1)));
    if (nativePoint.isEmpty()) return;
    m_queryPending = true;
    m_expanding = !previous.isEmpty();
    m_queryId = m_detector->query(nativePoint.topLeft(), nativeDetectionRect(previous));
    if (!m_detectionWatchdog.isActive()) m_detectionWatchdog.start(500);
}

void CaptureOverlay::recoverDetector()
{
    const QPointer<WindowDetector> detector = m_detector;
    const quint64 id = m_queryId;
    if (!detector || !m_queryPending || !isVisible()) return;
    setWindowTargets(detectWindowRects(reinterpret_cast<HWND>(winId()), m_detectionOptions.excludedPrograms));
    bool identified = false;
    for (const DetectedWindow &win : m_windowRects)
        if (win.native.contains(m_detectionPoint) && !win.program.isEmpty()) {
            identified = true;
            if (!m_detectionOptions.disabledUiPrograms.contains(win.program, Qt::CaseInsensitive)) {
                m_detectionOptions.disabledUiPrograms.append(win.program);
                if (windowHung(win.hwnd)) {
                    QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
                    QStringList disabled = settings.value(QStringLiteral("Snip/crashes")).toStringList();
                    if (!disabled.contains(win.program, Qt::CaseInsensitive)) disabled.append(win.program);
                    settings.setValue(QStringLiteral("Snip/crashes"), disabled);
                }
            }
            break;
        }
    if (!identified) m_detectionOptions.elements = false;
    // A stuck provider must not hold up a click or block the next capture session.
    detector->stop();
    handleDetectedRect(nativeDetectionRect(windowAt(m_detectionPoint)), id);
    if (isVisible()) startDetector();
    else m_detector = nullptr;
}

void CaptureOverlay::updateHoverRegion(const QRect &oldRect, const QRect &newRect)
{
    QRect region;
    if (!newRect.isNull()) region = newRect.adjusted(-2, -2, 2, 2);
    if (!oldRect.isNull()) region |= oldRect.adjusted(-2, -2, 2, 2);
    if (!region.isNull()) update(region);
}

void CaptureOverlay::clearHover()
{
    if (m_hover.isNull()) return;
    const QRect old = m_hover;
    m_hover = {};
    updateHoverRegion(old, QRect());
}

void CaptureOverlay::handleDetectedRect(const QRect &native, quint64 id)
{
    if (!id || !isVisible()) return;
    if (id != m_queryId) {
        // Hover previews follow the cursor during fast slides: a result already superseded
        // by a newer query still paints where the cursor just was instead of freezing the
        // border until the mouse stops. Clicks and expansion keep the strict identity check.
        if (m_mode != Selecting || !m_queryId || m_drag != H_None
            || m_waitingClick || m_expanding || id <= m_lastHoverId) return;
        m_lastHoverId = id;
        const QRect stale = localScreenRect(logicalFromNative(m_frames, native));
        if (stale.isEmpty()) return;
        const QRect old = m_hover;
        m_hover = stale;
        updateTip();
        updateHoverRegion(old, stale);
        return;
    }
    m_lastHoverId = id;
    m_queryPending = false;
    m_detectionWatchdog.stop();
    QRect detected = localScreenRect(logicalFromNative(m_frames, native));
    if (native.isEmpty() && m_detectionOptions.windows) {
        const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(m_detectionPoint));
        if (screen) detected = localScreenRect(screen->availableGeometry());
        if (!detected.contains(m_detectionPoint)) detected = QRect(m_detectionPoint, QSize(1, 1));
    }
    if (m_expanding && detected == m_hover) {
        const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(m_detectionPoint));
        const QRect available = screen ? localScreenRect(screen->availableGeometry()) : QRect();
        if (available != m_hover && available.contains(m_hover)) detected = available;
    }
    if (!detected.isEmpty() && !detected.contains(m_detectionPoint)) return;
    if (m_drag == H_New || m_waitingClick) {
        m_pressWindow = detected;
        if (m_waitingClick) {
            m_waitingClick = false;
            m_sel = detected;
            if (m_sel.width() >= 4 && m_sel.height() >= 4) {
                m_mode = Adjusting;
                showToolBars();
                placeToolbar();
                if (m_pinAfterDetection) { m_pinAfterDetection = false; pinCapture(); return; }
            } else resetSelection();
            m_hover = {};
        }
    } else if (m_drag == H_None) {
        const QRect old = m_hover;
        m_hover = detected;
        if (m_expanding && (m_hoverHistory.isEmpty() || m_hoverHistory.last() != detected))
            m_hoverHistory.append(detected);
        updateTip();
        updateHoverRegion(old, m_hover);
        return;
    }
    updateTip();
    updateInspector();
    update();
}

void CaptureOverlay::updateInspector()
{
    const QPoint local = mapFromGlobal(QCursor::pos());
    if (!isVisible() || editorActive() || m_live || m_erasing || m_itemDrag != H_None
        || m_drag == H_Move
        || (m_mode == Adjusting && m_drag == H_None
            && (m_atool != ATool::None || m_selAnn >= 0 || !m_sel.contains(local)))
        || (m_tools->isVisible() && m_tools->geometry().contains(local))
        || (m_toolOptions->isVisible() && m_toolOptions->geometry().contains(local))) {
        m_inspector->hide();
        return;
    }
    POINT cursor;
    if (GetCursorPos(&cursor)) {
        const QPoint pixel(cursor.x, cursor.y);
        for (const ScreenFrame &frame : m_frames) {
            const QRect bounds = localScreenRect(frame.logicalRect);
            const QRect hole = !m_hover.isNull() ? m_hover : m_sel;
            const qreal sx = qreal(frame.pixelRect.width()) / bounds.width();
            const qreal sy = qreal(frame.pixelRect.height()) / bounds.height();
            const QRectF selectionPixels((hole.x() - bounds.x()) * sx, (hole.y() - bounds.y()) * sy,
                                         hole.width() * sx, hole.height() * sy);
            if (!m_inspector->setSample(frame, pixel, hole.isEmpty() ? QRectF() : selectionPixels))
                continue;
            m_inspector->move(inspectorPosition(local, m_inspector->size(), bounds));
            m_inspector->show();
            m_inspector->raise();
            m_sizeTip->raise();
            return;
        }
    }
    m_inspector->hide();
}

QPoint CaptureOverlay::inspectorPosition(const QPoint &cursor, const QSize &size, const QRect &bounds)
{
    int x = cursor.x() + 8, y = cursor.y() + 20;
    if (x + size.width() > bounds.right() + 1) x = cursor.x() - size.width() - 8;
    if (y + size.height() > bounds.bottom() + 1) y = cursor.y() - size.height() - 20;
    return QPoint(qBound(bounds.left(), x, qMax(bounds.left(), bounds.right() + 1 - size.width())),
                  qBound(bounds.top(), y, qMax(bounds.top(), bounds.bottom() + 1 - size.height())));
}

void CaptureOverlay::resetSelection()
{
    stopItemDrag();
    if (m_colorDialog) m_colorDialog->close();
    m_mode = Selecting;                         // m_hover is kept: the next mouse move refreshes it.
    m_sel = QRect();
    m_drag = H_None;
    m_tools->hide();
    m_toolOptions->hide();
    m_sizeTip->hide();
    setCursor(Qt::CrossCursor);
    updateInspector();
    update();
}

QRect CaptureOverlay::windowAt(const QPoint &p) const
{
    if (!m_detectionOptions.windows) return {};
    for (const DetectedWindow &w : m_windowRects) {   // Top-down z-order: first hit wins.
        if (!w.native.contains(p))
            continue;
        return m_detectionOptions.elements ? ::childAt(w, p) : w.native;
    }
    return QRect();
}

QVector<QRect> CaptureOverlay::handleRects(const QRect &r)
{
    const int s = 8;
    const QPoint c = r.center();
    return {
        QRect(r.topLeft() - QPoint(s/2, s/2), QSize(s, s)),
        QRect(r.topRight() - QPoint(s/2 - 1, s/2), QSize(s, s)),
        QRect(r.bottomLeft() - QPoint(s/2, s/2 - 1), QSize(s, s)),
        QRect(r.bottomRight() - QPoint(s/2 - 1, s/2 - 1), QSize(s, s)),
        QRect(r.left() - s/2, c.y() - s/2, s, s),
        QRect(r.right() - s/2 + 1, c.y() - s/2, s, s),
        QRect(c.x() - s/2, r.top() - s/2, s, s),
        QRect(c.x() - s/2, r.bottom() - s/2 + 1, s, s),
    };
}

CaptureOverlay::Handle CaptureOverlay::hitHandle(const QPoint &p) const
{
    if (m_mode != Adjusting || m_sel.isEmpty())
        return H_None;
    const QVector<QRect> hs = handleRects(m_sel);
    for (int i = 0; i < hs.size(); ++i)
        if (hs[i].adjusted(-2, -2, 2, 2).contains(p))
            return Handle(H_TL + i);
    if (m_sel.contains(p))
        return H_Move;
    if (p.x() < m_sel.left()) return p.y() < m_sel.top() ? H_TL : p.y() > m_sel.bottom() ? H_BL : H_L;
    if (p.x() > m_sel.right()) return p.y() < m_sel.top() ? H_TR : p.y() > m_sel.bottom() ? H_BR : H_R;
    if (p.y() < m_sel.top()) return H_T;
    if (p.y() > m_sel.bottom()) return H_B;
    return H_None;
}

Qt::CursorShape CaptureOverlay::cursorForHandle(Handle h)
{
    switch (h) {
    case H_TL: case H_BR: return Qt::SizeFDiagCursor;
    case H_TR: case H_BL: return Qt::SizeBDiagCursor;
    case H_L: case H_R:   return Qt::SizeHorCursor;
    case H_T: case H_B:   return Qt::SizeVerCursor;
    case H_Move:          return Qt::SizeAllCursor;
    default:              return Qt::CrossCursor;
    }
}

QRect CaptureOverlay::resizeBy(Handle h, const QPoint &pos)
{
    QRect r = m_pressSel;
    switch (h) {
    case H_TL:  r.setTopLeft(pos); break;
    case H_TR:  r.setTopRight(pos); break;
    case H_BL:  r.setBottomLeft(pos); break;
    case H_BR:  r.setBottomRight(pos); break;
    case H_L:   r.setLeft(pos.x()); break;
    case H_R:   r.setRight(pos.x()); break;
    case H_T:   r.setTop(pos.y()); break;
    case H_B:   r.setBottom(pos.y()); break;
    case H_Move: {
        const QPoint d = pos - m_pressPos;
        r.translate(d);
        r.moveTo(qBound(0, r.left(), width() - r.width()),
                 qBound(0, r.top(), height() - r.height()));
        return r;
    }
    default: break;
    }
    return QRect(QPoint(qMin(r.left(),r.right()),qMin(r.top(),r.bottom())),
                 QPoint(qMax(r.left(),r.right()),qMax(r.top(),r.bottom())));
}

QVector<int> CaptureOverlay::snapAnchorLines(bool horizontal) const
{
    QVector<int> out;
    for (const QScreen *s : QGuiApplication::screens()) {
        const QRect g = localScreenRect(s->geometry());
        if (horizontal)
            out << g.left() << g.right() + 1 << g.center().x();
        else
            out << g.top() << g.bottom() + 1 << g.center().y();
    }
    for (const DetectedWindow &w : m_windowRects) {  // Window edges as magnetic anchors.
        if (horizontal)
            out << w.native.left() << w.native.right() + 1;
        else
            out << w.native.top() << w.native.bottom() + 1;
    }
    return out;
}

void CaptureOverlay::applySnap(Handle h)
{
    m_snapX = m_snapY = -1;
    if (h == H_None || m_sel.isNull())
        return;
    const QVector<int> ax = snapAnchorLines(true), ay = snapAnchorLines(false);
    int (*nearest)(const QVector<int> &, int) = [](const QVector<int> &anchors, int v) {
        int best = v, dist = SnapThreshold + 1;
        for (int a : anchors) {
            const int d = qAbs(a - v);
            if (d < dist) { dist = d; best = a; }
        }
        return best;
    };
    if (h == H_New || h == H_Move) {
        // Whole rectangle: shift so that any edge lands on the nearest anchor.
        for (int axis = 0; axis < 2; ++axis) {
            const QVector<int> &anchors = axis ? ay : ax;
            const int e0 = axis ? m_sel.top() : m_sel.left();
            const int e1 = axis ? m_sel.bottom() + 1 : m_sel.right() + 1;
            int shift = 0, dist = SnapThreshold + 1, anchor = 0;
            for (int e : {e0, e1})
                for (int a : anchors) {
                    const int d = qAbs(a - e);
                    if (d < dist) { dist = d; shift = a - e; anchor = a; }
                }
            if (dist <= SnapThreshold) {
                if (axis) { m_snapY = anchor; m_sel.translate(0, shift); }
                else      { m_snapX = anchor; m_sel.translate(shift, 0); }
            }
        }
        return;
    }
    // Edge/corner resize: only the dragged edges snap.
    const bool left = h == H_L || h == H_TL || h == H_BL;
    const bool right = h == H_R || h == H_TR || h == H_BR;
    const bool top = h == H_T || h == H_TL || h == H_TR;
    const bool bottom = h == H_B || h == H_BL || h == H_BR;
    if (left) {
        const int v = nearest(ax, m_sel.left());
        if (v != m_sel.left()) { m_snapX = v; m_sel.setLeft(v); }
    }
    if (right) {
        const int v = nearest(ax, m_sel.right() + 1);
        if (v != m_sel.right() + 1) { m_snapX = v; m_sel.setRight(v - 1); }
    }
    if (top) {
        const int v = nearest(ay, m_sel.top());
        if (v != m_sel.top()) { m_snapY = v; m_sel.setTop(v); }
    }
    if (bottom) {
        const int v = nearest(ay, m_sel.bottom() + 1);
        if (v != m_sel.bottom() + 1) { m_snapY = v; m_sel.setBottom(v - 1); }
    }
    m_sel = m_sel.normalized();
}

void CaptureOverlay::updateTip()
{
    const QRect r = !m_hover.isNull() ? m_hover : m_sel;
    if (r.isNull()) { m_sizeTip->hide(); return; }
    m_sizeTip->setText(QString("%1 × %2").arg(r.width()).arg(r.height()));
    m_sizeTip->adjustSize();
    m_sizeTip->move(r.topLeft() - QPoint(0, m_sizeTip->height() + 4));
    m_sizeTip->show();
}

void CaptureOverlay::buildToolLayout()
{
    if (m_tools->layout()) return;
    auto *lay = new QHBoxLayout(m_tools);
    lay->setContentsMargins(1, 1, 1, 1);
    lay->setSpacing(0);
    for (int i = 0; i < m_toolButtons.size(); ++i) {
        if (i == 8 || i == 10) lay->addWidget(barSeparator());   // Tools | undo, redo | outputs.
        static_cast<QToolButton *>(m_toolButtons[i])->setFixedSize(32, 32);
        lay->addWidget(m_toolButtons[i]);
    }
    m_tools->adjustSize();
}

void CaptureOverlay::placeToolbar()
{
    const QRect old = m_tools->geometry().united(m_toolOptions->geometry());
    const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(m_sel.center()));
    const QRect avail = localScreenRect((screen ? screen : QGuiApplication::primaryScreen())->availableGeometry());
    const QSize main = m_tools->sizeHint();
    const QSize options = optionsTool() == ATool::None ? QSize(0, 0) : m_toolOptions->sizeHint();
    const int width = qMax(main.width(), options.width());
    const int height = main.height() + options.height();
    const int selRight = m_sel.right() + 1, selBottom = m_sel.bottom() + 1;
    const int x = qBound(avail.left(), selRight - width, avail.right() + 1 - width);
    const int y = selBottom + 6 + height <= avail.bottom() + 1 ? selBottom + 6
        : m_sel.top() - 6 - height >= avail.top() ? m_sel.top() - 6 - height
        : qMax(avail.top(), avail.bottom() + 1 - height - 8);
    m_tools->setGeometry(x + width - main.width(), y, main.width(), main.height());
    positionToolOptions();
    const QRect glass = m_toolOptions->isVisible()
        ? m_tools->geometry().united(m_toolOptions->geometry()) : m_tools->geometry();
    update(glass.united(old).adjusted(-1, -1, 1, 1));
}

void CaptureOverlay::positionToolOptions()                      // Option strip hugs the toolbar's bottom edge.
{
    if (!m_toolOptions->isVisible() && optionsTool() == ATool::None) return;
    const QRect bar = m_tools->geometry();
    const QSize hint = m_toolOptions->sizeHint();       // Strip width follows its own contents.
    m_toolOptions->setGeometry(qMax(0, qMin(bar.left(), bar.right() + 1 - hint.width())), bar.bottom() + 1,
                               hint.width(), hint.height());
}

void CaptureOverlay::hideToolBars()
{
    if (optionsTool() != ATool::None) return;     // Active tool options remain available throughout a drag.
    if (m_toolOptions->isVisible()) {
        update(m_toolOptions->geometry());
        m_toolOptions->hide();
    }
    if (m_tools->isVisible()) {
        update(m_tools->geometry());
        m_tools->hide();
    }
}

void CaptureOverlay::showToolBars()
{
    m_tools->show();
    if (optionsTool() != ATool::None) {
        positionToolOptions();
        m_toolOptions->show();
    }
}

QPixmap CaptureOverlay::cropped() const
{
    if (m_sel.isEmpty() || m_shot.isNull()) return {};
    const qreal scale = m_shot.devicePixelRatio();
    const QRect r = m_sel.normalized();
    QPixmap pix = m_shot.copy(QRect(qRound(r.x() * scale), qRound(r.y() * scale),
                                   qRound(r.width() * scale), qRound(r.height() * scale)));
    pix.setDevicePixelRatio(1);
    if (!m_anns.isEmpty() || editorActive()) {   // Unfinished text is also included in previews.
        QPainter p(&pix);
        paintAnnotations(p, -QPointF(r.topLeft()) * scale, scale, QRectF(r));
    }
    return pix;
}

void CaptureOverlay::ocrCapture()
{
    stopItemDrag();
    commitTextEdit();
    const QImage image = cropped().toImage();
    if (image.isNull()) { showOutputError(QStringLiteral("识别失败，请先选择有效截图区域")); return; }
    const QRect captureRect=globalScreenRect(m_sel.normalized());
    cancelDetection();
    if (m_colorDialog) m_colorDialog->close();
    m_tools->hide(); m_toolOptions->hide(); m_inspector->hide();
    hide();
    auto *dialog = showOcr(image,this,captureRect);
    m_ocrDialog=dialog;
    connect(dialog,&QDialog::finished,this,[this](int result) {
        if (result==QDialog::Accepted) {
            show(); placeToolbar(); showToolBars(); activateWindow(); setFocus();
            updateInspector();
        } else {
            QTimer::singleShot(0,this,&QWidget::close);
        }
    });
}

void CaptureOverlay::undoAnnotation()
{
    stopItemDrag();
    commitTextEdit();
    if (m_undoStack.isEmpty()) return;
    m_redoStack.append(annotationSnapshot());
    m_anns = m_undoStack.takeLast();
    m_selAnn = -1;
    normalizeNumbers();
    syncUndoButtons();
    updateToolOptions();
    update();
}

void CaptureOverlay::redoAnnotation()
{
    stopItemDrag();
    commitTextEdit();
    if (m_redoStack.isEmpty()) return;
    m_undoStack.append(annotationSnapshot());
    m_anns = m_redoStack.takeLast();
    m_selAnn = -1;
    normalizeNumbers();
    syncUndoButtons();
    updateToolOptions();
    update();
}

void CaptureOverlay::saveCapture()
{
    commitTextEdit();
    if (m_sel.isEmpty()) return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存截图"),
        dir + QStringLiteral("/pinshot.png"),
        QStringLiteral("PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)"));
    if (path.isEmpty()) {
        activateWindow();                       // Cancelled dialog: reclaim focus and keep the selection.
        setFocus();
        return;
    }
    saveCaptureTo(path);
}

bool CaptureOverlay::saveCaptureTo(const QString &path)
{
    if (!saveImageAtomic(cropped().toImage(), path)) {
        showOutputError(QStringLiteral("保存失败，请检查路径或磁盘空间后重试"));
        return false;
    }
    close();
    return true;
}

void CaptureOverlay::copyCapture()
{
    commitTextEdit();
    if (!copyImageToClipboard(cropped().toImage())) {
        showOutputError(QStringLiteral("复制失败，剪贴板可能被占用，请重试"));
        return;
    }
    close();
}

void CaptureOverlay::showOutputError(const QString &message)
{
    m_sizeTip->setText(message);
    tipPosition(m_sel.center());
    m_sizeTip->show();
    activateWindow();
    setFocus();
}

void CaptureOverlay::pinCapture()
{
    commitTextEdit();
    const QPixmap pix = cropped();
    if (pix.isNull()) {
        showOutputError(QStringLiteral("贴图失败，请确认选区有效并检查内存后重试"));
        return;
    }
    const QRect global = globalScreenRect(m_sel.normalized());
    close();
    emit grabbed(pix, global);
}

void CaptureOverlay::longCapture()
{
    commitTextEdit();
    const QRect global = globalScreenRect(m_sel);
    close();
    emit longRequested(global);
}
