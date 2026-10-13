#include "capture_overlay.h"
#include <QClipboard>
#include <QAbstractItemView>
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
#include <QFontDatabase>
#include <QSlider>
#include <QVBoxLayout>
#include <QDialog>
#include <QRegularExpression>
#include <QDoubleValidator>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QCursor>
#include <QFontInfo>
#include <QPolygonF>
#include <qt_windows.h>
#include <imm.h>
#include <array>
#include <functional>

QRect CaptureOverlay::annotationRect(const Annotation &ann)
{
    if (ann.tool == ATool::Mosaic && ann.mosaicBrush) {
        const int w = qCeil(ann.pen / 2.0) + 2;
        return ann.stroke.boundingRect().toAlignedRect().adjusted(-w, -w, w, w);
    }
    switch (ann.tool) {
    case ATool::Arrow:  return QRect(ann.a, ann.b).normalized();
    case ATool::Pen:    return ann.stroke.boundingRect().toRect();
    case ATool::Marker: {                          // Fat translucent stroke: include its width.
        const int w = ann.pen * 5 / 2 + 2;
        return ann.stroke.boundingRect().toRect().adjusted(-w, -w, w, w);
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

bool CaptureOverlay::annotationHit(const Annotation &ann, const QPoint &pos, int tolBoost)
{
    const int tol = qMax(5, ann.pen / 2 + 4);    // Areas keep the base reach; the boost
    const int lineTol = tol + tolBoost;          // only widens outline / stroke hits.
    if (ann.tool == ATool::Mosaic && ann.mosaicBrush) {
        if (ann.stroke.isEmpty()) return false;
        for (int i = 1; i < ann.stroke.size(); ++i)
            if (distToSegment(pos, ann.stroke[i - 1], ann.stroke[i]) <= ann.pen / 2.0 + lineTol)
                return true;
        return QLineF(pos, ann.stroke.first()).length() <= ann.pen / 2.0 + lineTol;
    }
    switch (ann.tool) {
    case ATool::Rect:                            // Outlines select on their border only.
        return ann.rect.adjusted(-lineTol, -lineTol, lineTol, lineTol).contains(pos)
            && !ann.rect.adjusted(tol, tol, -tol, -tol).contains(pos);
    case ATool::Ellipse: {
        const QPointF d = pos - ann.rect.center();
        const qreal rx = ann.rect.width() / 2.0, ry = ann.rect.height() / 2.0;
        if (rx < 1 || ry < 1) return ann.rect.contains(pos);
        const qreal outer = qPow(d.x() / (rx + lineTol), 2) + qPow(d.y() / (ry + lineTol), 2);
        const qreal inner = qPow(d.x() / qMax(1.0, rx - tol), 2)
                          + qPow(d.y() / qMax(1.0, ry - tol), 2);
        return outer <= 1.0 && inner >= 1.0;
    }
    case ATool::Arrow:
        return distToSegment(QPointF(pos), ann.a, ann.b) <= lineTol + ann.pen / 2;
    case ATool::Pen:
    case ATool::Marker:
        if (ann.stroke.isEmpty()) return false;
        for (int i = 1; i < ann.stroke.size(); ++i)
            if (distToSegment(QPointF(pos), ann.stroke[i - 1], ann.stroke[i])
                <= lineTol + ann.pen / 2)
                return true;
        return distToSegment(QPointF(pos), ann.stroke.first(), ann.stroke.first())
            <= lineTol + ann.pen / 2;
    default:                                     // Area objects select anywhere inside.
        return ann.rect.adjusted(-tol, -tol, tol, tol).contains(pos);
    }
}

int CaptureOverlay::annotationAt(const QPoint &pos) const        // Topmost annotation first.
{
    for (int i = m_anns.size() - 1; i >= 0; --i)
        if (!m_anns[i].mosaicBrush && annotationHit(m_anns[i], mapToLocal(m_anns[i], pos))) return i;
    return -1;
}

QVector<QPoint> CaptureOverlay::annAnchors(const Annotation &ann) const
{
    if (ann.tool == ATool::Mosaic && ann.mosaicBrush) return {};
    if (ann.tool == ATool::Arrow) return {ann.a, ann.b};
    switch (ann.tool) {
    case ATool::Rect: case ATool::Ellipse: case ATool::Mosaic:
    case ATool::Text: case ATool::Number: {
        const QRect r = ann.rect.normalized();
        return {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()};
    }
    default:
        return {};
    }
}

CaptureOverlay::Handle CaptureOverlay::annHandleAt(const Annotation &ann, const QPoint &pos) const
{
    if (ann.mosaicBrush) return H_None;
    if (ann.tool != ATool::Mosaic
        && QRect(rotateAnchor(ann) - QPoint(6, 6), QSize(12, 12)).contains(pos)) return H_Rotate;
    const QVector<QPoint> anchors = annAnchors(ann);
    static const Handle ids[] = {H_TL, H_TR, H_BL, H_BR, H_TL, H_BR};
    for (int i = 0; i < anchors.size(); ++i)
        if (QRect(mapFromLocal(ann, anchors[i]) - QPoint(5, 5), QSize(10, 10)).contains(pos))
            return ids[i];
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

QPoint CaptureOverlay::rotateAnchor(const Annotation &ann)  // Rotate stalk sits below the item.
{
    const QRect r = annotationRect(ann);
    return mapFromLocal(ann, QPoint(r.center().x(), r.bottom() + 22));
}

void CaptureOverlay::paintMosaicBorder(QPainter &p, const Annotation &ann, bool selected) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setClipRect(QRectF(m_sel), Qt::IntersectClip);
    p.setCompositionMode(QPainter::CompositionMode_Difference);
    QPen border(Qt::white, 0);
    if (selected) border.setDashPattern({4, 4});
    p.setPen(border);
    p.setBrush(Qt::NoBrush);
    const QRectF bounds = QRectF(ann.rect).adjusted(0, 0, -1, -1);
    p.drawRect(bounds);
    if (selected) {
        p.setPen(QPen(Qt::white, 0));
        for (const QPoint &anchor : annAnchors(ann))
            p.drawRect(QRect(anchor - QPoint(3, 3), QSize(6, 6)));
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
    if ((m_tools->isVisible() && m_tools->geometry().contains(pos))
        || (m_toolOptions->isVisible() && m_toolOptions->geometry().contains(pos))) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    if (!m_live && m_selAnn >= 0 && m_selAnn < m_anns.size()
        && m_anns[m_selAnn].tool == ATool::Mosaic && !m_anns[m_selAnn].mosaicBrush) {
        const Handle handle = annHandleAt(m_anns[m_selAnn], pos);
        if (handle != H_None) { setCursor(cursorForHandle(handle)); return; }
        if (annotationHit(m_anns[m_selAnn], pos)) { setCursor(Qt::SizeAllCursor); return; }
    }
    const Handle handle = hitHandle(pos);
    if (m_atool == ATool::Mosaic && mosaicBrush() && handle == H_Move && !m_spaceHeld) {
        const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(pos));
        setCursor(mosaicBrushCursor(toolSize(), screen ? screen->devicePixelRatio() : devicePixelRatioF()));
    } else setCursor(m_atool != ATool::None && handle == H_Move ? Qt::CrossCursor : cursorForHandle(handle));
}

void CaptureOverlay::translateAnnotation(Annotation &ann, const QPoint &d)
{
    ann.rect.translate(d);
    ann.a += d;
    ann.b += d;
    ann.stroke.translate(d);
}

void CaptureOverlay::applyItemDrag(const QPoint &pos, Qt::KeyboardModifiers mods)
{
    if (m_selAnn < 0 || m_selAnn >= m_anns.size()) { stopItemDrag(); return; }
    Annotation ann = m_pressAnn;                 // Always computed from the press snapshot.
    if (m_itemDrag == H_Move) translateAnnotation(ann, pos - m_pressPos);
    else if (m_itemDrag == H_Rotate) {           // Drag the stalk to rotate around the center.
        const QPointF c = QPointF(annotationRect(ann).center());
        qreal delta = QLineF(c, QPointF(m_pressPos)).angle()
                    - QLineF(c, QPointF(pos)).angle();
        qreal rot = ann.rot + delta;
        if (mods & Qt::ShiftModifier) rot = qRound(rot / 15.0) * 15.0;   // Snap to 15 degrees.
        ann.rot = rot;
    } else {
        const QPoint local = mapToLocal(ann, pos);   // Handle math runs in item space.
        if (ann.tool == ATool::Arrow) {
            if (m_itemDrag == H_TL) ann.a = local; else ann.b = local;
        } else {
            const QRect r = m_pressAnn.rect.normalized();
            if (ann.tool == ATool::Mosaic && (m_itemDrag == H_L || m_itemDrag == H_R
                || m_itemDrag == H_T || m_itemDrag == H_B)) {
                ann.rect = r;
                if (m_itemDrag == H_L) ann.rect.setLeft(local.x());
                else if (m_itemDrag == H_R) ann.rect.setRight(local.x());
                else if (m_itemDrag == H_T) ann.rect.setTop(local.y());
                else ann.rect.setBottom(local.y());
                ann.rect = ann.rect.normalized();
                m_anns[m_selAnn] = ann;
                return;
            }
            const QPoint fixed = m_itemDrag == H_TL ? r.bottomRight()
                : m_itemDrag == H_TR ? r.bottomLeft()
                : m_itemDrag == H_BL ? r.topRight() : r.topLeft();
            ann.rect = QRect(fixed, local).normalized();
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
        ann.mosaicCache = {};
        ann.mosaicSource = {};
        ann.mosaicCacheStrength = 0;
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
}

void CaptureOverlay::normalizeNumbers()                          // Sequence numbers follow list order.
{
    int n = 0;
    for (Annotation &ann : m_anns)
        if (ann.tool == ATool::Number) ann.text = QString::number(++n);
}

void CaptureOverlay::eraseAt(const QPoint &pos)
{
    const int tolBoost = m_apen * 2;              // Eraser thickness widens the hit zone.
    for (int i = m_anns.size() - 1; i >= 0; --i)
        if (annotationHit(m_anns[i], mapToLocal(m_anns[i], pos), tolBoost)) {
            if (!m_eraseSnapshotted) {           // One snapshot covers the whole drag.
                pushUndo();
                m_eraseSnapshotted = true;
            }
            const QRect dirty = rotatedBounds(m_anns[i]).adjusted(-24, -24, 24, 24);
            m_anns.removeAt(i);
            if (m_selAnn == i) m_selAnn = -1;
            else if (m_selAnn > i) --m_selAnn;
            normalizeNumbers();
            update(dirty);
        }
}

void CaptureOverlay::deleteSelected()
{
    stopItemDrag();
    if (m_selAnn < 0 || m_selAnn >= m_anns.size()) { m_selAnn = -1; return; }
    pushUndo();
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
    return font;
}

int CaptureOverlay::numberDiameter(const Annotation &ann)
{
    return qMax(1, QFontInfo(textFont(ann)).pixelSize()) * 2;
}

void CaptureOverlay::setupTextDocument(QTextDocument &doc, const Annotation &ann)
{
    doc.setDefaultFont(textFont(ann));
    doc.setDocumentMargin(6);
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
    const QPolygon mask = m_textView->mapFromScene(
        m_editor->mapToScene(m_editor->boundingRect()));
    m_textView->setMask(QRegion(mask).intersected(QRegion(m_sel)));
    update();
}

bool CaptureOverlay::eventFilter(QObject *obj, QEvent *ev)
{
    if ((obj == m_tools || obj == m_toolOptions)
        && (ev->type() == QEvent::Enter || ev->type() == QEvent::Leave))
        updateInspector();
    if (obj == m_mosaicWidthLabel && ev->type() == QEvent::Wheel) {
        const int delta = static_cast<QWheelEvent *>(ev)->angleDelta().y();
        if (delta) tuneWidth(delta > 0 ? 1 : -1, mapFromGlobal(QCursor::pos()));
        ev->accept();
        return true;
    }
    if ((obj == m_mosaicSlider || obj == m_mosaicStrengthLabel || obj == m_mosaicPreview)
        && ev->type() == QEvent::Wheel) {
        const int delta = static_cast<QWheelEvent *>(ev)->angleDelta().y();
        if (delta) setMosaicStrength(mosaicStrength() + (delta > 0 ? 1 : -1));
        ev->accept();
        return true;
    }
    if (m_colorDialog && (obj == m_rgbInput || obj == m_hexInput) && ev->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(ev)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            const QColor color = parseAnnotationColor(static_cast<QLineEdit *>(obj)->text());
            if (color.isValid()) pickToolColor(m_colorDialogTool, color);
            return true;                         // Enter applies a color; it never pins the capture.
        }
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
                && !m_editor->boundingRect().adjusted(6, 6, -6, -6).contains(local)) {
                m_textMoving = true;
                m_textMovePress = me->globalPos();
                m_textMoveOrigin = m_editor->pos();
                m_textView->viewport()->grabMouse();
                return true;
            }
        } else if (m_textMoving && ev->type() == QEvent::MouseMove) {
            auto *me = static_cast<QMouseEvent *>(ev);
            const QPoint delta = me->globalPos() - m_textMovePress;
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
    m_atool = ATool::Text;
    m_selAnn = existing;
    m_acolor = m_colorText = m_editColor;
    m_afont = m_editFont;
    m_afamily = m_editFamily;
    m_abold = m_editBold;
    m_aitalic = m_editItalic;
    syncToolButtons();
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
        m_editor = new QGraphicsTextItem;
        scene->addItem(m_editor);
        m_editor->setTextInteractionFlags(Qt::TextEditorInteraction);
        m_editor->setTextWidth(-1);
        m_editor->document()->setDocumentMargin(6);
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
    m_editor->setPos(existing >= 0 ? m_anns[existing].rect.topLeft() : pos - QPoint(6, 6));
    if (existing >= 0) m_editor->setRotation(m_anns[existing].rot);
    QTextCursor cur = m_editor->textCursor();
    const int hit = m_editor->document()->documentLayout()->hitTest(
        m_editor->mapFromScene(QPointF(pos)), Qt::FuzzyHit);
    cur.setPosition(qMax(0, hit));
    m_editor->setTextCursor(cur);
    ImmAssociateContext(reinterpret_cast<HWND>(winId()), m_imeCtx);
    m_textView->show();
    m_textView->raise();
    m_tools->raise();
    m_toolOptions->raise();
    m_sizeTip->hide();
    m_inspector->hide();
    m_textView->setFocus();
    m_editor->setFocus(Qt::MouseFocusReason);
    syncTextEditorGeometry();
    update();
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
            || old.rot != edited.rot;
    }
    if (changed) {
        pushUndo();
        if (text.trimmed().isEmpty()) {         // Clearing the text deletes the annotation.
            m_anns.removeAt(m_editAnn);
            if (m_selAnn == m_editAnn) m_selAnn = -1;
            else if (m_selAnn > m_editAnn) --m_selAnn;
        } else if (replace) {
            m_anns[m_editAnn] = edited;
            m_selAnn = m_editAnn;
        } else {
            m_anns.append(edited);
            m_selAnn = m_anns.size() - 1;        // Auto-select fresh annotations.
        }
    }
    m_editAnn = -1;
    m_committingText = false;
    setFocus();
    syncSizeInput();
    update();
}

void CaptureOverlay::applyEditorFont()                          // Size, weight, and slant in one place.
{
    if (!m_editor) return;
    Annotation ann;
    ann.font = m_editFont;
    ann.family = m_editFamily;
    ann.bold = m_editBold;
    ann.italic = m_editItalic;
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
    m_sizeTip->adjustSize();
    m_sizeTip->move(qBound(0, at.x() - m_sizeTip->width() / 2, width() - m_sizeTip->width()),
                    qMax(0, at.y() - m_sizeTip->height() - 16));
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
    if (m_live) return m_ann.tool;
    if (m_atool != ATool::Eraser && m_selAnn >= 0 && m_selAnn < m_anns.size())
        return m_anns[m_selAnn].tool;
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

void CaptureOverlay::switchMosaicEffect()
{
    if (optionsTool() != ATool::Mosaic) return;
    const bool blur = !mosaicBlur();
    m_mosaicBlur = blur;
    Annotation *ann = m_live ? &m_ann : nullptr;
    if (!ann && m_selAnn >= 0 && m_selAnn < m_anns.size()) {
        pushUndo();
        ann = &m_anns[m_selAnn];
    }
    if (ann) {
        ann->mosaicBlur = blur;
        ann->mosaicStrength = ann->mosaicLevels[blur];
        ann->mosaicCacheStrength = 0;
    }
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
    const bool texty = tool == ATool::Text || tool == ATool::Number;
    if (m_live) return texty ? m_ann.font : m_ann.pen;
    if (m_atool != ATool::Eraser && m_selAnn >= 0 && m_selAnn < m_anns.size())
        return texty ? m_anns[m_selAnn].font : m_anns[m_selAnn].pen;
    return texty ? m_afont : m_apen;
}

void CaptureOverlay::syncSizeInput()
{
    if (m_mosaicWidthLabel) m_mosaicWidthLabel->setText(QString::number(toolSize()));
    if (m_mosaicPreview) {
        QImage preview(24, 24, QImage::Format_ARGB32);
        preview.fill(Qt::transparent);
        QPainter painter(&preview);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::white, 2.5));
        painter.drawEllipse(QRectF(7, 7, 10, 10));
        painter.end();
        preview = mosaicBlur() ? mosaicBlurred(preview, mosaicStrength())
                               : mosaicTiles(preview, mosaicStrength(), 1).scaled(24, 24);
        m_mosaicPreview->setIcon(QIcon(QPixmap::fromImage(preview)));
        m_mosaicPreview->setToolTip(mosaicBlur() ? QStringLiteral("模糊：点击切换为像素化，滚轮调强度")
                                              : QStringLiteral("像素化：点击切换为模糊，滚轮调强度"));
    }
    if (m_mosaicStrengthLabel)
        m_mosaicStrengthLabel->setText(QString::number(mosaicStrength()));
    if (m_mosaicSlider) {
        const QSignalBlocker lock(m_mosaicSlider);
        m_mosaicSlider->setValue(mosaicStrength());
        m_mosaicSlider->setToolTip(QStringLiteral("马赛克强度 %1：拖动调节，数值越大像素块越大").arg(mosaicStrength()));
    }
    if (!m_sizeInput) return;
    const QSignalBlocker lock(m_sizeInput);
    m_sizeInput->setEditText(QString::number(toolSize()));
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
                  : tool == ATool::Mosaic ? qBound(strength ? 3 : 4, qRound(value), strength ? 24 : 50)
                                         : qBound(1, qRound(value), 12);
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
        m_ann.pen = m_apen = qRound(value);
    } else if (m_atool != ATool::Eraser && m_selAnn >= 0 && m_selAnn < m_anns.size()) {
        pushUndo();
        Annotation &ann = m_anns[m_selAnn];
        if (texty) ann.font = value; else ann.pen = qRound(value);
        if (ann.tool == ATool::Text) fitTextAnnotation(ann);
        if (ann.tool == ATool::Number) {          // Circles stay square and wrap the digits.
            const int s = numberDiameter(ann);
            ann.rect = QRect(ann.rect.center() - QPoint(s / 2, s / 2), QSize(s, s));
        }
    } else if (texty) m_afont = value;
    else m_apen = qRound(value);
    syncSizeInput();
    update();
}

void CaptureOverlay::tuneWidth(int dir, const QPoint &at)
{
    const ATool tool = optionsTool();
    if (tool == ATool::None) return;
    const bool texty = tool == ATool::Text || tool == ATool::Number;
    setToolSize(toolSize() + dir * (texty ? 0.5 : 1));
    updateAnnotationCursor(at);
    m_sizeTip->setText(texty ? QStringLiteral("字号 %1 pt").arg(toolSize())
                           : tool == ATool::Mosaic && !mosaicBrush() ? QStringLiteral("强度 %1").arg(toolSize())
                           : QStringLiteral("粗细 %1").arg(toolSize()));
    tipPosition(at);
    m_sizeTip->show();
}

void CaptureOverlay::beginAnnotation(const QPoint &pos)
{
    m_live = true;
    m_ann = Annotation();
    m_ann.tool = m_atool;
    m_ann.color = m_acolor;
    m_ann.pen = m_apen;
    if (m_atool == ATool::Mosaic) {
        m_ann.pen = m_mosaicWidth;
        m_ann.mosaicBrush = m_mosaicBrush;
        m_ann.mosaicBlur = m_mosaicBlur;
        m_ann.mosaicLevels[0] = m_mosaicLevels[0];
        m_ann.mosaicLevels[1] = m_mosaicLevels[1];
        m_ann.mosaicStrength = m_mosaicLevels[m_mosaicBlur];
        m_ann.mosaicSeed = QRandomGenerator::global()->generate();
    }
    if (m_atool == ATool::Rect || m_atool == ATool::Ellipse) m_ann.fill = m_afill;
    if (m_atool == ATool::Arrow) m_ann.arrowStyle = m_arrowStyle;
    if (m_atool == ATool::Pen || m_atool == ATool::Marker || m_ann.mosaicBrush) m_ann.stroke << pos;
    else if (m_atool == ATool::Arrow) m_ann.a = m_ann.b = pos;
    else m_ann.rect = QRect(pos, pos);
    syncSizeInput();                             // Keep the active options visible while drawing.
    m_inspector->hide();
    m_sizeTip->hide();
}

void CaptureOverlay::updateAnnotation(const QPoint &pos, Qt::KeyboardModifiers mods)
{
    if (m_ann.mosaicBrush) {
        if ((pos - m_ann.stroke.last().toPoint()).manhattanLength() >= 2) m_ann.stroke << pos;
        return;
    }
    switch (m_atool) {
    case ATool::Pen:
    case ATool::Marker:
        if ((pos - m_ann.stroke.last().toPoint()).manhattanLength() >= 2)
            m_ann.stroke << pos;                 // Decimate near-duplicate samples.
        break;
    case ATool::Arrow: {
        QPointF b = pos;
        if (mods & Qt::ShiftModifier) {          // Snap the direction to 45-degree steps.
            const QLineF line(QPointF(m_ann.a), QPointF(pos));
            if (line.length() > 1) {
                const qreal angle = qRound(qAtan2(-(pos.y() - m_ann.a.y()),
                                                    pos.x() - m_ann.a.x())
                                           / (M_PI / 4)) * (M_PI / 4);
                b = QPointF(m_ann.a.x() + qCos(angle) * line.length(),
                            m_ann.a.y() - qSin(angle) * line.length());
            }
        }
        m_ann.b = b.toPoint();
        break;
    }
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
        : m_ann.tool == ATool::Pen || m_ann.tool == ATool::Marker
        ? m_ann.stroke.size() >= 2
        : m_ann.tool == ATool::Arrow
              ? (m_ann.a - m_ann.b).manhattanLength() >= 6
              : m_ann.rect.width() >= 3 && m_ann.rect.height() >= 3;
    if (meaningful) {                            // Discard accidental tiny strokes.
        pushUndo();
        m_anns.append(m_ann);
        normalizeNumbers();
        m_selAnn = m_ann.mosaicBrush ? -1 : m_anns.size() - 1;  // Completed mosaic strokes are not re-editable.
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
    m_sizeTip->hide();
    placeToolbar();
    showToolBars();
    syncSizeInput();
    update();
}

void CaptureOverlay::setToolNone()
{
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
    stopItemDrag();
    commitTextEdit();
    m_atool = tool;
    if (tool == ATool::Rect) m_shapeEllipse = false;        // R/O/popup pick the sub-shape.
    else if (tool == ATool::Ellipse) m_shapeEllipse = true;
    m_selAnn = -1;                               // Tools and item selection exclude each other.
    m_acolor = toolColor(tool);
    syncToolButtons();
    updateToolOptions();
    updateAnnotationCursor(mapFromGlobal(QCursor::pos()));
    updateInspector();
    update();
}

void CaptureOverlay::syncToolButtons()                           // Rect and Ellipse share one button.
{
    const QToolButton *active = m_atool == ATool::None ? nullptr : m_toolBtn[int(m_atool)];
    for (int t = 1; t < m_toolBtn.size(); ++t) {
        QSignalBlocker lock(m_toolBtn[t]);
        m_toolBtn[t]->setChecked(m_toolBtn[t] == active);
    }
    const QSignalBlocker lock(m_shapeButton);
    m_shapeButton->setIcon(makeIcon(m_shapeEllipse ? Icon::Ellipse : Icon::Rect));
}

QColor CaptureOverlay::toolColor(ATool tool) const               // Each tool remembers its own color.
{
    switch (tool) {
    case ATool::Rect: case ATool::Ellipse: return m_colorShape;
    case ATool::Arrow: return m_colorArrow;
    case ATool::Pen: return m_colorPen;
    case ATool::Marker: return m_colorMarker;
    case ATool::Text: return m_colorText;
    case ATool::Number: return m_colorNumber;
    default: return m_acolor;
    }
}

void CaptureOverlay::pickToolColor(ATool family, const QColor &color, QLineEdit *source)
{
    if (!color.isValid()) return;
    switch (family) {
    case ATool::Rect: case ATool::Ellipse: m_colorShape = color; break;
    case ATool::Arrow: m_colorArrow = color; break;
    case ATool::Pen: m_colorPen = color; break;
    case ATool::Marker: m_colorMarker = color; break;
    case ATool::Text: m_colorText = color; break;
    case ATool::Number: m_colorNumber = color; break;
    default: return;
    }
    const bool sameFamily = [](ATool a, ATool b) {
        const bool shapes = (a == ATool::Rect || a == ATool::Ellipse)
                         && (b == ATool::Rect || b == ATool::Ellipse);
        return a == b || shapes;
    }(m_atool, family);
    if (sameFamily) m_acolor = color;
    if (editorActive() && family == ATool::Text) {
        m_editColor = color;
        applyEditorFont();
    } else if (m_live && sameFamily) {
        m_ann.color = color;                    // Committed annotations retain their original colors.
        update();
    }
    if (m_colorDialog && m_colorDialogTool == family) {
        const QSignalBlocker lock(m_colorWheel);
        m_colorWheel->setCurrentColor(color);
        const QSignalBlocker sliderLock(m_colorBrightness);
        m_colorBrightness->setValue(m_colorWheel->brightness());
        m_colorBrightness->setStyleSheet(QStringLiteral(
            "QSlider::groove:vertical{width:12px;border:1px solid #626468;"
            "background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %1,stop:1 #000000);}"
            "QSlider::handle:vertical{background:#fff;border:1px solid #626468;height:8px;margin:0 -3px;}")
            .arg(m_colorWheel->brightColor().name()));
        m_colorPreview->setStyleSheet(QStringLiteral("background:%1;border:1px solid #929498;border-radius:3px;").arg(color.name()));
        for (QLineEdit *field : {m_rgbInput.data(), m_hexInput.data()}) {
            if (!field || field == source) continue;
            const QSignalBlocker fieldLock(field);
            field->setText(field == m_rgbInput ? rgbColorText(color) : color.name());
            field->setStyleSheet(QString());
        }
    }
}

QString CaptureOverlay::rgbColorText(const QColor &color)
{
    return QStringLiteral("rgb(%1,%2,%3)").arg(color.red()).arg(color.green()).arg(color.blue());
}

QColor CaptureOverlay::parseAnnotationColor(const QString &text)
{
    const QString value = text.trimmed();
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    if (hex.match(value).hasMatch()) return QColor(value);
    static const QRegularExpression rgb(QStringLiteral("^rgb\\(\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*\\)$"),
                                         QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = rgb.match(value);
    if (!match.hasMatch()) return {};
    const int r = match.captured(1).toInt(), g = match.captured(2).toInt(), b = match.captured(3).toInt();
    return r <= 255 && g <= 255 && b <= 255 ? QColor(r, g, b) : QColor();
}

void CaptureOverlay::showColorPalette(ATool family, QToolButton *anchor)
{
    if (m_colorDialog) m_colorDialog->close();
    auto *dialog = new QDialog(this, Qt::Popup | Qt::FramelessWindowHint);
    m_colorDialog = dialog;
    m_colorDialogTool = family;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("调色板"));
    dialog->setStyleSheet(QStringLiteral(
        "QDialog{background:rgb(29,31,35);border:1px solid #626468;}"
        "QLineEdit{color:#fff;background:rgb(29,31,35);border:1px solid #626468;"
        "padding:3px;font-size:12px;selection-background-color:#007aff;}"));
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    m_colorWheel = new ColorWheel(toolColor(family), dialog);
    top->addWidget(m_colorWheel);
    m_colorBrightness = new QSlider(Qt::Vertical, dialog);
    m_colorBrightness->setRange(0, 255);
    m_colorBrightness->setFixedSize(18, 200);
    m_colorBrightness->setAccessibleName(QStringLiteral("明暗"));
    m_colorBrightness->setToolTip(QStringLiteral("拖动调整明暗"));
    top->addWidget(m_colorBrightness);
    layout->addLayout(top);
    auto *bottom = new QHBoxLayout;
    bottom->setSpacing(8);
    m_colorPreview = new QLabel(dialog);
    m_colorPreview->setFixedSize(32, 54);
    m_colorPreview->setToolTip(QStringLiteral("当前颜色"));
    bottom->addWidget(m_colorPreview);
    auto *fields = new QVBoxLayout;
    fields->setSpacing(4);
    m_rgbInput = new QLineEdit(dialog);
    m_hexInput = new QLineEdit(dialog);
    m_rgbInput->setPlaceholderText(QStringLiteral("rgb(255,0,0)"));
    m_hexInput->setPlaceholderText(QStringLiteral("#ff0000"));
    m_rgbInput->setToolTip(QStringLiteral("RGB 分量为 0–255，例如 rgb(255,0,0)"));
    m_hexInput->setToolTip(QStringLiteral("六位 HEX 颜色，例如 #ff0000"));
    m_rgbInput->setAccessibleName(QStringLiteral("RGB 颜色"));
    m_hexInput->setAccessibleName(QStringLiteral("HEX 颜色"));
    for (QLineEdit *field : {m_rgbInput.data(), m_hexInput.data()}) {
        field->installEventFilter(this);
        fields->addWidget(field);
        connect(field, &QLineEdit::textEdited, this, [this, family, field](const QString &text) {
            const QColor color = parseAnnotationColor(text);
            field->setStyleSheet(color.isValid() ? QString() : QStringLiteral("border:1px solid #ff3b30;"));
            if (color.isValid()) pickToolColor(family, color, field);
        });
    }
    bottom->addLayout(fields);
    layout->addLayout(bottom);
    connect(m_colorBrightness, &QSlider::valueChanged, m_colorWheel, &ColorWheel::setBrightness);
    connect(m_colorWheel, &ColorWheel::colorChanged, this,
            [this, family](const QColor &color) { pickToolColor(family, color); });
    connect(dialog, &QDialog::finished, this, [this] {
        if (!isVisible()) return;
        if (editorActive()) { m_textView->setFocus(); m_editor->setFocus(); }
        else setFocus();
    });
    pickToolColor(family, toolColor(family));
    dialog->show();
    dialog->adjustSize();
    const QPoint below = anchor->mapToGlobal(QPoint(0, anchor->height()));
    const QScreen *screen = QGuiApplication::screenAt(below);
    const QRect available = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    const int y = below.y() + dialog->height() <= available.bottom() + 1 ? below.y()
        : qMax(available.top(), anchor->mapToGlobal(QPoint()).y() - dialog->height());
    dialog->move(qBound(available.left(), below.x(), available.right() + 1 - dialog->width()), y);
}

void CaptureOverlay::pickArrowStyle(int style)
{
    m_arrowStyle = style;
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()
        && m_anns[m_selAnn].tool == ATool::Arrow) {
        pushUndo();
        m_anns[m_selAnn].arrowStyle = style;
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

void CaptureOverlay::updateToolOptions()
{
    if (m_colorDialog) m_colorDialog->close();
    const QRect old = m_toolOptions->geometry();
    const ATool tool = optionsTool();
    if (tool == ATool::None) {
        m_toolOptions->hide();
        update(old);
        return;
    }
    m_sizeInput = nullptr;
    m_mosaicSlider = nullptr;
    m_mosaicStrengthLabel = nullptr;
    m_mosaicWidthLabel = nullptr;
    m_mosaicPreview = nullptr;
    m_mosaicDragUndo = false;
    m_colorButton = nullptr;
    delete m_toolOptions->layout();              // Drops the previous row.
    const QList<QWidget *> stale = m_toolOptions->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *w : stale) delete w;
    auto *lay = new QHBoxLayout(m_toolOptions);
    lay->setContentsMargins(4, 2, 4, 2);
    lay->setSpacing(2);
    lay->setAlignment(Qt::AlignLeft);
    auto addOption = [this](QHBoxLayout *l, Icon icon, const QString &tip, bool on,
                            const std::function<void()> &apply) {
        auto *b = iconButton(icon, tip);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCheckable(true);
        b->setChecked(on);
        connect(b, &QToolButton::clicked, this, [this, apply, on] { apply(); Q_UNUSED(on); });
        l->addWidget(b);
    };
    auto addSeparator = [](QHBoxLayout *l) {
        auto *line = new QFrame;
        line->setFrameShape(QFrame::VLine);
        line->setFixedHeight(20);
        l->addWidget(line);
    };
    auto addSwatches = [this](QHBoxLayout *l, ATool family) {
        static const QColor swatches[] = {"#ff3b30", "#ff9500", "#ffcc00", "#34c759",
                                          "#007aff", "#af52de", "#ffffff", "#1c1c1e"};
        for (const QColor &color : swatches) {
            auto *swatch = new QToolButton;
            swatch->setObjectName(QStringLiteral("presetColor"));
            swatch->setFixedSize(18, 18);
            swatch->setFocusPolicy(Qt::NoFocus);
            swatch->setCursor(Qt::PointingHandCursor);
            swatch->setToolTip(color.name());
            swatch->setStyleSheet(QString("QToolButton{background:%1;border:1px solid #929498;}")
                                      .arg(color.name()));
            connect(swatch, &QToolButton::clicked, this,
                    [this, family, color] { pickToolColor(family, color); });
            l->addWidget(swatch);
        }
        m_colorButton = iconButton(Icon::Palette, QStringLiteral("调色板：点击或拖动选色，或输入 RGB / HEX"));
        m_colorButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_colorButton->setFocusPolicy(Qt::NoFocus);
        m_colorButton->setCursor(Qt::PointingHandCursor);
        QToolButton *button = m_colorButton;
        connect(button, &QToolButton::clicked, this, [this, family, button] { showColorPalette(family, button); });
        l->addWidget(button);
    };
    const bool texty = tool == ATool::Text || tool == ATool::Number;
    auto addSize = [this, lay, texty, tool] {
        const bool strength = tool == ATool::Mosaic && !mosaicBrush();
        const int minimum = strength ? 3 : tool == ATool::Mosaic ? 4 : 1;
        const int maximum = texty ? 1638 : tool == ATool::Mosaic ? strength ? 24 : 50 : 12;
        m_sizeInput = new QComboBox;
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
        m_sizeInput->setToolTip(texty ? QStringLiteral("标准字号 (pt)：小四=12，五号=10.5")
                                    : QStringLiteral("输入、下拉选择或滚轮调节粗细"));
        m_sizeInput->setFocusPolicy(Qt::ClickFocus);
        m_sizeInput->setFixedWidth(texty ? 62 : 40);
        m_sizeInput->setStyleSheet(QStringLiteral("QComboBox{padding:1px 2px;}QComboBox::drop-down{width:12px;}"));
        m_sizeInput->lineEdit()->setAlignment(Qt::AlignCenter);
        m_sizeInput->setAccessibleName(texty ? QStringLiteral("字号") : QStringLiteral("粗细"));
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
    case ATool::Ellipse:
        addOption(lay, Icon::Rect, QStringLiteral("矩形"), !m_shapeEllipse,
                  [this] { selectTool(ATool::Rect); });
        addOption(lay, Icon::Ellipse, QStringLiteral("椭圆"), m_shapeEllipse,
                  [this] { selectTool(ATool::Ellipse); });
        addSize();
        addSeparator(lay);
        addSwatches(lay, ATool::Rect);
        break;
    case ATool::Arrow: {
        struct { Icon icon; const char *tip; } const styles[] = {
            {Icon::Arrow, "单向"}, {Icon::ArrowBoth, "双向"},
            {Icon::ArrowLine, "无箭头"}, {Icon::ArrowHollow, "镂空"},
        };
        for (int k = 0; k < 4; ++k)
            addOption(lay, styles[k].icon, QString::fromUtf8(styles[k].tip),
                      m_arrowStyle == k, [this, k] { pickArrowStyle(k); });
        addSize();
        addSeparator(lay);
        addSwatches(lay, ATool::Arrow);
        break;
    }
    case ATool::Text: {
        auto *bold = iconButton(Icon::Bold, QStringLiteral("粗体"));
        auto *italic = iconButton(Icon::Italic, QStringLiteral("斜体"));
        for (auto *b : {bold, italic}) {
            b->setFocusPolicy(Qt::NoFocus);
            b->setCheckable(true);
            b->setCursor(Qt::PointingHandCursor);
        }
        bold->setChecked(editorActive() ? m_editBold : m_abold);
        italic->setChecked(editorActive() ? m_editItalic : m_aitalic);
        bold->setToolTip(QStringLiteral("粗体"));
        italic->setToolTip(QStringLiteral("斜体"));
        connect(bold, &QToolButton::toggled, this, &CaptureOverlay::setTextBold);
        connect(italic, &QToolButton::toggled, this, &CaptureOverlay::setTextItalic);
        lay->addWidget(bold);
        lay->addWidget(italic);
        auto *font = new FontComboBox;
        font->setObjectName(QStringLiteral("annotationFont"));
        font->setEditable(false);
        font->addItems(QFontDatabase().families());
        font->setFocusPolicy(Qt::NoFocus);
        font->setFixedWidth(40);
        font->view()->setMinimumWidth(200);
        font->setAccessibleName(QStringLiteral("字体"));
        font->setCurrentText(editorActive() ? m_editFamily
            : m_selAnn >= 0 && m_selAnn < m_anns.size() ? m_anns[m_selAnn].family : m_afamily);
        font->setToolTip(QStringLiteral("字体：%1").arg(font->currentText()));
        connect(font, &QComboBox::currentTextChanged, this, [this, font](const QString &family) {
            font->setToolTip(QStringLiteral("字体：%1").arg(family));
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
        if (brush) {
            m_mosaicWidthLabel = new QLabel(QString::number(toolSize()));
            m_mosaicWidthLabel->setObjectName(QStringLiteral("mosaicWidthValue"));
            m_mosaicWidthLabel->setToolTip(QStringLiteral("涂抹粗细：滚轮调节，不可手动编辑"));
            m_mosaicWidthLabel->setAccessibleName(QStringLiteral("涂抹粗细（只读）"));
            m_mosaicWidthLabel->setTextInteractionFlags(Qt::NoTextInteraction);
            m_mosaicWidthLabel->setFocusPolicy(Qt::NoFocus);
            m_mosaicWidthLabel->setAlignment(Qt::AlignCenter);
            m_mosaicWidthLabel->setFixedWidth(24);
            m_mosaicWidthLabel->installEventFilter(this);
            lay->addWidget(m_mosaicWidthLabel);
        }
        addOption(lay, Icon::Rect, QStringLiteral("矩形马赛克"), !brush,
                  [this] { selectMosaicMode(false); });
        lay->itemAt(lay->count() - 1)->widget()->setObjectName(QStringLiteral("mosaicRectangle"));
        addOption(lay, Icon::Pen, QStringLiteral("马赛克笔：滚轮调节笔宽"), brush,
                  [this] { selectMosaicMode(true); });
        lay->itemAt(lay->count() - 1)->widget()->setObjectName(QStringLiteral("mosaicBrush"));
        m_mosaicPreview = iconButton(Icon::Mosaic, QStringLiteral("像素化 / 模糊"));
        m_mosaicPreview->setObjectName(QStringLiteral("mosaicEffectPreview"));
        m_mosaicPreview->setAccessibleName(QStringLiteral("切换像素化 / 模糊"));
        m_mosaicPreview->setFocusPolicy(Qt::NoFocus);
        m_mosaicPreview->installEventFilter(this);
        connect(m_mosaicPreview, &QToolButton::clicked, this, &CaptureOverlay::switchMosaicEffect);
        lay->addWidget(m_mosaicPreview);
        m_mosaicSlider = new QSlider(Qt::Horizontal);
        m_mosaicSlider->setObjectName(QStringLiteral("mosaicStrength"));
        m_mosaicSlider->setAccessibleName(QStringLiteral("马赛克强度"));
        m_mosaicSlider->setRange(3, 24);
        m_mosaicSlider->setPageStep(1);
        m_mosaicSlider->setFixedSize(128, 22);
        m_mosaicSlider->setFocusPolicy(Qt::NoFocus);
        m_mosaicSlider->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal{height:4px;background:#626468;border-radius:2px;}"
            "QSlider::sub-page:horizontal{background:#fff;border-radius:2px;}"
            "QSlider::handle:horizontal{width:10px;margin:-4px 0;background:#fff;border-radius:5px;}"));
        m_mosaicSlider->installEventFilter(this);
        syncSizeInput();
        connect(m_mosaicSlider, &QSlider::valueChanged, this, &CaptureOverlay::setMosaicStrength);
        connect(m_mosaicSlider, &QSlider::sliderPressed, this, [this] { m_mosaicDragUndo = false; });
        connect(m_mosaicSlider, &QSlider::sliderReleased, this, [this] { m_mosaicDragUndo = false; });
        lay->addWidget(m_mosaicSlider);
        m_mosaicStrengthLabel = new QLabel;
        m_mosaicStrengthLabel->setObjectName(QStringLiteral("mosaicStrengthValue"));
        m_mosaicStrengthLabel->setAccessibleName(QStringLiteral("马赛克强度"));
        m_mosaicStrengthLabel->setToolTip(QStringLiteral("马赛克强度：拖动左侧滑条或滚轮调节"));
        m_mosaicStrengthLabel->setAlignment(Qt::AlignCenter);
        m_mosaicStrengthLabel->setFixedWidth(24);
        m_mosaicStrengthLabel->installEventFilter(this);
        lay->addWidget(m_mosaicStrengthLabel);
        syncSizeInput();
        break;
    }
    case ATool::Eraser: {
        addSize();
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
    placeToolbar();
    m_toolOptions->raise();
    update(old.united(m_toolOptions->geometry()).adjusted(-1, -1, 1, 1));
}

void CaptureOverlay::paintAnnotations(QPainter &p, const QPointF &origin, qreal scale, const QRectF &clip,
                      bool includeEditor) const
{
    if (m_anns.isEmpty() && !m_live && !editorActive()) return;
    p.save();
    p.translate(origin);
    p.scale(scale, scale);
    p.setClipRect(clip, Qt::IntersectClip);
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
    const auto paint = [&](const Annotation &ann, bool visible = true) {
        if (ann.tool == ATool::Mosaic) --remainingMosaics;
        if (visible) drawAnnotation(p, ann, lower);
        if (remainingMosaics > 0) {
            const QImage source = ann.tool == ATool::Mosaic ? lower : QImage();
            QPainter layer(&lower);
            layer.scale(m_shot.devicePixelRatioF(), m_shot.devicePixelRatioF());
            layer.setClipRect(QRectF(m_sel));
            drawAnnotation(layer, ann, source);
        }
    };
    for (int i = 0; i < m_anns.size(); ++i)
        if (!editorActive() || i != m_editAnn) paint(m_anns[i]);
        else paint(editorAnnotation(), includeEditor);
    if (editorActive() && m_editAnn < 0) paint(editorAnnotation(), includeEditor);
    if (m_live) paint(m_ann);
    p.restore();
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

void CaptureOverlay::drawAnnotation(QPainter &p, const Annotation &ann, const QImage &source) const
{
    QPen pen(ann.color, ann.pen);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setBrush(Qt::NoBrush);
    p.save();
    if (!qFuzzyIsNull(ann.rot) && ann.tool != ATool::Mosaic) { // Effects rotate their mask, not their source.
        const QTransform t = annTransform(ann);
        p.setTransform(t, true);
    }
    switch (ann.tool) {
    case ATool::Rect:
    case ATool::Ellipse:
        if (ann.fill) {                          // Fill mode: translucent wash plus outline.
            QColor wash = ann.color;
            wash.setAlpha(70);
            p.setBrush(wash);
        }
        p.setPen(pen);
        if (ann.tool == ATool::Rect) p.drawRect(ann.rect);
        else p.drawEllipse(ann.rect);
        break;
    case ATool::Pen:
        p.setPen(pen);
        p.drawPolyline(ann.stroke);
        break;
    case ATool::Marker: {                        // Translucent fat stroke, marker style.
        QColor wash = ann.color;
        wash.setAlpha(110);
        QPen washPen(wash, ann.pen * 5);
        washPen.setCapStyle(Qt::RoundCap);
        washPen.setJoinStyle(Qt::RoundJoin);
        p.setPen(washPen);
        p.drawPolyline(ann.stroke);
        break;
    }
    case ATool::Text: {
        if (ann.text.isEmpty()) break;
        QTextDocument doc;
        setupTextDocument(doc, ann);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, ann.color);
        p.translate(ann.rect.topLeft());
        doc.documentLayout()->draw(&p, context);
        break;
    }
    case ATool::Number: {                        // Filled circle with a sequence digit.
        if (ann.text.isEmpty()) break;
        QFont font = textFont(ann);
        font.setBold(true);
        p.setPen(Qt::NoPen);
        p.setBrush(ann.color);
        p.drawEllipse(ann.rect);
        p.setPen(ann.color.lightness() > 165 ? Qt::black : Qt::white);
        p.setFont(font);
        p.drawText(ann.rect, Qt::AlignCenter, ann.text);
        break;
    }
    case ATool::Arrow: {
        p.setPen(pen);
        const QLineF shaft(ann.a, ann.b);
        if (shaft.length() < 1) break;
        if (ann.arrowStyle == 2) {               // Plain line, no heads.
            p.drawLine(shaft);
            break;
        }
        const qreal head = qMax(qreal(ann.pen * 2 + 8), qreal(11));
        const QPointF dir = (shaft.p2() - shaft.p1()) / shaft.length();
        const QPointF norm(-dir.y(), dir.x());
        QLineF line = shaft;
        line.setLength(qMax(qreal(1), shaft.length() - head * 0.55));
        if (ann.arrowStyle == 1) {               // Double: pull the shaft back twice.
            QLineF mid(line.p1() + dir * head * 0.55, line.p2() - dir * head * 0.55);
            if (mid.length() < 1) break;
            p.drawLine(mid);
        } else {
            p.drawLine(line);
        }
        if (ann.arrowStyle != 3) p.setBrush(ann.color);   // Hollow keeps the outline only.
        QPolygonF tip;
        tip << shaft.p2() << shaft.p2() - dir * head + norm * head * 0.45
            << shaft.p2() - dir * head - norm * head * 0.45;
        p.drawPolygon(tip);
        if (ann.arrowStyle == 1) {               // Second head at the start.
            QPolygonF tail;
            tail << shaft.p1() << shaft.p1() + dir * head + norm * head * 0.45
                 << shaft.p1() + dir * head - norm * head * 0.45;
            p.drawPolygon(tail);
        }
        break;
    }
    case ATool::Mosaic: {                        // Redact lower layers through the object's mask.
        const qreal dpr = m_shot.devicePixelRatioF();
        const QRect area = annotationRect(ann).intersected(
            QRect(0, 0, qFloor(m_shot.width() / dpr), qFloor(m_shot.height() / dpr)));
        if (area.isEmpty() || (ann.mosaicBrush && ann.stroke.isEmpty())) break;
        QPainterPath mask;
        if (ann.mosaicBrush) {
            if (ann.stroke.size() == 1) mask.addEllipse(ann.stroke.first(), ann.pen / 2.0, ann.pen / 2.0);
            else {
                QPainterPath line;
                line.moveTo(ann.stroke.first());
                for (int i = 1; i < ann.stroke.size(); ++i) line.lineTo(ann.stroke[i]);
                QPainterPathStroker stroker;
                stroker.setWidth(ann.pen);
                stroker.setCapStyle(Qt::RoundCap);
                stroker.setJoinStyle(Qt::RoundJoin);
                mask = stroker.createStroke(line);
            }
        } else mask.addRect(QRectF(ann.rect));
        p.setClipPath(mask, Qt::IntersectClip);
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
    default:
        break;
    }
    p.restore();
}
