#include "shared/dialog_chrome.h"
#include "capture/capture_overlay.h"
#include <QClipboard>
#include <QAbstractItemView>
#include "platform/output.h"
#include "shared/widgets.h"
#include "annotate/smart_erase.h"
#include <QFutureWatcher>
#include <QtConcurrent>
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
#include <QIcon>
#include <QColor>
#include <QFont>
#include <QTransform>
#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QTextDocument>
#include <QTextCursor>
#include <QInputMethodEvent>
#include <QAbstractTextDocumentLayout>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <QSpinBox>
#include <QStylePainter>
#include <QStyledItemDelegate>
#include <QStyleOptionComboBox>
#include <QStyleOptionSlider>
#include <QScrollBar>
#include <QFontDatabase>
#include <QSlider>
#include <QVBoxLayout>
#include <QDialog>
#include <QRegularExpression>
#include <QDoubleValidator>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QCursor>
#include <QFontInfo>
#include <QPolygonF>
#include <qt_windows.h>
#include <imm.h>
#include <array>
#include <functional>
#include <algorithm>

namespace {
QIcon arrowStyleIcon(int style, qreal dpr)
{
    // One drawing grid for all ten glyphs: the head construction alone tells the styles apart.
    const int index = qBound(0, style, 9);
    QPixmap pm(qCeil(40 * dpr), qCeil(20 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    constexpr qreal y = 10.0;
    const QColor ink = Bar::ink;
    auto stroke = [&](qreal width, Qt::PenCapStyle cap = Qt::RoundCap) {
        return QPen(ink, width, Qt::SolidLine, cap, Qt::RoundJoin);
    };
    auto line = [&](qreal x1, qreal x2) {
        p.setPen(stroke(2));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(x1, y), QPointF(x2, y));
    };
    auto fill = [&](const QPolygonF &polygon) {
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawPolygon(polygon);
    };
    auto head = [&](qreal tip, qreal dir, bool flat, bool slotted) {
        const qreal base = tip - dir * 8.0;
        QPolygonF polygon{QPointF(tip, y), QPointF(base, y - 5.0)};
        if (flat) {
            polygon << QPointF(base, y + 5.0);
        } else {
            const qreal notch = base + dir * (slotted ? 2.2 : 2.4);
            if (slotted)
                polygon << QPointF(notch, y - 0.8) << QPointF(notch, y + 0.8);
            else
                polygon << QPointF(notch, y);
            polygon << QPointF(base, y + 5.0);
        }
        fill(polygon);
    };
    auto openHead = [&](qreal tip, qreal dir) {
        p.setPen(stroke(2));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(tip, y), QPointF(tip - dir * 8.0, y - 5.0));
        p.drawLine(QPointF(tip, y), QPointF(tip - dir * 8.0, y + 5.0));
    };
    auto bar = [&](qreal x) {
        p.setPen(stroke(2.2, Qt::FlatCap));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(x, 4), QPointF(x, 16));
    };

    switch (index) {
    case 0:
        line(6, 35);
        head(36, 1, false, true);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawEllipse(QPointF(5.5, y), 1.5, 1.5);
        break;
    case 1:
        line(5, 35);
        head(36, 1, false, true);
        head(4, -1, false, true);
        break;
    case 2:
        line(5, 36);
        openHead(36, 1);
        break;
    case 3:
        line(4, 36);
        openHead(36, 1);
        openHead(4, -1);
        break;
    case 4: {
        QPolygonF outline;
        outline << QPointF(5, 8.0) << QPointF(27, 8.0) << QPointF(27, 4.8)
                << QPointF(36, 10) << QPointF(27, 15.2) << QPointF(27, 12.0)
                << QPointF(5, 12.0);
        p.setPen(stroke(1.6));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(outline);
        break;
    }
    case 5: {
        QPolygonF body;
        body << QPointF(5, 9.0) << QPointF(28, 7.4) << QPointF(28, 5.0)
             << QPointF(36, 10) << QPointF(28, 15.0) << QPointF(28, 12.6)
             << QPointF(5, 11.0);
        fill(body);
        break;
    }
    case 6:
        bar(8);
        bar(32);
        line(8, 32);
        break;
    case 7:
        line(5, 30);
        head(36, 1, true, false);
        break;
    case 8:
        line(4, 36);
        head(36, 1, true, false);
        head(4, -1, true, false);
        break;
    case 9:
        bar(6);
        bar(34);
        line(11, 29);
        head(7, -1, false, false);
        head(33, 1, false, false);
        break;
    }
    return QIcon(pm);
}

// Popup rows: a rounded wash under the current or hovered row, no style selection marker.
class FlatItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem plain = option;
        if (option.state & (QStyle::State_MouseOver | QStyle::State_Selected)) {   // Rounded row: accent when current, grey on hover.
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(option.state & QStyle::State_Selected ? Bar::accent : QColor(0x3c, 0x3c, 0x3c));
            painter->drawRoundedRect(QRectF(option.rect).adjusted(1, 1, -1, -1), 4, 4);
            painter->restore();
        }
        plain.state &= ~(QStyle::State_MouseOver | QStyle::State_Selected | QStyle::State_HasFocus);
        QStyledItemDelegate::paint(painter, plain, index);
    }
};

// Popup rows show only the glyph; names stay in tooltips and accessible text.
class GlyphItemDelegate : public FlatItemDelegate
{
public:
    using FlatItemDelegate::FlatItemDelegate;
protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->text.clear();
        option->decorationAlignment = Qt::AlignCenter;
        option->displayAlignment = Qt::AlignCenter;
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        return QSize(QStyledItemDelegate::sizeHint(option, index).width(), 24);
    }
};

// Popup scrollbar in the dark bar language: a slim thumb at rest; on hover a wider thumb
// over a visible track with up/down step arrows. The stylesheet only fixes the geometry
// (12px bar, 12px step buttons) so hit testing matches what paintEvent draws.
class FlatScrollBar : public QScrollBar
{
public:
    explicit FlatScrollBar(QWidget *parent = nullptr) : QScrollBar(Qt::Vertical, parent)
    {
        setAttribute(Qt::WA_Hover);
        setStyleSheet(QStringLiteral(
            "QScrollBar:vertical{width:12px;margin:12px 0px 12px 0px;background:transparent;border:none;}"
            "QScrollBar::handle:vertical{min-height:20px;}"
            "QScrollBar::sub-line:vertical{height:12px;subcontrol-position:top;subcontrol-origin:margin;}"
            "QScrollBar::add-line:vertical{height:12px;subcontrol-position:bottom;subcontrol-origin:margin;}"));
    }
protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave) update();
        return QScrollBar::event(e);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const auto part = [&](QStyle::SubControl sc) {
            return QRectF(style()->subControlRect(QStyle::CC_ScrollBar, &opt, sc, this));
        };
        const bool hot = underMouse() || isSliderDown();
        if (hot) {
            p.fillRect(rect(), QColor(0x2e, 0x2e, 0x30));
            p.setBrush(QColor(0xa1, 0xa1, 0xaa));
            const QPointF up = part(QStyle::SC_ScrollBarSubLine).center();
            const QPointF down = part(QStyle::SC_ScrollBarAddLine).center();
            p.drawPolygon(QPolygonF{up + QPointF(0, -2), up + QPointF(3, 1.5), up + QPointF(-3, 1.5)});
            p.drawPolygon(QPolygonF{down + QPointF(0, 2), down + QPointF(3, -1.5), down + QPointF(-3, -1.5)});
        }
        const QRectF handle = part(QStyle::SC_ScrollBarSlider);
        const qreal w = hot ? 8 : 4;
        p.setBrush(hot ? QColor(0x71, 0x71, 0x7a) : QColor(0x52, 0x52, 0x5b));
        p.drawRoundedRect(QRectF(hot ? (width() - w) / 2 : width() - w - 2, handle.top() + 1, w,
                                 qMax(0.0, handle.height() - 2)), w / 2, w / 2);
    }
};

// Pill combo: no native drop-down button, plus a thin chevron.
class FlatComboBox : public QComboBox
{
public:
    explicit FlatComboBox(bool glyphOnly = false) : m_glyphOnly(glyphOnly)
    {
        if (glyphOnly) {   // Narrow glyph pills keep the sample centered between the edge and the chevron.
            setItemDelegate(new GlyphItemDelegate(this));
            setStyleSheet(QStringLiteral("QComboBox{padding-left:6px;}QComboBox::drop-down{width:16px;}"));
        }
        else setItemDelegate(new FlatItemDelegate(this));
        view()->setVerticalScrollBar(new FlatScrollBar);
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        if (m_glyphOnly) option.currentText.clear();
        const QRect arrow = style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                    QStyle::SC_ComboBoxArrow, this);
        option.subControls &= ~QStyle::SC_ComboBoxArrow;
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Bar::muted, 1.5));
        const QPointF c = QRectF(arrow).center();
        painter.drawPolyline(QPolygonF{c + QPointF(-3, -1.5), c + QPointF(0, 1.5), c + QPointF(3, -1.5)});
    }
private:
    bool m_glyphOnly;
};
}

QComboBox *flatComboBox() { return new FlatComboBox; }

namespace {

// The native text item draws a dashed placeholder box for an empty focused document;
// the overlay paints its own thin frame, so that decoration is masked here.
class EditorTextItem : public QGraphicsTextItem
{
public:
    using QGraphicsTextItem::QGraphicsTextItem;
protected:
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override
    {
        QStyleOptionGraphicsItem clean = *option;
        clean.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus);
        QGraphicsTextItem::paint(painter, &clean, widget);
    }
};
}

QRect CaptureOverlay::annotationRect(const Annotation &ann)
{
    if ((ann.tool == ATool::Mosaic && ann.mosaicBrush) || (ann.tool == ATool::Eraser && !ann.fill)) {
        const int w = qCeil(ann.pen / 2.0) + 2;
        return ann.stroke.boundingRect().toAlignedRect().adjusted(-w, -w, w, w);
    }
    switch (ann.tool) {
    case ATool::Arrow: {
        const int m = qCeil(ann.pen * 3) + 2;     // Heads and dimension bars extend past the endpoints.
        return QRect(ann.a, ann.b).normalized().adjusted(-m, -m, m, m);
    }
    case ATool::Pen:    return ann.stroke.boundingRect().toRect();
    case ATool::Marker: {                          // Fat translucent stroke: include its width.
        if (ann.fill) return ann.rect;
        const int w = ann.pen * 5 / 2 + 2;
        return QRect(ann.a, ann.b).normalized().adjusted(-w, -w, w, w);
    }
    default:            return ann.rect;
    }
}

QTransform CaptureOverlay::annTransform(const Annotation &ann)
{
    QTransform t;
    if (ann.tool == ATool::Mosaic || qFuzzyIsNull(ann.rot)) return t;
    const QPointF c = QPointF(annotationRect(ann).center());
    t.translate(c.x(), c.y());
    t.rotate(ann.rot);
    t.translate(-c.x(), -c.y());
    return t;
}

QRect CaptureOverlay::rotatedBounds(const Annotation &ann)
{
    const QRect r = annotationRect(ann);
    return ann.tool == ATool::Mosaic || qFuzzyIsNull(ann.rot) ? r : annTransform(ann).mapToPolygon(r).boundingRect();
}

QPoint CaptureOverlay::mapToLocal(const Annotation &ann, const QPoint &pos)
{
    return qFuzzyIsNull(ann.rot) ? pos : annTransform(ann).inverted().map(QPointF(pos)).toPoint();
}

QPoint CaptureOverlay::mapFromLocal(const Annotation &ann, const QPoint &pos)
{
    return qFuzzyIsNull(ann.rot) ? pos : annTransform(ann).map(QPointF(pos)).toPoint();
}

qreal CaptureOverlay::distToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const qreal len = QLineF(a, b).length();
    if (len < 0.001) return QLineF(a, p).length();
    const qreal t = qBound(0.0, ((p.x() - a.x()) * (b.x() - a.x())
                               + (p.y() - a.y()) * (b.y() - a.y())) / (len * len), 1.0);
    return QLineF(p, a + (b - a) * t).length();
}

bool CaptureOverlay::annotationHit(const Annotation &ann, const QPoint &pos)
{
    const int tol = qMax(5, ann.pen / 2 + 4);
    if (ann.tool == ATool::Eraser && !ann.fill) return false;   // Brush erasures are not selectable.
    if (ann.tool == ATool::Mosaic && ann.mosaicBrush) {
        if (ann.stroke.isEmpty()) return false;
        for (int i = 1; i < ann.stroke.size(); ++i)
            if (distToSegment(pos, ann.stroke[i - 1], ann.stroke[i]) <= ann.pen / 2.0 + tol)
                return true;
        return QLineF(pos, ann.stroke.first()).length() <= ann.pen / 2.0 + tol;
    }
    switch (ann.tool) {
    case ATool::Rect:                            // Outlines select on their border only.
        return ann.rect.adjusted(-tol, -tol, tol, tol).contains(pos)
            && !ann.rect.adjusted(tol, tol, -tol, -tol).contains(pos);
    case ATool::Ellipse: {
        const QPointF d = pos - ann.rect.center();
        const qreal rx = ann.rect.width() / 2.0, ry = ann.rect.height() / 2.0;
        if (rx < 1 || ry < 1) return ann.rect.contains(pos);
        const qreal outer = qPow(d.x() / (rx + tol), 2) + qPow(d.y() / (ry + tol), 2);
        const qreal inner = qPow(d.x() / qMax(1.0, rx - tol), 2)
                          + qPow(d.y() / qMax(1.0, ry - tol), 2);
        return outer <= 1.0 && inner >= 1.0;
    }
    case ATool::Arrow:
        return distToSegment(QPointF(pos), ann.a, ann.b) <= tol + ann.pen / 2
            || QLineF(pos, ann.a).length() <= 3.0 * ann.pen + tol
            || QLineF(pos, ann.b).length() <= 3.0 * ann.pen + tol;
    case ATool::Pen:
        if (ann.stroke.isEmpty()) return false;
        for (int i = 1; i < ann.stroke.size(); ++i)
            if (distToSegment(QPointF(pos), ann.stroke[i - 1], ann.stroke[i])
                <= tol + ann.pen / 2)
                return true;
        return distToSegment(QPointF(pos), ann.stroke.first(), ann.stroke.first())
            <= tol + ann.pen / 2;
    case ATool::Marker:
        return ann.fill ? ann.rect.adjusted(-tol, -tol, tol, tol).contains(pos)
            : distToSegment(pos, ann.a, ann.b) <= ann.pen * 2.5 + tol;
    default:                                     // Area objects select anywhere inside.
        return ann.rect.adjusted(-tol, -tol, tol, tol).contains(pos);
    }
}

int CaptureOverlay::annotationAt(const QPoint &pos) const        // Topmost annotation first.
{
    // Deselecting locks an item into the image: only the still-selected one can be picked.
    const int i = m_selAnn;
    if (i >= 0 && i < m_anns.size() && !m_anns[i].mosaicBrush
        && m_anns[i].tool != ATool::Pen && m_anns[i].tool != ATool::Eraser
        && annotationHit(m_anns[i], mapToLocal(m_anns[i], pos))) return i;
    return -1;
}

bool CaptureOverlay::textInnerHit(const Annotation &ann, const QPoint &pos) // Text body vs. control ring.
{
    const QRect r = ann.rect.normalized();
    const QRect inner = r.adjusted(TextMargin, TextMargin, -TextMargin, -TextMargin);
    if (inner.width() <= 0 || inner.height() <= 0) return false;
    return inner.contains(mapToLocal(ann, pos));
}

int CaptureOverlay::textAnnotationAt(const QPoint &pos) const
{
    // Text stays directly editable after it loses selection; other items stay locked.
    for (int i = m_anns.size() - 1; i >= 0; --i) {
        if (!annotationHit(m_anns[i], mapToLocal(m_anns[i], pos))) continue;
        return m_anns[i].tool == ATool::Text ? i : -1;   // Never reach text through a covering item.
    }
    return -1;
}

