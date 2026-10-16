#pragma once

#include "window_detection.h"
#include "pixel_inspector.h"
#include "widgets.h"
#include "ocr_dialog.h"
#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QColor>
#include <QFont>
#include <QTransform>
#include <QPaintEvent>
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QTextDocument>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <QSlider>
#include <QDialog>
#include <QColorDialog>
#include <QLineEdit>
#include <QCursor>
#include <QTimer>
#include <QPointer>
#include <QCloseEvent>
#include <QPolygonF>
#include <qt_windows.h>

// ---------- Fullscreen selection: frozen frame, adjustable region, and action toolbar. ----------
class CaptureOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit CaptureOverlay(const QVector<ScreenFrame> &frames,
                            const WindowTargets &nativeWindows = {},
                            const DetectionOptions &options = DetectionOptions(), bool liveDetection = false);
    ~CaptureOverlay() override;
    static int testOcr(const QString &previewPath);
    static int testSelection();
    static int testColorPicker(const QString &previewPath);
    static int testMosaic(const QString &previewPath);
    static int testWindowDetection();
    static int testTextAnnotation();
    static int testOutput();
signals:
    void grabbed(const QPixmap &pix, const QRect &globalRegion);
    void longRequested(const QRect &globalRegion);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
private:
    enum Handle { H_None = 0, H_New, H_Move, H_TL, H_TR, H_BL, H_BR, H_L, H_R, H_T, H_B, H_Rotate };

    // ---------- instant annotation ----------
    enum class ATool { None, Rect, Ellipse, Arrow, Pen, Marker, Text, Number, Mosaic, Eraser };
    struct Annotation {
        ATool tool = ATool::None;
        QColor color;
        int pen = 3;                                 // Stroke width in logical pixels.
        int mosaicStrength = 6;                      // Pixel block size (3-24), independent of brush width.
        bool mosaicBrush = false;
        bool mosaicBlur = false;
        int mosaicLevels[2] = {6, 6};
        quint32 mosaicSeed = 1;
        mutable int mosaicCacheStrength = 0;
        mutable QImage mosaicCache, mosaicSource;
        QRect rect;                                  // Rect / Ellipse / Mosaic / Text / Number box.
        QPoint a, b;                                 // Arrow endpoints.
        QPolygonF stroke;                            // Pen / Marker / Mosaic brush polyline.
        QString text;                                // Text / Number content.
        qreal font = 15;                             // Text / Number point size (1/72 inch).
        QString family = QStringLiteral("Microsoft YaHei UI");
        int arrowStyle = 0;                          // Heads only: 0 single, 1 single hollow, 2 double, 3 double hollow, 4 none.
        int lineStyle = 0;                           // Shaft path: 0 straight, 1 curve, 2 clicked vertices (straight tool, 3+ points).
        bool dashed = false;                         // Arrow shaft dashing, independent of the path.
        int numberStyle = 0;                         // 0 filled disc, 1 ring, 2 bare label.
        int numberFormat = 0;                        // 0 "1 2 3", 1 "I II III", 2 "A B C", 3 "a b c", 4 Chinese numerals.
        bool bold = false;                           // Text weight / slant.
        bool italic = false;
        qreal rot = 0;                               // Rotation around the box center (degrees).
        bool fill = false;                           // Rect / Ellipse solid fill; Marker / Eraser rectangle mode.
    };
    static QRect annotationRect(const Annotation &ann);
    // Rotation helpers: item geometry stays in local space, the angle only transforms paint and hit tests.
    static QTransform annTransform(const Annotation &ann);
    static QRect rotatedBounds(const Annotation &ann);
    static QPoint mapToLocal(const Annotation &ann, const QPoint &pos);
    static QPoint mapFromLocal(const Annotation &ann, const QPoint &pos);
    // ---- object editing: hit tests, selection anchors, move/resize, undo/redo ----
    static qreal distToSegment(const QPointF &p, const QPointF &a, const QPointF &b);
    static bool annotationHit(const Annotation &ann, const QPoint &pos);
    int annotationAt(const QPoint &pos) const;
    // Corner anchors for area shapes, endpoint anchors for arrows; strokes and text move only.
    QVector<QPoint> annAnchors(const Annotation &ann) const;
    Handle annHandleAt(const Annotation &ann, const QPoint &pos) const;
    static QPoint rotateAnchor(const Annotation &ann);
    void paintMosaicBorder(QPainter &p, const Annotation &ann, bool selected) const;
    static void paintHandle(QPainter &p, const QPointF &center);
    static int testSelectionBorders();
    static QCursor mosaicBrushCursor(qreal width, qreal dpr);
    static QCursor eraserCursor(qreal width, qreal dpr);
    static QPainterPath strokeMask(const QPolygonF &stroke, qreal width);
    static QPainterPath eraserMask(const Annotation &ann);
    void updateAnnotationCursor(const QPoint &pos);
    static void translateAnnotation(Annotation &ann, const QPoint &d);
    void applyItemDrag(const QPoint &pos, Qt::KeyboardModifiers mods = Qt::NoModifier);
    QVector<Annotation> annotationSnapshot() const;
    void pushUndo();
    void syncUndoButtons();
    void normalizeNumbers();
    void deleteSelected();
    static QFont textFont(const Annotation &ann);
    static int numberDiameter(const Annotation &ann);
    static void setupTextDocument(QTextDocument &doc, const Annotation &ann);
    static void fitTextAnnotation(Annotation &ann);
    // ---- native scene text editing: one layout for input, selection, and capture ----
    bool editorActive() const;
    Annotation editorAnnotation() const;
    void syncTextEditorGeometry();
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void beginTextEdit(const QPoint &pos, int existing = -1);
    void commitTextEdit();
    void applyEditorFont();
    void editorFontStep(int dir);
    void tipPosition(const QPoint &at);
    static QPoint keyDelta(int key);
    void nudgeSelected(int key);
    ATool optionsTool() const;
    bool mosaicBrush() const;
    int mosaicStrength() const;
    bool mosaicBlur() const;
    void switchMosaicEffect();
    void setMosaicStrength(int value);
    void selectMosaicMode(bool brush);
    void selectEraserMode(bool rect);
    bool eraserRect() const;
    // Rectangle mosaics and rectangle erasures: movable regions that keep their frame while their tool is armed.
    static bool regionItem(const Annotation &ann);
    qreal toolSize() const;
    void syncSizeInput();
    void commitSizeInput();
    void setToolSize(qreal value);
    void tuneWidth(int dir, const QPoint &at);
    void beginAnnotation(const QPoint &pos);
    void updateAnnotation(const QPoint &pos, Qt::KeyboardModifiers mods = Qt::NoModifier);
    void commitAnnotation();
    void stopItemDrag();
    void endAnnotationDrag();
    void setToolNone();
    void selectTool(ATool tool);
    void syncToolButtons();
    // One color shared by every colored tool; recolors the live item or the selection.
    void pickToolColor(const QColor &color, bool undoable = true);
    void showColorPalette();
    void pickArrowStyle(int style);
    void pickArrowLineStyle(int style);
    void toggleArrowDashed();
    void toggleFill();
    void pickNumberOption(bool format, int value);
    static QString numberLabel(int n, int format);
    void showSizePopup(QToolButton *anchor, int minimum, int maximum);
    void setTextBold(bool on);
    void setTextItalic(bool on);
    void setTextFamily(const QFont &font);
    // Options strip under the toolbar: contents follow the armed tool.
    void updateToolOptions();
    // Shared by the overlay (logical coords, origin 0, scale 1) and cropped() (native crop
    // coords): annotations are stored overlay-local and mapped through this transform.
    void paintAnnotations(QPainter &p, const QPointF &origin, qreal scale, const QRectF &clip,
                          bool includeEditor = true) const;
    static QImage mosaicTiles(const QImage &source, int block, quint32 seed);
    static QImage mosaicBlurred(const QImage &source, int radius);
    void drawAnnotation(QPainter &p, const Annotation &ann, const QImage &source) const;

    void startDetector();
    void setWindowTargets(const WindowTargets &windows);
    QRect localScreenRect(const QRect &global) const;
    QRect globalScreenRect(const QRect &local) const;
    QRect nativeDetectionRect(const QRect &local) const;
    void cancelDetection();
    void requestDetection(const QPoint &point, const QRect &previous = {});
    void recoverDetector();
    // Hover-driven repaints stay incremental: a full-desktop repaint per follow update
    // saturates the GUI thread during fast slides, and painting is stateless (frozen frame
    // plus mask), so the union of the old and new rectangles reproduces the surface.
    void updateHoverRegion(const QRect &oldRect, const QRect &newRect);
    void clearHover();
    void handleDetectedRect(const QRect &native, quint64 id);

    void updateInspector();
    static QPoint inspectorPosition(const QPoint &cursor, const QSize &size, const QRect &bounds);

    void resetSelection();
    QRect windowAt(const QPoint &p) const;
    static QVector<QRect> handleRects(const QRect &r);
    Handle hitHandle(const QPoint &p) const;
    static Qt::CursorShape cursorForHandle(Handle h);
    QRect resizeBy(Handle h, const QPoint &pos);
    // Magnetic edges: snap selection edges to nearby screen and window anchors.
    enum { SnapThreshold = 8 };
    QVector<int> snapAnchorLines(bool horizontal) const;
    void applySnap(Handle h);
    void updateTip();
    void buildToolLayout();
    void placeToolbar();
    void positionToolOptions();
    void hideToolBars();
    void showToolBars();
    QPixmap cropped() const;
