#pragma once

#include "ocr_engine.h"
#include "widgets.h"
#include "output.h"
#include <QObject>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWidget>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QLabel>
#include <QFont>
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
    // Text on the capture: drag across characters to select them, Ctrl+C or the context menu copies.
    void setTextLines(const QVector<OcrLine> &lines);
    QString selectedText() const;
    void selectRange(int anchor, int lead);   // Glyph indices; -1 clears.
    // Marks the glyphs whose characters the text pane has selected (yellow, under the blue selection).
    void highlightText(const QString &selection);
    const QVector<int> &highlighted() const { return m_highlighted; }
    int glyphCount() const { return int(m_glyphs.size()); }
    QPolygonF glyphPolygon(int index) const { return m_glyphs.value(index).polygon; }
    std::function<void(bool)> copied;     // Copy result of the selected text.
protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void drawForeground(QPainter *painter, const QRectF &rect) override;
private:
    struct Glyph { QPolygonF polygon; int textIndex = -1, line = -1; };
    int glyphAt(const QPoint &viewPos, bool nearest) const;
    void copySelection();
    QRectF m_fit;
    bool m_zoomed = false;
    QString m_textData;                   // Lines joined by '\n'; glyph textIndex points into it.
    QVector<Glyph> m_glyphs;
    QVector<int> m_highlighted;
    int m_anchor = -1, m_lead = -1, m_press = -1;
    bool m_selecting = false;
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
class OcrLoadingBar;

class OcrDialog : public QDialog
{
    Q_OBJECT
    friend class CaptureOverlay;   // Its OCR workflow test drives the card frame directly.
public:
    // translate mode: recognized lines go through the aggregating translation
    // gateway and the pane shows the translated text instead of the raw OCR.
    explicit OcrDialog(const QImage &image, const QRect &captureRect = {}, bool translate = false);
    ~OcrDialog() override;
    static QRect screenCentered(const QRect &screen, const QSize &size);
    static QSize loadingSize();
    static QSize preferredSize(const QRect &available, bool translate);
    static QString formatText(const QString &text, TextFormat kind);
    void start();
    static int testLifetime();
    static int testTranslate();
protected:
    bool eventFilter(QObject *object,QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
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
    void syncPickers();
    void updateMask();
    void setActionsEnabled(bool enabled);
    void showLoading(bool loading, const QString &text = {});
    void applyLayout(bool force = false);
    int layoutClass() const;
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
    QToolButton *m_retry = nullptr, *m_copy = nullptr, *m_maximize = nullptr,
                *m_format = nullptr, *m_replaceToggle = nullptr;
    QSplitter *m_split = nullptr, *m_textSplit = nullptr;
    OcrImageView *m_imageView = nullptr;
    QWidget *m_imagePanel = nullptr, *m_find = nullptr, *m_replaceRow = nullptr;
    QWidget *m_content = nullptr, *m_toolbar = nullptr, *m_loadingPage = nullptr;
    OcrLoadingBar *m_loadingBar = nullptr;
    QRect m_screenRect;
    QSize m_finalSize;
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
    QWidget *m_mask = nullptr;                // Haze over a translation the edited original no longer matches.
    QString m_translatedSource;               // Original text the shown translation was requested for.
    // Card-drawn window frame: edge resize, drag from any empty spot, maximize to the work area.
    Qt::Edges m_resizing;
    bool m_dragging = false, m_maximized = false;
    QPoint m_pressGlobal, m_dragOffset;
    QRect m_pressGeometry, m_restoreGeometry;
    QWidget *m_header = nullptr;
    int m_match = -1, m_mode = 0, m_layoutClass = -1;
    bool m_applyingLayout = false;
    bool m_previousModified = false, m_rows = false, m_closed = false, m_translate = false;
};

OcrDialog *showOcr(const QImage &image, QWidget *source, const QRect &captureRect);
OcrDialog *showTranslate(const QImage &image, QWidget *source, const QRect &captureRect);