QVector<QPoint> CaptureOverlay::annAnchors(const Annotation &ann) const
{
    if (ann.tool == ATool::Mosaic && ann.mosaicBrush) return {};
    if (ann.tool == ATool::Arrow || (ann.tool == ATool::Marker && !ann.fill)) return {ann.a, ann.b};
    switch (ann.tool) {
    case ATool::Marker:
    case ATool::Eraser:
        if (!ann.fill) return {};
        [[fallthrough]];
    case ATool::Rect: case ATool::Ellipse: case ATool::Mosaic:
    case ATool::Text: case ATool::Number: {
        const QRect r = ann.rect.normalized();
        const QPoint c = r.center();
        QVector<QPoint> anchors{r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight(),
                               QPoint(r.left(), c.y()), QPoint(r.right(), c.y()),
                               QPoint(c.x(), r.top()), QPoint(c.x(), r.bottom())};
        if (ann.tool == ATool::Text || ann.tool == ATool::Number) anchors.resize(4);
        if (ann.tool == ATool::Ellipse || (ann.tool == ATool::Number && ann.numberStyle != 2)) {
            const QPointF center = QRectF(r.topLeft(), r.bottomRight()).center();
            for (int i = 0; i < 4; ++i)
                anchors[i] = (center + (QPointF(anchors[i]) - center) * M_SQRT1_2).toPoint();
        }
        return anchors;
    }
    default:
        return {};
    }
}

CaptureOverlay::Handle CaptureOverlay::annHandleAt(const Annotation &ann, const QPoint &pos) const
{
    if (ann.tool == ATool::Pen || ann.mosaicBrush || (ann.tool == ATool::Eraser && !ann.fill)) return H_None;
    const QVector<QPoint> anchors = annAnchors(ann);
    static const Handle ids[] = {H_TL, H_TR, H_BL, H_BR, H_L, H_R, H_T, H_B};
    const bool endpoints = ann.tool == ATool::Arrow || (ann.tool == ATool::Marker && !ann.fill);
    for (int i = 0; i < anchors.size(); ++i)
        if (QRect(mapFromLocal(ann, anchors[i]) - QPoint(5, 5), QSize(10, 10)).contains(pos))
            return endpoints ? (i == 0 ? H_TL : H_BR) : ids[i];
    if (ann.tool == ATool::Mosaic) {
        const QRect r = ann.rect;
        if (r.adjusted(-5, -5, 5, 5).contains(pos)) {
            if (qAbs(pos.x() - r.left()) <= 5) return H_L;
            if (qAbs(pos.x() - r.right()) <= 5) return H_R;
            if (qAbs(pos.y() - r.top()) <= 5) return H_T;
            if (qAbs(pos.y() - r.bottom()) <= 5) return H_B;
        }
    }
    return H_None;
}

void CaptureOverlay::paintHandle(QPainter &p, const QPointF &center)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0xf7, 0xf8, 0xfa), 1));   // Accent dot inside a thin light ring.
    p.setBrush(Bar::accent);
    p.drawEllipse(center, 4, 4);                 // Hit ranges stay unchanged.
    p.restore();
}

void CaptureOverlay::paintMosaicBorder(QPainter &p, const Annotation &ann, bool selected) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    // The frame and handles stay visible outside the region; the effect pixels
    // remain visible on screen and are clipped only by the final capture output.
    if (!selected) {
        p.setCompositionMode(QPainter::CompositionMode_Difference);
        p.setPen(QPen(Qt::white, 0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(ann.rect).adjusted(0, 0, -1, -1));
    }
    if (selected) {
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        for (const QPoint &anchor : annAnchors(ann))
            paintHandle(p, anchor);
    }
    p.restore();
}

QCursor CaptureOverlay::mosaicBrushCursor(qreal width, qreal dpr)
{
    const int side = qCeil(width) | 1;
    QPixmap pixmap(qRound(side * dpr), qRound(side * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    QRectF circle(0, 0, width, width);
    circle.moveCenter(QPointF(side / 2.0, side / 2.0));
    painter.drawEllipse(circle);
    painter.end();
    return QCursor(pixmap);
}

void CaptureOverlay::updateAnnotationCursor(const QPoint &pos)
{
    if (m_mode != Adjusting) { setCursor(Qt::CrossCursor); return; }
    if (overToolChrome(pos)) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    const Handle frame = hitHandle(pos);
    if (!m_embedded && !m_pinEdit && frame != H_None && frame != H_Move) {
        setCursor(cursorForHandle(frame));      // The frame and outside area resize the region.
        return;
    }
    if (!m_live && (m_atool == ATool::Text || m_atool == ATool::None)) {
        const int hit = textAnnotationAt(pos);
        if (hit >= 0) {
            if (hit == m_selAnn) {
                const Handle handle = annHandleAt(m_anns[hit], pos);
                if (handle != H_None) { setCursor(cursorForHandle(handle)); return; }
            }
            setCursor(textInnerHit(m_anns[hit], pos) ? Qt::IBeamCursor : Qt::SizeAllCursor);
            return;
        }
    }
    if (!m_live && m_selAnn >= 0 && m_selAnn < m_anns.size()) {
        const Annotation &ann = m_anns[m_selAnn];
        const Handle handle = annHandleAt(ann, pos);
        if (handle != H_None) { setCursor(cursorForHandle(handle)); return; }
        if (annotationAt(pos) == m_selAnn || (regionItem(ann) && annotationHit(ann, mapToLocal(ann, pos)))) {
            setCursor(Qt::SizeAllCursor);
            return;
        }
    }
    const Handle handle = hitHandle(pos);
    if (m_atool == ATool::Mosaic && mosaicBrush() && handle == H_Move && !m_spaceHeld) {
        const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(pos));
        setCursor(mosaicBrushCursor(toolSize(), screen ? screen->devicePixelRatio() : devicePixelRatioF()));
    } else if (m_atool == ATool::Eraser && !eraserRect() && handle == H_Move && !m_spaceHeld) {
        const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(pos));
        setCursor(eraserCursor(toolSize(), screen ? screen->devicePixelRatio() : devicePixelRatioF()));
    } else setCursor(m_atool != ATool::None && handle == H_Move ? Qt::CrossCursor : cursorForHandle(handle));
}

void CaptureOverlay::translateAnnotation(Annotation &ann, const QPoint &d)
{
    ann.rect.translate(d);
    ann.a += d;
    ann.b += d;
    ann.stroke.translate(d);
}

void CaptureOverlay::applyItemDrag(const QPoint &pos)
{
    if (m_selAnn < 0 || m_selAnn >= m_anns.size()) { stopItemDrag(); return; }
    Annotation ann = m_pressAnn;                 // Always computed from the press snapshot.
    if (m_itemDrag == H_Move) translateAnnotation(ann, pos - m_pressPos);
    else {
        QPoint local = mapToLocal(ann, pos);   // Handle math runs in item space.
        if (ann.tool == ATool::Arrow || (ann.tool == ATool::Marker && !ann.fill)) {
            if (m_itemDrag == H_TL) ann.a = local; else ann.b = local;
        } else {
            const QRect r = m_pressAnn.rect.normalized();
            const QPoint fixed = m_itemDrag == H_TL ? r.bottomRight()
                : m_itemDrag == H_TR ? r.bottomLeft()
                : m_itemDrag == H_BL ? r.topRight() : r.topLeft();
            if (m_itemDrag == H_L || m_itemDrag == H_R
                || m_itemDrag == H_T || m_itemDrag == H_B) {
                QPoint first = r.topLeft(), last = r.bottomRight();
                if (m_itemDrag == H_L) first.setX(local.x());
                else if (m_itemDrag == H_R) last.setX(local.x());
                else if (m_itemDrag == H_T) first.setY(local.y());
                else last.setY(local.y());
                ann.rect = QRect(QPoint(qMin(first.x(), last.x()), qMin(first.y(), last.y())),
                                 QPoint(qMax(first.x(), last.x()), qMax(first.y(), last.y())));
            } else {
                if (ann.tool == ATool::Ellipse || (ann.tool == ATool::Number && ann.numberStyle != 2)) {
                    const QPoint corner = m_itemDrag == H_TL ? r.topLeft()
                        : m_itemDrag == H_TR ? r.topRight() : m_itemDrag == H_BL ? r.bottomLeft() : r.bottomRight();
                    const int index = m_itemDrag == H_TL ? 0 : m_itemDrag == H_TR ? 1 : m_itemDrag == H_BL ? 2 : 3;
                    // The contour handle sits inside the box corner; invert its fixed-corner ratio.
                    local = corner + (QPointF(local - annAnchors(m_pressAnn)[index]) * (2 / (1 + M_SQRT1_2))).toPoint();
                }
                ann.rect = QRect(QPoint(qMin(fixed.x(), local.x()), qMin(fixed.y(), local.y())),
                                 QPoint(qMax(fixed.x(), local.x()), qMax(fixed.y(), local.y())));
            }
            if (ann.tool == ATool::Text) {       // Resize letters, not an invisible wrapping box.
                const qreal scale = qMax(qreal(ann.rect.width()) / qMax(1, r.width()),
                                         qreal(ann.rect.height()) / qMax(1, r.height()));
                ann.font = qBound(qreal(1), qRound(m_pressAnn.font * scale * 2) / 2.0, qreal(1638));
                fitTextAnnotation(ann);
                if (m_itemDrag == H_TL) ann.rect.moveBottomRight(fixed);
                else if (m_itemDrag == H_TR) ann.rect.moveBottomLeft(fixed);
                else if (m_itemDrag == H_BL) ann.rect.moveTopRight(fixed);
                else ann.rect.moveTopLeft(fixed);
            } else if (ann.tool == ATool::Number) { // Circles stay square while resizing.
                const int s = qMax(ann.rect.width(), ann.rect.height());
                ann.rect = QRect(ann.rect.topLeft(), QSize(s, s));
                if (m_itemDrag == H_TL) ann.rect.moveBottomRight(fixed);
                else if (m_itemDrag == H_TR) ann.rect.moveBottomLeft(fixed);
                else if (m_itemDrag == H_BL) ann.rect.moveTopRight(fixed);
                else ann.rect.moveTopLeft(fixed);
            }
        }
    }
    m_anns[m_selAnn] = ann;
    syncSizeInput();
}

QVector<CaptureOverlay::Annotation> CaptureOverlay::annotationSnapshot() const
{
    QVector<Annotation> snapshot = m_anns;
    for (Annotation &ann : snapshot) {
        // Keep undo snapshots semantic. Full-size effect layers are rebuilt on
        // demand; retaining one per snapshot can cost several GB on 4K captures.
        ann.mosaicCache = {};
        ann.mosaicCacheStrength = 0;
        ann.smartResult = {};
        ann.mosaicSource = {};
        ann.smartPending = ann.smartFailed = false;
    }
    return snapshot;
}

void CaptureOverlay::pushUndo()                                  // Snapshot stacks: one list per step.
{
    m_undoStack.append(annotationSnapshot());
    if (m_undoStack.size() > 100) m_undoStack.removeFirst();
    m_redoStack.clear();
    syncUndoButtons();
}

void CaptureOverlay::syncUndoButtons()                           // Buttons follow the stacks, like the bar.
{
    if (m_undoButton) m_undoButton->setEnabled(!m_undoStack.isEmpty());
    if (m_redoButton) m_redoButton->setEnabled(!m_redoStack.isEmpty());
    if (m_embedded) emit undoAvailability(!m_undoStack.isEmpty(), !m_redoStack.isEmpty());
}

void CaptureOverlay::normalizeNumbers()                          // Sequence numbers follow list order.
{
    int n = 0;
    for (Annotation &ann : m_anns)
        if (ann.tool == ATool::Number) ann.text = numberLabel(++n, ann.numberFormat);
}