private slots:
    void ocrCapture();
    void undoAnnotation();
    void redoAnnotation();
    void saveCapture();
    bool saveCaptureTo(const QString &path);
    void copyCapture();
    void showOutputError(const QString &message);
    void pinCapture();
    void longCapture();
private:
    enum Mode { Selecting, Adjusting };
    QPixmap m_shot;
    QPointer<OcrDialog> m_ocrDialog;
    mutable QImage m_mosaicFrame;
    QVector<ScreenFrame> m_frames;
    QVector<DetectedWindow> m_windowRects;          // Detected windows (logical, overlay-local) for click-select and snapping.
    DetectionOptions m_detectionOptions;
    QPointer<WindowDetector> m_detector;
    quint64 m_queryId = 0;
    quint64 m_lastHoverId = 0;
    QPoint m_detectionPoint;
    QVector<QRect> m_hoverHistory;
    bool m_queryPending = false, m_waitingClick = false, m_expanding = false;
    bool m_pinAfterDetection = false;
    bool m_dragStarted = false;
    QRect m_hover;                                  // Detected window under the cursor; drives the mask hole while idle.
    QRect m_pressWindow;                            // Window under the press point; a click without drag selects it.
    int m_snapX = -1, m_snapY = -1;                 // Active magnetic-anchor lines while dragging.
    PixelInspector *m_inspector = nullptr;
    QPoint m_pressPos;
    QRect m_sel, m_pressSel;
    Handle m_drag = H_None;
    Mode m_mode = Selecting;
    QLabel *m_sizeTip = nullptr;
    QWidget *m_tools = nullptr;
    QVector<QWidget *> m_toolButtons;
    ATool m_atool = ATool::None;
    QColor m_acolor = QColor("#ed1c24");             // Red first: the loudest marking color.
    int m_apen = 3;
    int m_mosaicLevels[2] = {6, 6}, m_mosaicWidth = 14;
    bool m_mosaicBrush = true, m_mosaicBlur = false, m_mosaicDragUndo = false;
    qreal m_afont = 15;                              // Base text/number font size (pt).
    QString m_afamily = QStringLiteral("Microsoft YaHei UI");
    bool m_afill = false;                            // Default fill mode for rect / ellipse.
    bool m_spaceHeld = false;                        // Space turns the next drag into a move.
    bool m_nudging = false;                          // Coalesces WASD bursts into one undo step.
    QVector<Annotation> m_anns;
    Annotation m_ann;                                // Annotation under construction.
    bool m_live = false;
    int m_selAnn = -1;                               // Annotation selected for editing.
    Annotation m_pressAnn;                           // Snapshot for move/resize drags.
    Handle m_itemDrag = H_None;
    bool m_eraserRect = false;                       // Eraser starts in brush mode.
    int m_eraserWidth = 14;
    QVector<QVector<Annotation>> m_undoStack, m_redoStack;
    QTimer m_detectionWatchdog;
    QToolButton *m_undoButton = nullptr;             // Enabled state follows the stacks.
    QToolButton *m_redoButton = nullptr;
    QGraphicsTextItem *m_editor = nullptr;           // Native editable text with content-sized bounds.
    QGraphicsView *m_textView = nullptr;             // Masked to the text item; canvas stays clickable.
    bool m_textPreedit = false, m_textMoving = false;
    QPoint m_textMovePress;
    QPointF m_textMoveOrigin;
    int m_editAnn = -1;                              // Annotation being re-edited.
    QColor m_editColor = QColor("#ed1c24");
    qreal m_editFont = 15;
    QString m_editFamily;
    bool m_committingText = false;
    HIMC m_imeCtx = nullptr;                         // Detached IME context for text editing.
    QToolButton *m_shapeButton = nullptr;            // Merged rect / ellipse tool button.
    QVector<QToolButton *> m_toolBtn;                // Tool button per ATool id (shape shared).
    QWidget *m_toolOptions = nullptr;                // Per-tool option strip under the toolbar.
    QComboBox *m_sizeInput = nullptr;                 // Editable text point-size presets.
    QToolButton *m_sizeButton = nullptr;             // Width button; opens the slider popup.
    QPointer<QDialog> m_sizePopup;
    QPointer<QSlider> m_sizeSlider;
    QPointer<QLabel> m_sizeValue;
    bool m_sizeDragUndo = false;
    QSlider *m_mosaicSlider = nullptr;
    QLabel *m_mosaicStrengthLabel = nullptr;
    QLabel *m_mosaicWidthLabel = nullptr;
    QToolButton *m_mosaicPreview = nullptr;
    ColorBoard *m_colorBoard = nullptr;
    QPointer<QColorDialog> m_colorDialog;
    bool m_shapeEllipse = false;                     // Current sub-shape of the shape button.
    int m_arrowStyle = 0;                            // Head shape, see Annotation::arrowStyle.
    int m_arrowLineStyle = 0;                        // Picked shaft path: 0 straight, 1 curve.
    bool m_arrowDashed = false;
    int m_numberStyle = 0, m_numberFormat = 0;       // See Annotation::numberStyle / numberFormat.
    bool m_markerFill = false;                       // Marker drags a translucent rectangle.
    bool m_abold = false, m_aitalic = false;         // Text tool defaults.
    bool m_editBold = false, m_editItalic = false;   // Live editor weight / slant.
};
