// pinshot - screenshot capture, pinned images, and manual scrolling capture (Qt 5.15 / MinGW).
#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QClipboard>
#include <QFileDialog>
#include <QStandardPaths>
#include <QCursor>
#include <QMessageBox>
#include <QAbstractNativeEventFilter>
#include <QTimer>
#include <qt_windows.h>
#include <functional>

extern "C" __declspec(dllimport) int __stdcall SetWindowDisplayAffinity(void *hwnd, unsigned long dwAffinity);
#define PINSHOT_WDA_EXCLUDEFROMCAPTURE 0x00000011

static QPixmap grabScreen(const QRect &rect)
{
    const QRect whole = QGuiApplication::primaryScreen()->virtualGeometry();
    const QRect r = rect.isNull() || rect.isEmpty() ? whole : rect;
    QPixmap out;

    for (QScreen *s : QGuiApplication::screens()) {
        if (s->geometry().contains(r)) {
            out = s->grabWindow(0).copy(QRect(r.topLeft() - s->geometry().topLeft(), r.size()));
            if (!out.isNull())
                break;
        }
    }

    if (out.isNull()) {
        out = QPixmap(r.size());
        out.fill(Qt::black);
        QPainter p(&out);
        for (QScreen *s : QGuiApplication::screens()) {
            const QRect inter = s->geometry().intersected(r);
            if (!inter.isEmpty())
                p.drawPixmap(inter.topLeft() - r.topLeft(), s->grabWindow(0),
                             QRect(inter.topLeft() - s->geometry().topLeft(), inter.size()));
        }
    }
    return out;
}

class PinWindow : public QWidget
{
public:
    explicit PinWindow(const QPixmap &pix, const QPoint &at)
        : m_pix(pix), m_scale(1.0)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_DeleteOnClose);
        setCursor(Qt::SizeAllCursor);
        QSize s = pix.size();
        if (const QScreen *scr = QGuiApplication::screenAt(at)) {
            const QRect av = scr->availableGeometry();
            const double fit = qMin(1.0, qMin(av.width() * 0.9 / s.width(),
                                              av.height() * 0.85 / s.height()));
            m_scale = fit;
            s = QSize(qMax(1, qRound(pix.width() * fit)), qMax(1, qRound(pix.height() * fit)));
        }
        resize(s);
        move(at - QPoint(s.width(), s.height()) / 2);
        show();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.drawPixmap(0, 0, m_pix.scaled(size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        if (m_borderOn) {
            p.setPen(QPen(QColor("#1e90ff"), 2));
            p.drawRect(rect().adjusted(1, 1, -1, -1));
        } else {
            p.setPen(QPen(QColor(0, 0, 0, 90), 1));
            p.drawRect(rect().adjusted(0, 0, -1, -1));
        }
    }
    void contextMenuEvent(QContextMenuEvent *) override
    {
        QMenu m(this);
        QAction *copy = m.addAction(QStringLiteral("复制"));
        QAction *save = m.addAction(QStringLiteral("保存"));
        QAction *border = m.addAction(m_borderOn ? QStringLiteral("去边框高亮")
                                                 : QStringLiteral("加边框高亮"));
        QAction *chosen = m.exec(QCursor::pos());
        if (chosen == copy) {
            QGuiApplication::clipboard()->setPixmap(m_pix);
        } else if (chosen == save) {
            const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
            const QString path = QFileDialog::getSaveFileName(
                this, QStringLiteral("保存图片"), dir + QStringLiteral("/pin.png"),
                QStringLiteral("PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)"));
            if (!path.isEmpty())
                m_pix.save(path);
        } else if (chosen == border) {
            m_borderOn = !m_borderOn;
            update();
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton)
            m_dragOff = e->globalPos() - pos();
    }
    void mouseMoveEvent(QMouseEvent *e) override  { move(e->globalPos() - m_dragOff); }
    void wheelEvent(QWheelEvent *e) override
    {
        const double f = e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1;
        m_scale = qBound(0.05, m_scale * f, 32.0);

        const QPoint a = e->globalPosition().toPoint();
        const QSize ns = m_pix.size() * m_scale;
        QPoint np(a.x() - double(a.x() - x()) / width() * ns.width(),
                  a.y() - double(a.y() - y()) / height() * ns.height());
        setGeometry(QRect(np, ns));
    }
    void mouseDoubleClickEvent(QMouseEvent *) override { close(); }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape)
            close();
    }