QString CaptureOverlay::numberLabel(int n, int format)
{
    switch (format) {
    case 1: {                                    // Roman numerals.
        static const std::pair<int, const char *> table[] = {
            {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"},
            {50, "L"}, {40, "XL"}, {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"}};
        QString out;
        for (const auto &[value, glyph] : table)
            for (; n >= value; n -= value) out += QLatin1String(glyph);
        return out;
    }
    case 2:
    case 3: {                                    // Spreadsheet letters: A..Z, AA, AB...
        QString out;
        for (; n > 0; n = (n - 1) / 26) out.prepend(QChar((format == 2 ? 'A' : 'a') + (n - 1) % 26));
        return out;
    }
    case 4: {                                    // Chinese numerals up to 9999.
        static const QString digits = QStringLiteral("零一二三四五六七八九");
        static const QString units[] = {QString(), QStringLiteral("十"), QStringLiteral("百"), QStringLiteral("千")};
        if (n <= 0 || n > 9999) return QString::number(n);
        QString out;
        bool zero = false;
        for (int pos = 3; pos >= 0; --pos) {
            const int scale = pos == 3 ? 1000 : pos == 2 ? 100 : pos == 1 ? 10 : 1;
            const int d = n / scale % 10;
            if (d == 0) { zero = !out.isEmpty(); continue; }
            if (zero) out += digits[0];
            zero = false;
            if (!(d == 1 && pos == 1 && out.isEmpty())) out += digits[d];   // 十, not 一十.
            out += units[pos];
        }
        return out;
    }
    default:
        return QString::number(n);
    }
}

QPainterPath CaptureOverlay::strokeMask(const QPolygonF &stroke, qreal width)
{
    QPainterPath mask;
    if (stroke.isEmpty()) return mask;
    if (stroke.size() == 1) { mask.addEllipse(stroke.first(), width / 2, width / 2); return mask; }
    QPainterPath line;
    line.moveTo(stroke.first());
    for (int i = 1; i < stroke.size(); ++i) line.lineTo(stroke[i]);
    QPainterPathStroker stroker;
    stroker.setWidth(width);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    return stroker.createStroke(line);
}

QCursor CaptureOverlay::eraserCursor(qreal width, qreal dpr)   // Hollow ring, dark inside light, as wide as the brush.
{
    const int side = qCeil(width) + 5;
    QPixmap pixmap(qRound(side * dpr), qRound(side * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    const QPointF c(side / 2.0, side / 2.0);
    painter.setPen(QPen(Qt::white, 1));
    painter.drawEllipse(c, width / 2 + 1, width / 2 + 1);
    painter.setPen(QPen(QColor(0x20, 0x20, 0x20), 1));
    painter.drawEllipse(c, width / 2, width / 2);
    painter.drawEllipse(c, qMax(1.0, width / 2 - 2), qMax(1.0, width / 2 - 2));
    painter.end();
    return QCursor(pixmap);
}

void CaptureOverlay::deleteSelected()
{
    stopItemDrag();
    if (m_selAnn < 0 || m_selAnn >= m_anns.size()) { m_selAnn = -1; return; }
    pushUndo();
    ++m_smartEpoch;                              // In-flight erases must not land on shifted indexes.
    m_anns.removeAt(m_selAnn);
    m_selAnn = -1;
    normalizeNumbers();
    updateToolOptions();
    update();
}

QFont CaptureOverlay::textFont(const Annotation &ann)
{
    QFont font(ann.family);
    font.setPointSizeF(ann.font);
    font.setBold(ann.bold);
    font.setItalic(ann.italic);
    font.setUnderline(ann.underline);
    return font;
}

int CaptureOverlay::numberDiameter(const Annotation &ann)
{
    return qMax(1, QFontInfo(textFont(ann)).pixelSize()) * 2;
}

void CaptureOverlay::setupTextDocument(QTextDocument &doc, const Annotation &ann)
{
    doc.setDefaultFont(textFont(ann));
    doc.setDocumentMargin(TextMargin);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    doc.setDefaultTextOption(option);
    doc.setPlainText(ann.text);
    doc.setTextWidth(-1);
}

void CaptureOverlay::fitTextAnnotation(Annotation &ann)
{
    const QPoint origin = mapFromLocal(ann, ann.rect.topLeft());
    QTextDocument doc;
    setupTextDocument(doc, ann);
    const QSizeF size = doc.size();
    ann.rect.setSize(QSize(qCeil(size.width()), qCeil(size.height())));
    ann.rect.translate(origin - mapFromLocal(ann, ann.rect.topLeft()));
}

bool CaptureOverlay::editorActive() const { return m_textView && m_textView->isVisible(); }

CaptureOverlay::Annotation CaptureOverlay::editorAnnotation() const
{
    Annotation ann;
    ann.tool = ATool::Text;
    ann.text = m_editor->toPlainText();
    ann.font = m_editFont;
    ann.family = m_editFamily;
    ann.color = m_editColor;
    ann.bold = m_editBold;
    ann.italic = m_editItalic;
    ann.underline = m_editUnderline;
    ann.rot = m_editor->rotation();
    const QSizeF size = m_editor->boundingRect().size();
    // shortcut: Text geometry rounds to logical pixels; use fractional bounds for exact subpixel rotation.
    ann.rect = QRect(m_editor->pos().toPoint(), QSize(qCeil(size.width()), qCeil(size.height())));
    return ann;
}

void CaptureOverlay::syncTextEditorGeometry()
{
    if (!editorActive()) return;
    const QPointF origin = m_editor->mapToScene(QPointF());
    m_editor->setTransformOriginPoint(m_editor->boundingRect().center());
    m_editor->setPos(m_editor->pos() + origin - m_editor->mapToScene(QPointF()));
    // Keep the editor widget as small as the text. A full-screen mask rebuild on
    // every keystroke made typing and re-entering text visibly stutter.
    QRect viewRect = m_editor->mapRectToScene(m_editor->boundingRect()).toAlignedRect()
                        .adjusted(-2, -2, 2, 2).intersected(rect());
    // In a capture the editor stays fully visible outside the frame; only committed
    // content is clipped. A pin canvas remains fixed to the pin.
    if (!m_sel.isNull() && (m_pinEdit || m_embedded)) viewRect = viewRect.intersected(m_sel);
    if (viewRect.isEmpty())
        viewRect = QRect(std::clamp(qRound(m_editor->pos().x()), 0, (std::max)(0, width() - 1)),
                         std::clamp(qRound(m_editor->pos().y()), 0, (std::max)(0, height() - 1)), 1, 1);
    m_textView->setGeometry(viewRect);
    m_textView->setSceneRect(QRectF(viewRect));
    update();
}

bool CaptureOverlay::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == m_layoutCardWidget
        && (ev->type() == QEvent::Move || ev->type() == QEvent::Resize
            || ev->type() == QEvent::Show)) {
        updateLayoutCardRect();
        placeToolBars();
    }
    if (obj == m_morePanel && ev->type() == QEvent::Leave && m_morePanel->isVisible()) {
        QTimer::singleShot(180, this, [this] {
            const auto inside = [](QWidget *widget) {
                return widget && widget->isVisible()
                    && widget->rect().contains(widget->mapFromGlobal(QCursor::pos()));
            };
            if (m_morePanel && m_morePanel->isVisible()
                && !inside(m_morePanel) && !inside(m_moreButton)
                && m_moreButton) m_moreButton->setChecked(false);
        });
    }
    if (m_morePanel && m_morePanel->isVisible() && ev->type() == QEvent::MouseButtonPress
        && obj->isWidgetType()) {
        auto *widget = static_cast<QWidget *>(obj);
        if (widget != m_moreButton && !m_morePanel->isAncestorOf(widget)
            && m_moreButton) m_moreButton->setChecked(false);
    }
    if ((ev->type() == QEvent::FocusIn || ev->type() == QEvent::FocusOut) && obj->isWidgetType()) {
        for (QWidget *w = static_cast<QWidget *>(obj); w; w = w->parentWidget()) {
            if (w->objectName() != QStringLiteral("customField")) continue;
            w->setProperty("focus", ev->type() == QEvent::FocusIn);
            w->style()->unpolish(w);
            w->style()->polish(w);
            break;
        }
    }
    if (ev->type() == QEvent::ToolTip && obj->isWidgetType()) {   // No hover text anywhere in the capture UI.
        for (QWidget *w = static_cast<QWidget *>(obj); w; w = w->parentWidget())
            if (w == this) return true;
    }
    if (obj == m_sizeTip && !m_sel.isNull()
        && (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseButtonRelease)) {
        if (static_cast<QMouseEvent *>(ev)->button() == Qt::LeftButton
            && ev->type() == QEvent::MouseButtonPress)
            showCustomSizePopup();
        return true;
    }
    if (((m_sizeSlider && obj == m_sizeSlider) || obj == m_sizeButton) && ev->type() == QEvent::Wheel) {
        const int delta = static_cast<QWheelEvent *>(ev)->angleDelta().y();
        if (delta) setWidthControl(qRound(widthControlValue()) + (delta > 0 ? 1 : -1));
        ev->accept();
        return true;
    }
    if (ev->type() == QEvent::Wheel) {
        QSpinBox *spin = qobject_cast<QSpinBox *>(obj);
        if (!spin && obj->parent()) spin = qobject_cast<QSpinBox *>(obj->parent());
        if (spin && (spin == m_customX || spin == m_customY
                     || spin == m_customWidth || spin == m_customHeight
                     || spin == m_sizeRadiusSpin)) {
            const int delta = static_cast<QWheelEvent *>(ev)->angleDelta().y();
            if (delta)
                spin->setValue(spin->value() + (delta > 0 ? 1 : -1) * spin->singleStep());
            ev->accept();
            return true;
        }
    }
    if ((isToolBarWidget(obj) || obj == m_toolOptions)
        && (ev->type() == QEvent::Enter || ev->type() == QEvent::Leave))
        updateInspector();
    if (m_embedded && obj == m_toolOptions && ev->type() == QEvent::KeyPress) {
        // The detached strip is its own window: keys its controls leave
        // unhandled belong to the canvas, which takes focus back.
        window()->activateWindow();
        setFocus(Qt::OtherFocusReason);
        keyPressEvent(static_cast<QKeyEvent *>(ev));
        return true;
    }
    if ((obj == m_mosaicSlider || obj == m_mosaicStrengthLabel || obj == m_mosaicPreview)
        && ev->type() == QEvent::Wheel) {
        const int delta = static_cast<QWheelEvent *>(ev)->angleDelta().y();
        if (delta) setMosaicStrength(mosaicStrength() + (delta > 0 ? 1 : -1));
        ev->accept();
        return true;
    }
    if (m_sizeInput && (obj == m_sizeInput || obj == m_sizeInput->findChild<QLineEdit *>())) {
        if (ev->type() == QEvent::Wheel) {
            const int delta = static_cast<QWheelEvent *>(ev)->angleDelta().y();
            if (delta) tuneWidth(delta > 0 ? 1 : -1, mapFromGlobal(QCursor::pos()));
            ev->accept();
            return true;
        }
        if (ev->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(ev)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) {
                if (key == Qt::Key_Escape) syncSizeInput();
                else commitSizeInput();
                if (editorActive()) {
                    m_textView->setFocus();
                    m_editor->setFocus();
                } else setFocus();
                return true;
            }
        }
    }
    if (editorActive() && (obj == m_textView || obj == m_textView->viewport())) {
        if (ev->type() == QEvent::InputMethod) {
            m_textPreedit = !static_cast<QInputMethodEvent *>(ev)->preeditString().isEmpty();
        } else if (ev->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(ev);
            if (ke->key() == Qt::Key_Escape) {
                if (m_textPreedit) QGuiApplication::inputMethod()->reset();
                else commitTextEdit();
                m_textPreedit = false;
                return true;
            }
            if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
                && ke->modifiers() == Qt::ControlModifier && !m_textPreedit) {
                QTextCursor cur = m_editor->textCursor();
                cur.movePosition(QTextCursor::EndOfBlock);
                cur.insertText(QStringLiteral("\n"));
                m_editor->setTextCursor(cur);
                return true;
            }
            if (ke->key() == Qt::Key_V && ke->modifiers() == Qt::ControlModifier) {
                QTextCursor cur = m_editor->textCursor();
                cur.insertText(QGuiApplication::clipboard()->text());
                m_editor->setTextCursor(cur);
                return true;
            }
        } else if (ev->type() == QEvent::Wheel) {
            const int delta = static_cast<QWheelEvent *>(ev)->angleDelta().y();
            if (delta) editorFontStep(delta > 0 ? 1 : -1);
            return true;
        } else if (ev->type() == QEvent::ContextMenu) {
            return true;
        } else if (obj == m_textView->viewport() && ev->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(ev);
            if (me->button() == Qt::RightButton) { commitTextEdit(); return true; }
            if (me->button() == Qt::MiddleButton) {
                QTextCursor cur = m_editor->textCursor();
                cur.insertText(QGuiApplication::clipboard()->text());
                m_editor->setTextCursor(cur);
                return true;
            }
            const QPointF local = m_editor->mapFromScene(m_textView->mapToScene(me->pos()));
            if (me->button() == Qt::LeftButton
                && !m_editor->boundingRect().adjusted(TextMargin, TextMargin,
                                                      -TextMargin, -TextMargin).contains(local)) {
                m_textMoving = true;
                m_textMovePress = me->globalPosition().toPoint();
                m_textMoveOrigin = m_editor->pos();
                m_textView->viewport()->grabMouse();
                return true;
            }
        } else if (m_textMoving && ev->type() == QEvent::MouseMove) {
            auto *me = static_cast<QMouseEvent *>(ev);
            const QPoint delta = me->globalPosition().toPoint() - m_textMovePress;
            if (m_editor->pos() == m_textMoveOrigin
                && delta.manhattanLength() < QApplication::startDragDistance()) return true;
            m_editor->setPos(m_textMoveOrigin + delta);
            syncTextEditorGeometry();
            return true;
        } else if (m_textMoving && ev->type() == QEvent::MouseButtonRelease) {
            m_textMoving = false;
            m_textView->viewport()->releaseMouse();
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void CaptureOverlay::beginTextEdit(const QPoint &pos, int existing)
{
    commitTextEdit();
    m_editAnn = existing;
    m_editFont = existing >= 0 ? m_anns[existing].font : m_afont;
    m_editFamily = existing >= 0 ? m_anns[existing].family : m_afamily;
    m_editColor = existing >= 0 ? m_anns[existing].color : m_acolor;
    m_editBold = existing >= 0 ? m_anns[existing].bold : m_abold;
    m_editItalic = existing >= 0 ? m_anns[existing].italic : m_aitalic;
    m_editUnderline = existing >= 0 ? m_anns[existing].underline : m_aunderline;
    m_atool = ATool::Text;
    m_selAnn = existing;
    m_acolor = m_editColor;
    m_afont = m_editFont;
    m_afamily = m_editFamily;
    m_abold = m_editBold;
    m_aitalic = m_editItalic;
    m_aunderline = m_editUnderline;
    syncToolButtons();
    // The strip is already live for the text tool on the normal click path;
    // rebuilding its 200+ font items there made every re-entry feel sticky.
    if (m_toolOptions && m_toolOptions->findChild<QComboBox *>(QStringLiteral("annotationFont")))
        syncTextOptions();
    else
        updateToolOptions();
    if (!m_editor) {
        m_textView = new QGraphicsView(this);
        auto *scene = new QGraphicsScene(m_textView);
        m_textView->setScene(scene);
        m_textView->setFrameShape(QFrame::NoFrame);
        m_textView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_textView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_textView->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_textView->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        m_textView->setRenderHint(QPainter::Antialiasing);
        m_textView->setGeometry(rect());
        m_textView->setSceneRect(QRectF(rect()));
        m_textView->hide();
        m_editor = new EditorTextItem;
        scene->addItem(m_editor);
        m_editor->setTextInteractionFlags(Qt::TextEditorInteraction);
        m_editor->setTextWidth(-1);
        m_editor->document()->setDocumentMargin(TextMargin);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        m_editor->document()->setDefaultTextOption(option);
        m_textView->installEventFilter(this);
        m_textView->viewport()->installEventFilter(this);
        connect(m_editor->document()->documentLayout(),
                &QAbstractTextDocumentLayout::documentSizeChanged,
                this, [this] { syncTextEditorGeometry(); });
    }
    applyEditorFont();
    m_editor->setRotation(0);
    m_editor->setPlainText(existing >= 0 ? m_anns[existing].text : QString());
    m_editor->setTransformOriginPoint(m_editor->boundingRect().center());
    m_editor->setPos(existing >= 0 ? m_anns[existing].rect.topLeft() : pos - QPoint(TextMargin, TextMargin));
    if (existing >= 0) m_editor->setRotation(m_anns[existing].rot);
    QTextCursor cur = m_editor->textCursor();
    const int hit = m_editor->document()->documentLayout()->hitTest(
        m_editor->mapFromScene(QPointF(pos)), Qt::FuzzyHit);
    cur.setPosition(qMax(0, hit));
    m_editor->setTextCursor(cur);
    ImmAssociateContext(reinterpret_cast<HWND>(winId()), m_imeCtx);
    m_textView->show();
    m_textView->raise();
    raiseToolBars();
    m_toolOptions->raise();
    m_sizeBar->hide();
    m_inspector->hide();
    m_textView->setFocus();
    m_editor->setFocus(Qt::MouseFocusReason);
    syncTextEditorGeometry();
    update();
}

void CaptureOverlay::commitText()
{
    commitTextEdit();
}

void CaptureOverlay::commitTextEdit()
{
    if (m_colorDialog) m_colorDialog->close();
    if (m_committingText || !editorActive()) return;
    m_committingText = true;
    QGuiApplication::inputMethod()->commit();
    const Annotation edited = editorAnnotation();
    const QString text = edited.text;
    m_textView->hide();
    m_textMoving = m_textPreedit = false;
    ImmAssociateContext(reinterpret_cast<HWND>(winId()), nullptr); // Back to hotkey mode.
    const bool replace = m_editAnn >= 0 && m_editAnn < m_anns.size();
    bool changed = !text.trimmed().isEmpty();
    if (replace) {
        const Annotation &old = m_anns[m_editAnn];
        changed = old.text != text || old.rect != edited.rect || old.font != edited.font || old.family != edited.family
            || old.color != edited.color || old.bold != edited.bold || old.italic != edited.italic
            || old.underline != edited.underline || old.rot != edited.rot;
    }
    if (changed) {
        pushUndo();
        ++m_smartEpoch;                          // In-flight erases must not land on shifted indexes.
        if (text.trimmed().isEmpty()) {         // Clearing the text deletes the annotation.
            m_anns.removeAt(m_editAnn);
        } else if (replace) {
            m_anns[m_editAnn] = edited;
        } else {
            m_anns.append(edited);
        }
    }
    m_editAnn = -1;
    m_selAnn = -1;                               // Committing ends the text selection; a later click re-opens it.
    m_committingText = false;
    setFocus();
    syncSizeInput();
    update();
}

void CaptureOverlay::applyEditorFont()                          // Size, weight, slant, and underline in one place.
{
    if (!m_editor) return;
    Annotation ann;
    ann.font = m_editFont;
    ann.family = m_editFamily;
    ann.bold = m_editBold;
    ann.italic = m_editItalic;
    ann.underline = m_editUnderline;
    m_editor->setFont(textFont(ann));
    m_editor->setDefaultTextColor(m_editColor);
    syncTextEditorGeometry();
}

void CaptureOverlay::editorFontStep(int dir)
{
    setToolSize(m_editFont + dir * 0.5);
}

void CaptureOverlay::tipPosition(const QPoint &at)
{
    m_sizeBar->adjustSize();
    m_sizeBar->move(qBound(0, at.x() - m_sizeBar->width() / 2, width() - m_sizeBar->width()),
                    qMax(0, at.y() - m_sizeBar->height() - 16));
}

QPoint CaptureOverlay::keyDelta(int key)
{
    return key == Qt::Key_Left || key == Qt::Key_A ? QPoint(-1, 0)
        : key == Qt::Key_Right || key == Qt::Key_D ? QPoint(1, 0)
        : key == Qt::Key_Up || key == Qt::Key_W ? QPoint(0, -1)
        : key == Qt::Key_Down || key == Qt::Key_S ? QPoint(0, 1) : QPoint();
}

void CaptureOverlay::nudgeSelected(int key)                       // Arrows / WASD move the selected annotation one step.
{
    if (m_selAnn < 0 || m_selAnn >= m_anns.size() || m_live || editorActive())
        return;
    const QPoint d = keyDelta(key);
    if (d.isNull()) return;
    if (!m_nudging) {
        pushUndo();                               // Coalesce a key burst into one step.
        m_nudging = true;
    }
    translateAnnotation(m_anns[m_selAnn], d);
    update();
}

CaptureOverlay::ATool CaptureOverlay::optionsTool() const
{
    if (editorActive()) return ATool::Text;
    if (m_live)
        return m_atool == ATool::Eraser && m_ann.mosaicSmart ? ATool::Eraser : m_ann.tool;
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
        const Annotation &ann = m_anns[m_selAnn];
        if (ann.tool == ATool::Mosaic && ann.mosaicSmart) return ATool::Eraser;
        if (m_atool != ATool::Eraser) return ann.tool;
    }
    return m_atool;
}

bool CaptureOverlay::mosaicBrush() const
{
    if (m_live) return m_ann.mosaicBrush;
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()) return m_anns[m_selAnn].mosaicBrush;
    return m_mosaicBrush;
}

int CaptureOverlay::mosaicStrength() const
{
    if (m_live) return m_ann.mosaicStrength;
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()) return m_anns[m_selAnn].mosaicStrength;
    return m_mosaicLevels[m_mosaicBlur];
}

bool CaptureOverlay::mosaicBlur() const
{
    if (m_live) return m_ann.mosaicBlur;
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()) return m_anns[m_selAnn].mosaicBlur;
    return m_mosaicBlur;
}

