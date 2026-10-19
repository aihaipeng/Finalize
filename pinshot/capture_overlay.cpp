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
#include <QGridLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QStandardPaths>
#include <QCursor>
#include <QMessageBox>
#include <QInputDialog>
#include <QTimer>
#include <QVariantAnimation>
#include <QEventLoop>
#include <QThread>
#include <QSettings>
#include <QPointer>
#include <QCloseEvent>
#include <qt_windows.h>
#include <imm.h>

CaptureOverlay::CaptureOverlay(const QVector<ScreenFrame> &frames,
                        const WindowTargets &nativeWindows,
                        const DetectionOptions &options, bool liveDetection, bool pinEdit, QWidget *embedParent)
    : m_frames(frames), m_detectionOptions(options), m_pinEdit(pinEdit), m_embedded(embedParent != nullptr)
{
    m_liveDetection = liveDetection;
    if (m_embedded) {
        // A child canvas inside the pin: no session chrome, no freeze, no
        // foreground claim. The single frame is the pin's current pixels.
        setParent(embedParent);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setGeometry(QRect(QPoint(0, 0), embedParent->size()));
        m_shot = QPixmap(frames.isEmpty() ? QSize(1, 1) : frames.first().image.size());
        m_shot.fill(Qt::transparent);
        if (!frames.isEmpty()) {
            QPainter painter(&m_shot);
            painter.drawImage(QPoint(0, 0), frames.first().image);
        }
    } else {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_DeleteOnClose);
    // Let the hold-to-refresh button reveal the live desktop without repainting the frozen frame.
    setAttribute(Qt::WA_TranslucentBackground);
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

    QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
    m_roundEnabled = settings.value(QStringLiteral("Capture/roundEnabled"), false).toBool();
    m_roundRadius = qBound(0, settings.value(QStringLiteral("Capture/roundRadius"), 0).toInt(), 60);
    m_sizeUnit = settings.value(QStringLiteral("Capture/sizeUnit"), QStringLiteral("px")).toString()
                         .compare(QStringLiteral("dp"), Qt::CaseInsensitive) == 0 ? SizeUnit::Dp : SizeUnit::Px;
    m_frameSource = [] { return freezeScreens(); };
    buildShot(m_frames);
    }
    m_inspector = new PixelInspector(this);

    m_sizeTip = new QLabel(this);
    m_sizeTip->setStyleSheet(
        "background:#f2f2f2;color:#323232;padding:2px 6px;border:1px solid #2080f0;font-size:12px;");
    m_sizeTip->setCursor(Qt::PointingHandCursor);
    m_sizeTip->setAccessibleName(QStringLiteral("自定义截图"));
    m_sizeTip->setAccessibleDescription(QStringLiteral("点击调整区域大小、尺寸单位和长宽比"));
    m_sizeTip->installEventFilter(this);
    m_sizeTip->hide();

    // The overlay paints matching solid backgrounds for both toolbar containers.
    qApp->installEventFilter(this);                  // Swallows hover tooltips of every capture control.
    m_tools = new QWidget(this);
    m_tools->installEventFilter(this);
    m_tools->setCursor(Qt::ArrowCursor);
    // Dark bar language: flat 31px buttons, translucent zinc hover, accent glyph when checked, 1px dividers.
    const QString barQss = QStringLiteral(
        "QToolButton{color:#fcfcfc;background:transparent;border:none;border-radius:4px;padding:5px;}"
        "QToolButton:hover{background-color:#26262a;}"
        "QToolButton:pressed{background-color:#27272a;}"
        "QToolButton:checked{background:transparent;}"
        "QToolButton:checked:hover{background-color:#26262a;}"
        "QFrame#barSeparator{background-color:#232326;margin:0px 6px;border:none;}");
    m_tools->setStyleSheet(barQss);
    // Capture output actions; a pin edit session replaces them with one
    // confirm button that applies the result back to the pin.
    QList<QWidget *> outputButtons;
    if (!pinEdit) {
        auto *save = barButton(Icon::Save, QStringLiteral("保存到文件 (Ctrl+S)"));
        auto *copy = barButton(Icon::Copy, QStringLiteral("复制到剪贴板 (Ctrl+C)"));
        auto *pin = barButton(Icon::Pin, QStringLiteral("贴在屏幕上 (Enter / 双击)"));
        auto *scroll = barButton(Icon::LongShot, QStringLiteral("长截图：手动滚动拼接长图"));
        auto *ocr = barButton(Icon::Ocr, QStringLiteral("文字识别 (OCR)：截图与文本对照"));
        ocr->setObjectName(QStringLiteral("ocrButton"));
        ocr->setAccessibleName(QStringLiteral("文字识别 OCR"));
        const QString ocrMissing = OcrEngine::assetError(QCoreApplication::applicationDirPath());
        if (!ocrMissing.isEmpty()) { ocr->setEnabled(false); ocr->setToolTip(ocrMissing); }
        auto *translate = barButton(Icon::Translate, QStringLiteral("翻译：识别并翻译为中文"));
        translate->setAccessibleName(QStringLiteral("翻译"));
        if (!ocrMissing.isEmpty()) { translate->setEnabled(false); translate->setToolTip(ocrMissing); }
        auto *barcode = barButton(Icon::Barcode, QStringLiteral("识别二维码/条形码"));
        barcode->setObjectName(QStringLiteral("barcodeButton"));
        barcode->setAccessibleName(QStringLiteral("识别二维码/条形码"));
        m_roundButton = barButton(Icon::RoundRect, QStringLiteral("圆角截图"));
        m_roundButton->setObjectName(QStringLiteral("roundButton"));
        m_roundButton->setAccessibleName(QStringLiteral("圆角截图"));
        m_roundButton->setCheckable(true);
        m_roundButton->setChecked(m_roundEnabled);
        connect(m_roundButton, &QToolButton::clicked, this, [this](bool checked) {
            m_roundEnabled = checked;
            applyRoundSettings(true);
            if (checked) showRadiusPopup();
            else if (m_radiusPopup) m_radiusPopup->close();
            update();
        });
        m_refreshButton = barButton(Icon::Refresh, QStringLiteral("刷新截图（按住持续刷新）"));
        m_refreshButton->setObjectName(QStringLiteral("refreshButton"));
        m_refreshButton->setAccessibleName(QStringLiteral("刷新截图"));
        m_refreshButton->setCheckable(true);
        m_refreshAnimation = new QVariantAnimation(m_refreshButton);
        m_refreshAnimation->setDuration(1000);
        m_refreshAnimation->setStartValue(0.0);
        m_refreshAnimation->setEndValue(360.0);
        m_refreshAnimation->setLoopCount(-1);
        connect(m_refreshAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_refreshSpinAngle = value.toReal();
            updateRefreshIcon();
        });
        connect(m_refreshButton, &QToolButton::pressed, this, &CaptureOverlay::beginDesktopPreview);
        connect(m_refreshButton, &QToolButton::released, this, [this] {
            if (m_refreshAnimation) m_refreshAnimation->stop();
            m_refreshSpinAngle = 0;
            m_refreshButton->setIcon(barIcon(Icon::Refresh));
            m_refreshButton->setChecked(false);
            if (!m_desktopPreview) return;
            m_desktopPreview = false;
            refreshScreenshot();
        });
        connect(m_refreshButton, &QToolButton::clicked, this, [this] { m_refreshButton->setChecked(false); });
        connect(save, &QToolButton::clicked, this, &CaptureOverlay::saveCapture);
        connect(copy, &QToolButton::clicked, this, &CaptureOverlay::copyCapture);
        connect(pin, &QToolButton::clicked, this, &CaptureOverlay::pinCapture);
        connect(scroll, &QToolButton::clicked, this, &CaptureOverlay::longCapture);
        connect(ocr, &QToolButton::clicked, this, &CaptureOverlay::ocrCapture);
        connect(translate, &QToolButton::clicked, this, &CaptureOverlay::translateCapture);
        connect(barcode, &QToolButton::clicked, this, &CaptureOverlay::barcodeCapture);
        outputButtons = {m_roundButton, scroll, ocr, translate, barcode, pin, m_refreshButton, save, copy};
    }
    // Instant annotation tools: draw inside the committed selection; every capture
    // action composes them in. One shape button covers rect / ellipse; per-tool
    // options (sub-shape, arrow style, colors, text weight) live on the strip below.
    auto *undo = barButton(Icon::Undo, QStringLiteral("撤销 (Ctrl+Z)"));
    connect(undo, &QToolButton::clicked, this, &CaptureOverlay::undoAnnotation);
    m_undoButton = undo;
    auto *redo = barButton(Icon::Redo, QStringLiteral("重做 (Ctrl+Y)"));
    connect(redo, &QToolButton::clicked, this, &CaptureOverlay::redoAnnotation);
    m_redoButton = redo;
    syncUndoButtons();                             // Empty stacks: both start greyed out.
    m_shapeButton = new QToolButton;               // One button for both shapes.
    m_shapeButton->setIcon(barIcon(Icon::Rect));
    m_shapeButton->setIconSize(QSize(Bar::icon, Bar::icon));
    m_shapeButton->setToolTip(QStringLiteral("形状：矩形/椭圆 (R=矩形 / O=椭圆)"));
    m_shapeButton->setCheckable(true);
    m_shapeButton->setCursor(Qt::PointingHandCursor);
    connect(m_shapeButton, &QToolButton::clicked, this, [this] {
        const bool armed = m_atool == ATool::Rect || m_atool == ATool::Ellipse;
        selectTool(armed ? ATool::None : m_shapeEllipse ? ATool::Ellipse : ATool::Rect);   // Reclick disarms.
        setCursor(Qt::CrossCursor);
        update();
    });
    m_toolBtn.fill(nullptr, int(ATool::Eraser) + 1);
    m_toolBtn[int(ATool::Rect)] = m_toolBtn[int(ATool::Ellipse)] = m_shapeButton;
    struct { ATool tool; Icon icon; const char *tip; } const tools[] = {
        {ATool::Arrow, Icon::Arrow, "箭头 (A)"},
        {ATool::Pen, Icon::Pen, "画笔 (P)"},
        {ATool::Marker, Icon::Marker, "荧光笔 (H)"},
        {ATool::Text, Icon::Text, "文字 (T)"},
        {ATool::Number, Icon::Number, "序号 (N)"},
        {ATool::Mosaic, Icon::Mosaic, "马赛克 (M)"},
        {ATool::Eraser, Icon::Eraser, "橡皮 (E)"},
    };
    for (const auto &t : tools) {
        auto *button = barButton(t.icon, QString::fromUtf8(t.tip));
        button->setCheckable(true);
        const ATool tool = t.tool;
        connect(button, &QToolButton::clicked, this, [this, tool] {
            selectTool(tool == m_atool ? ATool::None : tool);   // Reclick disarms, like the shortcut key.
            update();
        });
        m_toolBtn[int(tool)] = button;
    }
    // Drawing tools | undo, redo | capture actions ending with pin, save, copy.
    if (pinEdit) {
        auto *done = barButton(Icon::Check, QStringLiteral("完成编辑 (Enter)"));
        connect(done, &QToolButton::clicked, this, &CaptureOverlay::pinCapture);
        m_toolButtons = {m_shapeButton, m_toolBtn[int(ATool::Arrow)], m_toolBtn[int(ATool::Pen)],
                         m_toolBtn[int(ATool::Marker)], m_toolBtn[int(ATool::Mosaic)], m_toolBtn[int(ATool::Text)],
                         m_toolBtn[int(ATool::Number)], m_toolBtn[int(ATool::Eraser)], undo, redo, done};
    } else {
        m_toolButtons = {m_shapeButton, m_toolBtn[int(ATool::Arrow)], m_toolBtn[int(ATool::Pen)],
                         m_toolBtn[int(ATool::Marker)], m_toolBtn[int(ATool::Mosaic)], m_toolBtn[int(ATool::Text)],
                         m_toolBtn[int(ATool::Number)], m_toolBtn[int(ATool::Eraser)], undo, redo};
        m_toolButtons += outputButtons;
    }
    for (QWidget *button : m_toolButtons) button->setFocusPolicy(Qt::NoFocus);

    const QString optionsQss = barQss + QStringLiteral(
        "QLabel{color:#fcfcfc;font-size:12px;border:none;}"
        "QComboBox{color:#fcfcfc;background:#060607;border:none;border-radius:12px;"
        "padding:1px 4px 1px 12px;font-size:12px;}"
        "QComboBox:hover{background:#0c0c0e;}"
        "QComboBox::drop-down{border:none;width:22px;background:transparent;}"
        "QComboBox::down-arrow{image:none;width:0px;height:0px;}"
        "QComboBox QAbstractItemView{outline:0px;color:#fcfcfc;background:#262626;border:1px solid #3c3c3c;"
        "padding:4px;selection-background-color:#3388ff;selection-color:#fcfcfc;}"
        "QLineEdit{color:#fcfcfc;background:transparent;border:none;"
        "selection-background-color:#3388ff;selection-color:#fcfcfc;}");
    if (m_embedded) {
        // The option strip detaches as a small top-level rounded bar;
        // the pin positions it below its own toolbar.
        m_toolOptions = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        m_toolOptions->setObjectName(QStringLiteral("pinStrip"));
        m_toolOptions->setAttribute(Qt::WA_StyledBackground);
        m_toolOptions->setStyleSheet(QStringLiteral(
            "#pinStrip{background:#18181b;border:none;border-radius:8px;}") + optionsQss);
    } else {
        m_toolOptions = new QWidget(this);
        m_toolOptions->setStyleSheet(optionsQss);
    }
    m_toolOptions->installEventFilter(this);
    m_toolOptions->setCursor(Qt::ArrowCursor);
    m_toolOptions->hide();
    buildToolLayout();
    m_tools->adjustSize();
    m_tools->hide();

    if (m_embedded) {
        // Whole canvas is the committed selection; drawing works immediately.
        presetSelection(rect(), ATool::None);
        return;
    }
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