private:
    QPixmap m_pix;
    double m_scale;
    QPoint m_dragOff;
    bool m_borderOn = true;
};

class LongCapture : public QObject
{
    Q_OBJECT
public:
    explicit LongCapture(const QRect &globalRegion) : m_region(globalRegion) {}
    void start()
    {
        buildUi();
        captureFrame();
    }
signals:
    void finished(const QPixmap &pix);
private:

    bool matchFullRes(const QImage &f, int &bestK) const
    {
        const int H = f.height(), W = f.width();
        bestK = 0;
        const int canvasRows = m_rowEnd - m_rowStart;
        if (canvasRows < H / 3 || W != m_width)
            return false;
        const int bufTail = m_rowEnd - m_bufStart;
        const int maxOff = H * 2 / 3;
        const int xs = qMax(1, W / 24);
        double bestScore = 1e18;
        for (int cand = -maxOff; cand <= maxOff; ++cand) {
            if (cand == 0)
                continue;
            const int yStart = qMax(0, -cand);
            const int yEnd = qMin(H, H - cand);
            if (yEnd - yStart < H / 3)
                continue;
            double acc = 0;
            int cnt = 0;
            for (int y = yStart; y < yEnd; y += 2) {
                const QRgb *rf = reinterpret_cast<const QRgb *>(f.constScanLine(y));
                const QRgb *rc = reinterpret_cast<const QRgb *>(m_buf.constScanLine(bufTail - H + y + cand));
                for (int x = 4; x < W - 4; x += xs) {
                    acc += qAbs(qRed(rf[x]) - qRed(rc[x]))
                         + qAbs(qGreen(rf[x]) - qGreen(rc[x]))
                         + qAbs(qBlue(rf[x]) - qBlue(rc[x]));
                    ++cnt;
                }
            }
            const double s = cnt ? acc / (cnt * 765.0) : 1e18;
            if (s < bestScore) { bestScore = s; bestK = cand; }
        }
        return bestScore <= 0.03;
    }

    void nextTick()
    {
        if (!m_done)
            QTimer::singleShot(150, this, &LongCapture::captureFrame);
    }
    void captureFrame()
    {
        if (m_done)
            return;
        if (m_rowEnd - m_rowStart >= 65000) { finish(); return; }
        setUiVisible(false);
        QApplication::processEvents();
        const QImage f = grabScreen(m_region).toImage().convertToFormat(QImage::Format_RGB32);
        setUiVisible(true);
        if (f.isNull()) { nextTick(); return; }
        if (m_rowStart == m_rowEnd) {
            initTiles(f.width());
            appendRows(f, 0, f.height());
            updateUi();
            nextTick();
            return;
        }
        int k = 0;
        if (matchFullRes(f, k)) {
            if (k >= 2)
                appendRows(f, f.height() - k, k);
            else if (k <= -2) {
                prependRows(f, 0, -k);
            }
            if (k != 0)
                updateUi();
        }

        nextTick();
    }

