#include "capture/pixel_inspector.h"
#include "shared/widgets.h"
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QClipboard>
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
    setAttribute(Qt::WA_TranslucentBackground);
    QFont f = font();
    f.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
    setFont(f);
    setFixedSize(152, 225);
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
    m_position = globalPixel;
    m_relativeOrigin = frame.pixelRect.topLeft();
    if (!selectionPixels.isEmpty())
        m_relativeOrigin += selectionPixels.topLeft().toPoint();
    const QPoint origin = pixel - QPoint(9, 6);
    m_patch = frame.image.copy(QRect(origin, QSize(19, 13)));
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
                 : QStringLiteral("%1 , %2 , %3").arg(m_color.red()).arg(m_color.green()).arg(m_color.blue());
}

QString PixelInspector::coordinateText() const
{
    if (!m_color.isValid())
        return QString();
    const QPoint p = m_relative ? m_position - m_relativeOrigin : m_position;
    return QStringLiteral("( %1 , %2 )").arg(p.x()).arg(p.y());
}

void PixelInspector::toggleFormat() { m_hex = !m_hex; update(); }

void PixelInspector::toggleCoordinates() { m_relative = !m_relative; update(); }

void PixelInspector::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    p.fillRect(QRect(0, 104, 152, 121), QColor(0, 0, 0, 188));

    const QRect view(0, 0, 152, 104);
    p.save();
    p.setClipRect(view);
    p.fillRect(view, Qt::black);
    if (!m_patch.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(view, m_patch);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    }
    p.setPen(QPen(QColor(175, 175, 175, 97), 1));
    for (int x = 8; x < view.width(); x += 8)
        p.drawLine(view.left() + x, view.top(), view.left() + x, view.bottom());
    for (int y = 8; y < view.height(); y += 8)
        p.drawLine(view.left(), view.top() + y, view.right(), view.top() + y);

    p.fillRect(QRect(72, 0, 8, 104), QColor(53, 106, 153, 235));
    p.fillRect(QRect(0, 48, 152, 8), QColor(53, 106, 153, 235));

    const QRect center(71, 47, 10, 10);
    p.fillRect(center, Qt::white);
    p.fillRect(center.adjusted(1, 1, -1, -1), Qt::black);
    p.fillRect(center.adjusted(2, 2, -2, -2), m_color);
    p.restore();

    // A 1px white hairline keeps the magnifier readable on bright desktops;
    // it also separates the magnifier from the borderless information panel.
    p.fillRect(QRect(0, 0, 152, 1), Qt::white);
    p.fillRect(QRect(0, 103, 152, 1), Qt::white);
    p.fillRect(QRect(0, 0, 1, 104), Qt::white);
    p.fillRect(QRect(151, 0, 1, 104), Qt::white);

    QFont ui = font();
    ui.setPixelSize(14);
    p.setFont(ui);
    const QFontMetricsF fm(ui);
    const QColor text(0xd0, 0xd0, 0xd0);
    const QColor bright(0xf2, 0xf2, 0xf2);

    p.setPen(text);
    p.drawText(QRect(0, 110, 152, 20), Qt::AlignHCenter | Qt::AlignVCenter, coordinateText());

    const QString value = colorText();
    const qreal valueWidth = fm.horizontalAdvance(value);
    const int rowLeft = qRound((152 - (15 + 8 + valueWidth)) / 2);
    p.fillRect(QRect(rowLeft, 134, 15, 15), Qt::white);
    p.fillRect(QRect(rowLeft + 1, 135, 13, 13), m_color);
    p.setPen(bright);
    p.drawText(QRectF(rowLeft + 23, 132, valueWidth + 2, 20),
               Qt::AlignLeft | Qt::AlignVCenter, value);

    const QString labels[3] = { QStringLiteral("Shift:"), QStringLiteral("C:"), QStringLiteral("P:") };
    const QString descs[3] = { tr("切换颜色格式"), tr("复制颜色值"), tr("切换全局/相对坐标") };
    QFont keyFont = ui;
    keyFont.setBold(true);
    const QFontMetricsF keyMetrics(keyFont);
    for (int i = 0; i < 3; ++i) {
        const qreal y = 154 + i * 18;
        const qreal labelWidth = keyMetrics.horizontalAdvance(labels[i]);
        const qreal gap = fm.horizontalAdvance(QLatin1Char(' '));
        const qreal descWidth = fm.horizontalAdvance(descs[i]);
        const int left = qRound((152 - (labelWidth + gap + descWidth)) / 2);
        p.setFont(keyFont);
        p.setPen(bright);
        p.drawText(QRectF(left, y, labelWidth + 2, 18), Qt::AlignLeft | Qt::AlignVCenter, labels[i]);
        p.setFont(ui);
        p.setPen(text);
        p.drawText(QRectF(left + labelWidth + gap, y, descWidth + 2, 18),
                   Qt::AlignLeft | Qt::AlignVCenter, descs[i]);
    }
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
    if (event->key() == Qt::Key_Shift && !event->isAutoRepeat()) {
        m_inspector->toggleFormat();
        return;
    }
    if (event->key() == Qt::Key_P && !event->isAutoRepeat()) {
        m_inspector->toggleCoordinates();
        return;
    }
    if (event->key() == Qt::Key_C
        && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        const QString text = m_inspector->colorText();
        if (!text.isEmpty()) QGuiApplication::clipboard()->setText(text);
        return;
    }
    QDialog::keyPressEvent(event);
}

