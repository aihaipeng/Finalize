#include "pin_window.h"
#include "ocr_dialog.h"
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QColor>
#include <QAction>
#include <QPaintEvent>
#include <QContextMenuEvent>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QtMath>
#include <QFileDialog>
#include <QStandardPaths>
#include <QCursor>
#include <QMessageBox>
#include <qt_windows.h>

PinWindow::PinWindow(const QPixmap &pix, const QRect &displayRect)
    : m_pix(pix), m_scale(pix.isNull() ? 1 : qMin(double(displayRect.width()) / pix.width(), double(displayRect.height()) / pix.height()))
{
    if (pix.isNull() || displayRect.isEmpty()) return;
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_DeleteOnClose);
    setCursor(Qt::SizeAllCursor);
    setGeometry(displayRect);
    show();
}

QRect PinWindow::fittedGeometry(const QSize &imageSize, const QRect &source, const QRect &available)
{
    if (imageSize.isEmpty() || source.isEmpty() || available.isEmpty())
        return {};
    // Fit from the capture origin, not the screen center; never enlarge the source width.
    const int roomWidth = qMax(1, available.right() + 1 - source.left());
    const int roomHeight = qMax(1, available.bottom() + 1 - source.top());
    const double scale = qMin(double(source.width()) / imageSize.width(),
                              qMin(double(roomWidth) / imageSize.width(),
                                   double(roomHeight) / imageSize.height()));
    return QRect(source.topLeft(), QSize(qMax(1, qRound(imageSize.width() * scale)),
                                         qMax(1, qRound(imageSize.height() * scale))));
}

void PinWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.drawPixmap(0, 0, m_pix.scaled(size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    if (m_borderOn) {                             // Highlight border, toggled through the context menu.
        p.setPen(QPen(QColor("#1e90ff"), 2));
        p.drawRect(rect().adjusted(1, 1, -1, -1));
    } else {
        p.setPen(QPen(QColor(0, 0, 0, 90), 1));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

void PinWindow::contextMenuEvent(QContextMenuEvent *)
{
    QMenu m(this);
    QAction *copy = m.addAction(QStringLiteral("复制"));
    QAction *save = m.addAction(QStringLiteral("保存"));
    QAction *ocr = m.addAction(QStringLiteral("文字识别 (OCR)…"));
    ocr->setEnabled(!m_pix.isNull());
    QAction *border = m.addAction(m_borderOn ? QStringLiteral("去边框高亮")
                                             : QStringLiteral("加边框高亮"));
    QAction *chosen = m.exec(QCursor::pos());
    if (chosen == copy) {
        if (!copyImageToClipboard(m_pix.toImage()))
            QMessageBox::warning(this, QStringLiteral("pinshot"), QStringLiteral("复制失败，剪贴板可能被占用，请重试"));
    } else if (chosen == save) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存图片"), dir + QStringLiteral("/pin.png"),
            QStringLiteral("PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)"));
        if (!path.isEmpty() && !saveImageAtomic(m_pix.toImage(), path))
            QMessageBox::warning(this, QStringLiteral("pinshot"), QStringLiteral("保存失败，请检查路径或磁盘空间后重试"));
    } else if (chosen == ocr) {
        showOcr(m_pix.toImage(), this, geometry());
    } else if (chosen == border) {
        m_borderOn = !m_borderOn;
        update();
    }
}

void PinWindow::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)            // Reserve right-click for the menu.
        m_dragOff = e->globalPos() - pos();
}

void PinWindow::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton) move(e->globalPosition().toPoint() - m_dragOff);
}

void PinWindow::wheelEvent(QWheelEvent *e)       // Zoom with mouse-wheel input.
{
    if (!e->angleDelta().y()) return;
    const double f = e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1;
    m_scale = qBound(1.0 / qMax(m_pix.width(), m_pix.height()), m_scale * f, 32.0);
    // Keep the cursor position anchored while zooming.
    const QPoint a = e->globalPosition().toPoint();
    const QSize ns(qMax(1, qRound(m_pix.width() * m_scale)), qMax(1, qRound(m_pix.height() * m_scale)));
    QPoint np(a.x() - double(a.x() - x()) / width() * ns.width(),
              a.y() - double(a.y() - y()) / height() * ns.height());
    setGeometry(QRect(np, ns));
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent *)  { close(); }

void PinWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape)
        close();
}