    void initTiles(int width)
    {
        m_width = width;
        m_bytesPerLine = width * 4;
        m_tileMaxRows = qMax(64, (64 << 20) / m_bytesPerLine);
    }
    void appendRows(const QImage &f, int from, int count)
    {
        const int used = m_rowEnd - m_bufStart;
        if (m_buf.isNull() || used + count > m_buf.height()) {
            const int oldH = m_buf.isNull() ? 0 : m_buf.height();
            QImage nb(m_width, oldH + qMax(m_tileMaxRows, 4096), QImage::Format_RGB32);
            if (oldH) {
                QPainter p(&nb);
                p.drawImage(0, m_headPad, m_buf, 0, m_headPad,
                            m_width, oldH - m_headPad - m_tailPad);
                p.end();
            }
            m_buf = nb;
        }
        const int dstY = m_rowEnd - m_bufStart;
        for (int i = 0; i < count; ++i)
            memcpy(m_buf.scanLine(dstY + i), f.constScanLine(from + i), m_bytesPerLine);
        m_rowEnd += count;
        m_tailPad = m_buf.height() - (m_rowEnd - m_bufStart);
    }
    void prependRows(const QImage &f, int from, int count)
    {
        if (m_headPad < count) {
            const int extra = qMax(m_tileMaxRows, 4096);
            QImage nb(m_width, m_buf.height() + extra, QImage::Format_RGB32);
            QPainter p(&nb);
            p.drawImage(0, m_headPad + extra, m_buf, 0, m_headPad,
                        m_width, m_buf.height() - m_headPad - m_tailPad);
            p.end();
            m_buf = nb;
            m_bufStart -= extra;
            m_headPad += extra;
        }
        const int dstY = m_rowStart - m_bufStart - count;
        for (int i = 0; i < count; ++i)
            memcpy(m_buf.scanLine(dstY + i), f.constScanLine(from + i), m_bytesPerLine);
        m_rowStart -= count;
        m_headPad = m_rowStart - m_bufStart;
    }
    QImage toImage() const
    {
        if (m_buf.isNull() || m_rowEnd <= m_rowStart)
            return QImage();
        return m_buf.copy(0, m_rowStart - m_bufStart, m_width, m_rowEnd - m_rowStart);
    }

