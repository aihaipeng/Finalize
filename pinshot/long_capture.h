#pragma once

#include "screen_capture.h"
#include "output.h"
#include "widgets.h"
#include "pin_window.h"
#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QWidget>
#include <QLabel>
#include <QtMath>
#include <QElapsedTimer>
#include <qt_windows.h>

// ---------- Manual scrolling capture ----------
// shortcut: Textureless or identical repeated overlaps cannot be uniquely aligned; add scroll-position data if needed.
class LongCapture : public QObject
{
    Q_OBJECT
public:
    explicit LongCapture(const QRect &globalRegion);
    void start();
    static int testStitching();
    static int testOutput();
    static int testLiveCapture(const QString &previewPath);
signals:
    void finished(const QPixmap &pix);
private:
    // Unique local texture votes for a displacement; animation and repeated rows do not vote.
    // shortcut: Overlaps need stable unique texture; add another evidence source for textureless or fully animated pages.
    bool matchFullRes(const QImage &f, int &bestK) const;
    // ---- Capture loop: the user scrolls manually; polling captures and stitches frames. ----
    void nextTick(int elapsedMs);
    void captureFrame();
    bool addFrame(const QImage &f);
    // ---- Growable row buffer with contiguous append/prepend. ----
    void initTiles(int width);
    bool reserveRows(int head, int tail);
    bool appendRows(const QImage &f, int from, int count);
    bool prependRows(const QImage &f, int from, int count);
    QImage toImage() const;
    // ---- UI: toolbar and live preview on the selection's screen. ----
    void buildUi();
    void setUiVisible(bool v);
    void updateLabel();
    void updateUi();
    void finish();
    void cancel();
    // Save/copy ends the session without creating a pinned image.
    void saveCapture();
    void copyCapture();
    void cleanupUi();
private:
    QRect m_region;
    QImage m_lastFrame;                              // Last aligned raw frame; rejected frames never replace it.
    int m_frameRow = 0;                              // Absolute logical row of that frame's top edge.
    QImage m_buf;                                    // Growable buffer for captured rows.
    int m_width = 0, m_bytesPerLine = 0, m_tileMaxRows = 0;
    int m_bufStart = 0;                              // Logical row corresponding to buffer row zero.
    int m_rowStart = 0, m_rowEnd = 0;                 // Logical coverage [start,end).
    int m_headPad = 0, m_tailPad = 0;
    QWidget *m_bar = nullptr;
    QLabel *m_label = nullptr;
    QLabel *m_preview = nullptr;
    QWidget *m_frame = nullptr;
    bool m_affinityOk = false;
    bool m_alignmentLost = false;
    bool m_storageFull = false;
    QElapsedTimer m_previewClock;
    bool m_done = false;
};