bool CaptureOverlay::eraserSmart() const
{
    if (m_live) return m_ann.mosaicSmart;
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
        const Annotation &ann = m_anns[m_selAnn];
        if (ann.tool == ATool::Mosaic) return ann.mosaicSmart;
        if (ann.tool == ATool::Eraser) return false;
    }
    return m_eraserSmart;
}

void CaptureOverlay::switchMosaicEffect()
{
    if (optionsTool() != ATool::Mosaic) return;
    // Cycle pixelate <-> blur. Smart erasing lives in the eraser strip.
    const bool nextBlur = !mosaicBlur();
    m_mosaicBlur = nextBlur;
    Annotation *ann = m_live ? &m_ann : nullptr;
    if (!ann && m_selAnn >= 0 && m_selAnn < m_anns.size()) {
        pushUndo();
        ann = &m_anns[m_selAnn];
    }
    if (ann) {
        ann->mosaicBlur = nextBlur;
        ann->mosaicSmart = false;
        ann->mosaicStrength = ann->mosaicLevels[nextBlur ? 1 : 0];
        ann->mosaicCacheStrength = 0;
    }
    // Rebuilding the strip here would delete the button mid-click; toggle instead.
    syncSizeInput();
    update();
}

void CaptureOverlay::setMosaicStrength(int value)
{
    if (optionsTool() != ATool::Mosaic) return;
    value = qBound(3, value, 24);
    if (value != mosaicStrength()) {
        m_mosaicLevels[mosaicBlur()] = value;
        if (m_live) {
            m_ann.mosaicStrength = value;
            m_ann.mosaicLevels[m_ann.mosaicBlur] = value;
            m_ann.mosaicSeed = QRandomGenerator::global()->generate();
            m_ann.mosaicCacheStrength = 0;
        }
        else if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
            const bool dragging = m_mosaicSlider && m_mosaicSlider->isSliderDown();
            if (!dragging || !m_mosaicDragUndo) pushUndo();
            m_mosaicDragUndo = dragging;
            m_anns[m_selAnn].mosaicStrength = value;
            m_anns[m_selAnn].mosaicLevels[m_anns[m_selAnn].mosaicBlur] = value;
            m_anns[m_selAnn].mosaicSeed = QRandomGenerator::global()->generate();
            m_anns[m_selAnn].mosaicCacheStrength = 0;
        }
        update();
    }
    syncSizeInput();
}

void CaptureOverlay::selectMosaicMode(bool brush)
{
    if (m_live) { m_live = false; commitAnnotation(); }
    m_mosaicBrush = brush;
    selectTool(ATool::Mosaic);
}

QImage CaptureOverlay::smartEraseMask(const Annotation &ann, const QSize &deviceSize, qreal dpr)
{
    if (deviceSize.isEmpty() || dpr <= 0) return {};
    QImage mask(deviceSize, QImage::Format_Grayscale8);
    mask.fill(255);                                  // 255 keeps pixels, 0 marks the hole.
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.scale(dpr, dpr);
    if (ann.mosaicBrush) painter.drawPath(strokeMask(ann.stroke, ann.pen));
    else painter.drawRect(QRectF(ann.rect));
    painter.end();
    return mask;
}

void CaptureOverlay::scheduleSmartErase(int index, const QImage &source)
{
    if (index < 0 || index >= m_anns.size() || source.isNull()) return;
    Annotation &ann = m_anns[index];
    if (ann.tool != ATool::Mosaic || !ann.mosaicSmart || ann.smartPending) return;
    const QImage mask = smartEraseMask(ann, source.size(), m_shot.devicePixelRatioF());
    if (mask.isNull()) return;
    ann.smartPending = true;
    ann.smartFailed = false;
    ann.smartResult = {};
    ann.mosaicSource = source;                       // Paint compares against its sampling layer.
    ann.smartRect = ann.rect;
    ann.smartStroke = ann.stroke;
    ann.smartPen = ann.pen;
    const int epoch = m_smartEpoch;
    auto *watcher = new QFutureWatcher<QPair<QImage, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<QImage, QString>>::finished, this,
            [this, watcher, epoch, index] {
        const QPair<QImage, QString> result = watcher->result();
        watcher->deleteLater();
        if (epoch != m_smartEpoch || index < 0 || index >= m_anns.size()) return;
        Annotation &ann = m_anns[index];
        ann.smartPending = false;
        if (result.first.isNull()) {
            ann.smartFailed = true;              // Renders as a black redaction, never the source.
            if (!result.second.isEmpty()) qWarning() << "smart erase:" << result.second;
        } else {
            ann.smartResult = result.first;
        }
        update();
    });
    watcher->setFuture(QtConcurrent::run([source, mask] {
        QString error;
        const QImage erased = SmartErase::erase(source, mask, &error);
        return QPair<QImage, QString>(erased, error);
    }));
}

void CaptureOverlay::selectEraserMode(bool rect)
{
    if (m_live) { m_live = false; commitAnnotation(); }
    m_eraserRect = rect;
    m_eraserSmart = false;
    selectTool(ATool::Eraser);
}

void CaptureOverlay::toggleEraserSmart()
{
    m_eraserSmart = !eraserSmart();
    selectTool(ATool::Eraser);
}

bool CaptureOverlay::eraserRect() const
{
    if (m_live) return m_ann.fill;
    if (m_selAnn >= 0 && m_selAnn < m_anns.size() && m_anns[m_selAnn].tool == ATool::Eraser)
        return m_anns[m_selAnn].fill;
    return m_eraserRect;
}

bool CaptureOverlay::regionItem(const Annotation &ann)
{
    return (ann.tool == ATool::Mosaic && !ann.mosaicBrush) || (ann.tool == ATool::Eraser && ann.fill);
}

qreal CaptureOverlay::toolSize() const
{
    if (editorActive()) return m_editFont;
    const ATool tool = optionsTool();
    if (tool == ATool::Mosaic) {
        if (!mosaicBrush()) return mosaicStrength();
        if (m_live) return m_ann.pen;
        if (m_selAnn >= 0 && m_selAnn < m_anns.size()) return m_anns[m_selAnn].pen;
        return m_mosaicWidth;
    }
    if (tool == ATool::Eraser) return m_live ? m_ann.pen : m_eraserWidth;
    const bool texty = tool == ATool::Text || tool == ATool::Number;
    if (m_live) return texty ? m_ann.font : m_ann.pen;
    if (m_selAnn >= 0 && m_selAnn < m_anns.size())
        return texty ? m_anns[m_selAnn].font : m_anns[m_selAnn].pen;
    return texty ? m_afont : m_toolPens[tool == ATool::None ? int(ATool::Pen) : int(tool)];
}

qreal CaptureOverlay::widthControlValue() const       // Rectangle mosaics tune strength on the canvas, width on the button.
{
    return optionsTool() == ATool::Mosaic && !mosaicBrush() ? m_mosaicWidth : toolSize();
}

void CaptureOverlay::setWidthControl(int value)
{
    if (optionsTool() != ATool::Mosaic || mosaicBrush()) { setToolSize(value); return; }
    m_mosaicWidth = qBound(4, value, 50);        // Sets up the next brush; rectangles have no width.
    syncSizeInput();
}

void CaptureOverlay::syncSizeInput()
{
    if (m_mosaicPreview) {
        QImage preview(24, 24, QImage::Format_ARGB32);
        preview.fill(Qt::transparent);
        QPainter painter(&preview);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);                // Solid light disc: whole tiles stay bright on the dark bar.
        painter.setBrush(Bar::ink);
        painter.drawEllipse(QRectF(4, 4, 16, 16));
        painter.end();
        preview = mosaicBlur() ? mosaicBlurred(preview, mosaicStrength())
                               : mosaicTiles(preview, mosaicStrength(), 1).scaled(24, 24);
        m_mosaicPreview->setIcon(QIcon(QPixmap::fromImage(preview)));
        m_mosaicPreview->setToolTip(mosaicBlur()
            ? tr("模糊：点击切换为像素化")
            : tr("像素化：点击切换为模糊，滚轮调强度"));
    }
    if (m_mosaicStrengthLabel) {
        m_mosaicStrengthLabel->setText(QString::number(mosaicStrength()));
        m_mosaicStrengthLabel->setVisible(true);
    }
    if (m_mosaicSlider) {
        m_mosaicSlider->setVisible(true);
        const QSignalBlocker lock(m_mosaicSlider);
        m_mosaicSlider->setValue(mosaicStrength());
        m_mosaicSlider->setToolTip(tr("马赛克强度 %1：拖动调节，数值越大像素块越大").arg(mosaicStrength()));
    }
    if (m_sizeButton) {                          // Line-width glyph with the value beside it.
        // Glyph and value pill share one fixed 16+4+28 canvas, so the button never resizes as the value changes.
        const qreal dpr = devicePixelRatioF();
        QPixmap glyph(qCeil(48 * dpr), qCeil(24 * dpr));
        glyph.setDevicePixelRatio(dpr);
        glyph.fill(Qt::transparent);
        QPainter painter(&glyph);
        {   // Three thickening bars, snapped to whole device pixels so they stay sharp at 16px.
            painter.save();
            painter.scale(1 / dpr, 1 / dpr);
            const auto px = [dpr](qreal v) { return qRound(v * dpr); };
            const qreal bars[][2] = {{6, 2}, {10, 3}, {15, 4}};   // top, height in whole logical px
            for (const auto &bar : bars)
                painter.fillRect(QRect(px(1), px(bar[0]), px(15) - px(1), qMax(1, px(bar[1]))), Bar::ink);
            painter.restore();
        }
        painter.setRenderHint(QPainter::Antialiasing);   // Value sits in a rounded field 4px after the glyph.
        painter.setPen(Qt::NoPen);
        painter.setBrush(Bar::field);
        painter.drawRoundedRect(QRectF(20, 0, 28, 24), 6, 6);
        painter.setFont(m_sizeButton->font());
        painter.setPen(Bar::ink);
        // Number size may carry a half step shared with text; the button shows whole values only.
        const QString value = QString::number(qRound(widthControlValue()));
        painter.drawText(QRect(20, 0, 28, 24), Qt::AlignCenter, value);
        painter.end();
        m_sizeButton->setToolButtonStyle(Qt::ToolButtonIconOnly);   // The value is drawn in the glyph.
        m_sizeButton->setIconSize(QSize(48, 24));
        m_sizeButton->setIcon(QIcon(glyph));
        m_sizeButton->setText(value);
    }
    if (m_sizeSlider) {
        const QSignalBlocker lock(m_sizeSlider);
        m_sizeSlider->setValue(qRound(widthControlValue()));
    }
    if (m_sizeValue) m_sizeValue->setText(QStringLiteral("%1 %2").arg(widthControlValue()).arg(optionsTool() == ATool::Number ? QStringLiteral("pt") : QStringLiteral("px")));
    if (!m_sizeInput) return;
    const QSignalBlocker lock(m_sizeInput);
    m_sizeInput->setEditText(QString::number(toolSize()));
}

void CaptureOverlay::showSizePopup(QToolButton *anchor, int minimum, int maximum)
{
    if (m_sizePopup) { m_sizePopup->close(); return; }
    auto *popup = new QDialog(this, Qt::Popup | Qt::FramelessWindowHint);
    m_sizePopup = popup;
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setStyleSheet(QStringLiteral("QDialog{background:#18191d;border:1px solid #2b2d33;border-radius:8px;}"
                                        "QLabel{color:#f4f5f8;font-size:12px;}"));
    auto *row = new QHBoxLayout(popup);
    row->setContentsMargins(12, 8, 12, 8);
    row->setSpacing(10);
    auto *label = new QLabel(optionsTool() == ATool::Number ? tr("字号") : tr("笔宽"), popup);
    label->setObjectName(QStringLiteral("sizePopupLabel"));
    row->addWidget(label);
    m_sizeSlider = new QSlider(Qt::Horizontal, popup);
    m_sizeSlider->setObjectName(QStringLiteral("sizeSlider"));
    m_sizeSlider->setAccessibleName(anchor->accessibleName());
    m_sizeSlider->setRange(minimum, maximum);
    m_sizeSlider->setPageStep(1);
    m_sizeSlider->setFixedSize(128, 22);
    m_sizeSlider->setFocusPolicy(Qt::NoFocus);
    m_sizeSlider->setStyleSheet(darkSliderQss);
    m_sizeSlider->installEventFilter(this);
    m_sizeValue = new QLabel(popup);
    m_sizeValue->setFixedWidth(40);
    m_sizeValue->setObjectName(QStringLiteral("sizePopupValue"));
    m_sizeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(m_sizeSlider);
    row->addWidget(m_sizeValue);
    m_sizeDragUndo = false;
    syncSizeInput();
    connect(m_sizeSlider, &QSlider::valueChanged, this, [this](int value) { setWidthControl(value); });
    connect(m_sizeSlider, &QSlider::sliderPressed, this, [this] { m_sizeDragUndo = false; });
    connect(m_sizeSlider, &QSlider::sliderReleased, this, [this] { m_sizeDragUndo = false; });
    connect(popup, &QDialog::finished, this, [this] { if (isVisible()) setFocus(); });
    popup->show();
    popup->adjustSize();
    const QRect strip(m_toolOptions->mapToGlobal(QPoint()), m_toolOptions->size());
    const QScreen *screen = QGuiApplication::screenAt(strip.center());
    const QRect available = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    const int y = strip.bottom() + 1 + Bar::gap + popup->height() <= available.bottom() + 1
        ? strip.bottom() + 1 + Bar::gap : qMax(available.top(), strip.top() - Bar::gap - popup->height());
    popup->move(qBound(available.left(), strip.right() + 1 - popup->width(),
                      qMax(available.left(), available.right() + 1 - popup->width())), y);
}

void CaptureOverlay::commitSizeInput()
{
    if (!m_sizeInput) return;
    bool ok = false;
    const qreal value = m_sizeInput->currentText().toDouble(&ok);
    if (ok && m_sizeInput->lineEdit()->hasAcceptableInput()) setToolSize(value);
    syncSizeInput();
}

void CaptureOverlay::setToolSize(qreal value)                    // Numeric edits and wheel input share validation and undo.
{
    const ATool tool = optionsTool();
    if (tool == ATool::None || !qIsFinite(value)) return;
    const bool texty = tool == ATool::Text || tool == ATool::Number;
    const bool strength = tool == ATool::Mosaic && !mosaicBrush();
    value = texty ? qBound(qreal(1), value, qreal(1638))
                  : tool == ATool::Mosaic
                        ? qBound(strength ? 3 : 4, qRound(value), strength ? 24 : 50)
                        : qBound(1, qRound(value), 50);
    if (tool == ATool::Eraser) {                 // Only the brush width; rectangle erasures have none.
        m_eraserWidth = qRound(value);
        if (m_live) m_ann.pen = m_eraserWidth;
        syncSizeInput();
        updateAnnotationCursor(mapFromGlobal(QCursor::pos()));
        update();
        return;
    }
    if (tool == ATool::Mosaic) {
        if (!mosaicBrush()) { setMosaicStrength(qRound(value)); return; }
        if (value != toolSize()) {
            m_mosaicWidth = qRound(value);
            if (m_live) m_ann.pen = m_mosaicWidth;
            else if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
                pushUndo();
                m_anns[m_selAnn].pen = m_mosaicWidth;
            }
            update();
        }
        syncSizeInput();
        updateAnnotationCursor(mapFromGlobal(QCursor::pos()));
        return;
    }
    if (value == toolSize()) { syncSizeInput(); return; }
    if (editorActive()) {
        m_afont = m_editFont = value;
        applyEditorFont();
    } else if (m_live) {
        m_ann.pen = m_toolPens[int(tool)] = qRound(value);
    } else if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
        const bool dragging = m_sizeSlider && m_sizeSlider->isSliderDown();
        if (!dragging || !m_sizeDragUndo) pushUndo();   // One undo step per slider drag.
        m_sizeDragUndo = dragging;
        Annotation &ann = m_anns[m_selAnn];
        if (texty) ann.font = value; else ann.pen = qRound(value);
        if (ann.tool == ATool::Text) fitTextAnnotation(ann);
        if (ann.tool == ATool::Number) {          // Circles stay square and wrap the digits.
            // Exact center: QRect::center() rounds down on even sizes and would drift on every step.
            const QPoint c = ann.rect.topLeft() + QPoint(ann.rect.width() / 2, ann.rect.height() / 2);
            const int s = numberDiameter(ann);
            ann.rect = QRect(c - QPoint(s / 2, s / 2), QSize(s, s));
        }
    } else if (texty) m_afont = value;
    else m_toolPens[int(tool)] = qRound(value);
    syncSizeInput();
    update();
}

