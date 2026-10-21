#include "capture/pixel_inspector.h"
#include "shared/widgets.h"
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QColor>
#include <QFont>
#include <QPaintEvent>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QLabel>
#include <QGuiApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QShowEvent>
#include <QtMath>
#include <qt_windows.h>

PixelInspector::PixelInspector(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont f(QStringLiteral("Consolas"));
    f.setPointSize(11);
    f.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
    setFont(f);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(230, 220);
    hide();
}

bool PixelInspector::setSample(const ScreenFrame &frame, const QPoint &globalPixel,
               const QRectF &selectionPixels)
{
    if (frame.image.isNull() || !frame.pixelRect.contains(globalPixel)
        || frame.image.size() != frame.pixelRect.size())
        return false;
    const QPoint pixel = globalPixel - frame.pixelRect.topLeft();
    m_color = frame.image.pixelColor(pixel);
    const QPoint origin = pixel - QPoint(12, 8);
    m_patch = frame.image.copy(QRect(origin, QSize(25, 17)));
    m_patch.setDevicePixelRatio(1);
    if (!selectionPixels.isEmpty()) {
        QPainter p(&m_patch);
        p.translate(-origin);
        QPainterPath mask;
        mask.addRect(QRectF(origin, m_patch.size()));
        mask.addRect(selectionPixels);
        p.fillPath(mask, QColor(0, 0, 0, 120));
        p.setPen(QPen(Bar::accent, 1));
        p.drawRect(selectionPixels.adjusted(0, 0, -1, -1));
    }
    update();
    return true;
}

QString PixelInspector::colorText() const
{
    if (!m_color.isValid())
        return QString();
    return m_hex ? m_color.name(QColor::HexRgb)
                 : QStringLiteral("%1, %2, %3").arg(m_color.red()).arg(m_color.green()).arg(m_color.blue());
}

void PixelInspector::toggleFormat() { m_hex = !m_hex; update(); }

void PixelInspector::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Bar::divider,1));p.setBrush(Bar::surface);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5),8,8);
    p.setBrush(Qt::NoBrush);
    const QRect view((width() - 177) / 2, 12, 177, 121);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.save();
    p.setClipRect(view);
    p.drawImage(view.adjusted(1, 1, -1, -1), m_patch);
    const QRect pixel(view.left() + 85, view.top() + 57, 7, 7);
    QColor crosshair = Bar::accent;
    crosshair.setAlpha(88);
    p.fillRect(QRect(pixel.left(), view.top() + 1, 7, 119), crosshair);
    p.fillRect(QRect(view.left() + 1, pixel.top(), 175, 7), crosshair);
    p.fillRect(pixel, m_color);
    p.setPen(Qt::black);
    p.drawRect(pixel.adjusted(-1, -1, 0, 0));
    p.setPen(Qt::white);
    p.drawRect(pixel.adjusted(0, 0, -1, -1));
    p.restore();
    p.setPen(Qt::white);
    p.drawRect(view.adjusted(0, 0, -1, -1));
    QFont valueFont=font();valueFont.setPixelSize(14);p.setFont(valueFont);
    p.setPen(Bar::ink);p.setBrush(m_color);p.drawRoundedRect(QRect(20,150,20,20),4,4);
    p.drawText(QRect(52,148,160,24),Qt::AlignVCenter,m_color.name(QColor::HexRgb).toUpper());
    QFont rgb=valueFont;rgb.setPixelSize(12);p.setFont(rgb);p.setPen(Bar::muted);
    p.drawText(QRect(20,180,190,20),Qt::AlignVCenter,
        QStringLiteral("RGB  %1, %2, %3").arg(m_color.red()).arg(m_color.green()).arg(m_color.blue()));
}

namespace {
QPoint nativePixelFor(const ScreenFrame &frame, const QPoint &logicalPixel)
{
    if (frame.logicalRect.isEmpty() || frame.image.size() != frame.pixelRect.size())
        return QPoint(-1, -1);
    const qreal sx = qreal(frame.pixelRect.width()) / frame.logicalRect.width();
    const qreal sy = qreal(frame.pixelRect.height()) / frame.logicalRect.height();
    return frame.pixelRect.topLeft() + QPoint(qRound((logicalPixel.x() - frame.logicalRect.left()) * sx),
                                             qRound((logicalPixel.y() - frame.logicalRect.top()) * sy));
}
}

QColor ScreenColorPicker::sample(const QVector<ScreenFrame> &frames, const QPoint &logicalPixel)
{
    for (const ScreenFrame &frame : frames) {
        if (!frame.logicalRect.contains(logicalPixel)) continue;
        const QPoint pixel = nativePixelFor(frame, logicalPixel);
        if (frame.pixelRect.contains(pixel)) return frame.image.pixelColor(pixel - frame.pixelRect.topLeft());
    }
    return {};
}

ScreenColorPicker::ScreenColorPicker(const QVector<ScreenFrame> &frames, QWidget *parent)
    : QDialog(parent), m_frames(frames)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setModal(true);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    QRect area;
    for (const ScreenFrame &frame : m_frames) area = area.united(frame.logicalRect);
    if (!area.isEmpty()) setGeometry(area);
    m_inspector = new PixelInspector(this);
    m_hint = new QLabel(tr("点击取色 · Esc 取消"), this);
    m_hint->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_hint->setStyleSheet(QStringLiteral(
        "background:rgba(16,17,20,235);color:#f4f5f8;border:1px solid #2b2d33;"
        "border-radius:8px;padding:7px 12px;font-size:13px;"));
    m_hint->adjustSize();
    const QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    const QRect host = screen ? screen->geometry() : area;
    const QRect localHost(mapFromGlobal(host.topLeft()), host.size());
    m_hint->move(localHost.center().x() - m_hint->width() / 2, localHost.top() + 18);
}

void ScreenColorPicker::paintEvent(QPaintEvent *)
{
    // Alpha 1 keeps every pixel in the layered window's hit-test shape while staying invisible.
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0, 0, 0, 1));
}

void ScreenColorPicker::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    track(mapFromGlobal(QCursor::pos()));
}

void ScreenColorPicker::track(const QPoint &position)
{
    const QPoint global = mapToGlobal(position);
    for (const ScreenFrame &frame : m_frames) {
        if (!frame.logicalRect.contains(global)) continue;
        const QPoint pixel = nativePixelFor(frame, global);
        if (!m_inspector->setSample(frame, pixel)) continue;
        m_color = m_inspector->color();
        const QRect bounds = frame.logicalRect;
        const QSize size = m_inspector->size();
        int x = global.x() + 8, y = global.y() + 20;
        if (x + size.width() > bounds.right() + 1) x = global.x() - size.width() - 8;
        if (y + size.height() > bounds.bottom() + 1) y = global.y() - size.height() - 20;
        x = qBound(bounds.left(), x, qMax(bounds.left(), bounds.right() + 1 - size.width()));
        y = qBound(bounds.top(), y, qMax(bounds.top(), bounds.bottom() + 1 - size.height()));
        m_inspector->move(mapFromGlobal(QPoint(x, y)));
        m_inspector->show();
        m_inspector->raise();
        return;
    }
    m_inspector->hide();
    m_color = QColor();
}

void ScreenColorPicker::mouseMoveEvent(QMouseEvent *event)
{
    track(event->pos());
    event->accept();
}

void ScreenColorPicker::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        reject();
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    track(event->pos());
    if (m_color.isValid()) accept();
}

void ScreenColorPicker::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && m_color.isValid()) {
        accept();
        return;
    }
    QDialog::keyPressEvent(event);
}
