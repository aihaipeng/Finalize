#pragma once

#include "platform/screen_capture.h"
#include "platform/output.h"
#include "shared/widgets.h"
class OcrDialog;
#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>
#include <QHash>
#include <QWidget>
#include <QLabel>
#include <QList>
#include <QPointer>
#include <QtMath>
#include <QElapsedTimer>
#include <qt_windows.h>

class QComboBox;
class QToolButton;
class QMessageBox;
class QCheckBox;
class QTimer;

// Live preview beside the selection: a slice of the stitched image around the current
// frame, an accent outline, a match frame on the current frame, and fades with
// chevrons where the image continues past the slice.
class LongPreview : public QWidget
{
public:
    LongPreview();
    QImage image;                      // Slice in screen orientation.
    QRect imageRect, frameRect;        // Widget coordinates.
    bool vertical = true, atStart = true, atEnd = true, showFrame = false;
protected:
    void paintEvent(QPaintEvent *) override;
};

// ---------- Scrolling capture ----------
// shortcut: Textureless or identical repeated overlaps cannot be uniquely aligned; add scroll-position data if needed.
class LongCapture : public QObject
{
    Q_OBJECT
public:
    explicit LongCapture(const QRect &globalRegion);
    void start();
    QRect region() const { return m_region; }
    // Wheel delta per tick for the scroll-axis extent of the selection, in logical pixels.
    static int autoScrollStep(int extent);
    // Placed capture (editor or pin): native size when it fits, scaled down to fit otherwise, centered with room for the bar below.
    static QRect editorGeometry(const QSize &pixels, qreal dpr, const QRect &available);
    static int testStitching();
    static int testThemeUi(const QString &previewPath);
    static int testOutput();
    static int testLiveCapture(const QString &previewPath);
    void cancel();
    void resume();                                   // Back from the editor: the session continues where it was.
    bool suspended() const { return m_suspended && !m_done; }
signals:
    void captured(const QImage &image);             // Any successful save/copy/pin output.
    void finished(const QPixmap &pix, bool edit);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    // Unique local texture votes for a displacement; animation and repeated rows do not vote.
    // shortcut: Overlaps need stable unique texture; add another evidence source for textureless or fully animated pages.
    struct Feature { int x = 0, y = 0; bool repeated = false; };
    using FeatureIndex = QHash<quint64, Feature>;
    bool matchFullRes(const QImage &f, int &bestK) const;
    mutable FeatureIndex m_cachedFeatures;
    mutable qint64 m_cachedFeaturesKey = -1;
    // ---- Capture loop: polling captures and stitches frames; horizontal frames are rotated onto the vertical stitcher. ----
    void nextTick(int elapsedMs);
    void captureFrame();
    bool ingest(const QImage &raw);
    bool addFrame(const QImage &f);
    void resetCapture();
    int crossExtent() const { return m_vertical ? m_region.width() : m_region.height(); }
    int logicalLength() const;
    static QImage transposed(const QImage &src);     // Swaps rows and columns; its own inverse.
    // ---- Growable row buffer with contiguous append/prepend. ----
    void initTiles(int width);
    bool reserveRows(int head, int tail);
    bool appendRows(const QImage &f, int from, int count);
    bool prependRows(const QImage &f, int from, int count);
    QImage toImage() const;                          // Stitched rows in the capture orientation.
    QImage exportImage() const;                      // Final image in screen orientation.
    // ---- UI: selection mask, action bar and live preview on the selection's screen. ----
    void buildUi();
    void placeBar();
    void setUiVisible(bool v);
    void updateLabel();
    void updateButtons();
    void updatePreview();
    void updateUi();
    void setVertical(bool vertical);
    void setAutoScroll(int direction);               // 0 off, 1 down/right, -1 up/left.
    void scrollOnce();                               // One wheel tick toward the active direction.
    QPoint moveRegion(QPoint delta);                 // Returns the applied part of the delta.
    void notice(const QString &title, const QString &text, const QString &noAskKey);
    void fail(const QString &message);
    void finish(bool edit = false);
    void reportCaptured(const QImage &image);        // One history entry per long capture.
    void suspend();                                  // Hidden and still while the editor is open.
    // ---- Crop: keeps the rows from the current frame to one end; auto crop trims when scrolling turns back. ----
    bool cropRows(int from, int to);                 // Keeps stored rows [from,to) of the current coverage.
    void cropBefore();                               // Top, or left when horizontal.
    void cropAfter();                                // Bottom, or right when horizontal.
    void autoCrop(int growth);                       // growth: -1 head, 1 tail, 0 inside the coverage.
    void setAutoCrop(bool on);
    void updateCropDirection();
    void refreshCropPanel();
    void placeCropPanel();
    // Save/copy/OCR end the session without creating a pinned image.
    void saveCapture();
    void copyCapture();
    void openTextCard(bool translate, bool barcode = false);   // Same text/barcode card as a normal capture.
    void cleanupUi();
private:
    QRect m_region;
    QImage m_lastFrame;                              // Last aligned frame; rejected frames never replace it.
    int m_frameRow = 0;                              // Absolute logical row of that frame's top edge.
    QImage m_buf;                                    // Growable buffer for captured rows.
    int m_width = 0, m_bytesPerLine = 0, m_tileMaxRows = 0;
    int m_bufStart = 0;                              // Logical row corresponding to buffer row zero.
    int m_rowStart = 0, m_rowEnd = 0;                 // Logical coverage [start,end).
    int m_headPad = 0, m_tailPad = 0;
    bool m_vertical = true;
    int m_autoScroll = 0;                            // 0 off, 1 down/right, -1 up/left.
    bool m_paused = false;                           // A dialog is open over the desktop.
    QElapsedTimer m_scrollClock, m_previewClock, m_failClock, m_errorClock;
    QString m_errorText;
    QWidget *m_mask = nullptr;
    QWidget *m_bar = nullptr;
    QWidget *m_grip = nullptr;                       // Drags the bar itself.
    QWidget *m_axis = nullptr;                       // Drags the selection along the scroll axis.
    QWidget *m_dragging = nullptr;
    QPoint m_dragAt;
    QLabel *m_label = nullptr, *m_feedback = nullptr;
    bool m_feedbackAffinity = false, m_hidFeedback = false;
    void updateFeedback();
    QComboBox *m_dir = nullptr;
    QToolButton *m_auto = nullptr, *m_autoUp = nullptr, *m_save = nullptr;
    QList<QToolButton *> m_gated;                    // Edit, OCR, translate, pin, copy: bounded image length only.
    QToolButton *m_ocr = nullptr, *m_translate = nullptr, *m_barcode = nullptr;
    QString m_ocrMissing;
    bool m_barMoved = false;                         // The user dragged the bar away from its anchor.
    LongPreview *m_preview = nullptr;
    bool m_barAffinity = false, m_previewAffinity = false, m_previewTried = false;
    bool m_hidBar = false, m_hidPreview = false;
    bool m_pending = false;                          // The preview lags the latest frame.
    int m_sliceStart = 0, m_sliceFrame = 0;          // First row shown in the preview; frame row it last followed.
    int m_previewHead = 0, m_previewTail = 0;        // Coverage when the preview was last laid out.
    bool m_alignmentLost = false;
    bool m_storageFull = false, m_storageWarned = false;
    bool m_failWarned = false;
    QPointer<QMessageBox> m_notice;
    QPointer<OcrDialog> m_ocrDialog;
    QToolButton *m_crop = nullptr, *m_cropStart = nullptr, *m_cropEnd = nullptr;
    QWidget *m_cropPanel = nullptr;                  // Opens while the crop button or the panel is hovered.
    QCheckBox *m_autoCropBox = nullptr;
    QTimer *m_cropTimer = nullptr;                   // Delays hiding so the pointer can cross to the panel.
    bool m_cropHover = false, m_panelHover = false, m_hidCropPanel = false;
    bool m_autoCrop = false;
    int m_cropState = 0;                             // Growth direction auto crop follows: -1 head, 1 tail, 0 none yet.
    int m_cropBase = 0;                              // Frame offset at the previous tick.
    bool m_suspended = false;
    bool m_done = false;
    bool m_captured = false;
};