    void buildUi()
    {
        const QString barQss =
            "QWidget{background:#2d2d2d;border-radius:4px;}"
            "QLabel{color:#fff;font-size:13px;padding:0 8px;}"
            "QPushButton{color:#fff;background:transparent;border:none;padding:4px 10px;font-size:13px;}"
            "QPushButton:hover{background:#454545;border-radius:3px;}";
        m_bar = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        m_bar->setStyleSheet(barQss);
        auto *lay = new QHBoxLayout(m_bar);
        lay->setContentsMargins(6, 0, 4, 0);
        m_label = new QLabel;
        auto *save = new QPushButton(QStringLiteral("保存"));
        auto *copy = new QPushButton(QStringLiteral("复制"));
        auto *pin = new QPushButton(QStringLiteral("贴图"));
        auto *no = new QPushButton(QStringLiteral("取消"));
        lay->addWidget(m_label);
        lay->addWidget(save);
        lay->addWidget(copy);
        lay->addWidget(pin);
        lay->addWidget(no);
        connect(save, &QPushButton::clicked, this, &LongCapture::saveCapture);
        connect(copy, &QPushButton::clicked, this, &LongCapture::copyCapture);
        connect(pin, &QPushButton::clicked, this, &LongCapture::finish);
        connect(no, &QPushButton::clicked, this, &LongCapture::cancel);
        updateLabel();

        const QScreen *scr = QGuiApplication::screenAt(m_region.center());
        const QRect av = (scr ? scr : QGuiApplication::primaryScreen())->availableGeometry();
        m_bar->adjustSize();
        int bx = qBound(av.left(), m_region.center().x() - m_bar->width() / 2,
                        av.right() - m_bar->width());
        int by = m_region.bottom() + 8;
        if (by + m_bar->height() > av.bottom())
            by = m_region.top() - m_bar->height() - 8;
        m_bar->move(bx, qBound(av.top(), by, av.bottom() - m_bar->height()));
        m_bar->show();

        m_preview = new QLabel(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        m_preview->setStyleSheet("background:#1d1d1d;");
        m_preview->setAlignment(Qt::AlignCenter);
        const int ph = int(av.height() * 0.6);
        const int pw = 200;
        m_preview->resize(pw, ph);
        int px = m_region.right() + 8;
        if (px + pw > av.right())
            px = m_region.left() - 8 - pw;
        m_preview->move(qBound(av.left(), px, av.right() - pw),
                        qBound(av.top(), av.center().y() - ph / 2, av.bottom() - ph));
        m_preview->show();
        QApplication::processEvents();

        m_affinityOk = SetWindowDisplayAffinity(reinterpret_cast<HWND>(m_preview->winId()),
                                                PINSHOT_WDA_EXCLUDEFROMCAPTURE);

        m_frame = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                        | Qt::WindowTransparentForInput);
        m_frame->setStyleSheet("background:#1e90ff;");
        m_frame->setGeometry(m_region.adjusted(-2, -2, 2, 2));
        const QRect fr = m_frame->rect();
        m_frame->setMask(QRegion(fr) - QRegion(fr.adjusted(2, 2, -2, -2)));
        m_frame->show();
    }
    void setUiVisible(bool v)
    {
        if (!m_affinityOk) {
            m_bar->setVisible(v);
            m_preview->setVisible(v);
            QApplication::processEvents();
        }
    }
    void updateLabel()
    {
        if (m_label)
            m_label->setText(QStringLiteral("● 长截图 · %1 px").arg(m_rowEnd - m_rowStart));
        if (m_bar)
            m_bar->adjustSize();
    }
    void updateUi()
    {
        updateLabel();
        if (m_preview && m_rowEnd > m_rowStart) {
            const int rows = m_rowEnd - m_rowStart;
            const double s = qMin(double(m_preview->width()) / m_width,
                                  double(m_preview->height()) / rows);
            const QImage scaled = m_buf.copy(0, m_rowStart - m_bufStart, m_width, rows)
                                      .scaled(qMax(1, qRound(m_width * s)), qMax(1, qRound(rows * s)),
                                              Qt::KeepAspectRatio, Qt::FastTransformation);
            m_preview->setPixmap(QPixmap::fromImage(scaled));
        }
    }
    void finish()
    {
        if (m_done)
            return;
        m_done = true;
        cleanupUi();
        const QImage img = toImage();
        if (!img.isNull())
            emit finished(QPixmap::fromImage(img));
        deleteLater();
    }
    void cancel()
    {
        if (m_done)
            return;
        m_done = true;
        cleanupUi();
        deleteLater();
    }

    void saveCapture()
    {
        const QImage img = toImage();
        if (img.isNull()) { cancel(); return; }
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        const QString path = QFileDialog::getSaveFileName(
            nullptr, QStringLiteral("保存长截图"), dir + QStringLiteral("/longshot.png"),
            QStringLiteral("PNG (*.png)"));
        if (path.isEmpty())
            return;
        img.save(path);
        cancel();
    }
    void copyCapture()
    {
        const QImage img = toImage();
        if (!img.isNull())
            QGuiApplication::clipboard()->setImage(img);
        cancel();
    }
    void cleanupUi()
    {
        if (m_bar) { m_bar->hide(); m_bar->deleteLater(); m_bar = nullptr; }
        if (m_preview) { m_preview->hide(); m_preview->deleteLater(); m_preview = nullptr; }
        if (m_frame) { m_frame->hide(); m_frame->deleteLater(); m_frame = nullptr; }
    }
private:
    QRect m_region;
    QImage m_buf;
    int m_width = 0, m_bytesPerLine = 0, m_tileMaxRows = 0;
    int m_bufStart = 0;
    int m_rowStart = 0, m_rowEnd = 0;
    int m_headPad = 0, m_tailPad = 0;
    QWidget *m_bar = nullptr;
    QLabel *m_label = nullptr;
    QLabel *m_preview = nullptr;
    QWidget *m_frame = nullptr;
    bool m_affinityOk = false;
    bool m_done = false;
};

class CaptureOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit CaptureOverlay(const QPixmap &frozen)
        : m_shot(frozen)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_DeleteOnClose);
        setCursor(Qt::CrossCursor);
        setGeometry(QGuiApplication::primaryScreen()->virtualGeometry());

        m_sizeTip = new QLabel(this);
        m_sizeTip->setStyleSheet(
            "background:#2d2d2d;color:#fff;padding:2px 6px;border-radius:3px;font-size:12px;");
        m_sizeTip->hide();

        m_tools = new QWidget(this);
        m_tools->setStyleSheet(
            "QWidget{background:#2d2d2d;border-radius:4px;}"
            "QPushButton{color:#fff;background:transparent;border:none;padding:4px 12px;font-size:13px;}"
            "QPushButton:hover{background:#454545;border-radius:3px;}");
        auto *lay = new QHBoxLayout(m_tools);
        lay->setContentsMargins(4, 0, 4, 0);
        lay->setSpacing(2);
        auto addBtn = [&lay](const QString &text, const char *tip) {
            auto *b = new QPushButton(text);
            b->setToolTip(QString::fromUtf8(tip));
            lay->addWidget(b);
            return b;
        };
        connect(addBtn(QStringLiteral("保存"), "保存到文件 (Ctrl+S)"), &QPushButton::clicked,
                this, &CaptureOverlay::saveCapture);
        connect(addBtn(QStringLiteral("复制"), "复制到剪贴板 (Ctrl+C)"), &QPushButton::clicked,
                this, &CaptureOverlay::copyCapture);
        connect(addBtn(QStringLiteral("贴图"), "贴在屏幕上 (Enter / 双击)"), &QPushButton::clicked,
                this, &CaptureOverlay::pinCapture);
        connect(addBtn(QStringLiteral("长截图"), "自动滚动拼接长图"), &QPushButton::clicked,
                this, &CaptureOverlay::longCapture);
        m_tools->adjustSize();
        m_tools->hide();

        show();
        activateWindow();
    }