void CaptureOverlay::buildShot(const QVector<ScreenFrame> &frames)
{
    qreal scale = 1;
    for (const ScreenFrame &frame : frames)
        scale = qMax(scale, frame.image.devicePixelRatio() > 1 ? frame.image.devicePixelRatio()
                        : qreal(frame.image.width()) / frame.logicalRect.width());
    m_shot = QPixmap(qRound(width() * scale), qRound(height() * scale));
    m_shot.setDevicePixelRatio(scale);
    m_shot.fill(Qt::black);
    QPainter painter(&m_shot);
    for (const ScreenFrame &frame : frames) {
        const QPointF local = windowHandle()->mapFromGlobal(QPointF(frame.logicalRect.topLeft()));
        const QSizeF size = frame.image.devicePixelRatio() > 1
            ? frame.image.deviceIndependentSize() : QSizeF(frame.logicalRect.size());
        painter.drawImage(QRectF(local, size), frame.image);
    }
}

CaptureOverlay::~CaptureOverlay()
{
    if (m_detector) m_detector->stop();
    if (m_embedded) delete m_toolOptions;   // The detached strip has no parent.
}

void CaptureOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const auto paintBars = [&] {
        for (QWidget *glass : {m_tools, m_toolOptions}) {   // Rounded dark surface with a faint light rim.
            if (m_embedded) break;                   // Detached or unused bars paint themselves.
            if (!glass->isVisible()) continue;
            const QRectF bar = glass->geometry();
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 17));    // 1px light rim lifts the bar off dark captures.
            p.drawRoundedRect(bar.adjusted(-1, -1, 1, 1), Bar::radius + 1, Bar::radius + 1);
            p.setBrush(Bar::surface);
            p.drawRoundedRect(bar, Bar::radius, Bar::radius);
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    };
    if (m_desktopPreview) {                       // Hold-to-refresh: keep the frame over the live desktop.
        if (!m_sel.isNull()) {
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(QColor("#2080f0"), 1));
            p.setBrush(QBrush(Qt::NoBrush));
            if (m_roundEnabled && m_roundRadius > 0 && !m_embedded) p.drawPath(selectionPath(m_sel));
            else p.drawRect(QRectF(m_sel).adjusted(0.5, 0.5, -0.5, -0.5));
            p.setRenderHint(QPainter::Antialiasing, false);
            if (!m_pinEdit)
                for (const QRect &h : handleRects(m_sel))
                    paintHandle(p, h.center() + QPoint(1, 1));
        }
        paintBars();
        return;
    }
    // Embedded: a transparent layer of marks only; the pin paints its own
    // pixels, border and text markups underneath, unchanged.
    if (!m_embedded) p.drawPixmap(0, 0, m_shot);   // Frozen frame covers the virtual desktop.
    // The mask hole tracks the detected target, falling back to the committed selection.
    const QRect hole = !m_hover.isNull() ? m_hover : m_sel;
    if (hole.isNull()) {
        p.fillRect(rect(), QColor(0, 0, 0, 60));
        return;
    }
    if (!m_pinEdit) {                           // Pin editing draws on the pin as-is:
        QPainterPath mask;                      // no dim mask and no selection frame.
        mask.setFillRule(Qt::OddEvenFill);
        mask.addRect(QRectF(rect()));
        mask.addPath(selectionPath(hole));
        p.fillPath(mask, QColor(0, 0, 0, 120));
        p.setPen(QPen(QColor("#2080f0"), 1));
        p.setBrush(QBrush(Qt::NoBrush));
        if (m_roundEnabled && m_roundRadius > 0 && !m_embedded) {
            p.setRenderHint(QPainter::Antialiasing);
            p.drawPath(selectionPath(hole));
            p.setRenderHint(QPainter::Antialiasing, false);
        } else {
            p.drawRect(QRectF(hole).adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
    if (m_mode == Adjusting && !m_sel.isNull()) {   // Annotations clip to the selection shape.
        p.save();
        p.setClipPath(selectionPath(m_sel), Qt::IntersectClip);
        paintAnnotations(p, QPointF(0, 0), 1.0, QRectF(m_sel), false);
        p.restore();
    }
    if (m_live && regionItem(m_ann))              // Thin frame shows the dragged region.
        paintMosaicBorder(p, m_ann, false);
    if (m_mode == Adjusting && !m_live
        && (editorActive() || (m_selAnn >= 0 && m_selAnn < m_anns.size()
                              && !m_anns[m_selAnn].mosaicBrush
                              && !(m_anns[m_selAnn].tool == ATool::Eraser && !m_anns[m_selAnn].fill)
                              ))) {   // The freshly drawn item keeps its handles until deselected.
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
    if (m_mode == Adjusting && !m_sel.isNull() && m_hover.isNull() && !m_pinEdit)  // Eight resize handles.
        for (const QRect &h : handleRects(m_sel))
            paintHandle(p, h.center() + QPoint(1, 1));
    if (m_pinEdit && !m_embedded && !m_sel.isNull()) {   // The pin keeps its resting hairline; overlay chrome, never baked.
        p.setPen(QPen(QColor(0, 0, 0, 90), 1));
        p.drawRect(m_sel.adjusted(0, 0, -1, -1));
    }
    paintBars();
}

void CaptureOverlay::mousePressEvent(QMouseEvent *e)
{
    cancelDetection();
    m_nudging = false;
    const bool wasEditing = editorActive();
    if (wasEditing) commitTextEdit();           // Canvas clicks finish input; controls keep it live.
    if (e->button() == Qt::RightButton) {       // Right click ends input, then the ladder.
        m_eatContextMenu = m_live || wasEditing || m_selAnn >= 0 || m_atool != ATool::None;
        stopItemDrag();
        if (m_live) { m_live = false; commitAnnotation(); }
        else if (wasEditing) return;
        else if (m_selAnn >= 0) { m_selAnn = -1; updateToolOptions(); update(); }
        else if (m_atool != ATool::None) return;   // An armed tool stays; reclicking its button disarms it.
        else if (m_mode == Adjusting) {
            if (m_embedded) return;              // No session to reset; annotations simply stay.
            if (m_pinEdit) close(); else resetSelection();
        } else { close(); }
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
    const bool hadItem = m_selAnn >= 0;
    m_pressPos = e->pos();
    m_dragStarted = false;
    m_inspector->hide();
    if (m_mode == Adjusting && !m_spaceHeld
        && (m_atool == ATool::Mosaic || m_atool == ATool::Eraser || m_atool == ATool::None)) {
        const bool regionTool = m_atool == ATool::None || (m_atool == ATool::Mosaic ? !m_mosaicBrush : m_eraserRect);
        const auto ownRegion = [this](const Annotation &ann) {   // An armed tool only grabs its own regions.
            return regionItem(ann) && (m_atool == ATool::None || ann.tool == m_atool);
        };
        if (regionTool && m_selAnn >= 0 && m_selAnn < m_anns.size() && ownRegion(m_anns[m_selAnn])) {
            const Annotation &ann = m_anns[m_selAnn];
            const Handle handle = annHandleAt(ann, e->pos());
            if (handle != H_None || annotationHit(ann, mapToLocal(ann, e->pos()))) {
                m_itemDrag = handle == H_None ? H_Move : handle;
                m_pressAnn = ann;
                updateAnnotationCursor(e->pos());
                return;
            }
        }
        const int hit = regionTool ? annotationAt(e->pos()) : -1;
        if (hit >= 0 && ownRegion(m_anns[hit])) {
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
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()
        && (m_anns[m_selAnn].tool == ATool::Mosaic || m_anns[m_selAnn].tool == ATool::Eraser)) {
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
    if (m_mode == Adjusting && m_atool != ATool::None && !m_spaceHeld
        && m_selAnn >= 0 && m_selAnn < m_anns.size()) {   // The fresh item stays adjustable with its tool armed.
        const Handle h = annHandleAt(m_anns[m_selAnn], e->pos());
        if (h != H_None || annotationAt(e->pos()) == m_selAnn) {
            m_itemDrag = h == H_None ? H_Move : h;
            m_pressAnn = m_anns[m_selAnn];
            updateAnnotationCursor(e->pos());
            return;
        }
    }
    if (m_mode == Adjusting && m_atool != ATool::None
        && !m_spaceHeld && hitHandle(e->pos()) == H_Move) { // A tool acts inside the selection.
        if (m_atool == ATool::Text) { beginTextEdit(e->pos()); return; }
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
    const QVector<QRect> selHandles = handleRects(m_sel);
    if (m_mode == Adjusting && m_atool != ATool::None && !m_spaceHeld && !m_sel.contains(e->pos())
        && std::none_of(selHandles.cbegin(), selHandles.cend(),
                        [e](const QRect &h) { return h.contains(e->pos()); })) {
        if (hadItem) m_selAnn = -1;               // With a tool armed an outside click only finishes the item.
        updateToolOptions();
        update();
        return;
    }
    if (m_mode == Adjusting) {
        if (m_embedded) {   // Nothing to draw on: the press moves the pin itself.
            m_fwdDrag = true;
            QMouseEvent fwd(e->type(), e->localPos(), e->windowPos(), e->screenPos(),
                            e->button(), e->buttons(), e->modifiers());
            QApplication::sendEvent(parent(), &fwd);
            return;
        }
        if (m_pinEdit) return;   // The pin canvas is fixed: no re-selecting, moving or resizing it.
        m_drag = hitHandle(e->pos());
        m_pressSel = m_sel;
        if (m_drag != H_None && m_drag != H_Move && !m_sel.contains(e->pos())) {
            m_sel = resizeBy(m_drag, e->pos());  // Outside presses extend the existing edge or corner.
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
    if (m_fwdDrag && (e->buttons() & Qt::LeftButton)) {   // A forwarded pin drag continues.
        QMouseEvent fwd(e->type(), e->localPos(), e->windowPos(), e->screenPos(),
                        e->button(), e->buttons(), e->modifiers());
        QApplication::sendEvent(parent(), &fwd);
        return;
    }
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
    updateInspector();
    updateTip();
    update();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_fwdDrag) {                              // End of a forwarded pin drag.
        m_fwdDrag = false;
        QMouseEvent fwd(e->type(), e->localPos(), e->windowPos(), e->screenPos(),
                        e->button(), e->buttons(), e->modifiers());
        QApplication::sendEvent(parent(), &fwd);
        return;
    }
    if (m_live && m_ann.tool == ATool::Arrow && m_ann.lineStyle == 2) {
        // A drag on the first press draws one arrow; otherwise vertices come from clicks.
        if (m_ann.stroke.size() == 2
            && (e->pos() - m_ann.stroke.first().toPoint()).manhattanLength() >= QApplication::startDragDistance()) {
            m_ann.stroke << m_ann.stroke.last();   // Keep the dragged end; commit drops the rubber band.
            m_live = false;
            commitAnnotation();
        }
        return;
    }
    if (m_live) {                               // Commit the annotation; the tool stays active.
        m_live = false;
        commitAnnotation();
        return;
    }
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
    if (m_embedded && !editorActive() && m_atool == ATool::None && m_selAnn < 0) {
        // Nothing armed and nothing selected: the wheel belongs to the pin
        // (zoom / opacity), as it did before any annotation existed.
        QApplication::sendEvent(parent(), e);
        return;
    }
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
    if (m_customSizePopup) m_customSizePopup->close();
    cancelDetection();
    if (m_detector) m_detector->stop();
    QWidget::closeEvent(e);
}

void CaptureOverlay::contextMenuEvent(QContextMenuEvent *e)
{
    // A right click that finished or deselected an item, or hit an armed tool, opens no menu;
    // otherwise the embedding pin shows its own.
    if (m_eatContextMenu || m_atool != ATool::None) e->accept(); else e->ignore();
    m_eatContextMenu = false;
}

void CaptureOverlay::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (editorActive()) return;                  // A text double-click never pins the capture.
    if (m_live && m_ann.tool == ATool::Arrow && m_ann.lineStyle == 2) return;   // Its first press already placed the vertex.
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
    else if (m_embedded) {
        QMouseEvent fwd(e->type(), e->localPos(), e->windowPos(), e->screenPos(),
                        e->button(), e->buttons(), e->modifiers());
        QApplication::sendEvent(parent(), &fwd);   // The pin keeps its double-click actions.
    }
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
        if (m_lockAspect && m_sel.height() > 0) m_aspect = qreal(m_sel.width()) / m_sel.height();
        m_mode = Adjusting;
        m_hover = {};
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
                const QPoint local = windowHandle() ? windowHandle()->mapFromGlobal(QPointF(QCursor::pos())).toPoint()
                                                    : mapFromGlobal(QCursor::pos());
                QMouseEvent move(QEvent::MouseMove,local,QCursor::pos(),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
                mouseMoveEvent(&move);
            } else updateInspector();
            return;
        }
        if (arrow && m_mode == Adjusting && m_drag == H_None && !m_sel.isEmpty()
            && movementModifiers == Qt::NoModifier && !m_pinEdit) {   // The pin canvas stays put.
            cancelDetection();
            m_sel.translate(delta);
            m_sel.moveTo(qBound(0,m_sel.x(),width()-m_sel.width()),qBound(0,m_sel.y(),height()-m_sel.height()));
            placeToolbar(); showToolBars(); updateTip(); update();
            updateInspector();
            return;
        }
    }
    if ((e->modifiers() & Qt::ControlModifier)
        && !(e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier))) {
        if (e->key() == Qt::Key_T && m_mode == Adjusting && !m_live && !editorActive()
            && !m_embedded) {                      // Embedded canvas has no capture outputs.
            pinCapture();                        // Pin to screen (Ctrl+T).
            return;
        }
    }
    switch (e->key()) {
    case Qt::Key_F5:
        if (!m_embedded && !m_pinEdit) refreshScreenshot();
        break;
    case Qt::Key_Escape:
        if (m_live && m_ann.tool == ATool::Arrow && m_ann.lineStyle == 2) break;   // Only a right click ends it.
        if (m_live) { m_live = false; endAnnotationDrag(); }
        else if (editorActive()) commitTextEdit();
        else if (m_embedded) {                    // The pin owns the rest: hide its bar, then close.
            QKeyEvent fwd(e->type(), e->key(), e->modifiers(), e->text(), e->isAutoRepeat(), 1);
            QApplication::sendEvent(parent(), &fwd);
        }
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
        if (m_mode == Adjusting && !m_live && !m_embedded) pinCapture();
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
    if (!windowHandle()) return QRect(mapFromGlobal(global.topLeft()), global.size());   // Embedded child canvas.
    const QPointF top = windowHandle()->mapFromGlobal(QPointF(global.topLeft()));
    const QPointF end = windowHandle()->mapFromGlobal(QPointF(global.x() + global.width(), global.y() + global.height()));
    return QRect(QPoint(qRound(top.x()), qRound(top.y())),
                 QSize(qRound(end.x()) - qRound(top.x()), qRound(end.y()) - qRound(top.y())));
}

QRect CaptureOverlay::globalScreenRect(const QRect &local) const
{
    if (local.isEmpty()) return {};
    if (!windowHandle()) return QRect(mapToGlobal(local.topLeft()), local.size());   // Embedded child canvas.
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
    if (!isVisible() || editorActive() || m_live || m_itemDrag != H_None
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
    m_lockAspect = false;
    if (m_customLock) {
        const QSignalBlocker block(m_customLock);
        m_customLock->setChecked(false);
    }
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
    const qreal aspect = (m_lockAspect && m_pressSel.width() > 0 && m_pressSel.height() > 0)
        ? qreal(m_pressSel.width()) / m_pressSel.height() : 0;
    if (aspect <= 0)
        return QRect(QPoint(qMin(r.left(),r.right()),qMin(r.top(),r.bottom())),
                     QPoint(qMax(r.left(),r.right()),qMax(r.top(),r.bottom())));
    int w = r.width(), height = r.height();
    const QPoint center = m_pressSel.center();
    switch (h) {
    case H_TL: case H_TR: case H_BL: case H_BR: {
        // One fixed opposite corner; both edges grow together at the current ratio.
        w = qMax(1, w);
        height = qMax(1, height);
        if (w / aspect >= height) height = qMax(1, qRound(w / aspect));
        else w = qMax(1, qRound(height * aspect));
        const bool leftEdge = h == H_TL || h == H_BL;
        const bool topEdge = h == H_TL || h == H_TR;
        const int x = leftEdge ? m_pressSel.right() - w + 1 : m_pressSel.left();
        const int y = topEdge ? m_pressSel.bottom() - height + 1 : m_pressSel.top();
        return QRect(x, y, w, height);
    }
    case H_L: case H_R:
        w = qMax(1, w);
        height = qMax(1, qRound(w / aspect));
        return QRect(h == H_R ? m_pressSel.left() : m_pressSel.right() - w + 1,
                     center.y() - (height - 1) / 2, w, height);
    case H_T: case H_B:
        height = qMax(1, height);
        w = qMax(1, qRound(height * aspect));
        return QRect(center.x() - (w - 1) / 2,
                     h == H_B ? m_pressSel.top() : m_pressSel.bottom() - height + 1, w, height);
    default:
        break;
    }
    return QRect(QPoint(qMin(r.left(),r.right()),qMin(r.top(),r.bottom())),
                 QPoint(qMax(r.left(),r.right()),qMax(r.top(),r.bottom())));
}

void CaptureOverlay::updateTip()
{
    if (m_pinEdit) { m_sizeTip->hide(); return; }   // Size label is capture chrome.
    const QRect r = !m_hover.isNull() ? m_hover : m_sel;
    if (r.isNull()) { m_sizeTip->hide(); return; }
    const QSize shown = sizeInUnit(r, m_sizeUnit);
    QString text = QStringLiteral("%1 × %2 %3").arg(shown.width()).arg(shown.height())
                       .arg(m_sizeUnit == SizeUnit::Dp ? QStringLiteral("dp") : QStringLiteral("px"));
    if (m_lockAspect && r == m_sel) text += QStringLiteral(" · 锁定比例");
    m_sizeTip->setText(text);
    m_sizeTip->adjustSize();
    m_sizeTip->move(r.topLeft() - QPoint(0, m_sizeTip->height() + 4));
    m_sizeTip->show();
    if (m_customSizePopup && m_customSizePopup->isVisible() && !m_sel.isNull() && !m_updatingCustom) {
        QWidget *focus = QApplication::focusWidget();
        if (!focus || !m_customSizePopup->isAncestorOf(focus))
            updateCustomSizeInputs();
    }
}

void CaptureOverlay::buildToolLayout()
{
    if (m_tools->layout()) return;
    auto *lay = new QHBoxLayout(m_tools);
    lay->setContentsMargins(6, 5, 6, 6);         // 42px bar around 31px buttons.
    lay->setSpacing(4);
    for (int i = 0; i < m_toolButtons.size(); ++i) {
        if (i == 8 || i == 10) lay->addWidget(barSeparator(true));   // Tools | undo, redo | outputs.
        static_cast<QToolButton *>(m_toolButtons[i])->setFixedSize(Bar::button, Bar::button);
        lay->addWidget(m_toolButtons[i]);
    }
    m_tools->adjustSize();
}

void CaptureOverlay::placeToolbar()
{
    if (m_embedded) return;   // The pin toolbar owns placement; the strip is top-level.
    const QRect old = m_tools->geometry().united(m_toolOptions->geometry());
    const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(m_sel.center()));
    const QRect avail = localScreenRect((screen ? screen : QGuiApplication::primaryScreen())->availableGeometry());
    const QSize main = m_tools->sizeHint();
    const QSize options = optionsTool() == ATool::None ? QSize(0, 0) : m_toolOptions->sizeHint();
    const int width = qMax(main.width(), options.width());
    const int height = main.height() + (options.isEmpty() ? 0 : Bar::gap + options.height());
    const int selRight = m_sel.right() + 1, selBottom = m_sel.bottom() + 1;
    // A pin edit keeps the toolbar centered below the pin, the same spot the
    // pin's own toolbar occupied; capture mode right-aligns under the selection.
    const int x = m_pinEdit
        ? qBound(avail.left(), m_sel.center().x() - width / 2, qMax(avail.left(), avail.right() + 1 - width))
        : qBound(avail.left(), selRight - width, avail.right() + 1 - width);
    const int y = selBottom + Bar::gap + height <= avail.bottom() + 1 ? selBottom + Bar::gap
        : m_sel.top() - Bar::gap - height >= avail.top() ? m_sel.top() - Bar::gap - height
        : qMax(avail.top(), avail.bottom() + 1 - height - 8);
    m_tools->setGeometry(x + width - main.width(), y, main.width(), main.height());
    positionToolOptions();
    const QRect glass = m_toolOptions->isVisible()
        ? m_tools->geometry().united(m_toolOptions->geometry()) : m_tools->geometry();
    update(glass.united(old).adjusted(-4, -4, 4, 4));
}

void CaptureOverlay::positionToolOptions()                      // Option strip hugs the toolbar's bottom edge.
{
    if (m_embedded) return;                       // The pin positions the detached strip.
    if (!m_toolOptions->isVisible() && optionsTool() == ATool::None) return;
    const QRect bar = m_tools->geometry();
    const QSize hint = m_toolOptions->sizeHint();       // Strip width follows its own contents.
    m_toolOptions->setGeometry(qBound(0, bar.left(), qMax(0, width() - hint.width())), bar.bottom() + 1 + Bar::gap,
                               hint.width(), hint.height());   // Left-aligned 8px below the main bar.
}

void CaptureOverlay::hideToolBars()
{
    if (optionsTool() != ATool::None) return;     // Active tool options remain available throughout a drag.
    if (m_toolOptions->isVisible()) {
        update(m_toolOptions->geometry().adjusted(-4, -4, 4, 4));
        m_toolOptions->hide();
    }
    if (m_tools->isVisible()) {
        update(m_tools->geometry().adjusted(-4, -4, 4, 4));
        m_tools->hide();
    }
}

void CaptureOverlay::showToolBars()
{
    if (!m_embedded) m_tools->show();              // Embedded pins use their own toolbar.
    if (optionsTool() != ATool::None) {
        positionToolOptions();
        m_toolOptions->show();
    }
}

void CaptureOverlay::presetSelection(const QRect &localSel, ATool tool)
{
    // Same state as a user-committed click selection: adjusting mode, toolbars
    // visible, optionally a drawing tool armed. Re-annotation of an existing
    // image (pin edit) starts here instead of a fresh region drag.
    m_sel = localSel.normalized();
    if (m_sel.width() < 4 || m_sel.height() < 4) { m_sel = {}; return; }
    m_mode = Adjusting;
    m_hover = {};
    m_drag = H_None;
    if (!m_pinEdit) setCursor(Qt::SizeAllCursor);   // Region moving is a capture-only gesture.
    placeToolbar();
    showToolBars();
    updateTip();
    updateInspector();
    if (tool != ATool::None) selectTool(tool);
    update();
}

QPainterPath CaptureOverlay::selectionPath(const QRect &r) const
{
    QPainterPath path;
    if (r.isEmpty()) return path;
    if (m_roundEnabled && m_roundRadius > 0 && !m_embedded && !m_pinEdit)
        path.addRoundedRect(QRectF(r), m_roundRadius, m_roundRadius);
    else
        path.addRect(QRectF(r));
    return path;
}

void CaptureOverlay::applyRoundSettings(bool persist)
{
    if (m_embedded) return;
    if (m_roundButton) {
        const QSignalBlocker lock(m_roundButton);
        m_roundButton->setChecked(m_roundEnabled);
    }
    if (m_radiusSlider) {
        const QSignalBlocker lock(m_radiusSlider);
        m_radiusSlider->setValue(m_roundRadius);
    }
    if (m_radiusValue) m_radiusValue->setText(QString::number(m_roundRadius));
    if (persist) {
        QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
        settings.setValue(QStringLiteral("Capture/roundEnabled"), m_roundEnabled);
        settings.setValue(QStringLiteral("Capture/roundRadius"), m_roundRadius);
    }
    update();
}

void CaptureOverlay::showRadiusPopup()
{
    if (m_radiusPopup) { m_radiusPopup->close(); return; }
    auto *popup = new QDialog(this, Qt::Popup | Qt::FramelessWindowHint);
    m_radiusPopup = popup;
    popup->setObjectName(QStringLiteral("roundRadiusPanel"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setStyleSheet(QStringLiteral("QDialog{background:#18181b;border:1px solid #27272a;}"
                                        "QLabel{color:#fcfcfc;font-size:12px;}"));
    auto *row = new QHBoxLayout(popup);
    row->setContentsMargins(8, 6, 8, 6);
    row->setSpacing(6);
    auto *label = new QLabel(QStringLiteral("圆角半径"), popup);
    m_radiusSlider = new QSlider(Qt::Horizontal, popup);
    m_radiusSlider->setObjectName(QStringLiteral("roundRadiusSlider"));
    m_radiusSlider->setAccessibleName(QStringLiteral("圆角半径"));
    m_radiusSlider->setRange(0, 60);
    m_radiusSlider->setPageStep(1);
    m_radiusSlider->setFixedSize(128, 22);
    m_radiusSlider->setFocusPolicy(Qt::NoFocus);
    m_radiusSlider->setStyleSheet(darkSliderQss);
    m_radiusValue = new QLabel(QString::number(m_roundRadius), popup);
    m_radiusValue->setObjectName(QStringLiteral("roundRadiusValue"));
    m_radiusValue->setFixedWidth(24);
    m_radiusValue->setAlignment(Qt::AlignCenter);
    row->addWidget(label);
    row->addWidget(m_radiusSlider);
    row->addWidget(m_radiusValue);
    connect(m_radiusSlider, &QSlider::valueChanged, this, [this](int value) {
        m_roundRadius = value;
        if (value > 0) m_roundEnabled = true;   // Match the reference: a non-zero radius enables the button.
        applyRoundSettings(true);
    });
    connect(popup, &QDialog::finished, this, [this] { if (isVisible()) setFocus(); });
    popup->show();
    popup->adjustSize();
    const QPoint below = m_roundButton->mapToGlobal(QPoint(0, m_roundButton->height()));
    const QScreen *screen = QGuiApplication::screenAt(below);
    const QRect available = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    const int y = below.y() + popup->height() <= available.bottom() + 1 ? below.y()
        : qMax(available.top(), m_roundButton->mapToGlobal(QPoint()).y() - popup->height());
    popup->move(qBound(available.left(), below.x(), available.right() + 1 - popup->width()), y);
}

QRect CaptureOverlay::nativeFromGlobal(const QRect &global) const
{
    if (global.isEmpty()) return {};
    for (const ScreenFrame &frame : m_frames) {
        if (frame.logicalRect.isEmpty() || !frame.logicalRect.contains(global.center()))
            continue;
        const qreal sx = qreal(frame.pixelRect.width()) / frame.logicalRect.width();
        const qreal sy = qreal(frame.pixelRect.height()) / frame.logicalRect.height();
        if (sx <= 0 || sy <= 0) break;
        return QRect(qRound(frame.pixelRect.left() + (global.left() - frame.logicalRect.left()) * sx),
                     qRound(frame.pixelRect.top() + (global.top() - frame.logicalRect.top()) * sy),
                     qRound(global.width() * sx), qRound(global.height() * sy));
    }
    return global;
}

QRect CaptureOverlay::globalFromUnit(int x, int y, int width, int height, SizeUnit unit) const
{
    if (unit == SizeUnit::Dp) return QRect(x, y, width, height);
    const QPoint native(x, y);
    for (const ScreenFrame &frame : m_frames) {
        if (frame.pixelRect.isEmpty() || !frame.pixelRect.contains(native))
            continue;
        const qreal sx = qreal(frame.logicalRect.width()) / frame.pixelRect.width();
        const qreal sy = qreal(frame.logicalRect.height()) / frame.pixelRect.height();
        return QRect(qRound(frame.logicalRect.left() + (x - frame.pixelRect.left()) * sx),
                     qRound(frame.logicalRect.top() + (y - frame.pixelRect.top()) * sy),
                     qMax(1, qRound(width * sx)), qMax(1, qRound(height * sy)));
    }
    return QRect(x, y, width, height);
}

QRect CaptureOverlay::unitFromLocal(const QRect &local, SizeUnit unit) const
{
    if (local.isEmpty()) return {};
    const QRect global = globalScreenRect(local);
    return unit == SizeUnit::Dp ? global : nativeFromGlobal(global);
}

QSize CaptureOverlay::sizeInUnit(const QRect &local, SizeUnit unit) const
{
    return unitFromLocal(local, unit).size();
}

void CaptureOverlay::setSizeUnit(SizeUnit unit, bool persist)
{
    m_sizeUnit = unit;
    if (persist) {
        QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
        settings.setValue(QStringLiteral("Capture/sizeUnit"),
                          unit == SizeUnit::Dp ? QStringLiteral("dp") : QStringLiteral("px"));
    }
    updateTip();
}

void CaptureOverlay::updateCustomSizeInputs()
{
    if (!m_customX || !m_customY || !m_customWidth || !m_customHeight || m_sel.isNull()) return;
    const QRect shown = unitFromLocal(m_sel, m_sizeUnit);
    m_updatingCustom = true;
    m_customX->setValue(shown.x());
    m_customY->setValue(shown.y());
    m_customWidth->setValue(qMax(1, shown.width()));
    m_customHeight->setValue(qMax(1, shown.height()));
    if (m_customUnit) m_customUnit->setCurrentIndex(m_sizeUnit == SizeUnit::Dp ? 1 : 0);
    if (m_customLock) m_customLock->setChecked(m_lockAspect);
    m_updatingCustom = false;
}

void CaptureOverlay::showCustomSizePopup()
{
    if (m_embedded || m_pinEdit || m_sel.isNull()) return;
    if (m_customSizePopup) {
        loadSizePresets();
        refreshPresetCombo();
        updateCustomSizeInputs();
        m_customSizePopup->show();
        m_customSizePopup->raise();
        m_customSizePopup->activateWindow();
        return;
    }
    auto *popup = new QDialog(this, Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint
                                        | Qt::WindowStaysOnTopHint);
    m_customSizePopup = popup;
    popup->setObjectName(QStringLiteral("customSizePanel"));
    popup->setWindowTitle(QStringLiteral("自定义截图"));
    popup->setModal(false);
    popup->setAttribute(Qt::WA_DeleteOnClose, false);
    popup->setStyleSheet(QStringLiteral(
        "QDialog{background:#18181b;}"
        "QLabel{color:#fcfcfc;font-size:12px;}"
        "QSpinBox{color:#fcfcfc;background:#060607;border:1px solid #3c3c3c;border-radius:6px;"
        "padding:2px 4px;min-height:22px;font-size:12px;}"
        "QComboBox{color:#fcfcfc;background:#060607;border:1px solid #3c3c3c;border-radius:6px;"
        "padding:2px 6px;min-height:22px;font-size:12px;}"
        "QComboBox::drop-down{border:none;width:16px;background:transparent;}"
        "QCheckBox{color:#fcfcfc;font-size:12px;spacing:6px;}"
        "QPushButton{color:#fcfcfc;background:#262626;border:1px solid #3c3c3c;border-radius:6px;"
        "padding:4px 12px;font-size:12px;}"
        "QPushButton:hover{background:#303034;}"));
    auto *grid = new QGridLayout(popup);
    grid->setContentsMargins(10, 8, 10, 8);
    grid->setSpacing(6);

    auto makeSpin = [popup](const QString &name, int minimum, int maximum, int width) {
        auto *spin = new QSpinBox(popup);
        spin->setObjectName(name);
        spin->setRange(minimum, maximum);
        spin->setAlignment(Qt::AlignRight);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setFixedWidth(width);
        return spin;
    };
    m_customX = makeSpin(QStringLiteral("customX"), -999999, 999999, 70);
    m_customX->setAccessibleName(QStringLiteral("区域横向位置"));
    m_customY = makeSpin(QStringLiteral("customY"), -999999, 999999, 70);
    m_customY->setAccessibleName(QStringLiteral("区域纵向位置"));
    m_customWidth = makeSpin(QStringLiteral("customWidth"), 4, 99999, 64);
    m_customWidth->setAccessibleName(QStringLiteral("区域宽度"));
    m_customHeight = makeSpin(QStringLiteral("customHeight"), 4, 99999, 64);
    m_customHeight->setAccessibleName(QStringLiteral("区域高度"));
    m_customUnit = new QComboBox(popup);
    m_customUnit->setObjectName(QStringLiteral("customUnit"));
    m_customUnit->setAccessibleName(QStringLiteral("尺寸单位"));
    m_customUnit->addItems({QStringLiteral("px"), QStringLiteral("dp")});
    m_customUnit->setFixedWidth(56);
    m_customLock = new QCheckBox(QStringLiteral("锁定长宽比"), popup);
    m_customLock->setObjectName(QStringLiteral("customLockAspect"));
    m_customLock->setAccessibleName(QStringLiteral("锁定长宽比"));

    auto *positionLabel = new QLabel(QStringLiteral("位置"), popup);
    auto *xLabel = new QLabel(QStringLiteral("X"), popup);
    auto *yLabel = new QLabel(QStringLiteral("Y"), popup);
    grid->addWidget(positionLabel, 0, 0);
    grid->addWidget(xLabel, 0, 1);
    grid->addWidget(m_customX, 0, 2);
    grid->addWidget(yLabel, 0, 3);
    grid->addWidget(m_customY, 0, 4);

    auto *sizeLabel = new QLabel(QStringLiteral("大小"), popup);
    auto *times = new QLabel(QStringLiteral("×"), popup);
    times->setAlignment(Qt::AlignCenter);
    grid->addWidget(sizeLabel, 1, 0);
    grid->addWidget(m_customWidth, 1, 1);
    grid->addWidget(times, 1, 2);
    grid->addWidget(m_customHeight, 1, 3);
    grid->addWidget(m_customUnit, 1, 4);
    auto *hint = new QLabel(QStringLiteral("dp = px ÷ 屏幕缩放比"), popup);
    hint->setStyleSheet(QStringLiteral("color:#a1a1aa;font-size:11px;"));
    grid->addWidget(hint, 2, 0, 1, 5);
    grid->addWidget(m_customLock, 3, 0, 1, 4);

    auto *presetLabel = new QLabel(QStringLiteral("预设"), popup);
    m_presetCombo = new QComboBox(popup);
    m_presetCombo->setObjectName(QStringLiteral("customPreset"));
    m_presetCombo->setAccessibleName(QStringLiteral("截图区域预设"));
    m_presetCombo->setMinimumWidth(138);
    auto *savePreset = new QPushButton(QStringLiteral("保存"), popup);
    savePreset->setObjectName(QStringLiteral("customPresetSave"));
    savePreset->setToolTip(QStringLiteral("保存当前区域为预设"));
    grid->addWidget(presetLabel, 4, 0);
    grid->addWidget(m_presetCombo, 4, 1, 1, 3);
    grid->addWidget(savePreset, 4, 4);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(6);
    actions->addStretch();
    auto *close = new QPushButton(QStringLiteral("关闭"), popup);
    close->setObjectName(QStringLiteral("customSizeClose"));
    actions->addWidget(close);
    grid->addLayout(actions, 5, 0, 1, 5);

    connect(m_customX, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
        if (!m_updatingCustom) customFieldsChanged();
    });
    connect(m_customY, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
        if (!m_updatingCustom) customFieldsChanged();
    });
    connect(m_customWidth, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (m_updatingCustom) return;
        if (m_lockAspect && m_aspect > 0 && m_customHeight) {
            const QSignalBlocker block(m_customHeight);
            m_customHeight->setValue(qMax(4, qRound(value / m_aspect)));
        }
        customFieldsChanged();
    });
    connect(m_customHeight, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (m_updatingCustom) return;
        if (m_lockAspect && m_aspect > 0 && m_customWidth) {
            const QSignalBlocker block(m_customWidth);
            m_customWidth->setValue(qMax(4, qRound(value * m_aspect)));
        }
        customFieldsChanged();
    });
    connect(m_customLock, &QCheckBox::toggled, this, [this](bool locked) {
        m_lockAspect = locked;
        if (locked && m_customWidth && m_customHeight && m_customHeight->value() > 0)
            m_aspect = qreal(m_customWidth->value()) / m_customHeight->value();
        updateTip();
    });
    connect(m_customUnit, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_updatingCustom) return;
        const SizeUnit next = index == 1 ? SizeUnit::Dp : SizeUnit::Px;
        if (next == m_sizeUnit) return;
        setSizeUnit(next, true);
        updateCustomSizeInputs();
    });
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (!m_updatingCustom && index > 0) applyCustomPreset(index);
    });
    connect(savePreset, &QPushButton::clicked, this, &CaptureOverlay::saveCurrentPreset);
    connect(close, &QPushButton::clicked, popup, &QDialog::reject);
    connect(popup, &QDialog::finished, this, [this] {
        if (isVisible()) setFocus();
        updateTip();
    });
    loadSizePresets();
    refreshPresetCombo();
    updateCustomSizeInputs();
    popup->show();
    popup->adjustSize();
    const QPoint below = m_sizeTip->mapToGlobal(QPoint(0, m_sizeTip->height()));
    const QScreen *screen = QGuiApplication::screenAt(below);
    const QRect available = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    const int y = below.y() + popup->height() <= available.bottom() + 1 ? below.y()
        : qMax(available.top(), m_sizeTip->mapToGlobal(QPoint()).y() - popup->height());
    popup->move(qBound(available.left(), below.x(), available.right() + 1 - popup->width()), y);
}

