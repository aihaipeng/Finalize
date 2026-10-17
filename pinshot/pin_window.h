#pragma once

#include "output.h"
#include "ocr_engine.h"
#include "widgets.h"
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QPaintEvent>
#include <QContextMenuEvent>
#include <QWidget>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QSizeF>
#include <QPointer>
#include <QMap>
#include <QPair>
#include <QtMath>
#include <qt_windows.h>

class QMenu;

// ---------- Pinned image window ----------
class PinWindow : public QWidget
{
    Q_OBJECT
public:
    explicit PinWindow(const QPixmap &pix, const QRect &displayRect);
    ~PinWindow() override;
    static QRect fittedGeometry(const QSize &imageSize, const QRect &source, const QRect &available);
    static int testPlacement();
    QMenu *createContextMenu();
protected:
    void paintEvent(QPaintEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    struct CharBox {
        QChar ch;
        QRectF rect; // In image logical coordinates
        int lineIdx = -1;
    };
private:
    void applyProcessedImage();
    void resetProcessing();
    void rotateBy(int degrees);
    void flipHorizontal();
    void flipVertical();
    void invertColors();
    void setOpacityPercent(int percent);
    void setBrightnessPercent(int percent);
    void setShadowEnabled(bool enabled);
    void toggleTextSelectable(bool enabled);
    void runOcrForTextSelection();
    int charIndexAt(const QPoint &pos, bool proximity = true) const;
    QString selectedText() const;
    void toggleToolbar(bool enabled);
    void updateToolbarPosition();
    // Text markup over the OCR selection, stored per character: highlight keeps
    // its own slot while the three line marks share one (they replace each
    // other), mirroring the reference pin's per-character map<type,color>.
    // Actions operate on the intersection with the selection, so selecting
    // more or fewer characters than were marked still hits the mark.
    enum TextDecoType { DecoHighlight = 0, DecoWavy, DecoLine, DecoStrike };
    void ensureSelToolbar();
    void updateSelToolbar();
    void drawTextDeco(int type, const QColor &color);   // Applies to selected chars.
    void clearTextDeco(int type = -1);                  // Removes from selected chars; -1 = every mark.
    bool selectionUniformDeco(int type, const QColor &color) const;
    void rebuildCharBoxes();                            // Rebuilds char boxes from the OCR lines.

    QPixmap m_originalPix;
    QPixmap m_pix;
    double m_scale;
    QPoint m_dragOff;
    bool m_borderOn = true;
    bool m_shadowOn = true;
    int m_rotation = 0;       // 0, 90, 180, 270
    bool m_flipH = false;
    bool m_flipV = false;
    bool m_inverted = false;
    int m_opacityPercent = 100;     // 10..100
    int m_brightnessPercent = 100;  // 0..200

    // Text selection support
    bool m_textSelectable = false;
    bool m_ocrRunning = false;
    OcrCancel m_ocrCancel;
    QVector<OcrLine> m_ocrLines;
    QVector<CharBox> m_charBoxes;
    QSizeF m_ocrImgSize;   // Raw pixel size of the image OCR boxed; boxes live in this space.
    int m_selAnchor = -1;
    int m_selLead = -1;
    bool m_selecting = false;
    bool m_selBarOpen = false;       // Shown only after a drag finishes; the close button clears it.
    // Per-character decorations keyed to the current OCR char boxes.
    QMap<int, QColor> m_decoHighlight;                  // charIdx -> fill color.
    QMap<int, QPair<int, QColor>> m_decoLine;           // charIdx -> {type, color}.
    QWidget *m_selToolbar = nullptr; // Floating markup toolbar under the selection.
    // Each markup tool remembers its own color (reference behavior); the last
    // tool clicked stays "current" and is the target of color/clear actions.
    int m_decoTool = 0;
    QColor m_decoToolColor[4];

    // Pin toolbar support
    bool m_toolbarOn = false;
    QWidget *m_toolbar = nullptr;
};

// Place a slider flyout fully outside its parent; shared with tests.
QPoint flyoutOutsideParent(const QMenu *subMenu, const QMenu *parent, const QRect &item);
void separateFlyoutEdge(QMenu *menu);