void CaptureOverlay::tuneWidth(int dir, const QPoint &at)
{
    const ATool tool = optionsTool();
    if (tool == ATool::None) return;
    const bool texty = tool == ATool::Text || tool == ATool::Number;
    setToolSize(toolSize() + dir * (texty ? 0.5 : 1));
    updateAnnotationCursor(at);                  // The bar's value is the only feedback; no floating text.
}

void CaptureOverlay::beginAnnotation(const QPoint &pos)
{
    m_live = true;
    m_selAnn = -1;                               // A canvas press releases the previous item, even if nothing is drawn.
    m_ann = Annotation();
    m_ann.tool = m_atool;
    m_ann.color = m_acolor;
    m_ann.pen = m_toolPens[int(m_atool)];
    if (m_atool == ATool::Mosaic) {
        m_ann.pen = m_mosaicWidth;
        m_ann.mosaicBrush = m_mosaicBrush;
        m_ann.mosaicBlur = m_mosaicBlur;
        m_ann.mosaicLevels[0] = m_mosaicLevels[0];
        m_ann.mosaicLevels[1] = m_mosaicLevels[1];
        m_ann.mosaicStrength = m_mosaicLevels[m_mosaicBlur];
        m_ann.mosaicSeed = QRandomGenerator::global()->generate();
    }
    if (m_atool == ATool::Eraser) {
        m_ann.pen = m_eraserWidth;
        if (m_eraserSmart) {
            // Smart erasure reuses the mosaic renderer, which already samples the layers below.
            m_ann.tool = ATool::Mosaic;
            m_ann.mosaicBrush = !m_eraserRect;
            m_ann.mosaicSmart = true;
            m_ann.mosaicBlur = false;
            m_ann.mosaicLevels[0] = m_mosaicLevels[0];
            m_ann.mosaicLevels[1] = m_mosaicLevels[1];
            m_ann.mosaicStrength = m_mosaicLevels[0];
            m_ann.mosaicSeed = QRandomGenerator::global()->generate();
        } else {
            m_ann.fill = m_eraserRect;
        }
    }
    if (m_atool == ATool::Rect || m_atool == ATool::Ellipse) {
        m_ann.fill = m_afill;
        m_ann.strokeStyle = m_shapeStyle;
    }
    if (m_atool == ATool::Marker) m_ann.fill = m_markerFill;
    if (m_atool == ATool::Marker && !m_ann.fill) m_ann.a = m_ann.b = pos;
    if (m_atool == ATool::Arrow) {
        m_ann.arrowStyle = m_arrowStyle;
        m_ann.strokeStyle = m_arrowLineStyle;
        m_ann.a = m_ann.b = pos;
    }
    else if (m_atool == ATool::Pen || (m_atool == ATool::Eraser && !m_ann.fill) || m_ann.mosaicBrush)
        m_ann.stroke << pos;
    else if (!(m_atool == ATool::Marker && !m_ann.fill)) m_ann.rect = QRect(pos, pos);
    syncSizeInput();                             // Keep the active options visible while drawing.
    m_inspector->hide();
    m_sizeBar->hide();
}

void CaptureOverlay::updateAnnotation(const QPoint &pos, Qt::KeyboardModifiers mods)
{
    if (m_ann.tool == ATool::Marker && !m_ann.fill) {
        QPointF end = pos;
        const QLineF line(m_ann.a, end);
        if (mods & Qt::ShiftModifier && line.length() > 1) {
            const qreal angle = qRound(qAtan2(-line.dy(), line.dx()) / (M_PI / 4)) * (M_PI / 4);
            end = QPointF(m_ann.a) + QPointF(qCos(angle), -qSin(angle)) * line.length();
        }
        m_ann.b = end.toPoint();
        return;
    }
    if (m_ann.tool == ATool::Arrow) {
        QPointF end = pos;
        const QLineF line(m_ann.a, end);
        if (mods & Qt::ShiftModifier && line.length() > 1) {   // Snap the arrow to 45-degree steps.
            const qreal angle = qRound(qAtan2(-line.dy(), line.dx()) / (M_PI / 4)) * (M_PI / 4);
            end = QPointF(m_ann.a) + QPointF(qCos(angle), -qSin(angle)) * line.length();
        }
        m_ann.b = end.toPoint();
        return;
    }
    if (m_ann.mosaicBrush) {
        if ((pos - m_ann.stroke.last().toPoint()).manhattanLength() >= 2) m_ann.stroke << pos;
        return;
    }
    const ATool shape = (m_ann.tool == ATool::Marker || m_ann.tool == ATool::Eraser) && m_ann.fill
        ? ATool::Rect : m_ann.tool;
    switch (shape) {
    case ATool::Pen:
    case ATool::Eraser:
        if ((pos - m_ann.stroke.last().toPoint()).manhattanLength() >= 2)
            m_ann.stroke << pos;                 // Decimate near-duplicate samples.
        break;
    default: {
        QPoint end = pos;
        if (mods & Qt::ShiftModifier) {          // Shift keeps squares and circles true.
            const int dx = pos.x() - m_pressPos.x(), dy = pos.y() - m_pressPos.y();
            const int s = qMax(qAbs(dx), qAbs(dy));
            end = QPoint(m_pressPos.x() + (dx < 0 ? -s : s),
                         m_pressPos.y() + (dy < 0 ? -s : s));
        }
        m_ann.rect = QRect(m_pressPos, end).normalized();
        break;
    }
    }
}

void CaptureOverlay::commitAnnotation()
{
    const bool meaningful = m_ann.mosaicBrush ? !m_ann.stroke.isEmpty()
        : m_ann.tool == ATool::Eraser && !m_ann.fill ? !m_ann.stroke.isEmpty()   // A click erases a dot.
        : m_ann.tool == ATool::Marker && !m_ann.fill ? (m_ann.a - m_ann.b).manhattanLength() >= 3
        : m_ann.tool == ATool::Pen
        ? m_ann.stroke.size() >= 2
        : m_ann.tool == ATool::Arrow
              ? (m_ann.a - m_ann.b).manhattanLength() >= 6
              : m_ann.rect.width() >= 3 && m_ann.rect.height() >= 3;
    if (meaningful) {                            // Discard accidental tiny strokes.
        pushUndo();
        m_anns.append(m_ann);
        normalizeNumbers();
        // Freehand strokes are finished on release; only geometric items retain controls.
        m_selAnn = m_ann.tool == ATool::Pen || m_ann.mosaicBrush
            || (m_ann.tool == ATool::Eraser && !m_ann.fill) ? -1 : m_anns.size() - 1;
    } else {
        updateToolOptions();                     // Options fall back to the tool's own defaults.
    }
    if (m_ann.color != m_acolor) {               // A pick made mid-draw does not outlive the drawing.
        m_acolor = m_ann.color;
        if (m_colorBoard) m_colorBoard->setCurrentColor(m_acolor);
    }
    endAnnotationDrag();
}

void CaptureOverlay::stopItemDrag()
{
    const bool moved = m_itemDrag != H_None && m_dragStarted;
    m_itemDrag = H_None;
    m_dragStarted = false;
    if (moved) endAnnotationDrag();
}

void CaptureOverlay::endAnnotationDrag()                         // Restore what stepped aside.
{
    m_sizeBar->hide();
    placeToolBars();
    showToolBars();
    syncSizeInput();
    update();
}

void CaptureOverlay::setToolNone()
{
    if (m_live) { m_live = false; commitAnnotation(); }   // A live arrow can still be open.
    stopItemDrag();
    commitTextEdit();
    m_atool = ATool::None;
    syncToolButtons();
    updateToolOptions();
    updateAnnotationCursor(mapFromGlobal(QCursor::pos()));
    updateInspector();
    update();
}

void CaptureOverlay::selectTool(ATool tool)                      // Shared by toolbar toggles and keyboard.
{
    if (m_moreButton && m_moreButton->isChecked()) m_moreButton->setChecked(false);
    if (m_live) { m_live = false; commitAnnotation(); }
    stopItemDrag();
    commitTextEdit();
    m_atool = tool;
    if (tool == ATool::Rect) m_shapeEllipse = false;        // R/O/popup pick the sub-shape.
    else if (tool == ATool::Ellipse) m_shapeEllipse = true;
    m_selAnn = -1;                               // Tools and item selection exclude each other.
    syncToolButtons();
    updateToolOptions();
    updateAnnotationCursor(mapFromGlobal(QCursor::pos()));
    updateInspector();
    update();
}

void CaptureOverlay::syncToolButtons()
{
    const QToolButton *active = m_atool == ATool::None ? nullptr : m_toolBtn[int(m_atool)];
    for (int t = 1; t < m_toolBtn.size(); ++t) {
        if (m_toolBtn[t] == m_shapeButton) continue;   // Rect and ellipse share one button.
        QSignalBlocker lock(m_toolBtn[t]);
        m_toolBtn[t]->setChecked(m_toolBtn[t] == active);
    }
    const bool shapeActive = m_atool == ATool::Rect || m_atool == ATool::Ellipse;
    const QSignalBlocker lock(m_shapeButton);
    m_shapeButton->setChecked(shapeActive);
    m_shapeButton->setIcon(barIcon(m_shapeEllipse ? Icon::Ellipse : Icon::Rect));
}

void CaptureOverlay::pickToolColor(const QColor &color, bool undoable)
{
    if (!color.isValid()) return;
    m_acolor = color;
    if (m_colorBoard) m_colorBoard->setCurrentColor(color);
    if (editorActive()) {                       // Stays inside the current text transaction.
        m_editColor = color;
        applyEditorFont();
    } else if (m_live) {
        m_ann.color = color;
        update();
    } else if (m_selAnn >= 0 && m_selAnn < m_anns.size() && m_anns[m_selAnn].tool != ATool::Mosaic
               && m_anns[m_selAnn].tool != ATool::Eraser && m_anns[m_selAnn].color != color) {
        if (undoable) pushUndo();
        m_anns[m_selAnn].color = color;
        update();
    }
}

void CaptureOverlay::showColorPalette()
{
    if (m_colorDialog) return;
    const QColor oldColor = m_acolor;
    const QVector<Annotation> before = m_anns;
    auto *dialog = new ColorDialog(oldColor, this);
    m_colorDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOption(QColorDialog::ShowAlphaChannel);
    dialog->setOption(QColorDialog::DontUseNativeDialog);
    dialog->setWindowTitle(tr("选择颜色"));
    connect(dialog, &QColorDialog::currentColorChanged, this, [this](const QColor &c) { pickToolColor(c, false); });
    connect(dialog, &ColorDialog::screenPickRequested, this, [this, dialog] {
        QPointer<ColorDialog> guard(dialog);
        dialog->hide();
        m_inspector->hide();
        ScreenColorPicker picker(m_frames, this);
        const bool accepted = picker.exec() == QDialog::Accepted && picker.pickedColor().isValid();
        if (guard) {
            if (accepted) guard->setCurrentColor(picker.pickedColor());
            guard->show();
            guard->raise();
            guard->activateWindow();
        }
    });
    connect(dialog, &QDialog::finished, this, [this, dialog, oldColor, before](int result) {
        m_colorDialog.clear();                   // Deletion is deferred; allow reopening right away.
        const QColor chosen = dialog->currentColor();
        m_anns = before;                         // Drop the live preview; a commit re-applies it as one undo step.
        if (result == QDialog::Accepted && chosen.isValid()) {
            pickToolColor(chosen);
        } else {
            m_acolor = oldColor;
            if (m_colorBoard) m_colorBoard->setCurrentColor(oldColor);
            if (editorActive()) { m_editColor = oldColor; applyEditorFont(); }
        }
        update();
        if (!isVisible()) return;
        if (editorActive()) { m_textView->setFocus(); m_editor->setFocus(); }
        else setFocus();
    });
    dialog->adjustSize();
    // Fixed offset from the current-color square: right of the strip, raised so the canvas stays visible.
    const QPoint anchor = m_colorBoard ? m_colorBoard->mapToGlobal(m_colorBoard->currentRect().topLeft())
                                       : m_toolOptions->mapToGlobal(QPoint());
    const QPoint target = anchor + QPoint(191, -224);
    const QScreen *screen = QGuiApplication::screenAt(anchor);
    const QRect available = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    dialog->move(qBound(available.left(), target.x(), available.right() + 1 - dialog->width()),
                 qBound(available.top(), target.y(), available.bottom() + 1 - dialog->height()));
    dialog->open();                              // Window-modal over the overlay, without a nested event loop.
}

void CaptureOverlay::pickShapeStyle(int style)
{
    m_shapeStyle = qBound(0, style, 4);
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()
        && (m_anns[m_selAnn].tool == ATool::Rect || m_anns[m_selAnn].tool == ATool::Ellipse)) {
        pushUndo();
        m_anns[m_selAnn].strokeStyle = m_shapeStyle;
        update();
    }
}

void CaptureOverlay::pickArrowStyle(int style)
{
    m_arrowStyle = qBound(0, style, 9);
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()
        && m_anns[m_selAnn].tool == ATool::Arrow) {
        pushUndo();
        m_anns[m_selAnn].arrowStyle = m_arrowStyle;
        update();
    }
}

void CaptureOverlay::pickArrowLineStyle(int style)
{
    m_arrowLineStyle = qBound(0, style, 4);
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()
        && m_anns[m_selAnn].tool == ATool::Arrow) {
        pushUndo();
        m_anns[m_selAnn].strokeStyle = m_arrowLineStyle;
        update();
    }
}

void CaptureOverlay::toggleFill()                                // Shapes: solid fill; marker: rectangle block.
{
    const ATool tool = optionsTool();
    const bool marker = tool == ATool::Marker;
    if (!marker && tool != ATool::Rect && tool != ATool::Ellipse) return;
    if (!m_live && m_selAnn >= 0 && m_selAnn < m_anns.size()
        && (m_anns[m_selAnn].tool == ATool::Rect || m_anns[m_selAnn].tool == ATool::Ellipse)) {
        pushUndo();
        m_anns[m_selAnn].fill = m_afill = !m_anns[m_selAnn].fill;
        update();
    } else if (marker) m_markerFill = !m_markerFill;   // A drawn highlight keeps its own form.
    else m_afill = !m_afill;
    updateToolOptions();
}

void CaptureOverlay::pickNumberOption(bool format, int value)
{
    (format ? m_numberFormat : m_numberStyle) = value;
    if (m_selAnn >= 0 && m_selAnn < m_anns.size() && m_anns[m_selAnn].tool == ATool::Number) {
        pushUndo();
        // Style is per marker; the format applies to the whole sequence so labels stay consistent.
        for (Annotation &ann : m_anns)
            if (ann.tool == ATool::Number && format) ann.numberFormat = value;
        if (!format) m_anns[m_selAnn].numberStyle = value;
        normalizeNumbers();
        update();
    }
}

void CaptureOverlay::setTextBold(bool on)
{
    m_abold = on;
    if (editorActive()) { m_editBold = on; applyEditorFont(); }
    else if (m_selAnn >= 0 && m_selAnn < m_anns.size()
             && m_anns[m_selAnn].tool == ATool::Text) {
        pushUndo();
        m_anns[m_selAnn].bold = on;
        fitTextAnnotation(m_anns[m_selAnn]);
        update();
    }
}