void CaptureOverlay::customFieldsChanged()
{
    if (m_updatingCustom || m_sel.isNull() || !m_customX || !m_customY
        || !m_customWidth || !m_customHeight)
        return;
    const QRect global = globalFromUnit(m_customX->value(), m_customY->value(),
                                        m_customWidth->value(), m_customHeight->value(), m_sizeUnit);
    QRect local = localScreenRect(global).intersected(rect());
    if (local.width() < 4 || local.height() < 4) return;
    cancelDetection();
    m_sel = local.normalized();
    if (m_lockAspect && m_sel.height() > 0) m_aspect = qreal(m_sel.width()) / m_sel.height();
    if (m_presetCombo) {
        const QSignalBlocker block(m_presetCombo);
        m_presetCombo->setCurrentIndex(0);
    }
    m_mode = Adjusting;
    m_hover = {};
    m_drag = H_None;
    placeToolbar();
    showToolBars();
    updateTip();
    updateInspector();
    update();
}

void CaptureOverlay::applyCustomPreset(int index)
{
    if (index <= 0 || index > m_sizePresets.size() || m_sel.isNull()) return;
    QRect local = localScreenRect(m_sizePresets.at(index - 1).global).intersected(rect());
    if (local.width() < 4 || local.height() < 4) return;
    cancelDetection();
    m_sel = local;
    if (m_lockAspect && m_sel.height() > 0) m_aspect = qreal(m_sel.width()) / m_sel.height();
    m_mode = Adjusting;
    m_hover = {};
    m_drag = H_None;
    updateCustomSizeInputs();
    placeToolbar();
    showToolBars();
    updateTip();
    updateInspector();
    update();
}

