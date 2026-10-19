#pragma once

#include "text/engines/ocr_engine.h"
#include "text/engines/barcode_engine.h"
#include "shared/widgets.h"
#include "platform/output.h"
#include <QCoreApplication>
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
#include <QStringList>
#include <QBoxLayout>
#include <QTextCursor>
#include <QFutureWatcher>
#include <functional>
#include <qt_windows.h>

class OcrImageView : public QGraphicsView
{
    Q_DECLARE_TR_FUNCTIONS(OcrImageView)
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
    bool selectedTextRange(int *from, int *to) const;   // Inclusive offsets in the recognized text.
    void selectRange(int anchor, int lead);   // Glyph indices; -1 clears.
    void clearSelection();                    // Drops the glyph selection without reporting it.
    QVector<int> selectedLines() const;       // OCR line indices touched by the glyph selection.
    // Marks the glyphs whose characters the text pane has selected (amber, under the blue selection).
    void highlightText(const QString &selection);
    void highlightTextRange(int from, int to); // Exact recognized-text range; -1 clears.
    // Barcode mode: mark every decoded symbol and emphasize the selected one.
    void setBarcodeRegions(const QVector<QPolygonF> &regions, int selected);
    const QVector<int> &highlighted() const { return m_highlighted; }
    int barcodeRegionCount() const { return int(m_barcodeRegions.size()); }
    int barcodeSelected() const { return m_barcodeSelected; }
    int glyphCount() const { return int(m_glyphs.size()); }
    QPolygonF glyphPolygon(int index) const { return m_glyphs.value(index).polygon; }
    std::function<void(bool)> copied;     // Copy result of the selected text.
    std::function<void()> selectionChanged;   // Glyph selection added, moved or cleared.
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
    QVector<QPolygonF> m_barcodeRegions;
    int m_barcodeSelected = -1;
    int m_anchor = -1, m_lead = -1, m_press = -1;
    bool m_selecting = false;
};

// Text pane with an optional Ctrl+wheel handler; the handler returns true when it consumed the wheel.
class OcrTextEdit : public QPlainTextEdit
{
public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<bool(QWheelEvent *)> wheelHandler;
protected:
    void wheelEvent(QWheelEvent *event) override;
};

class QSplitter;
class TranslateGateway;

// Picker that reads "prefix (choice)". On the first, automatic item it shows what the
// last run actually used ("语言检测 (韩语)") once that is known; the list keeps plain names.
class PickerCombo : public QComboBox
{
    Q_DECLARE_TR_FUNCTIONS(PickerCombo)
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
    explicit OcrDialog(const QImage &image, const QRect &captureRect = {}, bool translate = false,
                       bool barcode = false);
    ~OcrDialog() override;
    static QRect screenCentered(const QRect &screen, const QSize &size);
    static QSize loadingSize();
    static QSize preferredSize(const QRect &available, bool translate);
    static QString formatText(const QString &text, TextFormat kind);
    bool returnedToSource() const { return m_returnedToSource; }
    int barcodeRegionCount() const { return m_imageView ? m_imageView->barcodeRegionCount() : 0; }
    int barcodeSelected() const { return m_imageView ? m_imageView->barcodeSelected() : -1; }
    void start();
    static int testLifetime();
    static int testTranslate();
    static int testBarcode(const QString &previewPath);
protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *object,QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
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
    void startBarcode();
    void finishBarcode(const BarcodeResult &result);
    void showBarcodeItem(int index);
    void populateLanguages(const QString &source, const QString &target);
    void saveTranslateChoice() const;
    void setDisplay(int mode);
    void syncPickers();
    void updateMask();
    void setActionsEnabled(bool enabled);
    void showLoading(bool loading, const QString &text = {});
    void showEmptyState(const QString &title,const QString &detail);
    void applyLayout(bool force = false);
    int layoutClass() const;
    void linkFromOcrTextSelection();
    void linkFromOcrImageSelection();
    void highlightImageForPane(QPlainTextEdit *pane);
    void clearPaneSelection(QPlainTextEdit *pane);
    void clearSelectionHighlights();
    void clearLinkHighlights();
    void applyHighlights(QPlainTextEdit *pane);
    void setSelectionMark(QPlainTextEdit *pane, const QTextCursor &selection,
                          QList<QTextEdit::ExtraSelection> &marks);
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
    QLabel *m_imageCount = nullptr, *m_sourceCount = nullptr, *m_resultCount = nullptr;
    QLabel *m_sourceZoomLevel = nullptr, *m_resultZoomLevel = nullptr;
    QToolButton *m_retry = nullptr, *m_maximize = nullptr,
                *m_format = nullptr, *m_replaceToggle = nullptr;
    QSplitter *m_split = nullptr, *m_textSplit = nullptr;
    OcrImageView *m_imageView = nullptr;
    QWidget *m_imagePanel = nullptr, *m_find = nullptr, *m_replaceRow = nullptr;
    QWidget *m_content = nullptr, *m_toolbar = nullptr, *m_loadingPage = nullptr;
    OcrLoadingBar *m_loadingBar = nullptr;
    QToolButton *m_emptyRetry = nullptr;
    QLabel *m_emptyIcon = nullptr;
    QRect m_screenRect;
    QSize m_finalSize;
    QLineEdit *m_findEdit = nullptr, *m_replaceEdit = nullptr;
    OcrCancel m_cancel;
    QFutureWatcher<OcrResult> m_watcher;
    QFutureWatcher<BarcodeResult> m_barcodeWatcher;
    BarcodeCancel m_barcodeCancel;
    QVector<BarcodeItem> m_barcodeItems;
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
    QToolButton *m_open = nullptr;            // Barcode link action, shown only for URL payloads.
    QComboBox *m_barcodePicker = nullptr;     // One entry per decoded symbol; hidden for a single code.
    QLabel *m_barcodeLabel = nullptr;         // Format name or "N codes" above the payload.
    QWidget *m_mask = nullptr;                // Haze over a translation the edited original no longer matches.
    QString m_translatedSource;               // Original text the shown translation was requested for.
    QStringList m_ocrLineTexts;               // Recognized lines, in image-line order.
    QFont m_sourceBaseFont, m_resultBaseFont;
    qreal m_sourceScale = 1.0, m_resultScale = 1.0;
    QList<QTextEdit::ExtraSelection> m_resultLinkMarks, m_findMarks;
    // Card-drawn window frame: edge resize, drag from any empty spot, maximize to the work area.
    Qt::Edges m_resizing;
    bool m_dragging = false, m_maximized = false;
    QPoint m_pressGlobal, m_dragOffset;
    QRect m_pressGeometry, m_restoreGeometry;
    QWidget *m_header = nullptr;
    int m_match = -1, m_mode = 0, m_layoutClass = -1;
    bool m_applyingLayout = false;
    bool m_returnedToSource = false;
    bool m_previousModified = false, m_rows = false, m_closed = false, m_translate = false, m_barcode = false;
};

OcrDialog *showOcr(const QImage &image, QWidget *source, const QRect &captureRect);
OcrDialog *showTranslate(const QImage &image, QWidget *source, const QRect &captureRect);
OcrDialog *showBarcode(const QImage &image, QWidget *source, const QRect &captureRect);