void CaptureOverlay::setTextItalic(bool on)
{
    m_aitalic = on;
    if (editorActive()) { m_editItalic = on; applyEditorFont(); }
    else if (m_selAnn >= 0 && m_selAnn < m_anns.size()
             && m_anns[m_selAnn].tool == ATool::Text) {
        pushUndo();
        m_anns[m_selAnn].italic = on;
        fitTextAnnotation(m_anns[m_selAnn]);
        update();
    }
}

void CaptureOverlay::setTextUnderline(bool on)
{
    m_aunderline = on;
    if (editorActive()) { m_editUnderline = on; applyEditorFont(); }
    else if (m_selAnn >= 0 && m_selAnn < m_anns.size()
             && m_anns[m_selAnn].tool == ATool::Text) {
        pushUndo();
        m_anns[m_selAnn].underline = on;
        fitTextAnnotation(m_anns[m_selAnn]);
        update();
    }
}

void CaptureOverlay::setTextFamily(const QFont &font)
{
    m_afamily = font.family();
    if (editorActive()) {
        m_editFamily = m_afamily;
        applyEditorFont();
    } else if (m_selAnn >= 0 && m_selAnn < m_anns.size() && m_anns[m_selAnn].tool == ATool::Text
               && m_anns[m_selAnn].family != m_afamily) {
        pushUndo();
        m_anns[m_selAnn].family = m_afamily;
        fitTextAnnotation(m_anns[m_selAnn]);
        update();
    }
}

void CaptureOverlay::syncTextOptions()
{
    if (!m_toolOptions) return;
    const bool editing = editorActive();
    const bool selected = !editing && m_selAnn >= 0 && m_selAnn < m_anns.size()
                          && m_anns[m_selAnn].tool == ATool::Text;
    const Annotation *ann = selected ? &m_anns[m_selAnn] : nullptr;
    const auto state = [&](const char *name, bool value) {
        if (auto *button = m_toolOptions->findChild<QToolButton *>(QString::fromLatin1(name))) {
            const QSignalBlocker lock(button);
            button->setChecked(value);
        }
    };
    state("textBold", editing ? m_editBold : ann ? ann->bold : m_abold);
    state("textItalic", editing ? m_editItalic : ann ? ann->italic : m_aitalic);
    state("textUnderline", editing ? m_editUnderline : ann ? ann->underline : m_aunderline);
    const QString family = editing ? m_editFamily : ann ? ann->family : m_afamily;
    if (auto *font = m_toolOptions->findChild<QComboBox *>(QStringLiteral("annotationFont"))) {
        const QSignalBlocker lock(font);
        font->setCurrentText(family);
        font->setToolTip(tr("字体：%1").arg(font->currentText()));
    }
    if (m_colorBoard) m_colorBoard->setCurrentColor(editing ? m_editColor : ann ? ann->color : m_acolor);
    syncSizeInput();
    // Reusing the strip still has to show it: updateToolOptions() hides it for the
    // None tool but leaves the Text children alive for this fast path.
    if (m_embedded) {
        m_toolOptions->show();
        m_toolOptions->adjustSize();
        emit stripRebuilt();
    } else {
        m_toolOptions->show();
        placeToolBars();
        m_toolOptions->raise();
    }
}

void CaptureOverlay::updateToolOptions()
{
    if (m_colorDialog) m_colorDialog->close();
    const QRect old = m_toolOptions->geometry();
    const ATool tool = optionsTool();
    if (tool == ATool::None) {
        m_toolOptions->hide();
        if (m_embedded) { emit stripRebuilt(); return; }
        update(old.adjusted(-4, -4, 4, 4));
        return;
    }
    m_sizeInput = nullptr;
    m_sizeButton = nullptr;
    if (m_sizePopup) m_sizePopup->close();
    m_mosaicSlider = nullptr;
    m_mosaicStrengthLabel = nullptr;
    m_mosaicPreview = nullptr;
    m_mosaicDragUndo = false;
    m_colorBoard = nullptr;
    delete m_toolOptions->layout();              // Drops the previous row.
    const QList<QWidget *> stale = m_toolOptions->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *w : stale) delete w;
    auto *lay = new QHBoxLayout(m_toolOptions);
    lay->setContentsMargins(12, 7, 12, 7);       // 44px option strip with compact controls.
    lay->setSpacing(4);
    lay->setAlignment(Qt::AlignLeft);
    auto addOption = [this](QHBoxLayout *l, Icon icon, const QString &tip, bool on,
                            const std::function<void()> &apply) {
        auto *b = barButton(icon, tip);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCheckable(true);
        b->setChecked(on);
        connect(b, &QToolButton::clicked, this, [this, apply, on] { apply(); Q_UNUSED(on); });
        l->addWidget(b);
    };
    auto addSeparator = [](QHBoxLayout *l) { l->addWidget(barSeparator(true)); };
    auto addSwatches = [this](QHBoxLayout *l, ATool) {
        m_colorBoard = new ColorBoard(m_acolor);
        connect(m_colorBoard, &ColorBoard::presetPicked, this, [this](const QColor &color) { pickToolColor(color); });
        connect(m_colorBoard, &ColorBoard::customRequested, this, &CaptureOverlay::showColorPalette);
        l->addWidget(m_colorBoard);
    };
    const bool texty = tool == ATool::Text || tool == ATool::Number;
    auto addSize = [this, lay, texty, tool] {
        if (tool != ATool::Text) {               // Width button on the left; the slider pops up below it.
            // shortcut: number slider spans 6-96 pt; finer or larger sizes still come from the canvas wheel.
            const int minimum = tool == ATool::Number ? 6 : tool == ATool::Mosaic ? 4 : 1,
                      maximum = tool == ATool::Number ? 96 : 50;
            m_sizeButton = new QToolButton;
            m_sizeButton->setObjectName(QStringLiteral("sizeButton"));
            m_sizeButton->setAccessibleName(tool == ATool::Number ? tr("大小") : tr("粗细"));
            m_sizeButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            m_sizeButton->setIconSize(QSize(48, 24));
            m_sizeButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);   // Width hugs glyph + value.
            m_sizeButton->setFixedHeight(Bar::button);
            m_sizeButton->setFocusPolicy(Qt::NoFocus);
            m_sizeButton->setCursor(Qt::PointingHandCursor);
            m_sizeButton->installEventFilter(this);
            QToolButton *button = m_sizeButton;
            connect(button, &QToolButton::clicked, this,
                    [this, button, minimum, maximum] { showSizePopup(button, minimum, maximum); });
            lay->insertWidget(0, button);        // Always the first control of the strip.
            syncSizeInput();
            return;
        }
        const int minimum = 1, maximum = 1638;
        m_sizeInput = new FlatComboBox;
        m_sizeInput->setEditable(true);
        m_sizeInput->setInsertPolicy(QComboBox::NoInsert);
        m_sizeInput->setCompleter(nullptr);
        auto *validator = new QDoubleValidator(minimum, maximum, texty ? 1 : 0, m_sizeInput);
        validator->setNotation(QDoubleValidator::StandardNotation);
        validator->setLocale(QLocale::c());
        m_sizeInput->setValidator(validator);
        if (texty) {
            for (qreal size : {5.5, 6.5, 7.5, 8., 9., 10., 10.5, 11., 12., 14., 15., 16.,
                               18., 20., 22., 24., 26., 28., 36., 42., 48., 54., 72.})
                m_sizeInput->addItem(QString::number(size));
        } else {
            for (int size = minimum; size <= maximum; ++size) m_sizeInput->addItem(QString::number(size));
        }
        m_sizeInput->setToolTip(texty ? tr("标准字号 (pt)：小四=12，五号=10.5")
                                    : tr("输入、下拉选择或滚轮调节粗细"));
        m_sizeInput->setFocusPolicy(Qt::ClickFocus);
        m_sizeInput->setFixedSize(texty ? 62 : 46, 24);
        m_sizeInput->setStyleSheet(QStringLiteral("QComboBox{padding:1px 2px 1px 4px;border-radius:6px;}QComboBox::drop-down{width:14px;}"));   // Room for 1637.5.
        m_sizeInput->lineEdit()->setAlignment(Qt::AlignCenter);
        m_sizeInput->setAccessibleName(texty ? tr("字号") : tr("粗细"));
        m_sizeInput->installEventFilter(this);
        m_sizeInput->lineEdit()->installEventFilter(this);
        syncSizeInput();
        connect(m_sizeInput->lineEdit(), &QLineEdit::editingFinished, this, &CaptureOverlay::commitSizeInput);
        connect(m_sizeInput, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
            commitSizeInput();
            if (editorActive()) { m_textView->setFocus(); m_editor->setFocus(); }
            else setFocus();
        });
        lay->addWidget(m_sizeInput);
    };
    switch (tool) {
    case ATool::Rect:
    case ATool::Ellipse: {
        // A selected rectangle or ellipse switches shape in place and stays selected.
        const auto pickShape = [this](ATool shape) {
            if (m_live || m_selAnn < 0 || m_selAnn >= m_anns.size()
                || (m_anns[m_selAnn].tool != ATool::Rect && m_anns[m_selAnn].tool != ATool::Ellipse)) {
                selectTool(shape);
                return;
            }
            if (m_anns[m_selAnn].tool != shape) { pushUndo(); m_anns[m_selAnn].tool = shape; }
            m_atool = shape;
            m_shapeEllipse = shape == ATool::Ellipse;
            syncToolButtons();
            updateToolOptions();
            update();
        };
        addOption(lay, Icon::Rect, tr("矩形"), !m_shapeEllipse,
                  [pickShape] { pickShape(ATool::Rect); });
        addOption(lay, Icon::Ellipse, tr("椭圆"), m_shapeEllipse,
                  [pickShape] { pickShape(ATool::Ellipse); });
        addOption(lay, Icon::Fill, tr("填充 (F)"),
                  !m_live && m_selAnn >= 0 && m_selAnn < m_anns.size() ? m_anns[m_selAnn].fill : m_afill,
                  [this] { toggleFill(); });
        lay->itemAt(lay->count() - 1)->widget()->setObjectName(QStringLiteral("fillButton"));
        const bool selectedShape = !m_live && m_selAnn >= 0 && m_selAnn < m_anns.size()
                                   && (m_anns[m_selAnn].tool == ATool::Rect
                                       || m_anns[m_selAnn].tool == ATool::Ellipse);
        const int currentStyle = qBound(0, selectedShape ? m_anns[m_selAnn].strokeStyle : m_shapeStyle, 4);
        if (selectedShape) m_shapeStyle = currentStyle;
        auto styleIcon = [this](int style) {
            const qreal dpr = devicePixelRatioF();
            QPixmap pm(qCeil(40 * dpr), qCeil(20 * dpr));
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter painter(&pm);
            painter.setRenderHint(QPainter::Antialiasing);
            QPen sample(Bar::ink, 1.5);            // 40x20 sample spanning 5%..95% with flat ends.
            sample.setStyle(static_cast<Qt::PenStyle>(Qt::SolidLine + style));
            sample.setCapStyle(Qt::FlatCap);
            sample.setJoinStyle(Qt::BevelJoin);
            painter.setPen(sample);
            painter.drawLine(QPointF(2, 10), QPointF(38, 10));
            return QIcon(pm);
        };
        auto *styleCombo = new FlatComboBox(true);
        styleCombo->setObjectName(QStringLiteral("shapeStyleCombo"));
        const char *const styleNames[] = {
            QT_TRANSLATE_NOOP("CaptureOverlay", "实线"),
            QT_TRANSLATE_NOOP("CaptureOverlay", "虚线"),
            QT_TRANSLATE_NOOP("CaptureOverlay", "点线"),
            QT_TRANSLATE_NOOP("CaptureOverlay", "点划线"),
            QT_TRANSLATE_NOOP("CaptureOverlay", "双点划线")};
        for (int k = 0; k < 5; ++k) {
            const QString name = QCoreApplication::translate("CaptureOverlay", styleNames[k]);
            styleCombo->addItem(styleIcon(k), name);
        }
        styleCombo->setCurrentIndex(currentStyle);
        styleCombo->setFocusPolicy(Qt::NoFocus);
        styleCombo->setFixedSize(64, 24);
        styleCombo->setIconSize(QSize(40, 20));
        styleCombo->setAccessibleName(styleCombo->itemText(currentStyle));
        connect(styleCombo, QOverload<int>::of(&QComboBox::activated), this, [this, styleCombo](int k) {
            pickShapeStyle(k);
            styleCombo->setAccessibleName(styleCombo->itemText(k));
            setFocus();
        });
        lay->addWidget(styleCombo);
        addSize();
        addSeparator(lay);
        addSwatches(lay, ATool::Rect);
        break;
    }
    case ATool::Marker:
        addOption(lay, Icon::MarkerRect, tr("矩形高亮"), m_markerFill, [this] { toggleFill(); });
        if (auto *b = qobject_cast<QToolButton *>(lay->itemAt(lay->count() - 1)->widget())) {
            b->setObjectName(QStringLiteral("fillButton"));
            b->setIcon(barIcon(Icon::MarkerRect));
        }
        addSize();
        addSeparator(lay);
        addSwatches(lay, ATool::Marker);
        break;
    case ATool::Arrow: {
        auto addCombo = [this](QHBoxLayout *l, int current, int count,
                               const std::function<QIcon(int)> &iconAt,
                               const std::function<QString(int)> &textAt,
                               const std::function<void(int)> &pick) {
            auto *combo = new FlatComboBox(true);
            for (int k = 0; k < count; ++k)
                combo->addItem(iconAt(k), textAt(k));
            combo->setCurrentIndex(current);
            combo->setFocusPolicy(Qt::NoFocus);
            combo->setFixedSize(58, 24);
            combo->setIconSize(QSize(36, 16));
            combo->setAccessibleName(textAt(current));
            connect(combo, QOverload<int>::of(&QComboBox::activated), this, [this, combo, pick](int k) {
                pick(k);
                combo->setAccessibleName(combo->itemText(k));
                setFocus();
            });
            l->addWidget(combo);
        };
        const bool editing = !m_live && m_selAnn >= 0 && m_selAnn < m_anns.size()
                             && m_anns[m_selAnn].tool == ATool::Arrow;
        const int currentShape = editing ? qBound(0, m_anns[m_selAnn].arrowStyle, 9) : m_arrowStyle;
        const int currentLine = editing ? qBound(0, m_anns[m_selAnn].strokeStyle, 4) : m_arrowLineStyle;
        if (editing) {
            m_arrowStyle = currentShape;
            m_arrowLineStyle = currentLine;
        }
        addCombo(lay, currentShape, 10,
                 [this](int k) { return arrowStyleIcon(k, devicePixelRatioF()); },
                 [](int k) {
                     const char *const names[] = {
                         QT_TRANSLATE_NOOP("CaptureOverlay", "直线箭头"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "双向直线箭头"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "线条箭头"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "双向线条箭头"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "空心箭头"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "填充箭头"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "尺寸线"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "实心箭头"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "双向实心箭头"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "尺寸箭头")};
                     return QCoreApplication::translate("CaptureOverlay", names[k]);
                 },
                 [this](int k) { pickArrowStyle(k); });
        auto *shapeCombo = qobject_cast<QComboBox *>(lay->itemAt(lay->count() - 1)->widget());
        shapeCombo->setObjectName(QStringLiteral("arrowStyleCombo"));
        shapeCombo->setFixedSize(72, 24);
        shapeCombo->setIconSize(QSize(40, 20));
        auto lineIcon = [this](int style) {
            const qreal dpr = devicePixelRatioF();
            QPixmap pm(qCeil(40 * dpr), qCeil(20 * dpr));
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter painter(&pm);
            painter.setRenderHint(QPainter::Antialiasing);
            QPen sample(Bar::ink, 1.5);
            sample.setStyle(static_cast<Qt::PenStyle>(Qt::SolidLine + style));
            sample.setCapStyle(Qt::FlatCap);
            sample.setJoinStyle(Qt::BevelJoin);
            painter.setPen(sample);
            painter.drawLine(QPointF(2, 10), QPointF(38, 10));
            return QIcon(pm);
        };
        addCombo(lay, currentLine, 5,
                 [&](int k) { return lineIcon(k); },
                 [](int k) {
                     const char *const names[] = {
                         QT_TRANSLATE_NOOP("CaptureOverlay", "实线"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "虚线"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "点线"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "点划线"),
                         QT_TRANSLATE_NOOP("CaptureOverlay", "双点划线")};
                     return QCoreApplication::translate("CaptureOverlay", names[k]);
                 },
                 [this](int k) { pickArrowLineStyle(k); });
        auto *lineCombo = qobject_cast<QComboBox *>(lay->itemAt(lay->count() - 1)->widget());
        lineCombo->setObjectName(QStringLiteral("arrowLineStyleCombo"));
        lineCombo->setFixedSize(72, 24);
        lineCombo->setIconSize(QSize(40, 20));
        addSize();
        addSeparator(lay);
        addSwatches(lay, ATool::Arrow);
        break;
    }
    case ATool::Text: {
        const bool selected = !editorActive() && m_selAnn >= 0 && m_selAnn < m_anns.size()
                              && m_anns[m_selAnn].tool == ATool::Text;
        const Annotation *ann = selected ? &m_anns[m_selAnn] : nullptr;
        auto *bold = barButton(Icon::Bold, tr("粗体"));
        auto *italic = barButton(Icon::Italic, tr("斜体"));
        auto *underline = barButton(Icon::Underline, tr("下划线"));
        for (auto *b : {bold, italic, underline}) {
            b->setFocusPolicy(Qt::NoFocus);
            b->setCheckable(true);
            b->setCursor(Qt::PointingHandCursor);
        }
        bold->setObjectName(QStringLiteral("textBold"));
        italic->setObjectName(QStringLiteral("textItalic"));
        underline->setObjectName(QStringLiteral("textUnderline"));
        bold->setChecked(editorActive() ? m_editBold : ann ? ann->bold : m_abold);
        italic->setChecked(editorActive() ? m_editItalic : ann ? ann->italic : m_aitalic);
        underline->setChecked(editorActive() ? m_editUnderline : ann ? ann->underline : m_aunderline);
        bold->setToolTip(tr("粗体"));
        italic->setToolTip(tr("斜体"));
        underline->setToolTip(tr("下划线"));
        connect(bold, &QToolButton::toggled, this, &CaptureOverlay::setTextBold);
        connect(italic, &QToolButton::toggled, this, &CaptureOverlay::setTextItalic);
        connect(underline, &QToolButton::toggled, this, &CaptureOverlay::setTextUnderline);
        lay->addWidget(bold);
        lay->addWidget(italic);
        lay->addWidget(underline);
        auto *font = new FlatComboBox;               // Closed state names the family, as a standard combo.
        font->setObjectName(QStringLiteral("annotationFont"));
        font->setEditable(false);
        font->addItems(QFontDatabase().families());
        font->setFocusPolicy(Qt::NoFocus);
        font->setFixedSize(140, 24);
        font->view()->setMinimumWidth(200);
        font->setAccessibleName(tr("字体"));
        font->setCurrentText(editorActive() ? m_editFamily
            : m_selAnn >= 0 && m_selAnn < m_anns.size() ? m_anns[m_selAnn].family : m_afamily);
        font->setToolTip(tr("字体：%1").arg(font->currentText()));
        connect(font, &QComboBox::currentTextChanged, this, [this, font](const QString &family) {
            font->setToolTip(tr("字体：%1").arg(family));
            setTextFamily(QFont(family));
        });
        lay->addWidget(font);
        addSize();
        addSeparator(lay);
        addSwatches(lay, ATool::Text);
        break;
    }
    case ATool::Mosaic: {
        const bool brush = mosaicBrush();
        addOption(lay, Icon::Rect, tr("矩形马赛克"), !brush,
                  [this] { selectMosaicMode(false); });
        lay->itemAt(lay->count() - 1)->widget()->setObjectName(QStringLiteral("mosaicRectangle"));
        addOption(lay, Icon::Pen, tr("马赛克笔：滚轮调节笔宽"), brush,
                  [this] { selectMosaicMode(true); });
        lay->itemAt(lay->count() - 1)->widget()->setObjectName(QStringLiteral("mosaicBrush"));
        m_mosaicPreview = barButton(Icon::Mosaic, tr("像素化 / 模糊"));
        m_mosaicPreview->setObjectName(QStringLiteral("mosaicEffectPreview"));
        m_mosaicPreview->setAccessibleName(tr("切换像素化 / 模糊"));
        m_mosaicPreview->setFocusPolicy(Qt::NoFocus);
        m_mosaicPreview->installEventFilter(this);
        connect(m_mosaicPreview, &QToolButton::clicked, this, &CaptureOverlay::switchMosaicEffect);
        lay->addWidget(m_mosaicPreview);
        m_mosaicSlider = new QSlider(Qt::Horizontal);
        m_mosaicSlider->setObjectName(QStringLiteral("mosaicStrength"));
        m_mosaicSlider->setAccessibleName(tr("马赛克强度"));
        m_mosaicSlider->setRange(3, 24);
        m_mosaicSlider->setPageStep(1);
        m_mosaicSlider->setFixedSize(128, 22);
        m_mosaicSlider->setFocusPolicy(Qt::NoFocus);
        m_mosaicSlider->setStyleSheet(darkSliderQss);
        m_mosaicSlider->installEventFilter(this);
        connect(m_mosaicSlider, &QSlider::valueChanged, this, &CaptureOverlay::setMosaicStrength);
        connect(m_mosaicSlider, &QSlider::sliderPressed, this, [this] { m_mosaicDragUndo = false; });
        connect(m_mosaicSlider, &QSlider::sliderReleased, this, [this] { m_mosaicDragUndo = false; });
        lay->addWidget(m_mosaicSlider);
        m_mosaicStrengthLabel = new QLabel;
        m_mosaicStrengthLabel->setObjectName(QStringLiteral("mosaicStrengthValue"));
        m_mosaicStrengthLabel->setAccessibleName(tr("马赛克强度"));
        m_mosaicStrengthLabel->setToolTip(tr("马赛克强度：拖动左侧滑条或滚轮调节"));
        m_mosaicStrengthLabel->setAlignment(Qt::AlignCenter);
        m_mosaicStrengthLabel->setFixedWidth(24);
        m_mosaicStrengthLabel->installEventFilter(this);
        lay->addWidget(m_mosaicStrengthLabel);
        syncSizeInput();                          // Hides both when the smart mode is active.
        addSize();                               // Brush width stays visible in both modes.
        break;
    }
    case ATool::Eraser: {                        // Brush and rectangle shapes, plus the smart fill mode.
        const bool rect = eraserRect();
        const bool smart = eraserSmart();
        addOption(lay, Icon::Pen, tr("画笔橡皮擦"), !rect && !smart, [this] { selectEraserMode(false); });
        lay->itemAt(lay->count() - 1)->widget()->setObjectName(QStringLiteral("eraserBrush"));
        addOption(lay, Icon::Rect, tr("矩形橡皮擦"), rect && !smart, [this] { selectEraserMode(true); });
        lay->itemAt(lay->count() - 1)->widget()->setObjectName(QStringLiteral("eraserRectangle"));
        addOption(lay, Icon::MagicEraser,
                  tr("智能擦除：自动填补背景，使用当前画笔或矩形范围"), smart,
                  [this] { toggleEraserSmart(); });
        lay->itemAt(lay->count() - 1)->widget()->setObjectName(QStringLiteral("eraserSmart"));
        addSize();
        lay->removeWidget(m_sizeButton);         // Brush, rectangle, smart, then the width button.
        lay->addWidget(m_sizeButton);
        break;
    }
    case ATool::Number: {
        const bool editing = !m_live && m_selAnn >= 0 && m_selAnn < m_anns.size()
                             && m_anns[m_selAnn].tool == ATool::Number;
        // Style glyphs come from the number renderer itself: filled disc, ring, bare digit.
        auto styleGlyph = [this](int style) {
            const qreal dpr = devicePixelRatioF();
            QPixmap pm(qCeil(16 * dpr), qCeil(16 * dpr));
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            Annotation sample;
            sample.tool = ATool::Number;
            sample.color = Bar::ink;
            sample.rect = QRect(1, 1, 14, 14);
            sample.font = style == 2 ? 10 : 7;   // A bare digit has no disc, so it fills the cell itself.
            sample.text = QStringLiteral("1");
            sample.numberStyle = style;
            QPainter painter(&pm);
            painter.setRenderHint(QPainter::Antialiasing);
            drawAnnotation(painter, sample, QImage());
            return QIcon(pm);
        };
        auto addList = [this, lay, styleGlyph](const QString &name, const QStringList &items, int current, bool format) {
            auto *combo = new FlatComboBox(!format);   // Style shows glyphs only; format keeps its sample text.
            combo->setObjectName(format ? QStringLiteral("numberFormat") : QStringLiteral("numberStyle"));
            for (int k = 0; k < items.size(); ++k) {
                if (format) combo->addItem(items[k]);
                else combo->addItem(styleGlyph(k), items[k]);
            }
            combo->setCurrentIndex(current);
            combo->setAccessibleName(name);
            combo->setFocusPolicy(Qt::NoFocus);
            combo->setIconSize(QSize(16, 16));
            combo->setFixedSize(format ? 76 : 40, 24);   // Pill padding needs room for the sample text.
            connect(combo, QOverload<int>::of(&QComboBox::activated), this, [this, format](int k) {
                pickNumberOption(format, k);
                setFocus();
            });
            lay->addWidget(combo);
        };
        addSize();
        addList(tr("序号样式"), {tr("实心"), tr("空心"), tr("仅序号")},
                editing ? m_anns[m_selAnn].numberStyle : m_numberStyle, false);
        addList(tr("序号格式"), {QStringLiteral("1 2 3"), QStringLiteral("I II III"), QStringLiteral("A B C"),
                                          QStringLiteral("a b c"), QStringLiteral("一 二 三")},
                editing ? m_anns[m_selAnn].numberFormat : m_numberFormat, true);
        addSeparator(lay);
        addSwatches(lay, ATool::Number);
        break;
    }
    default:
        addSize();
        addSeparator(lay);
        addSwatches(lay, tool);                 // Pen, Marker, Number.
        break;
    }
    for (int i = 0; i < lay->count(); ++i) lay->itemAt(i)->widget()->show();
    m_toolOptions->show();
    if (m_embedded) {                             // Detached strip: the pin repositions it.
        m_toolOptions->adjustSize();
        m_toolOptions->raise();
        emit stripRebuilt();
        return;
    }
    placeToolBars();
    m_toolOptions->raise();
    update(old.united(m_toolOptions->geometry()).adjusted(-4, -4, 4, 4));
}