void CaptureOverlay::loadSizePresets()
{
    m_sizePresets.clear();
    QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
    for (const QString &entry : settings.value(QStringLiteral("Capture/presets")).toStringList()) {
        const QStringList parts = entry.split(QLatin1Char('|'));
        if (parts.size() != 5) continue;
        const QRect global(parts.at(1).toInt(), parts.at(2).toInt(),
                           parts.at(3).toInt(), parts.at(4).toInt());
        if (global.width() < 4 || global.height() < 4) continue;
        m_sizePresets.append({parts.at(0), global});
        if (m_sizePresets.size() >= 50) break;
    }
}

void CaptureOverlay::saveSizePresets() const
{
    QStringList entries;
    for (const SizePreset &preset : m_sizePresets)
        entries.append(QStringLiteral("%1|%2|%3|%4|%5").arg(preset.name)
                           .arg(preset.global.x()).arg(preset.global.y())
                           .arg(preset.global.width()).arg(preset.global.height()));
    QSettings(QStringLiteral("pinshot"), QStringLiteral("pinshot"))
        .setValue(QStringLiteral("Capture/presets"), entries);
}

void CaptureOverlay::refreshPresetCombo(const QString &selected)
{
    if (!m_presetCombo) return;
    m_updatingCustom = true;
    m_presetCombo->clear();
    m_presetCombo->addItem(QStringLiteral("选择预设…"));
    for (const SizePreset &preset : m_sizePresets) m_presetCombo->addItem(preset.name);
    const int at = selected.isEmpty() ? 0 : m_presetCombo->findText(selected);
    m_presetCombo->setCurrentIndex(at > 0 ? at : 0);
    m_updatingCustom = false;
}