signals:
    void grabbed(const QPixmap &pix);
    void longRequested(const QRect &globalRegion);
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.drawPixmap(0, 0, m_shot);
        if (m_sel.isNull()) {
            p.fillRect(rect(), QColor(0, 0, 0, 60));
            return;
        }
        QPainterPath hole;
        hole.addRect(rect());
        hole.addRect(m_sel);
        p.fillPath(hole, QColor(0, 0, 0, 120));
        p.setPen(QPen(QColor("#1e90ff"), 1));
        p.drawRect(m_sel);
        if (m_mode == Adjusting)
            for (const QRect &h : handleRects(m_sel)) {
                p.fillRect(h, Qt::white);
                p.drawRect(h);
            }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::RightButton) {
            if (m_mode == Adjusting) { resetSelection(); } else { close(); }
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        m_pressPos = e->pos();
        if (m_mode == Adjusting) {
            m_drag = hitHandle(e->pos());
            m_pressSel = m_sel;
            if (m_drag == H_None) {
                resetSelection();
                m_drag = H_New;
            }
        } else {
            m_drag = H_New;
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const QPoint pos = e->pos();
        if (m_drag == H_None) {
            if (m_mode == Adjusting) {
                setCursor(cursorForHandle(hitHandle(pos)));
                const QPoint tl = m_sel.topLeft() - QPoint(0, 20);
                m_sizeTip->move(tl);
            }
            return;
        }
        if (m_drag == H_New) {
            if (m_mode == Selecting)
                m_mode = Adjusting;
            m_sel = QRect(m_pressPos, pos).normalized();
        } else {
            m_sel = resizeBy(m_drag, pos);
        }
        updateTip();
        update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        Q_UNUSED(e)
        if (m_drag == H_None)
            return;
        m_drag = H_None;
        m_sel = m_sel.normalized();
        if (m_sel.width() < 4 || m_sel.height() < 4) {
            resetSelection();
            return;
        }
        m_mode = Adjusting;
        setCursor(Qt::SizeAllCursor);
        placeToolbar();
        m_tools->show();
        updateTip();
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (m_mode == Adjusting && m_sel.contains(e->pos()))
            pinCapture();
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        switch (e->key()) {
        case Qt::Key_Escape:
            if (m_mode == Adjusting) resetSelection(); else close();
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (m_mode == Adjusting) pinCapture();
            break;
        case Qt::Key_C:
            if (e->modifiers() & Qt::ControlModifier) copyCapture();
            break;
        case Qt::Key_S:
            if (e->modifiers() & Qt::ControlModifier) saveCapture();
            break;
        default:
            break;
        }
    }
private:
    enum Handle { H_None = 0, H_New, H_Move, H_TL, H_TR, H_BL, H_BR, H_L, H_R, H_T, H_B };

    void resetSelection()
    {
        m_mode = Selecting;
        m_sel = QRect();
        m_drag = H_None;
        m_tools->hide();
        m_sizeTip->hide();
        setCursor(Qt::CrossCursor);
        update();
    }
    static QVector<QRect> handleRects(const QRect &r)
    {
        const int s = 8;
        const QPoint c = r.center();
        return {
            QRect(r.topLeft() - QPoint(s/2, s/2), QSize(s, s)),
            QRect(r.topRight() - QPoint(s/2 - 1, s/2), QSize(s, s)),
            QRect(r.bottomLeft() - QPoint(s/2, s/2 - 1), QSize(s, s)),
            QRect(r.bottomRight() - QPoint(s/2 - 1, s/2 - 1), QSize(s, s)),
            QRect(r.left() - s/2, c.y() - s/2, s, s),
            QRect(r.right() - s/2 + 1, c.y() - s/2, s, s),
            QRect(c.x() - s/2, r.top() - s/2, s, s),
            QRect(c.x() - s/2, r.bottom() - s/2 + 1, s, s),
        };
    }
    Handle hitHandle(const QPoint &p) const
    {
        if (m_mode != Adjusting)
            return H_None;
        const QVector<QRect> hs = handleRects(m_sel);
        for (int i = 0; i < hs.size(); ++i)
            if (hs[i].adjusted(-2, -2, 2, 2).contains(p))
                return Handle(H_TL + i);
        if (m_sel.contains(p))
            return H_Move;
        return H_None;
    }
    static Qt::CursorShape cursorForHandle(Handle h)
    {
        switch (h) {
        case H_TL: case H_BR: return Qt::SizeFDiagCursor;
        case H_TR: case H_BL: return Qt::SizeBDiagCursor;
        case H_L: case H_R:   return Qt::SizeHorCursor;
        case H_T: case H_B:   return Qt::SizeVerCursor;
        case H_Move:          return Qt::SizeAllCursor;
        default:              return Qt::CrossCursor;
        }
    }
    QRect resizeBy(Handle h, const QPoint &pos)
    {
        QRect r = m_pressSel;
        switch (h) {
        case H_TL:  r.setTopLeft(pos); break;
        case H_TR:  r.setTopRight(pos); break;
        case H_BL:  r.setBottomLeft(pos); break;
        case H_BR:  r.setBottomRight(pos); break;
        case H_L:   r.setLeft(pos.x()); break;
        case H_R:   r.setRight(pos.x()); break;
        case H_T:   r.setTop(pos.y()); break;
        case H_B:   r.setBottom(pos.y()); break;
        case H_Move: {
            const QPoint d = pos - m_pressPos;
            r.translate(d);
            r.moveTo(qBound(0, r.left(), width() - r.width()),
                     qBound(0, r.top(), height() - r.height()));
            return r;
        }
        default: break;
        }
        return r.normalized();
    }
    void updateTip()
    {
        if (m_sel.isNull()) { m_sizeTip->hide(); return; }
        m_sizeTip->setText(QString("%1 × %2").arg(m_sel.width()).arg(m_sel.height()));
        m_sizeTip->adjustSize();
        m_sizeTip->move(m_sel.topLeft() - QPoint(0, m_sizeTip->height() + 4));
        m_sizeTip->show();
    }
    void placeToolbar()
    {
        m_tools->adjustSize();
        const int vw = width(), vh = height();
        int x = m_sel.center().x() - m_tools->width() / 2;
        x = qBound(0, x, vw - m_tools->width());
        int y = m_sel.bottom() + 8;
        if (y + m_tools->height() > vh)
            y = m_sel.top() - m_tools->height() - 8;
        y = qBound(0, y, vh - m_tools->height());
        m_tools->move(x, y);
    }
    QPixmap cropped() const { return m_shot.copy(m_sel.normalized()); }
private slots:
    void saveCapture()
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存截图"),
            dir + QStringLiteral("/pinshot.png"),
            QStringLiteral("PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)"));
        if (path.isEmpty())
            return;
        cropped().save(path);
        close();
    }
    void copyCapture()
    {
        QGuiApplication::clipboard()->setPixmap(cropped());
        close();
    }
    void pinCapture()
    {
        const QPixmap pix = cropped();
        close();
        emit grabbed(pix);
    }
    void longCapture()
    {
        const QRect global = m_sel.translated(geometry().topLeft());
        close();
        emit longRequested(global);
    }
