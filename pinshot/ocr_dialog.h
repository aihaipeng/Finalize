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
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QSplitter>
#include <QBoxLayout>
#include <QTextCursor>
#include <QFutureWatcher>
#include <qt_windows.h>

class OcrImageView : public QGraphicsView
{
public:
    explicit OcrImageView(const QImage &image, QWidget *parent = nullptr);
    void fit();
protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
private:
    QRectF m_fit;
    bool m_zoomed = false;
};

enum class TextFormat { Auto, JoinLines, RemoveSpaces, FullWidth, HalfWidth };

class OcrDialog : public QDialog
{
    Q_OBJECT
public:
    explicit OcrDialog(const QImage &image, const QRect &captureRect = {});
    ~OcrDialog() override;
    static QRect placement(const QRect &capture, const QRect &available);
    static QRect beside(const QRect &avoid, const QRect &available);
    static bool prefersRows(const QSize &image, const QSize &card);
    static QString formatText(const QString &text, TextFormat kind);
    void start();
    void setStacked(bool rows);
    static int testLifetime();
protected:
    bool eventFilter(QObject *object,QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    Qt::Edges resizeEdges(const QPoint &point) const;
    QList<QTextCursor> matches() const;
    void showFind(bool replace);
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
    QBoxLayout *m_imagePane = nullptr, *m_textPane = nullptr, *m_imageActions = nullptr, *m_textActions = nullptr;
    QWidget *m_imagePanel = nullptr, *m_find = nullptr, *m_replaceRow = nullptr;
    QLineEdit *m_findEdit = nullptr, *m_replaceEdit = nullptr;
    OcrCancel m_cancel;
    QFutureWatcher<OcrResult> m_watcher;
    QString m_previous, m_raw;
    QAction *m_autoFormat = nullptr;
    int m_match = -1;
    bool m_previousModified = false, m_rows = false, m_closed = false;
};

OcrDialog *showOcr(const QImage &image, QWidget *source, const QRect &captureRect);