void CaptureOverlay::saveCurrentPreset()
{
    if (m_sel.isNull()) return;
    bool ok = false;
    const QString defaultName = QStringLiteral("预设 %1").arg(m_sizePresets.size() + 1);
    const QString name = QInputDialog::getText(m_customSizePopup, QStringLiteral("保存预设"),
                                               QStringLiteral("预设名称："), QLineEdit::Normal,
                                               defaultName, &ok);
    if (ok) saveSizePreset(name);
}

void CaptureOverlay::saveSizePreset(const QString &name)
{
    QString clean = name.trimmed();
    clean.replace(QLatin1Char('|'), QLatin1Char(' '));
    if (clean.isEmpty() || m_sel.isNull()) return;
    const QRect global = globalScreenRect(m_sel);
    bool replaced = false;
    for (SizePreset &preset : m_sizePresets)
        if (preset.name == clean) {
            preset.global = global;
            replaced = true;
            break;
        }
    if (!replaced) m_sizePresets.append({clean, global});
    saveSizePresets();
    refreshPresetCombo(clean);
    if (m_customSizePopup) {
        m_customSizePopup->raise();
        m_customSizePopup->activateWindow();
    }
}

void CaptureOverlay::updateRefreshIcon()
{
    if (!m_refreshButton) return;
    const qreal dpr = devicePixelRatioF();
    QPixmap pm(qCeil(Bar::icon * dpr), qCeil(Bar::icon * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.translate(Bar::icon / 2.0, Bar::icon / 2.0);
    p.rotate(m_refreshSpinAngle);
    p.drawPixmap(QPointF(-Bar::icon / 2.0, -Bar::icon / 2.0),
                 barIcon(Icon::Refresh).pixmap(QSize(Bar::icon, Bar::icon)));
    p.end();
    m_refreshButton->setIcon(QIcon(pm));
}

void CaptureOverlay::beginDesktopPreview()
{
    if (m_embedded || m_pinEdit || m_refreshing) return;
    m_desktopPreview = true;
    if (m_refreshButton) {
        const QSignalBlocker lock(m_refreshButton);
        m_refreshButton->setChecked(true);
    }
    cancelDetection();
    if (m_detector) m_detector->stop();
    if (m_radiusPopup) m_radiusPopup->close();
    if (m_sizePopup) m_sizePopup->close();
    m_hover = QRect();
    m_sizeTip->hide();
    m_inspector->hide();
    m_toolOptions->hide();
    if (m_refreshAnimation) m_refreshAnimation->start();
    update();
}

void CaptureOverlay::refreshScreenshot()
{
    if (m_embedded || m_pinEdit || m_refreshing) return;
    m_refreshing = true;
    if (m_refreshAnimation) m_refreshAnimation->stop();
    m_refreshSpinAngle = 0;
    if (m_refreshButton) {
        m_refreshButton->setIcon(barIcon(Icon::Refresh));
        m_refreshButton->setChecked(false);
    }
    cancelDetection();
    if (m_detector) m_detector->stop();
    if (m_radiusPopup) m_radiusPopup->close();
    if (m_sizePopup) m_sizePopup->close();
    if (m_colorDialog) m_colorDialog->close();
    m_toolOptions->hide();
    m_inspector->hide();
    m_sizeTip->hide();
    const bool customVisible = m_customSizePopup && m_customSizePopup->isVisible();
    if (customVisible) m_customSizePopup->hide();

    const HWND hwnd = reinterpret_cast<HWND>(winId());
    // Excluding the capture window keeps the frame visible while the desktop
    // underneath is grabbed, so the selection never flashes away.
    const bool excluded = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    if (excluded) {
        m_desktopPreview = true;
        m_tools->show();
        update();
    } else {
        m_desktopPreview = false;
        hide();
    }
    QTimer::singleShot(150, this, [this, hwnd, excluded, customVisible] {
        if (!m_refreshing) {
            SetWindowDisplayAffinity(hwnd, WDA_NONE);
            if (customVisible && m_customSizePopup) m_customSizePopup->show();
            return;
        }
        const QVector<ScreenFrame> frames = m_frameSource ? m_frameSource() : freezeScreens();
        SetWindowDisplayAffinity(hwnd, WDA_NONE);
        if (frames.isEmpty()) {
            m_refreshing = false;
            m_desktopPreview = false;
            show(); raise(); activateWindow(); setFocus();
            if (!m_sel.isNull()) {
                placeToolbar();
                showToolBars();
                updateTip();
                updateInspector();
            } else if (m_liveDetection) {
                startDetector();
                requestDetection(mapFromGlobal(QCursor::pos()));
            }
            if (customVisible && m_customSizePopup) m_customSizePopup->show();
            showOutputError(QStringLiteral("刷新失败，请重试"));
            update();
            return;
        }
        applyRefreshFrames(frames);
        m_refreshing = false;
        m_desktopPreview = false;
        if (!excluded) show();
        raise(); activateWindow(); setFocus();
        if (!m_sel.isNull()) {
            placeToolbar();
            showToolBars();
            updateTip();
            updateInspector();
        } else if (m_liveDetection) {
            startDetector();
            requestDetection(mapFromGlobal(QCursor::pos()));
        }
        if (customVisible && m_customSizePopup) m_customSizePopup->show();
        update();
    });
}

void CaptureOverlay::applyRefreshFrames(const QVector<ScreenFrame> &frames)
{
    if (editorActive()) commitTextEdit();
    if (m_live) { m_live = false; commitAnnotation(); }
    m_frames = frames;
    buildShot(frames);
    m_mosaicFrame = QImage();
    // Refresh replaces the frozen pixels only: keep the committed selection,
    // annotations, history and armed tool exactly as the user left them.
    m_pressSel = QRect();
    m_hover = QRect();
    m_hoverHistory.clear();
    m_windowRects.clear();
    m_mode = m_sel.isNull() ? Selecting : Adjusting;
    m_drag = H_None;
    m_itemDrag = H_None;
    m_dragStarted = false;
    m_queryId = 0;
    m_queryPending = m_waitingClick = false;
    m_pinAfterDetection = false;
    m_detectionPoint = QPoint();
    syncToolButtons();
    syncUndoButtons();
    if (m_colorDialog) m_colorDialog->close();
    update();
}

QPixmap CaptureOverlay::cropped() const
{
    if (m_sel.isEmpty() || m_shot.isNull()) return {};
    const qreal scale = m_shot.devicePixelRatio();
    const QRect r = m_sel.normalized();
    QPixmap pix = m_shot.copy(QRect(qRound(r.x() * scale), qRound(r.y() * scale),
                                   qRound(r.width() * scale), qRound(r.height() * scale)));
    pix.setDevicePixelRatio(1);
    const bool rounded = m_roundEnabled && m_roundRadius > 0 && !m_embedded && !m_pinEdit;
    if (rounded) {
        // Render through a rounded clip so save/copy/pin keep transparent corners.
        QImage out(pix.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        const qreal radius = m_roundRadius * scale;
        clip.addRoundedRect(QRectF(0, 0, out.width(), out.height()), radius, radius);
        p.setClipPath(clip);
        p.drawPixmap(0, 0, pix);
        if (!m_anns.isEmpty() || editorActive())
            paintAnnotations(p, -QPointF(r.topLeft()) * scale, scale, QRectF(r));
        p.end();
        pix = QPixmap::fromImage(out);
    } else if (!m_anns.isEmpty() || editorActive()) {   // Unfinished text is also included in previews.
        QPainter p(&pix);
        paintAnnotations(p, -QPointF(r.topLeft()) * scale, scale, QRectF(r));
    }
    return pix;
}

void CaptureOverlay::armTool(ATool tool)          // Embedded pins arm tools from their own toolbar.
{
    if (tool == ATool::None || tool == m_atool) {
        stopItemDrag();
        m_selAnn = -1;   // Leaving the tool leaves no selection handles on the pin.
        setToolNone();
    } else selectTool(tool);
    if (optionsTool() == ATool::None) {
        m_toolOptions->hide();
    } else {
        m_toolOptions->adjustSize();
        m_toolOptions->show();
        m_toolOptions->raise();
    }
    emit stripRebuilt();
    setFocus(Qt::OtherFocusReason);   // Esc ladder and shortcuts reach the canvas immediately.
}

QPixmap CaptureOverlay::composited() const
{
    QPixmap pix = m_shot.copy(m_shot.rect());
    pix.setDevicePixelRatio(1);
    if (!m_anns.isEmpty() || editorActive()) {
        QPainter p(&pix);
        paintAnnotations(p, QPointF(0, 0), 1.0, QRectF(rect()), false);
    }
    return pix;
}

void CaptureOverlay::setImage(const QImage &image)
{
    if (image.isNull()) return;
    m_shot = QPixmap::fromImage(image);
    m_mosaicFrame = QImage();   // Mosaic samples the refreshed pixels.
    if (m_embedded) m_sel = rect();   // Marks clip to the whole pin at its current size.
    update();
}

void CaptureOverlay::clearAll()
{
    m_anns.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_selAnn = -1;
    syncUndoButtons();
    update();
}

void CaptureOverlay::ocrCapture() { openTextCard(false); }

void CaptureOverlay::translateCapture() { openTextCard(true); }

void CaptureOverlay::barcodeCapture() { openTextCard(false,true); }

void CaptureOverlay::openTextCard(bool translate, bool barcode)
{
    stopItemDrag();
    commitTextEdit();
    const QImage image = cropped().toImage();
    if (image.isNull()) {
        showOutputError(barcode ? QStringLiteral("识别失败，请先选择有效截图区域")
                                : translate ? QStringLiteral("翻译失败，请先选择有效截图区域")
                                            : QStringLiteral("识别失败，请先选择有效截图区域"));
        return;
    }
    const QRect captureRect=globalScreenRect(m_sel.normalized());
    cancelDetection();
    if (m_colorDialog) m_colorDialog->close();
    m_tools->hide(); m_toolOptions->hide(); m_inspector->hide();
    hide();
    auto *dialog = barcode ? showBarcode(image,this,captureRect)
                           : translate ? showTranslate(image,this,captureRect) : showOcr(image,this,captureRect);
    if (!dialog) {
        show(); showToolBars(); update();
        return;
    }
    m_ocrDialog=dialog;
    connect(dialog,&QDialog::finished,this,[this,dialog] {
        if (dialog->returnedToSource()) {
            // Back restores the live selection and its toolbars instead of ending the capture.
            m_ocrDialog=nullptr;
            show(); raise(); activateWindow();
            showToolBars(); update();
            return;
        }
        QTimer::singleShot(0,this,&QWidget::close);
    });
}

void CaptureOverlay::undoAnnotation()
{
    stopItemDrag();
    commitTextEdit();
    if (m_undoStack.isEmpty()) return;
    m_redoStack.append(annotationSnapshot());
    m_anns = m_undoStack.takeLast();
    ++m_smartEpoch;                              // Restored indexes must not receive stale erases.
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
    ++m_smartEpoch;                              // Restored indexes must not receive stale erases.
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
