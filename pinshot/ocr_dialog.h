#pragma once

#include "ocr_engine.h"
#include "widgets.h"
#include "output.h"
#include <QObject>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QResizeEvent>
#include <QWidget>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QLabel>
#include <QGraphicsView>
#include <QtMath>
#include <QToolButton>
#include <QDialog>
#include <QTimer>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QSplitter>
#include <QBoxLayout>
#include <QTextCursor>
#include <QFutureWatcher>
#include <functional>
#include <qt_windows.h>

class OcrImageView : public QGraphicsView
{
public:
    explicit OcrImageView(const QImage &image, QWidget *parent = nullptr);
    void fit();
    bool fitted() const { return !m_zoomed; }
    int percent() const;                  // 100 = one capture pixel per screen pixel.
    void zoomTo(int percent);             // Around the view centre, clamped to 2..1600.
    std::function<void()> zoomed;         // Called after every scale change.
protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
private:
    QRectF m_fit;
    bool m_zoomed = false;
};

class QSplitter;
class TranslateGateway;

// Picker that reads "prefix (choice)". On the first, automatic item it shows what the
// last run actually used ("语言检测 (韩语)") once that is known; the list keeps plain names.
class PickerCombo : public QComboBox
{
public:
    void setPrefix(const QString &prefix) { m_prefix = prefix; updateGeometry(); update(); }
    void setDetected(const QString &detected) { m_detected = detected; updateGeometry(); update(); }
    QString displayText() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QString m_prefix, m_detected;
};

enum class TextFormat { Auto, JoinLines, RemoveSpaces, FullWidth, HalfWidth };

class CaptureOverlay;

class OcrDialog : public QDialog
{
    Q_OBJECT
    friend class CaptureOverlay;   // Its OCR workflow test drives the card frame directly.
public:
    // translate mode: recognized lines go through the aggregating translation
    // gateway and the pane shows the translated text instead of the raw OCR.
    explicit OcrDialog(const QImage &image, const QRect &captureRect = {}, bool translate = false);
    ~OcrDialog() override;
    static QRect placement(const QRect &capture, const QRect &available);
    static QRect beside(const QRect &avoid, const QRect &available);
    static QRect centered(const QRect &anchor, const QRect &available);
    static bool prefersRows(const QSize &image, const QSize &card);
    static QString formatText(const QString &text, TextFormat kind);
    void start();
    void setStacked(bool rows);
    static int testLifetime();
    static int testTranslate();
protected:
    bool eventFilter(QObject *object,QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    Qt::Edges resizeEdges(const QPoint &point) const;
    void updateResizeCursor(Qt::Edges edges);
    bool dragsWindow(const QPoint &point) const;
    void toggleMaximize();
    void updateMaximizeButton();
    void translateResult(const OcrResult &result);
    void translateLines();
    void populateLanguages(const QString &source, const QString &target);
    void saveTranslateChoice() const;
    void setDisplay(int mode);
    void syncLayoutButton();
    void syncPickers();
    void updateMask();
    void setActionsEnabled(bool enabled);
    QList<QTextCursor> matches() const;
    void showFind(bool replace, QPlainTextEdit *target = nullptr);
    void hideFind();
    void findStep(int direction);
    void highlightMatches(bool reveal = false);
    void replaceOne();
    void replaceAll();
    QImage m_image;
    QPlainTextEdit *m_text = nullptr;
    QLabel *m_status = nullptr, *m_findCount = nullptr, *m_busy = nullptr;
    QToolButton *m_retry = nullptr, *m_copy = nullptr, *m_maximize = nullptr, *m_layout = nullptr,
                *m_format = nullptr, *m_replaceToggle = nullptr;
    QSplitter *m_split = nullptr;
    QWidget *m_imagePanel = nullptr, *m_find = nullptr, *m_replaceRow = nullptr;
    QLineEdit *m_findEdit = nullptr, *m_replaceEdit = nullptr;
    OcrCancel m_cancel;
    QFutureWatcher<OcrResult> m_watcher;
    QString m_previous, m_raw;
    QAction *m_autoFormat = nullptr;
    TranslateGateway *m_gateway = nullptr;
    QPlainTextEdit *m_sourcePane = nullptr;   // Recognized text; editing it re-translates.
    QPlainTextEdit *m_findTarget = nullptr;   // Pane the find strip searches and sits in.
    QWidget *m_sourceBox = nullptr;
    QBoxLayout *m_sourceLayout = nullptr, *m_resultLayout = nullptr;
    PickerCombo *m_provider = nullptr, *m_source = nullptr, *m_target = nullptr;
    QToolButton *m_view = nullptr;            // Display mode: image + texts / texts / translation only.
    QToolButton *m_again = nullptr;           // Re-translate the current original without recognizing again.
    QSplitter *m_texts = nullptr;             // Original above (or left of) the translation.
    QWidget *m_mask = nullptr;                // Haze over a translation the edited original no longer matches.
    QString m_translatedSource;               // Original text the shown translation was requested for.
    // Card-drawn window frame: edge resize, drag from any empty spot, maximize to the work area.
    Qt::Edges m_resizing;
    bool m_dragging = false, m_maximized = false;
    QPoint m_pressGlobal, m_dragOffset;
    QRect m_pressGeometry, m_restoreGeometry;
    QWidget *m_header = nullptr;
    int m_match = -1, m_mode = 0;
    bool m_previousModified = false, m_rows = false, m_closed = false, m_translate = false, m_textRows = true;
};

OcrDialog *showOcr(const QImage &image, QWidget *source, const QRect &captureRect);
OcrDialog *showTranslate(const QImage &image, QWidget *source, const QRect &captureRect);