void CaptureOverlay::paintAnnotations(QPainter &p, const QPointF &origin, qreal scale, const QRectF &clip,
                      bool includeEditor) const
{
    if (m_anns.isEmpty() && !m_live && !editorActive()) return;
    p.save();
    p.translate(origin);
    p.scale(scale, scale);
    p.setClipRect(clip, Qt::IntersectClip);
    // Erasures cut through annotations only, so they all paint into a transparent layer first.
    bool erasing = m_live && m_ann.tool == ATool::Eraser;
    for (const Annotation &ann : m_anns) erasing = erasing || ann.tool == ATool::Eraser;
    const qreal layerDpr = p.device()->devicePixelRatioF();
    // shortcut: an eraser allocates a layer for the repaint clip; if large captures lag, reuse one buffer.
    const QRect device = erasing ? p.transform().mapRect(clip).toAlignedRect() : QRect();
    QImage layerImage;
    QPainter layerPainter;
    if (erasing && !device.isEmpty()) {
        layerImage = QImage((QSizeF(device.size()) * layerDpr).toSize(), QImage::Format_ARGB32_Premultiplied);
        layerImage.setDevicePixelRatio(layerDpr);
        layerImage.fill(Qt::transparent);
        layerPainter.begin(&layerImage);
        layerPainter.setTransform(p.transform() * QTransform::fromTranslate(-device.x(), -device.y()));
        layerPainter.setClipRect(clip);
    }
    QPainter &out = layerPainter.isActive() ? layerPainter : p;
    int remainingMosaics = m_live && m_ann.tool == ATool::Mosaic ? 1 : 0;
    for (const Annotation &ann : m_anns) if (ann.tool == ATool::Mosaic) ++remainingMosaics;
    QImage lower;
    if (remainingMosaics) {
        if (m_mosaicFrame.isNull()) {
            m_mosaicFrame = m_shot.toImage().convertToFormat(QImage::Format_ARGB32);
            m_mosaicFrame.setDevicePixelRatio(1);
        }
        lower = m_mosaicFrame;
    }
    const auto paint = [&](const Annotation &ann, int index, bool visible = true) {
        if (ann.tool == ATool::Mosaic) --remainingMosaics;
        if (visible) drawAnnotation(out, ann, lower, index);
        if (remainingMosaics > 0) {
            const QImage source = ann.tool == ATool::Mosaic ? lower : QImage();
            QPainter layer(&lower);
            const qreal dpr = m_shot.devicePixelRatioF();
            layer.scale(dpr, dpr);
            layer.setClipRect(clip);
            if (ann.tool == ATool::Eraser) {     // Later mosaics see the original pixels through erasures.
                layer.setClipPath(annTransform(ann).map(eraserMask(ann)), Qt::IntersectClip);
                layer.drawImage(QRectF(0, 0, m_mosaicFrame.width() / dpr, m_mosaicFrame.height() / dpr),
                                m_mosaicFrame);
            } else drawAnnotation(layer, ann, source, index);
        }
    };
    for (int i = 0; i < m_anns.size(); ++i)
        if (!editorActive() || i != m_editAnn) paint(m_anns[i], i);
        else paint(editorAnnotation(), m_editAnn, includeEditor);
    if (editorActive() && m_editAnn < 0) paint(editorAnnotation(), -1, includeEditor);
    if (m_live) paint(m_ann, -1);
    if (layerPainter.isActive()) {
        layerPainter.end();
        p.save();
        p.resetTransform();
        p.drawImage(device.topLeft(), layerImage);
        p.restore();
    }
    p.restore();
}

QPainterPath CaptureOverlay::eraserMask(const Annotation &ann)
{
    if (!ann.fill) return strokeMask(ann.stroke, ann.pen);
    QPainterPath mask;
    mask.addRect(QRectF(ann.rect));
    return mask;
}

QImage CaptureOverlay::mosaicTiles(const QImage &source, int block, quint32 seed)
{
    if (source.isNull() || block <= 0) return {};
    QImage tiles(qMax(1, qCeil(qreal(source.width()) / block)),
                 qMax(1, qCeil(qreal(source.height()) / block)), QImage::Format_ARGB32);
    if (tiles.isNull()) return {};
    const qreal sx = qreal(source.width()) / tiles.width();
    const qreal sy = qreal(source.height()) / tiles.height();
    const int radius = qMax(1, qFloor(block * 0.375 - 0.5));
    QRandomGenerator random(seed);
    for (int y = 0; y < tiles.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(tiles.scanLine(y));
        for (int x = 0; x < tiles.width(); ++x) {
            const int cx = qFloor((x + random.generateDouble()) * sx);
            const int cy = qFloor((y + random.generateDouble()) * sy);
            const int left = qMax(0, cx - radius), right = qMin(source.width() - 1, cx + radius);
            const int top = qMax(0, cy - radius), bottom = qMin(source.height() - 1, cy + radius);
            quint64 red = 0, green = 0, blue = 0, alpha = 0;
            for (int py = top; py <= bottom; ++py) {
                const QRgb *pixels = reinterpret_cast<const QRgb *>(source.constScanLine(py));
                for (int px = left; px <= right; ++px) {
                    const QRgb color = pixels[px];
                    red += qRed(color); green += qGreen(color); blue += qBlue(color); alpha += qAlpha(color);
                }
            }
            const int count = (right - left + 1) * (bottom - top + 1);
            row[x] = qRgba(red / count, green / count, blue / count, alpha / count);
        }
    }
    return tiles;
}

