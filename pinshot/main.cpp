// pinshot — screenshot capture, pinned images, and manual scrolling capture (Qt 5.15 / MinGW).
// shortcut: No click-to-detect windows; add window detection if click selection is required.
// shortcut: Screen captures run serially; use concurrent streamers if synchronized frames are required.

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
#include <QFontMetrics>
#include <QMimeData>
#include <QEventLoop>
#include <qt_windows.h>
#include <imm.h>
#include <cstdio>
#include <cstdlib>
#include <functional>

// MinGW 8.1 lacks this Windows 10 2004+ declaration; 0x11 = WDA_EXCLUDEFROMCAPTURE.
extern "C" __declspec(dllimport) int __stdcall SetWindowDisplayAffinity(void *hwnd, unsigned long dwAffinity);
#define PINSHOT_WDA_EXCLUDEFROMCAPTURE 0x00000011

// ---------- Screen capture ----------
static QPixmap grabScreen(const QRect &rect)
{
    const QRect whole = QGuiApplication::primaryScreen()->virtualGeometry();
    const QRect r = rect.isNull() || rect.isEmpty() ? whole : rect;
    QPixmap out;
    // 1) Single-screen region: crop using screen-local coordinates.
    for (QScreen *s : QGuiApplication::screens()) {
        if (s->geometry().contains(r)) {
            out = s->grabWindow(0).copy(QRect(r.topLeft() - s->geometry().topLeft(), r.size()));
            if (!out.isNull())
                break;
        }
    }
    // 2) Cross-screen region: assemble individual screen captures.
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

// ---------- Pinned image window ----------
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
        if (const QScreen *scr = QGuiApplication::screenAt(at)) {  // Fit both dimensions on the target screen.
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
        if (m_borderOn) {                             // Highlight border, toggled through the context menu.
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
            QGuiApplication::clipboard()->setPixmap(m_pix);   // Preserve original resolution.
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
        if (e->button() == Qt::LeftButton)            // Reserve right-click for the menu.
            m_dragOff = e->globalPos() - pos();
    }
    void mouseMoveEvent(QMouseEvent *e) override  { move(e->globalPos() - m_dragOff); }
    void wheelEvent(QWheelEvent *e) override      // Zoom with mouse-wheel input.
    {
        const double f = e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1;
        m_scale = qBound(0.05, m_scale * f, 32.0);
        // Keep the cursor position anchored while zooming.
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

// ---------- Manual scrolling capture ----------
// shortcut: Textureless or identical repeated overlaps cannot be uniquely aligned; add scroll-position data if needed.
class LongCapture : public QObject
{
    Q_OBJECT
public:
    explicit LongCapture(const QRect &globalRegion) : m_region(globalRegion) {}
    void start()
    {
        buildUi();
        captureFrame();                             // Capture the initial frame immediately.
    }
    static int testStitching()
    {
        auto check = [](bool ok, const char *message) {
            if (!ok) {
                std::fprintf(stderr, "FAIL: %s\n", message);
                std::exit(1);
            }
        };
        const int W = 960, H = 360;
        for (const QColor background : {QColor("#0b1016"), QColor(Qt::white)}) {
            QImage page(W, 2400, QImage::Format_RGB32);
            page.fill(background);
            QPainter p(&page);
            p.setPen(background == QColor(Qt::white) ? Qt::black : Qt::white);
            for (int y = 0; y < page.height(); y += 65) {
                p.drawText(30, y + 22, QStringLiteral("test(mcp): align completion checks with provider readiness"));
                p.drawText(30, y + 45, QStringLiteral("author committed 2 hours ago"));
                p.drawText(810, y + 22, QStringLiteral("%1").arg(y * 7919, 6, 16, QLatin1Char('0')));
            }
            p.end();
            LongCapture lc{QRect()};
            const int initial = 360;
            int first = initial, last = initial + H;
            check(lc.addFrame(page.copy(0, initial, W, H)), "initial frame");
            // Force buffer growth to check append/prepend copies pixel by pixel.
            lc.m_buf = lc.toImage();
            lc.m_tileMaxRows = 64;
            lc.m_tailPad = 0;
            for (int pos : {360, 361, 361, 430, 550, 730, 610, 500, 320, 200,
                            50, 0, 150, 350, 570, 780, 980, 1180, 1300}) {
                check(lc.addFrame(page.copy(0, pos, W, H)), "scroll frame must align");
                check(lc.m_frameRow == pos - initial, "frame must retain its absolute position");
                first = qMin(first, pos);
                last = qMax(last, pos + H);
                check(lc.toImage() == page.copy(0, first, W, last - first),
                      "stationary/down/up/revisit must preserve content and order");
            }
            const QImage before = lc.toImage();
            check(!lc.addFrame(page.copy(0, 2000, W, H)), "non-overlapping jump must be rejected");
            check(lc.toImage() == before, "rejected jump must not corrupt output");
            check(lc.addFrame(page.copy(0, 1400, W, H)), "return to overlap must recover");
            check(lc.toImage() == page.copy(0, first, W, 1400 + H - first), "recovery must not leave a gap");
            check(!lc.addFrame(QImage()), "empty frame must be rejected");
            check(!lc.addFrame(page.copy(0, 1400, W - 1, H)), "resized frame must be rejected");
            check(!lc.addFrame(page.copy(0, 1400, W, H - 1)), "height change must be rejected");
            QImage changed = page.copy(0, 1400, W, H);
            changed.fill(Qt::red);
            check(!lc.addFrame(changed), "unrelated content must be rejected");
            check(lc.addFrame(page.copy(0, 1500, W, H)), "bad frames must not replace the alignment reference");
            check(lc.toImage() == page.copy(0, first, W, 1500 + H - first), "bad frames must not remove content");
        }
        QImage repeated(W, H * 2, QImage::Format_RGB32);
        repeated.fill(Qt::white);
        QPainter p(&repeated);
        p.setPen(Qt::black);
        for (int y = 0; y < repeated.height(); y += 40)
            p.drawText(30, y + 22, QStringLiteral("identical rows without a unique identifier"));
        p.end();
        LongCapture ambiguous{QRect()};
        check(ambiguous.addFrame(repeated.copy(0, 0, W, H)), "repeated initial frame");
        const QImage before = ambiguous.toImage();
        check(!ambiguous.addFrame(repeated.copy(0, 17, W, H)), "ambiguous repeated content must be rejected");
        check(ambiguous.toImage() == before, "ambiguous frame must not grow the output");
        QImage blank(W, H, QImage::Format_RGB32);
        blank.fill(Qt::white);
        LongCapture emptyPage{QRect()};
        check(emptyPage.addFrame(blank) && emptyPage.addFrame(blank), "stationary blank frame");
        check(emptyPage.toImage() == blank, "blank frames must not invent content");
        std::puts("PASS: long-capture stitching regression checks");
        return 0;
    }
signals:
    void finished(const QPixmap &pix);
private:
    // f[y] == lastFrame[y+k]; include zero offset so stationary/repeated lists do not force a splice.
    bool matchFullRes(const QImage &f, int &bestK) const
    {
        bestK = 0;
        if (f.size() != m_lastFrame.size())
            return false;
        if (f == m_lastFrame)
            return true;
        const int H = f.height(), maxOff = H * 2 / 3;
        auto distance = [](QRgb a, QRgb b) {
            return qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b))
                 + qAbs(qBlue(a) - qBlue(b));
        };
        struct Sample { int x, y; QRgb color; };
        auto samples = [&distance](const QImage &image) {
            QVector<Sample> points;
            const int step = qMax(1, image.width() / 192);
            for (int y = 0; y < image.height(); y += 2) {
                const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
                for (int x = 1; x < image.width() - 1; x += step)
                    if (distance(row[x], row[x - 1]) >= 36)
                        points.append({x, y, row[x]});
            }
            return points;
        };
        // Only text/shape edges vote; large uniform backgrounds are not alignment evidence.
        const auto previous = samples(m_lastFrame), current = samples(f);
        QVector<double> scores(2 * maxOff + 1, 1e18);
        double bestScore = 1e18;
        for (int cand = -maxOff; cand <= maxOff; ++cand) {
            double acc = 0;
            int cnt = 0;
            for (const Sample &point : previous) {
                const int y = point.y - cand;
                if (y >= 0 && y < H) {
                    const QRgb *row = reinterpret_cast<const QRgb *>(f.constScanLine(y));
                    acc += distance(point.color, row[point.x]);
                    ++cnt;
                }
            }
            for (const Sample &point : current) {
                const int y = point.y + cand;
                if (y >= 0 && y < H) {
                    const QRgb *row = reinterpret_cast<const QRgb *>(m_lastFrame.constScanLine(y));
                    acc += distance(point.color, row[point.x]);
                    ++cnt;
                }
            }
            if (cnt < 32)
                continue;
            const double score = acc / (cnt * 765.0);
            scores[cand + maxOff] = score;
            if (score < bestScore || (score == bestScore && qAbs(cand) < qAbs(bestK))) {
                bestScore = score;
                bestK = cand;
            }
        }
        if (bestScore > 0.03)
            return false;
        if (bestK != 0)
            for (int cand = -maxOff; cand <= maxOff; ++cand)
                if (qAbs(cand - bestK) > 2 && scores[cand + maxOff] <= bestScore + 0.01)
                    return false;                    // Multiple plausible alignments: do not guess the position.
        return true;
    }
    // ---- Capture loop: the user scrolls manually; polling captures and stitches frames. ----
    void nextTick()
    {
        if (!m_done)
            QTimer::singleShot(150, this, &LongCapture::captureFrame);   // Limit gaps between scrolling frames.
    }
    void captureFrame()
    {
        if (m_done)
            return;
        if (m_rowEnd - m_rowStart >= 65000) { finish(); return; }   // Session height cap.
        setUiVisible(false);
        QApplication::processEvents();
        const QImage f = grabScreen(m_region).toImage().convertToFormat(QImage::Format_RGB32);
        setUiVisible(true);
        const bool wasLost = m_alignmentLost;
        const int oldRows = m_rowEnd - m_rowStart;
        m_alignmentLost = !addFrame(f);
        if (oldRows != m_rowEnd - m_rowStart)
            updateUi();
        else if (wasLost != m_alignmentLost)
            updateLabel();
        nextTick();
    }
    bool addFrame(const QImage &f)
    {
        if (f.isNull() || f.format() != QImage::Format_RGB32)
            return false;
        if (m_rowStart == m_rowEnd) {                 // First frame.
            initTiles(f.width());
            appendRows(f, 0, f.height());
            m_lastFrame = f;
            return true;
        }
        int k = 0;
        if (!matchFullRes(f, k))
            return false;
        const int row = m_frameRow + k;
        if (row < m_rowStart)
            prependRows(f, 0, m_rowStart - row);
        if (row + f.height() > m_rowEnd) {
            const int from = m_rowEnd - row;
            appendRows(f, from, f.height() - from);
        }
        m_frameRow = row;
        m_lastFrame = f;
        return true;
    }
    // ---- Growable row buffer with contiguous append/prepend. ----
    void initTiles(int width)
    {
        m_width = width;
        m_bytesPerLine = width * 4;
        m_tileMaxRows = qMax(64, (64 << 20) / m_bytesPerLine);  // Allocate up to 64 MB of rows per growth step.
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
        if (m_headPad < count) {                      // Not enough headroom: grow and shift existing rows.
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
    // ---- UI: toolbar and live preview on the selection's screen. ----
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
        // Keep the toolbar below the selection and the preview alongside it on the same screen.
        const QScreen *scr = QGuiApplication::screenAt(m_region.center());
        const QRect av = (scr ? scr : QGuiApplication::primaryScreen())->availableGeometry();
        m_bar->adjustSize();
        int bx = qBound(av.left(), m_region.center().x() - m_bar->width() / 2,
                        av.right() - m_bar->width());
        int by = m_region.bottom() + 8;
        if (by + m_bar->height() > av.bottom())      // No room below: place above the selection.
            by = m_region.top() - m_bar->height() - 8;
        m_bar->move(bx, qBound(av.top(), by, av.bottom() - m_bar->height()));
        m_bar->show();

        m_preview = new QLabel(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        m_preview->setStyleSheet("background:#1d1d1d;");
        m_preview->setAlignment(Qt::AlignCenter);
        const int ph = int(av.height() * 0.6);
        const int pw = 200;
        m_preview->resize(pw, ph);
        int px = m_region.right() + 8;                // Place the preview alongside the selection.
        if (px + pw > av.right())                     // No room on the right: use the left side.
            px = m_region.left() - 8 - pw;
        m_preview->move(qBound(av.left(), px, av.right() - pw),
                        qBound(av.top(), av.center().y() - ph / 2, av.bottom() - ph));
        m_preview->show();
        QApplication::processEvents();
        // Test capture exclusion; hide the preview during capture if unsupported.
        m_affinityOk = SetWindowDisplayAffinity(reinterpret_cast<HWND>(m_preview->winId()),
                                                PINSHOT_WDA_EXCLUDEFROMCAPTURE);

        // Input-transparent 2 px blue border outside the selection; scrolling passes through.
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
        if (!m_affinityOk) {                          // Older systems: hide UI while capturing.
            m_bar->setVisible(v);
            m_preview->setVisible(v);
            QApplication::processEvents();
        }
    }
    void updateLabel()
    {
        if (m_label)
            m_label->setText(m_alignmentLost ? QStringLiteral("无法对齐，请缓慢滚回上一位置")
                                            : QStringLiteral("● 长截图 · %1 px").arg(m_rowEnd - m_rowStart));
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
    // Save/copy ends the session without creating a pinned image.
    void saveCapture()
    {
        const QImage img = toImage();
        if (img.isNull()) { cancel(); return; }
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        const QString path = QFileDialog::getSaveFileName(
            nullptr, QStringLiteral("保存长截图"), dir + QStringLiteral("/longshot.png"),
            QStringLiteral("PNG (*.png)"));            // Save long captures as PNG.
        if (path.isEmpty())
            return;                                    // Cancelled dialog: keep the session active.
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
    QImage m_lastFrame;                              // Last aligned raw frame; rejected frames never replace it.
    int m_frameRow = 0;                              // Absolute logical row of that frame's top edge.
    QImage m_buf;                                    // Growable buffer for captured rows.
    int m_width = 0, m_bytesPerLine = 0, m_tileMaxRows = 0;
    int m_bufStart = 0;                              // Logical row corresponding to buffer row zero.
    int m_rowStart = 0, m_rowEnd = 0;                 // Logical coverage [start,end).
    int m_headPad = 0, m_tailPad = 0;
    QWidget *m_bar = nullptr;
    QLabel *m_label = nullptr;
    QLabel *m_preview = nullptr;
    QWidget *m_frame = nullptr;
    bool m_affinityOk = false;
    bool m_alignmentLost = false;
    bool m_done = false;
};

// ---------- Frozen desktop pixels and cursor inspection ----------
struct ScreenFrame
{
    QRect logicalRect, pixelRect;
    QImage image;
};

static QVector<ScreenFrame> freezeScreens()
{
    QVector<ScreenFrame> frames;
    for (QScreen *screen : QGuiApplication::screens()) {
        QImage image = screen->grabWindow(0).toImage();
        if (image.isNull())
            return {};
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        const QString name = screen->name();
        if (!EnumDisplaySettingsW(reinterpret_cast<const wchar_t *>(name.utf16()),
                                  ENUM_CURRENT_SETTINGS, &mode))
            return {};                              // Do not guess native coordinates after a display-query failure.
        if (image.size() != QSize(mode.dmPelsWidth, mode.dmPelsHeight))
            return {};                              // Reject a display-mode change during capture.
        image.setDevicePixelRatio(1);
        frames.append({screen->geometry(),
                       QRect(mode.dmPosition.x, mode.dmPosition.y, mode.dmPelsWidth, mode.dmPelsHeight),
                       image});
    }
    return frames;
}

class PixelInspector : public QWidget
{
public:
    explicit PixelInspector(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        QFont f(QStringLiteral("Microsoft YaHei"));
        f.setPixelSize(13);
        setFont(f);
        const QFontMetrics fm(f);
        setFixedSize(qMax(192, fm.horizontalAdvance(QStringLiteral("按 Shift 切换 RGB/HEX")) + 16),
                     138 + 4 * fm.height());
        hide();
    }
    bool setSample(const ScreenFrame &frame, const QPoint &globalPixel)
    {
        if (frame.image.isNull() || !frame.pixelRect.contains(globalPixel)
            || frame.image.size() != frame.pixelRect.size())
            return false;
        const QPoint pixel = globalPixel - frame.pixelRect.topLeft();
        m_color = frame.image.pixelColor(pixel);
        m_position = globalPixel;
        m_patch = frame.image.copy(QRect(pixel - QPoint(11, 7), QSize(23, 15)));
        update();
        return true;
    }
    QString colorText() const
    {
        if (!m_color.isValid())
            return QString();
        return m_hex ? m_color.name(QColor::HexRgb).toUpper()
                     : QStringLiteral("%1, %2, %3").arg(m_color.red()).arg(m_color.green()).arg(m_color.blue());
    }
    void toggleFormat() { m_hex = !m_hex; update(); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        const QRect view((width() - 176) / 2, 8, 176, 120);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.save();
        p.setClipRect(view);
        p.drawImage(QRect(view.left() - 4, view.top(), 184, 120), m_patch);
        const QRect pixel(view.left() + 84, view.top() + 56, 8, 8);
        p.fillRect(QRect(pixel.left(), view.top(), 8, 120), QColor(30, 144, 255, 75));
        p.fillRect(QRect(view.left(), pixel.top(), 176, 8), QColor(30, 144, 255, 75));
        p.fillRect(pixel, m_color);
        p.setPen(Qt::black);
        p.drawRect(pixel.adjusted(-1, -1, 0, 0));
        p.setPen(Qt::white);
        p.drawRect(pixel.adjusted(0, 0, -1, -1));
        p.restore();
        p.setPen(Qt::white);
        p.drawRect(view.adjusted(0, 0, -1, -1));
        const int line = fontMetrics().height(), y = view.bottom() + 5;
        p.drawText(QRect(0, y, width(), line), Qt::AlignCenter,
                   QStringLiteral("(%1, %2)").arg(m_position.x()).arg(m_position.y()));
        const QString text = colorText();
        const int left = (width() - fontMetrics().horizontalAdvance(text) - 24) / 2;
        const QRect swatch(left, y + line + (line - 12) / 2, 12, 12);
        p.fillRect(swatch, m_color);
        p.drawRect(swatch.adjusted(0, 0, -1, -1));
        p.drawText(QRect(left + 24, y + line, width() - left - 24, line), Qt::AlignVCenter, text);
        p.drawText(QRect(0, y + 2 * line, width(), line), Qt::AlignCenter, QStringLiteral("按 C 复制颜色值"));
        p.drawText(QRect(0, y + 3 * line, width(), line), Qt::AlignCenter, QStringLiteral("按 Shift 切换 RGB/HEX"));
    }
private:
    QImage m_patch;
    QColor m_color;
    QPoint m_position;
    bool m_hex = false;
};

// ---------- Fullscreen selection: frozen frame, adjustable region, and action toolbar. ----------
class CaptureOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit CaptureOverlay(const QVector<ScreenFrame> &frames)
        : m_frames(frames)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_DeleteOnClose);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::CrossCursor);
        setGeometry(QGuiApplication::primaryScreen()->virtualGeometry());

        qreal scale = 1;
        for (const ScreenFrame &frame : m_frames)
            scale = qMax(scale, qreal(frame.image.width()) / frame.logicalRect.width());
        m_shot = QPixmap(qRound(width() * scale), qRound(height() * scale));
        m_shot.setDevicePixelRatio(scale);
        m_shot.fill(Qt::black);
        QPainter painter(&m_shot);
        for (const ScreenFrame &frame : m_frames)
            painter.drawImage(frame.logicalRect.translated(-geometry().topLeft()), frame.image);
        painter.end();
        m_inspector = new PixelInspector(this);

        m_sizeTip = new QLabel(this);
        m_sizeTip->setStyleSheet(
            "background:#2d2d2d;color:#fff;padding:2px 6px;border-radius:3px;font-size:12px;");
        m_sizeTip->hide();

        // Dark rounded toolbar: save / copy / pin.
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
        connect(addBtn(QStringLiteral("长截图"), "手动滚动拼接长图"), &QPushButton::clicked,
                this, &CaptureOverlay::longCapture);
        m_tools->adjustSize();
        m_tools->hide();

        show();
        // Claim foreground synchronously (hotkey handling grants activation rights)
        // and detach the IME so plain C/Shift always reach this window.
        HWND handle = reinterpret_cast<HWND>(winId());
        SetForegroundWindow(handle);
        SetFocus(handle);
        ImmAssociateContext(handle, nullptr);
        setFocus();
        updateInspector();
    }
    static int testColorPicker(const QString &previewPath)
    {
        QClipboard *clipboard = QGuiApplication::clipboard();
        auto *saved = new QMimeData;
        if (const QMimeData *data = clipboard->mimeData())
            for (const QString &format : data->formats()) saved->setData(format, data->data(format));
        auto check = [clipboard, saved](bool ok, const char *message) {
            if (!ok) {
                clipboard->setMimeData(saved);
                std::fprintf(stderr, "FAIL: %s\n", message);
                std::exit(1);
            }
        };
        QWidget calibration(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        calibration.setGeometry(QRect(QGuiApplication::primaryScreen()->geometry().center(), QSize(64, 64)));
        calibration.setStyleSheet("background:#010409;");
        calibration.show();
        QEventLoop settle;
        QTimer::singleShot(200, &settle, &QEventLoop::quit);
        settle.exec();
        const QVector<ScreenFrame> liveFrames = freezeScreens();
        POINT native{32, 32};
        check(ClientToScreen(reinterpret_cast<HWND>(calibration.winId()), &native), "native calibration coordinates");
        PixelInspector probe(nullptr);
        bool sampled = false;
        for (const ScreenFrame &live : liveFrames)
            if (probe.setSample(live, QPoint(native.x, native.y))) sampled = true;
        check(sampled && probe.colorText() == QStringLiteral("1, 4, 9"), "live desktop capture preserves native calibration pixels");
        calibration.hide();
        QImage image(240, 180, QImage::Format_RGB32);
        image.fill(QColor(1, 4, 9));
        image.setPixelColor(110, 83, QColor(90, 150, 210));
        ScreenFrame frame{QRect(QGuiApplication::primaryScreen()->geometry().topLeft(), QSize(160, 120)),
                          QRect(-320, -240, 240, 180), image};
        CaptureOverlay overlay({frame});
        overlay.setAttribute(Qt::WA_DeleteOnClose, false);
        PixelInspector *inspector = overlay.m_inspector;
        const QPoint sample = frame.pixelRect.topLeft() + QPoint(120, 90);
        check(inspector->setSample(frame, sample), "native pixel sample with negative screen origin");
        inspector->show();
        check(inspector->colorText() == QStringLiteral("1, 4, 9"), "RGB channel order and formatting");
        for (const QSize logical : {QSize(240, 180), QSize(160, 120), QSize(120, 90)}) {
            frame.logicalRect.setSize(logical);
            check(inspector->setSample(frame, sample) && inspector->colorText() == QStringLiteral("1, 4, 9"),
                  "100/150/200 percent scaling must not resample native colors");
        }
        QImage panel(inspector->size(), QImage::Format_RGB32);
        inspector->render(&panel);
        const int left = (inspector->width() - 176) / 2;
        check(panel.pixelColor(left + 88, 68) == QColor(1, 4, 9), "highlighted pixel retains its raw color");
        check(panel.pixelColor(left + 8, 12) == QColor(90, 150, 210), "nearest-neighbor magnifier preserves neighboring pixels");
        if (!previewPath.isEmpty()) check(panel.save(previewPath), "save magnifier preview");
        const QImage frozen = overlay.m_shot.toImage();
        check(overlay.m_shot.devicePixelRatio() == 1.5, "frozen desktop preserves high-DPI capture resolution");
        const QPoint origin = (frame.logicalRect.topLeft() - overlay.geometry().topLeft()) * 1.5;
        check(frozen.pixelColor(origin + QPoint(110, 83)) == QColor(90, 150, 210), "frozen capture retains native pixel detail");
        QKeyEvent copyRgb(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &copyRgb);
        check(clipboard->text() == QStringLiteral("1, 4, 9") && overlay.isVisible(), "C copies RGB without closing capture");
        QKeyEvent shift(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        QApplication::sendEvent(&overlay, &shift);
        check(inspector->colorText() == QStringLiteral("#010409"), "Shift switches to six-digit HEX");
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, QString(), true);
        QApplication::sendEvent(&overlay, &repeat);
        check(inspector->colorText() == QStringLiteral("#010409"), "holding Shift must not repeatedly toggle formats");
        QApplication::sendEvent(&overlay, &copyRgb);
        check(clipboard->text() == QStringLiteral("#010409"), "C copies the displayed HEX format");
        QApplication::sendEvent(&overlay, &shift);
        check(inspector->colorText() == QStringLiteral("1, 4, 9"), "Shift switches back to RGB");
        for (const QPoint pixel : {QPoint(0, 0), QPoint(239, 0), QPoint(0, 179), QPoint(239, 179)})
            check(inspector->setSample(frame, frame.pixelRect.topLeft() + pixel), "screen-edge samples remain valid");
        check(!inspector->setSample(frame, frame.pixelRect.topLeft() - QPoint(1, 0)), "outside-screen sample is rejected");
        check(!inspector->setSample(ScreenFrame{}, sample), "empty frame is rejected");
        ScreenFrame mismatch = frame;
        mismatch.pixelRect.setWidth(241);
        check(!inspector->setSample(mismatch, sample), "unexpected native size is rejected");
        const QRect bounds(-640, -100, 640, 480);
        for (const QPoint cursor : {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()}) {
            const QRect positioned(inspectorPosition(cursor, inspector->size(), bounds), inspector->size());
            check(bounds.contains(positioned) && !positioned.contains(cursor), "panel stays on screen and avoids the cursor");
        }
        check(overlay.m_shot.toImage() == frozen, "magnifier and format changes must not alter capture pixels");
        overlay.m_sel = QRect(2, 2, 30, 20);
        overlay.m_mode = Adjusting;
        const QImage cropped = overlay.cropped().toImage();
        check(cropped.size() == QSize(45, 30), "selection crop uses native capture coordinates");
        check(cropped.devicePixelRatio() == 1, "capture output uses raw pixels for pinning and clipboard consumers");
        QKeyEvent copyImage(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
        QApplication::sendEvent(&overlay, &copyImage);
        check(clipboard->image() == cropped && !overlay.isVisible(), "Ctrl+C still copies only the selection and closes capture");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        for (const Mode mode : {Selecting, Adjusting}) {
            overlay.m_mode = mode;
            overlay.show();
            inspector->show();
            overlay.m_tools->setVisible(mode == Adjusting);
            QApplication::sendEvent(&overlay, &escape);
            check(!overlay.isVisible() && !inspector->isVisible() && !overlay.m_tools->isVisible(),
                  "Escape exits capture before and after selecting a region");
        }
        clipboard->setMimeData(saved);
        std::puts("PASS: pixel sampling, magnifier, color shortcuts, and capture regression checks");
        return 0;
    }
signals:
    void grabbed(const QPixmap &pix);
    void longRequested(const QRect &globalRegion);
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.drawPixmap(0, 0, m_shot);                 // Frozen frame covers the virtual desktop.
        if (m_sel.isNull()) {
            p.fillRect(rect(), QColor(0, 0, 0, 60));
            return;
        }
        QPainterPath hole;                          // Dim outside the selection, leave the inside clear.
        hole.addRect(rect());
        hole.addRect(m_sel);
        p.fillPath(hole, QColor(0, 0, 0, 120));
        p.setPen(QPen(QColor("#1e90ff"), 1));
        p.drawRect(m_sel);
        if (m_mode == Adjusting)                    // Eight resize handles.
            for (const QRect &h : handleRects(m_sel)) {
                p.fillRect(h, Qt::white);
                p.drawRect(h);
            }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::RightButton) {       // Clear an existing selection; otherwise exit.
            if (m_mode == Adjusting) { resetSelection(); } else { close(); }
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        setFocus();                                   // Any click restores keyboard handling.
        m_pressPos = e->pos();
        if (m_mode == Adjusting) {
            m_drag = hitHandle(e->pos());
            m_pressSel = m_sel;
            if (m_drag == H_None) {                 // Clicking outside starts a new selection.
                resetSelection();
                m_drag = H_New;
            }
        } else {
            m_drag = H_New;
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        updateInspector();
        const QPoint pos = e->pos();
        if (m_drag == H_None) {                     // Hover cursor and size hint.
            if (m_mode == Adjusting) {
                setCursor(cursorForHandle(hitHandle(pos)));
                const QPoint tl = m_sel.topLeft() - QPoint(0, 20);
                m_sizeTip->move(tl);
            }
            return;
        }
        if (m_drag == H_New) {                      // New selection.
            if (m_mode == Selecting)
                m_mode = Adjusting;                 // Dragging enters adjustment mode.
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
            close();
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (m_mode == Adjusting) pinCapture();
            break;
        case Qt::Key_C:
            if (e->modifiers() & Qt::ControlModifier) {
                if (m_mode == Adjusting) copyCapture();
            } else if (!(e->modifiers() & (Qt::AltModifier | Qt::MetaModifier))
                       && m_inspector->isVisible() && !m_inspector->colorText().isEmpty()) {
                QGuiApplication::clipboard()->setText(m_inspector->colorText());
            }
            break;
        case Qt::Key_Shift:
            if (!e->isAutoRepeat()) m_inspector->toggleFormat();
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

    void updateInspector()
    {
        POINT cursor;
        if (GetCursorPos(&cursor)) {
            const QPoint pixel(cursor.x, cursor.y);
            for (const ScreenFrame &frame : m_frames) {
                if (!m_inspector->setSample(frame, pixel))
                    continue;
                const QPoint local = mapFromGlobal(QCursor::pos());
                const QRect bounds = frame.logicalRect.translated(-geometry().topLeft());
                m_inspector->move(inspectorPosition(local, m_inspector->size(), bounds));
                m_inspector->show();
                m_tools->raise();
                m_sizeTip->raise();
                return;
            }
        }
        m_inspector->hide();
    }
    static QPoint inspectorPosition(const QPoint &cursor, const QSize &size, const QRect &bounds)
    {
        int x = cursor.x() + 24, y = cursor.y() + 24;
        if (x + size.width() > bounds.right() + 1) x = cursor.x() - size.width() - 24;
        if (y + size.height() > bounds.bottom() + 1) y = cursor.y() - size.height() - 24;
        return QPoint(qBound(bounds.left(), x, qMax(bounds.left(), bounds.right() + 1 - size.width())),
                      qBound(bounds.top(), y, qMax(bounds.top(), bounds.bottom() + 1 - size.height())));
    }

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
        if (y + m_tools->height() > vh)             // No room below: place above the selection.
            y = m_sel.top() - m_tools->height() - 8;
        y = qBound(0, y, vh - m_tools->height());
        m_tools->move(x, y);
    }
    QPixmap cropped() const
    {
        const qreal scale = m_shot.devicePixelRatio();
        const QRect r = m_sel.normalized();
        QPixmap pix = m_shot.copy(QRect(qRound(r.x() * scale), qRound(r.y() * scale),
                                       qRound(r.width() * scale), qRound(r.height() * scale)));
        pix.setDevicePixelRatio(1);
        return pix;
    }
private slots:
    void saveCapture()
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存截图"),
            dir + QStringLiteral("/pinshot.png"),
            QStringLiteral("PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)"));
        if (path.isEmpty()) {
            activateWindow();                       // Cancelled dialog: reclaim focus and keep the selection.
            setFocus();
            return;
        }
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
        const QRect global = m_sel.translated(geometry().topLeft());  // Selection uses local coordinates.
        close();
        emit longRequested(global);
    }
private:
    enum Mode { Selecting, Adjusting };
    QPixmap m_shot;
    QVector<ScreenFrame> m_frames;
    PixelInspector *m_inspector = nullptr;
    QPoint m_pressPos;
    QRect m_sel, m_pressSel;
    Handle m_drag = H_None;
    Mode m_mode = Selecting;
    QLabel *m_sizeTip = nullptr;
    QWidget *m_tools = nullptr;
};

// ---------- F1 global hotkey filter ----------
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
        const QVector<ScreenFrame> frames = freezeScreens();   // Freeze before showing the selection overlay.
        if (frames.isEmpty()) {
            QMessageBox::warning(nullptr, QStringLiteral("pinshot"), QStringLiteral("无法读取屏幕像素"));
            return;
        }
        auto *ov = new CaptureOverlay(frames);
        connect(ov, &CaptureOverlay::grabbed, this, [](const QPixmap &pix) {
            new PinWindow(pix, QCursor::pos());
        });
        connect(ov, &CaptureOverlay::longRequested, this, [](const QRect &region) {
            auto *lc = new LongCapture(region);
            connect(lc, &LongCapture::finished, lc, [lc](const QPixmap &pix) {
                // Center the long pin on the screen containing the cursor (the clicked pin button).
                const QScreen *scr = QGuiApplication::screenAt(QCursor::pos());
                const QRect av = (scr ? scr : QGuiApplication::primaryScreen())->availableGeometry();
                new PinWindow(pix, av.center());     // PinWindow scales oversized images to fit.
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
    if (app.arguments().contains(QStringLiteral("--test-stitching")))
        return LongCapture::testStitching();
    if (app.arguments().contains(QStringLiteral("--test-color-picker")))
        return CaptureOverlay::testColorPicker(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-color-picker")) + 1));
    app.setQuitOnLastWindowClosed(false);            // Keep the tray active after all pins are closed.
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