private:
    enum Mode { Selecting, Adjusting };
    QPixmap m_shot;
    QPoint m_pressPos;
    QRect m_sel, m_pressSel;
    Handle m_drag = H_None;
    Mode m_mode = Selecting;
    QLabel *m_sizeTip = nullptr;
    QWidget *m_tools = nullptr;
};

class HotkeyFilter : public QAbstractNativeEventFilter
{
public:
    std::function<void()> onHotkey;
    bool nativeEventFilter(const QByteArray &, void *message, long *) override
    {
        MSG *msg = static_cast<MSG *>(message);
        if (msg->message == WM_HOTKEY && msg->wParam == 1 && onHotkey)
            onHotkey();
        return false;
    }
};

class Controller : public QObject
{
    Q_OBJECT
public:
    Controller()
    {
        m_tray.setIcon(QApplication::windowIcon());
        m_tray.setToolTip(QStringLiteral("pinshot — F1 截图"));
        QMenu *m = new QMenu;
        m->addAction(QStringLiteral("截图 (F1)"), this, &Controller::startCapture);
        m->addSeparator();
        m->addAction(QStringLiteral("退出"), qApp, &QApplication::quit);
        m_tray.setContextMenu(m);
        m_tray.show();
    }
public slots:
    void startCapture()
    {
        const QPixmap frozen = grabScreen(QRect());
        auto *ov = new CaptureOverlay(frozen);
        connect(ov, &CaptureOverlay::grabbed, this, [](const QPixmap &pix) {
            new PinWindow(pix, QCursor::pos());
        });
        connect(ov, &CaptureOverlay::longRequested, this, [](const QRect &region) {
            auto *lc = new LongCapture(region);
            connect(lc, &LongCapture::finished, lc, [lc](const QPixmap &pix) {

                const QScreen *scr = QGuiApplication::screenAt(QCursor::pos());
                const QRect av = (scr ? scr : QGuiApplication::primaryScreen())->availableGeometry();
                new PinWindow(pix, av.center());
            });
            lc->start();
        });
    }
private:
    QSystemTrayIcon m_tray;
};

#include "main.moc"

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName(QStringLiteral("pinshot"));

    Controller ctrl;
    HotkeyFilter hf;
    hf.onHotkey = [&ctrl] { ctrl.startCapture(); };
    app.installNativeEventFilter(&hf);
    if (!RegisterHotKey(nullptr, 1, MOD_NOREPEAT, VK_F1))
        QMessageBox::warning(nullptr, QStringLiteral("pinshot"),
                             QStringLiteral("注册 F1 热键失败（可能被其他截图工具占用）"));

    return app.exec();
}