QImage CaptureOverlay::mosaicBlurred(const QImage &source, int radius)
{
    if (source.isNull() || radius < 0 || radius > 24) return {};
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    if (radius == 0 || image.isNull()) return image;
    const int divisor = (radius + 1) * (radius + 1);
    int shift = 9;
    for (int value = divisor; value > 1; value >>= 1) ++shift;
    const int multiplier = ((1 << shift) + divisor - 1) / divisor;
    const int diameter = 2 * radius + 1;
    using Channels = std::array<int, 4>;
    QVector<Channels> stack(diameter);
    for (bool vertical : {false, true}) {
        const int length = vertical ? image.height() : image.width();
        const int lines = vertical ? image.width() : image.height();
        for (int line = 0; line < lines; ++line) {
            const auto pixel = [&](int offset) -> QRgb & {
                const int x = vertical ? line : offset, y = vertical ? offset : line;
                return reinterpret_cast<QRgb *>(image.scanLine(y))[x];
            };
            const auto channels = [](QRgb value) -> Channels {
                return {{qRed(value), qGreen(value), qBlue(value), qAlpha(value)}};
            };
            Channels sum{}, incoming{}, outgoing{};
            for (int i = -radius; i <= radius; ++i) {
                const Channels color = channels(pixel(qBound(0, i, length - 1)));
                stack[i + radius] = color;
                for (int c = 0; c < 4; ++c) {
                    sum[c] += color[c] * (radius + 1 - qAbs(i));
                    (i <= 0 ? outgoing : incoming)[c] += color[c];
                }
            }
            int cursor = radius;
            for (int x = 0; x < length; ++x) {
                pixel(x) = qRgba((sum[0] * multiplier) >> shift, (sum[1] * multiplier) >> shift,
                                 (sum[2] * multiplier) >> shift, (sum[3] * multiplier) >> shift);
                Channels &start = stack[(cursor - radius + diameter) % diameter];
                const Channels next = channels(pixel(qMin(x + radius + 1, length - 1)));
                for (int c = 0; c < 4; ++c) {
                    sum[c] -= outgoing[c];
                    outgoing[c] -= start[c];
                    start[c] = next[c];
                    incoming[c] += next[c];
                    sum[c] += incoming[c];
                }
                cursor = (cursor + 1) % diameter;
                for (int c = 0; c < 4; ++c) {
                    outgoing[c] += stack[cursor][c];
                    incoming[c] -= stack[cursor][c];
                }
            }
        }
    }
    return image;
}

void CaptureOverlay::drawAnnotation(QPainter &p, const Annotation &ann, const QImage &source, int index) const
{
    QPen pen(ann.color, ann.pen);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setBrush(Qt::NoBrush);
    p.setRenderHint(QPainter::Antialiasing);      // Strokes and heads render smooth on screen and export.
    p.save();
    if (!qFuzzyIsNull(ann.rot) && ann.tool != ATool::Mosaic) { // Effects rotate their mask, not their source.
        const QTransform t = annTransform(ann);
        p.setTransform(t, true);
    }
    switch (ann.tool) {
    case ATool::Rect:
    case ATool::Ellipse: {
        if (ann.fill) p.setBrush(ann.color);     // Fill mode: solid shape in the stroke color.
        QPen outline = pen;
        // A filled shape keeps a continuous outline; dashes would leave the fill edge toothed.
        outline.setStyle(ann.fill ? Qt::SolidLine
                                  : static_cast<Qt::PenStyle>(Qt::SolidLine + qBound(0, ann.strokeStyle, 4)));
        const bool thick = ann.pen >= 4;         // Thin outlines keep square ends and mitred corners.
        outline.setCapStyle(thick ? Qt::RoundCap : Qt::SquareCap);
        outline.setJoinStyle(thick ? Qt::RoundJoin : Qt::MiterJoin);
        p.setPen(outline);
        if (ann.tool == ATool::Rect) p.drawRect(ann.rect);
        else p.drawEllipse(ann.rect);
        break;
    }
    case ATool::Pen:
        p.setPen(pen);
        p.drawPolyline(ann.stroke);
        break;
    case ATool::Marker: {                        // Translucent fat stroke, marker style.
        QColor wash = ann.color;
        wash.setAlpha(110);
        if (ann.fill) { p.fillRect(ann.rect, wash); break; }   // Rectangle highlight block.
        QPen washPen(wash, ann.pen * 5);
        washPen.setCapStyle(Qt::RoundCap);
        washPen.setJoinStyle(Qt::RoundJoin);
        p.setPen(washPen);
        p.drawLine(ann.a, ann.b);
        break;
    }
    case ATool::Text: {
        if (ann.text.isEmpty()) break;
        QTextDocument doc;
        setupTextDocument(doc, ann);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, ann.color);
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);   // Keep the underline a crisp 1px line.
        p.translate(ann.rect.topLeft());
        doc.documentLayout()->draw(&p, context);
        p.restore();
        break;
    }
    case ATool::Number: {                        // Filled circle with a sequence digit.
        if (ann.text.isEmpty()) break;
        QFont font = textFont(ann);
        font.setBold(true);
        const qreal room = ann.rect.width() * 0.8;    // Long labels (VIII, 十一) shrink to fit the disc.
        const qreal advance = QFontMetricsF(font).horizontalAdvance(ann.text);
        if (ann.numberStyle != 2 && advance > room && advance > 0)
            font.setPointSizeF(qMax(1.0, font.pointSizeF() * room / advance));
        if (ann.numberStyle == 0) {                   // Filled disc, contrasting digits.
            p.setPen(Qt::NoPen);
            p.setBrush(ann.color);
            p.drawEllipse(ann.rect);
            p.setPen(ann.color.lightness() > 165 ? Qt::black : Qt::white);
        } else {
            if (ann.numberStyle == 1) {               // Ring at a weight that follows the size.
                const qreal ring = qMax(1.5, ann.rect.width() / 14.0);
                p.setPen(QPen(ann.color, ring));
                p.drawEllipse(QRectF(ann.rect).adjusted(ring / 2, ring / 2, -ring / 2, -ring / 2));
            }
            p.setPen(ann.color);
        }
        p.setFont(font);
        p.drawText(ann.rect, Qt::AlignCenter | Qt::TextDontClip, ann.text);
        break;
    }
    case ATool::Arrow: {
        const qreal w = qMax<qreal>(1.0, ann.pen);
        const int style = qBound(0, ann.strokeStyle, 4);
        const int shape = qBound(0, ann.arrowStyle, 9);
        const QPointF start(ann.a), tip(ann.b);
        const qreal length = QLineF(start, tip).length();
        if (length < 0.5) break;
        const QPointF dir = (tip - start) / length;
        const QPointF normal(-dir.y(), dir.x());
        const bool both = shape == 1 || shape == 3 || shape == 8 || shape == 9;
        const bool barbed = shape == 0 || shape == 1 || shape == 5 || shape == 9;
        const bool flat = shape == 7 || shape == 8;
        const bool openLine = shape == 2 || shape == 3;
        const qreal lenLimit = length * (both ? 0.42 : 0.72);
        qreal headLen = 3.4 * w;
        qreal headHalf = 1.4 * w;
        qreal slotLen = 0.0;
        qreal slotHalf = 0.0;
        const bool slotted = shape == 0 || shape == 1;
        if (barbed) {
            headLen = (shape == 5 ? 2.4 : shape == 9 ? 2.5 : 3.0) * w;
            headHalf = (shape == 5 || shape == 9 ? 1.35 : 1.6) * w;
            slotLen = headLen * (shape == 5 || shape == 9 ? 0.90 : 0.775);
            slotHalf = (shape == 5 ? 0.55 : shape == 9 ? 0.60 : 0.65) * w;
        } else if (flat) {
            headLen = 3.2 * w;
            headHalf = 1.65 * w;
        } else if (openLine) {
            headLen = 2.2 * w;
            headHalf = 1.7 * w;
        } else if (shape == 4) {
            headLen = 2.0 * w;
            headHalf = 1.25 * w;
        }
        headLen = qMin(headLen, lenLimit);
        headHalf = qMin(headHalf, headLen * 0.80);
        slotLen = qMin(slotLen, headLen * 0.92);
        slotHalf = qMin(slotHalf, headHalf * 0.75);
        const qreal base = qMax<qreal>(0.0, length - headLen);
        const qreal barHalf = qMin(2.5 * w, length * 0.45);
        const qreal barWidth = qMax<qreal>(1.0, 1.15 * w);

        QPen shaft(ann.color, w);
        shaft.setCapStyle(both ? Qt::FlatCap : Qt::RoundCap);
        shaft.setJoinStyle(Qt::MiterJoin);
        switch (style) {
        case 1: shaft.setDashPattern({4, 2}); break;
        case 2: shaft.setDashPattern({1, 2}); break;
        case 3: shaft.setDashPattern({4, 2, 1, 2}); break;
        case 4: shaft.setDashPattern({4, 2, 1, 2, 1, 2}); break;
        default: break;
        }
        auto solidPen = [&] {
            QPen pen = shaft;
            pen.setStyle(Qt::SolidLine);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            return pen;
        };
        QPen flatShaft = shaft;
        flatShaft.setCapStyle(Qt::FlatCap);
        auto headNormal = [](const QPointF &d) { return QPointF(-d.y(), d.x()); };
        auto paintBarbedHead = [&](const QPointF &end, const QPointF &d) {
            const QPointF n = headNormal(d);
            QPolygonF head;
            head << end << end - d * headLen + n * headHalf;
            if (slotted)
                head << end - d * slotLen + n * slotHalf
                     << end - d * slotLen - n * slotHalf;
            else
                head << end - d * slotLen;
            head << end - d * headLen - n * headHalf;
            p.setPen(Qt::NoPen);
            p.setBrush(ann.color);
            p.drawPolygon(head);
        };
        auto paintFlatHead = [&](const QPointF &end, const QPointF &d) {
            const QPointF n = headNormal(d);
            QPolygonF head;
            head << end << end - d * headLen + n * headHalf
                 << end - d * headLen - n * headHalf;
            p.setPen(Qt::NoPen);
            p.setBrush(ann.color);
            p.drawPolygon(head);
        };
        auto paintLineHead = [&](const QPointF &end, const QPointF &d) {
            const QPointF n = headNormal(d);
            QPen arm = solidPen();
            arm.setCapStyle(Qt::RoundCap);
            arm.setJoinStyle(Qt::RoundJoin);
            p.setPen(arm);
            p.setBrush(Qt::NoBrush);
            p.drawLine(end, end - d * headLen + n * headHalf);
            p.drawLine(end, end - d * headLen - n * headHalf);
        };
        auto paintBar = [&](const QPointF &end) {
            QPen bar(ann.color, barWidth);
            bar.setCapStyle(Qt::FlatCap);
            p.setPen(bar);
            p.setBrush(Qt::NoBrush);
            p.drawLine(end - normal * barHalf, end + normal * barHalf);
        };

        if (shape == 4) {                            // Open outline: two tapered rails and a barbed V.
            const qreal startHalf = w * 0.25;
            const qreal baseHalf = w * 0.75;
            const qreal barbBack = headLen * 0.25;
            const QPointF tailUpper = start + normal * startHalf;
            const QPointF baseUpper = start + dir * base + normal * baseHalf;
            const QPointF barbUpper = start + dir * (base - barbBack) + normal * headHalf;
            const QPointF barbLower = start + dir * (base - barbBack) - normal * headHalf;
            const QPointF baseLower = start + dir * base - normal * baseHalf;
            const QPointF tailLower = start - normal * startHalf;
            QPainterPath outline(tailUpper);
            outline.lineTo(baseUpper);
            outline.lineTo(barbUpper);
            outline.lineTo(tip);
            outline.lineTo(barbLower);
            outline.lineTo(baseLower);
            outline.lineTo(tailLower);
            QPen edge = shaft;
            edge.setWidthF(qMax<qreal>(1.0, w * 0.30));
            edge.setCapStyle(Qt::RoundCap);
            edge.setJoinStyle(Qt::RoundJoin);
            p.setPen(edge);
            p.setBrush(Qt::NoBrush);
            p.drawPath(outline);
            break;
        }
        if (shape == 5) {                            // Solid wedge tapering into a barbed head.
            const qreal tailHalf = w * 0.06;
            const qreal bodyHalf = w * 0.70;
            const qreal bodyEnd = qMax<qreal>(0.0, length - slotLen);
            QPolygonF body;
            body << start + normal * tailHalf
                 << start + dir * bodyEnd + normal * bodyHalf
                 << start + dir * bodyEnd - normal * bodyHalf
                 << start - normal * tailHalf;
            p.setPen(Qt::NoPen);
            p.setBrush(ann.color);
            p.drawPolygon(body);
            paintBarbedHead(tip, dir);
            break;
        }

        p.setBrush(Qt::NoBrush);
        qreal startInset = 0.0;
        qreal tipInset = 0.0;
        if (barbed) {
            const qreal shaftEnd = qMax<qreal>(0.0, slotLen - (slotted ? w * 0.5 : 0.0));
            if (both) startInset = shaftEnd;
            tipInset = shaftEnd;
        } else if (flat) {
            if (both) startInset = headLen;
            tipInset = headLen;
        }
        if (barbed && shape == 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(ann.color);
            p.drawEllipse(start, w / 2.0, w / 2.0);
            p.setBrush(Qt::NoBrush);
        }
        p.setPen(barbed ? flatShaft : shaft);
        p.drawLine(start + dir * startInset, tip - dir * tipInset);

        switch (shape) {
        case 0: paintBarbedHead(tip, dir); break;
        case 1: paintBarbedHead(tip, dir); paintBarbedHead(start, -dir); break;
        case 2: paintLineHead(tip, dir); break;
        case 3: paintLineHead(tip, dir); paintLineHead(start, -dir); break;
        case 6: paintBar(start); paintBar(tip); break;
        case 7: paintFlatHead(tip, dir); break;
        case 8: paintFlatHead(tip, dir); paintFlatHead(start, -dir); break;
        case 9:
            paintBarbedHead(tip, dir);
            paintBarbedHead(start, -dir);
            paintBar(start);
            paintBar(tip);
            break;
        default: break;
        }
        break;
    }
    case ATool::Mosaic: {                        // Redact lower layers through the object's mask.
        const qreal dpr = m_shot.devicePixelRatioF();
        const QRect area = annotationRect(ann).intersected(
            QRect(0, 0, qFloor(m_shot.width() / dpr), qFloor(m_shot.height() / dpr)));
        if (area.isEmpty() || (ann.mosaicBrush && ann.stroke.isEmpty())) break;
        QPainterPath mask;
        if (ann.mosaicBrush) mask = strokeMask(ann.stroke, ann.pen);
        else mask.addRect(QRectF(ann.rect));
        p.setClipPath(mask, Qt::IntersectClip);
        if (ann.mosaicSmart) {
            if (ann.smartFailed) {                  // A failed erase must not expose the source pixels.
                p.fillRect(area, Qt::black);
                break;
            }
            const bool stale = ann.smartResult.isNull() || ann.mosaicSource != source
                || ann.smartRect != ann.rect || ann.smartStroke != ann.stroke || ann.smartPen != ann.pen;
            if (ann.smartPending || stale) {
                if (!ann.smartPending && index >= 0)
                    const_cast<CaptureOverlay *>(this)->scheduleSmartErase(index, source);
                // Marker-style wash shows the erase extent while drawing and until the fill is ready.
                QColor wash = ann.color;
                wash.setAlpha(110);
                p.fillRect(area, wash);
                break;
            }
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(QRectF(0, 0, m_shot.width() / dpr, m_shot.height() / dpr), ann.smartResult);
            break;
        }
        if (ann.mosaicCacheStrength != ann.mosaicStrength || ann.mosaicCache.isNull()
            || ann.mosaicSource != source) {
            ann.mosaicSource = source;
            ann.mosaicCache = ann.mosaicBlur ? mosaicBlurred(source, ann.mosaicStrength)
                : mosaicTiles(source, qMax(3, qRound(ann.mosaicStrength * dpr)), ann.mosaicSeed);
            ann.mosaicCacheStrength = ann.mosaicStrength;
        }
        const QRectF desktop(0, 0, m_shot.width() / dpr, m_shot.height() / dpr);
        p.setClipRect(desktop, Qt::IntersectClip);
        if (ann.mosaicCache.isNull()) p.fillRect(desktop, Qt::black); // Failed redaction must not expose source pixels.
        else {
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(desktop, ann.mosaicCache);
        }
        break;
    }
    case ATool::Eraser:                          // Clears lower annotations; the capture shows through.
        p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        p.fillPath(eraserMask(ann), Qt::black);
        break;
    default:
        break;
    }
    p.restore();
}
