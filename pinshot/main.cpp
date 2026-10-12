// pinshot — screenshot capture, pinned images, and manual scrolling capture (Qt 5.15 / MinGW).
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
#include <QToolButton>
#include <QPainterPath>
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
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QElapsedTimer>
#include <QSettings>
#include <QInputDialog>
#include <QPointer>
#include <QCloseEvent>
#include <QFileInfo>
#include <qt_windows.h>
#include <dwmapi.h>
#include <imm.h>
#include <oleacc.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>

// MinGW 8.1 lacks this Windows 10 2004+ declaration; 0x11 = WDA_EXCLUDEFROMCAPTURE.
extern "C" __declspec(dllimport) int __stdcall SetWindowDisplayAffinity(void *hwnd, unsigned long dwAffinity);
// Qt's default Windows target hides this Vista+ declaration in the MinGW headers.
extern "C" __declspec(dllimport) BOOL WINAPI QueryFullProcessImageNameW(HANDLE process, DWORD flags, LPWSTR path, PDWORD size);
#define PINSHOT_WDA_EXCLUDEFROMCAPTURE 0x00000011
#ifndef CAPTUREBLT
#define CAPTUREBLT 0x40000000                      // Include layered windows in BitBlt.
#endif

// ---------- Screen capture ----------
// One desktop BitBlt with CAPTUREBLT also captures layered windows; falls back
// to the per-screen Qt path when the GDI call fails.
static QImage grabNative(const QRect &native)
{
    if (native.width() <= 0 || native.height() <= 0)
        return {};
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, native.width(), native.height());
    QImage img;
    if (bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        if (BitBlt(mem, 0, 0, native.width(), native.height(), screen,
                   native.x(), native.y(), SRCCOPY | CAPTUREBLT)) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = native.width();
            info.bmiHeader.biHeight = -native.height();  // Top-down rows.
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            img = QImage(native.width(), native.height(), QImage::Format_RGB32);
            if (!GetDIBits(mem, bmp, 0, native.height(), img.bits(), &info, DIB_RGB_COLORS))
                img = {};
        }
        if (old)
            SelectObject(mem, old);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

// Map a logical rect onto native pixels via the screen at its center; exact for
// uniform DPI, approximate across mixed-DPI boundaries (known limitation).
static QRect nativeFromLogical(const QRect &logical)
{
    const QScreen *s = QGuiApplication::screenAt(logical.center());
    if (!s)
        s = QGuiApplication::primaryScreen();
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(reinterpret_cast<const wchar_t *>(s->name().utf16()),
                              ENUM_CURRENT_SETTINGS, &mode))
        return {};
    const QRect lg(s->geometry());
    const QRect px(mode.dmPosition.x, mode.dmPosition.y, mode.dmPelsWidth, mode.dmPelsHeight);
    const double sx = double(px.width()) / lg.width(), sy = double(px.height()) / lg.height();
    return QRect(qRound(px.left() + (logical.left() - lg.left()) * sx),
                 qRound(px.top() + (logical.top() - lg.top()) * sy),
                 qRound(logical.width() * sx), qRound(logical.height() * sy));
}

static QPixmap grabScreen(const QRect &rect)
{
    const QRect whole = QGuiApplication::primaryScreen()->virtualGeometry();
    const QRect r = rect.isNull() || rect.isEmpty() ? whole : rect;
    QPixmap out = QPixmap::fromImage(grabNative(nativeFromLogical(r)));
    if (!out.isNull())
        return out;
    // Fallback: per-screen Qt captures (no layered windows, multi-shot).
    // 1) Single-screen region: crop using screen-local coordinates.
    for (QScreen *s : QGuiApplication::screens()) {
        if (s->geometry().contains(r)) {
            out = s->grabWindow(0).copy(QRect(r.topLeft() - s->geometry().topLeft(), r.size()));
            if (!out.isNull())
                return out;
        }
    }
    // 2) Cross-screen region: assemble individual screen captures.
    out = QPixmap(r.size());
    out.fill(Qt::black);
    QPainter p(&out);
    for (QScreen *s : QGuiApplication::screens()) {
        const QRect inter = s->geometry().intersected(r);
        if (!inter.isEmpty())
            p.drawPixmap(inter.topLeft() - r.topLeft(), s->grabWindow(0),
                         QRect(inter.topLeft() - s->geometry().topLeft(), inter.size()));
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

// ---------- Shared icon set for all buttons. ----------
// Stroke glyphs on a 24-unit grid with 2-unit round strokes, following the free
// Feather/Lucide line-icon style: one vocabulary for every button in the app, painted
// in code so no resource files ship and any icon stays crisp at every scale.

enum class Icon { Save, Copy, Pin, LongShot, Cancel };

static void buildIconPath(QPainterPath &path, Icon icon)
{
    switch (icon) {
    case Icon::Save:                                  // Floppy disk with label slot.
        path.moveTo(16, 3); path.lineTo(5, 3); path.lineTo(3, 5); path.lineTo(3, 19);
        path.lineTo(5, 21); path.lineTo(19, 21); path.lineTo(21, 19); path.lineTo(21, 8);
        path.lineTo(16, 3);
        path.moveTo(7, 21); path.lineTo(7, 13); path.lineTo(17, 13); path.lineTo(17, 21);
        path.moveTo(7, 3); path.lineTo(7, 8); path.lineTo(15, 8); path.lineTo(15, 3);
        break;
    case Icon::Copy:                                  // Two stacked sheets.
        path.addRoundedRect(9, 9, 12, 12, 2, 2);
        path.moveTo(6, 15); path.lineTo(5, 15); path.quadTo(3, 15, 3, 13); path.lineTo(3, 5);
        path.quadTo(3, 3, 5, 3); path.lineTo(13, 3); path.quadTo(15, 3, 15, 5); path.lineTo(15, 6);
        break;
    case Icon::Pin:                                   // Thumbtack: head plus needle.
        path.addRoundedRect(7, 3, 10, 9, 3, 3);
        path.moveTo(12, 12); path.lineTo(12, 21);
        break;
    case Icon::LongShot:                              // Chevrons pointing down: scrolling capture.
        path.moveTo(7, 6); path.lineTo(12, 11); path.lineTo(17, 6);
        path.moveTo(7, 13); path.lineTo(12, 18); path.lineTo(17, 13);
        break;
    case Icon::Cancel:                                // Cross.
        path.moveTo(6, 6); path.lineTo(18, 18);
        path.moveTo(18, 6); path.lineTo(6, 18);
        break;
    }
}

static QIcon makeIcon(Icon icon)
{
    QPainterPath path;
    buildIconPath(path, icon);
    QIcon result;
    for (int px : {16, 24, 32}) {                     // 1x / 1.5x / 2x variants of one logical size.
        QPixmap pm(px, px);
        pm.setDevicePixelRatio(px / 16.0);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(16.0 / 24.0, 16.0 / 24.0);
        QPen pen(QColor("#ececec"), 1.6);             // Fine hairline strokes at 16px.
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPath(path);
        result.addPixmap(pm);
    }
    return result;
}

static QToolButton *iconButton(Icon icon, const QString &tip)
{
    auto *button = new QToolButton;
    button->setIcon(makeIcon(icon));
    button->setIconSize(QSize(16, 16));
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

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
            "QToolButton{background:transparent;border:none;border-radius:3px;padding:4px;}"
            "QToolButton:hover{background:#454545;}";
        m_bar = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        m_bar->setStyleSheet(barQss);
        auto *lay = new QHBoxLayout(m_bar);
        lay->setContentsMargins(6, 0, 4, 0);
        m_label = new QLabel;
        auto *save = iconButton(Icon::Save, QStringLiteral("保存到文件"));
        auto *copy = iconButton(Icon::Copy, QStringLiteral("复制到剪贴板"));
        auto *pin = iconButton(Icon::Pin, QStringLiteral("贴在屏幕上"));
        auto *no = iconButton(Icon::Cancel, QStringLiteral("取消 (Esc)"));
        lay->addWidget(m_label);
        lay->addWidget(save);
        lay->addWidget(copy);
        lay->addWidget(pin);
        lay->addWidget(no);
        connect(save, &QToolButton::clicked, this, &LongCapture::saveCapture);
        connect(copy, &QToolButton::clicked, this, &LongCapture::copyCapture);
        connect(pin, &QToolButton::clicked, this, &LongCapture::finish);
        connect(no, &QToolButton::clicked, this, &LongCapture::cancel);
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
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        const QString name = screen->name();
        if (!EnumDisplaySettingsW(reinterpret_cast<const wchar_t *>(name.utf16()),
                                  ENUM_CURRENT_SETTINGS, &mode))
            return {};                              // Do not guess native coordinates after a display-query failure.
        const QRect native(mode.dmPosition.x, mode.dmPosition.y, mode.dmPelsWidth, mode.dmPelsHeight);
        QImage image = grabNative(native);          // Single BitBlt incl. layered windows.
        if (image.isNull())
            image = screen->grabWindow(0).toImage(); // GDI failed (locked/station): plain per-screen shot.
        if (image.isNull() || image.size() != native.size())
            return {};                              // Reject a display-mode change during capture.
        image.setDevicePixelRatio(1);
        frames.append({screen->geometry(), native, image});
    }
    return frames;
}

struct DetectionOptions {
    bool windows = true;
    bool elements = false;
    QStringList excludedPrograms;
    QStringList disabledUiPrograms;
};

// Rectangles use native pixels in the detector and local logical coordinates in the overlay.
struct DetectedWindow {
    QRect native;
    QVector<QRect> nativeChildren;
    HWND hwnd = nullptr;
    QString program;
    bool maximized = false;
    DetectedWindow(const QRect &rect = QRect(), const QVector<QRect> &children = {})
        : native(rect), nativeChildren(children) {}
};
typedef QVector<DetectedWindow> WindowTargets;
Q_DECLARE_METATYPE(WindowTargets)

// Class names that never carry capturable content but often own fullscreen
// rects (host surfaces and floating overlays).
static const wchar_t *const kExcludedWindowClasses[] = {
    L"Progman",                        // Desktop background.
    L"WorkerW",                        // Desktop wallpaper behind icons.
    L"CEF-OSC-WIDGET",                 // Embedded-browser floating overlays.
    L"EdgeUiInputTopWndClass",         // Edge gesture strips at screen edges.
    L"Windows.UI.Core.CoreWindow",     // UWP host surfaces (mostly cloaked shells).
    L"NarratorHelperWindow",
    L"Tao Thread Event Target",        // Helper message windows of toolkits.
};

static qint64 rectArea(const QRect &r) { return qint64(r.width()) * r.height(); }

static void prepareChildren(DetectedWindow &win)
{
    QVector<QRect> rects;
    for (const QRect &child : win.nativeChildren) {
        const QRect clipped = child.intersected(win.native);
        if (!clipped.isEmpty() && !rects.contains(clipped)) rects.append(clipped);
    }
    const auto smaller = [](const QRect &a, const QRect &b) { return rectArea(a) < rectArea(b); };
    std::stable_sort(rects.begin(), rects.end(), smaller);
    QVector<QRect> strips;
    for (int i = rects.size() - 1; i >= 0; --i)
        for (int j = i - 1; j >= 0; --j)
            if (rects[i].contains(rects[j])
                && (rects[i].width() == rects[j].width() || rects[i].height() == rects[j].height()))
                for (const QRect &strip : QRegion(rects[i]).subtracted(QRegion(rects[j])))
                    if (!rects.contains(strip) && !strips.contains(strip)) strips.append(strip);
    rects += strips;
    std::stable_sort(rects.begin(), rects.end(), smaller);
    rects.removeAll(win.native);
    win.nativeChildren = rects;
}

static QRect windowFrame(HWND hwnd)
{
    RECT r{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &r, sizeof(r)))) {
        UINT thickness = 0;
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(hwnd, &placement) && placement.showCmd != SW_SHOWMAXIMIZED
            && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_BORDER))
            DwmGetWindowAttribute(hwnd, 37 /* DWMWA_VISIBLE_FRAME_BORDER_THICKNESS */, &thickness, sizeof(thickness));
        return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top)
            .adjusted(thickness, thickness, -int(thickness), -int(thickness));
    }
    return GetWindowRect(hwnd, &r) ? QRect(r.left, r.top, r.right - r.left, r.bottom - r.top) : QRect();
}

// Preserve z-order; exclude only the capture overlay, not this application's pinned images.
static QString windowProgramName(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[32768];
    DWORD length = sizeof(path) / sizeof(path[0]);
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    return ok ? QFileInfo(QString::fromWCharArray(path, length)).fileName().toLower() : QString();
}

static QRect clampWindowFrame(const QRect &rect, bool maximized)
{
    struct Monitors { HMONITOR center; QRect rect; bool crosses; };
    Monitors monitors{MonitorFromPoint(POINT{rect.center().x(), rect.center().y()}, MONITOR_DEFAULTTONEAREST), rect, false};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT bounds, LPARAM lp) -> BOOL {
        Monitors *m = reinterpret_cast<Monitors *>(lp);
        if (monitor != m->center && m->rect.intersects(QRect(bounds->left, bounds->top,
            bounds->right - bounds->left, bounds->bottom - bounds->top))) m->crosses = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if ((!maximized && monitors.crosses) || !GetMonitorInfoW(monitors.center, &info)) return rect;
    const RECT &r = info.rcMonitor;
    return rect.intersected(QRect(r.left, r.top, r.right - r.left, r.bottom - r.top));
}

static WindowTargets detectWindowRects(HWND overlay = nullptr, const QStringList &excluded = {})
{
    struct Ctx { WindowTargets out; HWND overlay; QStringList excluded; } ctx{{}, overlay, excluded};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        Ctx *ctx = reinterpret_cast<Ctx *>(lp);
        if (hwnd == ctx->overlay || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
            return TRUE;
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TRANSPARENT)                  // Click-through overlay: never a capture target.
            return TRUE;
        if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_DISABLED)
            return TRUE;
        int cloaked = 0;
        if (DwmGetWindowAttribute(hwnd, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked)) >= 0 && cloaked)
            return TRUE;                             // Hidden on another virtual desktop.
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        for (const wchar_t *bad : kExcludedWindowClasses)
            if (!_wcsicmp(cls, bad))
                return TRUE;
        DetectedWindow win;
        win.hwnd = hwnd;
        win.program = windowProgramName(hwnd);
        if (!win.program.isEmpty() && ctx->excluded.contains(win.program, Qt::CaseInsensitive)) return TRUE;
        win.native = windowFrame(hwnd);
        if (win.native.isEmpty()) return TRUE;
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        win.maximized = GetWindowPlacement(hwnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED;
        win.native = clampWindowFrame(win.native, win.maximized);
        if (win.native.isEmpty()) return TRUE;
        struct ChildCtx { QVector<QRect> *out; } childCtx { &win.nativeChildren };
        EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
            ChildCtx *c = reinterpret_cast<ChildCtx *>(lp);
            if (!IsWindow(child) || !IsWindowVisible(child) || IsIconic(child))
                return TRUE;
            RECT r {};
            if (GetWindowRect(child, &r) && r.left < r.right && r.top < r.bottom)
                c->out->append(QRect(r.left, r.top, r.right - r.left, r.bottom - r.top));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&childCtx));
        prepareChildren(win);
        ctx->out.append(win);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

static QRect childAt(const DetectedWindow &win, const QPoint &point, const QRect &previous = {})
{
    for (const QRect &child : win.nativeChildren)
        if (previous.contains(point) ? (child != previous && child.contains(previous)) : child.contains(point))
            return child;
    return win.native.contains(point) ? win.native : QRect();
}

static QRect accessibleRect(IAccessible *object, const VARIANT &id, const QRect &bounds)
{
    LONG x = 0, y = 0, w = 0, h = 0;
    if (FAILED(object->accLocation(&x, &y, &w, &h, id)) || w <= 0 || h <= 0) return {};
    return QRect(x, y, w, h).intersected(bounds);
}

// This toolchain's oleacc import library resolves IID_IAccessible to a thunk, not GUID data.
static const GUID kAccessibleIID = {0x618736e0, 0x3c3d, 0x11cf, {0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Minimal UI Automation client. The toolchain ships only a stub UIA header, so these interfaces
// follow the vtable order of the registered UIAutomationCore type library (verified per slot).
struct UiaElement {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(const GUID &riid, void **out) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFocus() = 0;                                        // 0x18
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeId(void **runtimeId) = 0;                    // 0x20
    virtual HRESULT STDMETHODCALLTYPE FindFirst(int scope, void *condition, UiaElement **found) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindAll(int scope, void *condition, void **found) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindFirstBuildCache(int scope, void *condition, void *cache, UiaElement **found) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindAllBuildCache(int scope, void *condition, void *cache, void **found) = 0;
    virtual HRESULT STDMETHODCALLTYPE BuildUpdatedCache(void *cache, UiaElement **updated) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPropertyValue(int property, VARIANT *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPropertyValueEx(int property, int ignore, VARIANT *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedPropertyValue(int property, VARIANT *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedPropertyValueEx(int property, int ignore, VARIANT *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPatternAs(int pattern, const GUID &riid, void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedPatternAs(int pattern, const GUID &riid, void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPattern(int pattern, void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedPattern(int pattern, void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedParent(UiaElement **parent) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedChildren(void **children) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentProcessId(int *out) = 0;                        // 0xa0
    virtual HRESULT STDMETHODCALLTYPE CurrentControlType(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentLocalizedControlType(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentName(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentAcceleratorKey(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentAccessKey(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentHasKeyboardFocus(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsKeyboardFocusable(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsEnabled(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentAutomationId(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentClassName(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentHelpText(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentCulture(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsControlElement(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsContentElement(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsPassword(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentNativeWindowHandle(void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentItemType(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsOffscreen(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentOrientation(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentFrameworkId(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsRequiredForForm(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentItemStatus(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentBoundingRectangle(RECT *out) = 0;              // 0x158
};
struct UiaTreeWalker {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(const GUID &riid, void **out) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParentElement(UiaElement *element, UiaElement **parent) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFirstChildElement(UiaElement *element, UiaElement **child) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLastChildElement(UiaElement *element, UiaElement **child) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNextSiblingElement(UiaElement *element, UiaElement **next) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPreviousSiblingElement(UiaElement *element, UiaElement **previous) = 0;
    virtual HRESULT STDMETHODCALLTYPE NormalizeElement(UiaElement *element, UiaElement **normalized) = 0;
};
struct UiaAutomation {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(const GUID &riid, void **out) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual HRESULT STDMETHODCALLTYPE CompareElements(UiaElement *first, UiaElement *second, int *same) = 0;
    virtual HRESULT STDMETHODCALLTYPE CompareRuntimeIds(void *first, void *second, int *same) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRootElement(UiaElement **root) = 0;
    virtual HRESULT STDMETHODCALLTYPE ElementFromHandle(HWND hwnd, UiaElement **element) = 0;
    virtual HRESULT STDMETHODCALLTYPE ElementFromPoint(POINT point, UiaElement **element) = 0;   // 0x38
    virtual HRESULT STDMETHODCALLTYPE GetFocusedElement(UiaElement **element) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRootElementBuildCache(void *cache, UiaElement **root) = 0;
    virtual HRESULT STDMETHODCALLTYPE ElementFromHandleBuildCache(HWND hwnd, void *cache, UiaElement **element) = 0;
    virtual HRESULT STDMETHODCALLTYPE ElementFromPointBuildCache(POINT point, void *cache, UiaElement **element) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFocusedElementBuildCache(void *cache, UiaElement **element) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTreeWalker(void *condition, UiaTreeWalker **walker) = 0;
    virtual HRESULT STDMETHODCALLTYPE ControlViewWalker(UiaTreeWalker **walker) = 0;         // 0x70
};
static const GUID kUiaClassId = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
static const GUID kUiaIid = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};

static bool windowHung(HWND hwnd)
{
    DWORD_PTR reply = 0;
    return IsHungAppWindow(hwnd)
        || (!SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 60, &reply)
            && GetLastError() == ERROR_TIMEOUT);
}

// All COM objects and VARIANTs remain on the querying thread.
static QRect accessibleAt(HWND hwnd, const QPoint &point, const QRect &bounds, const QRect &previous = {})
{
    if (windowHung(hwnd)) return {};
    WINDOWINFO info{};
    info.cbSize = sizeof(info);
    if (!GetWindowInfo(hwnd, &info) || (info.dwWindowStatus == WS_ACTIVECAPTION
        && (info.dwStyle & (WS_THICKFRAME | WS_CAPTION) || info.dwExStyle & WS_EX_WINDOWEDGE))) return {};
    IAccessible *object = nullptr;
    const HRESULT objectResult = AccessibleObjectFromWindow(hwnd, OBJID_WINDOW, kAccessibleIID,
                                        reinterpret_cast<void **>(&object));
    if (FAILED(objectResult) || !object) return {};
    QElapsedTimer timer;
    timer.start();
    QRect best;
    const auto accepts = [&point, &previous](const QRect &r) {
        return r.contains(point) && (!previous.contains(point) || (r != previous && r.contains(previous)));
    };
    for (int depth = 0; object && depth < 60; ++depth) {
        VARIANT self{};
        self.vt = VT_I4;
        self.lVal = CHILDID_SELF;
        const QRect current = accessibleRect(object, self, bounds);
        if (accepts(current)) best = current;
        VARIANT state{};
        const bool invisible = SUCCEEDED(object->get_accState(self, &state)) && state.vt == VT_I4
                               && (state.lVal & STATE_SYSTEM_INVISIBLE);
        VariantClear(&state);
        VARIANT hit{};
        // shortcut: The budget bounds traversal, not a single stalled provider call; isolate COM in a helper process if this occurs.
        if (invisible || timer.elapsed() >= 100 || FAILED(object->accHitTest(point.x(), point.y(), &hit))) {
            VariantClear(&hit);
            break;
        }
        if (hit.vt == VT_DISPATCH && hit.pdispVal) {
            IAccessible *next = nullptr;
            hit.pdispVal->QueryInterface(kAccessibleIID, reinterpret_cast<void **>(&next));
            VariantClear(&hit);
            object->Release();
            object = next;
            continue;
        }
        if (hit.vt == VT_EMPTY) { hit.vt = VT_I4; hit.lVal = CHILDID_SELF; }
        if (hit.vt == VT_I4) {
            const QRect leaf = accessibleRect(object, hit, bounds);
            if (accepts(leaf) && (best.isEmpty() || best.contains(leaf))) best = leaf;
            if (previous.contains(point)) {
                for (int up = depth; object && up < 60 && timer.elapsed() < 100; ++up) {
                    const QRect parentRect = accessibleRect(object, self, bounds);
                    if (accepts(parentRect)) { best = parentRect; break; }
                    IDispatch *parent = nullptr;
                    IAccessible *next = nullptr;
                    if (SUCCEEDED(object->get_accParent(&parent)) && parent) {
                        parent->QueryInterface(kAccessibleIID, reinterpret_cast<void **>(&next));
                        parent->Release();
                    }
                    object->Release();
                    object = next;
                }
            }
        }
        VariantClear(&hit);
        break;
    }
    if (object) object->Release();
    return best;
}

// Scoped UIA hit test for one window. The walk starts at the target window's own element and
// descends, so fullscreen capture overlays never hijack the result (a global ElementFromPoint
// returns the topmost overlay instead). Chromium engines expose their element tree only after
// an accessibility nudge, so wake them before walking; the walk is budget-capped and providers
// that overrun the cap are skipped for the rest of the session.
static QRect uiaRectAt(UiaAutomation *automation, HWND hwnd, const QPoint &point, const QRect &bounds,
                       const QRect &previous, QStringList *downgraded, const QString &program)
{
    if (!automation || windowHung(hwnd)) return QRect();
    if (!program.isEmpty() && downgraded->contains(program, Qt::CaseInsensitive)) return QRect();
    const auto rectOf = [bounds](UiaElement *element) {
        RECT r{};
        if (!element || FAILED(element->CurrentBoundingRectangle(&r)) || r.right <= r.left || r.bottom <= r.top)
            return QRect();
        return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top).intersected(bounds);
    };
    // Wake sleeping Chromium engines: they activate their accessibility provider when a client
    // queries WM_GETOBJECT with the UIA root object id; the tree then builds asynchronously.
    const auto nudge = [&]() {
        POINT local{point.x(), point.y()};
        ScreenToClient(hwnd, &local);
        const HWND child = ChildWindowFromPointEx(hwnd, local, CWP_SKIPINVISIBLE);
        DWORD_PTR reply = 0;
        SendMessageTimeoutW(child ? child : hwnd, WM_GETOBJECT, 0, -25 /* UiaRootObjectId */,
                            SMTO_ABORTIFHUNG, 30, &reply);
    };
    nudge();
    UiaElement *root = nullptr;
    if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) return QRect();
    UiaTreeWalker *walker = nullptr;
    const bool walking = SUCCEEDED(automation->ControlViewWalker(&walker)) && walker;
    // The walk budget keeps hover queries interactive: a full desktop repaint trails the cursor
    // when a single query costs hundreds of milliseconds, so heavy trees stop early and providers
    // that still overrun get skipped for the rest of the session.
    QElapsedTimer timer;
    timer.start();
    // Single-path descent with decoy skipping: some providers expose empty containers with the
    // same rectangle as the real branch (e.g. browser frames), so keep scanning siblings only
    // until a child actually shrinks the rectangle.
    std::function<UiaElement *(UiaElement *, int)> search;
    search = [&](UiaElement *element, int depth) -> UiaElement * {
        element->AddRef();                               // Owned reference for the caller.
        if (depth >= 60 || timer.elapsed() >= 120) return element;
        const int selfArea = rectArea(rectOf(element));
        UiaElement *child = nullptr;
        if (SUCCEEDED(walker->GetFirstChildElement(element, &child)) && child) {
            while (child && timer.elapsed() < 120) {
                if (rectOf(child).contains(point)) {
                    UiaElement *found = search(child, depth + 1);
                    if (rectArea(rectOf(found)) < selfArea) {
                        child->Release();                // `found` already owns a reference.
                        element->Release();
                        return found;                    // Shrunk: stop scanning siblings.
                    }
                    found->Release();
                }
                UiaElement *sibling = nullptr;
                const HRESULT hr = walker->GetNextSiblingElement(child, &sibling);
                child->Release();
                child = SUCCEEDED(hr) ? sibling : nullptr;
            }
        }
        return element;
    };
    UiaElement *held = walking ? search(root, 0) : nullptr;
    QRect deepest = rectOf(held ? held : root);
    QRect result;
    if (previous.isEmpty()) {
        result = deepest.contains(point) ? deepest : QRect();
    } else if (walking) {
        // Expansion: first control-view ancestor strictly containing the current region.
        while (held && timer.elapsed() < 120) {
            const QRect r = rectOf(held);
            if (!r.isEmpty() && r != previous && r.contains(previous)) { result = r; break; }
            UiaElement *parent = nullptr;
            if (FAILED(walker->GetParentElement(held, &parent)) || !parent) break;
            held->Release();
            held = parent;
        }
    }
    if (held) held->Release();
    if (walking) walker->Release();
    root->Release();
    if (timer.elapsed() > 120 && !program.isEmpty() && !downgraded->contains(program))
        downgraded->append(program);
    return result;
}

class WindowDetector : public QThread
{
    Q_OBJECT
public:
    WindowDetector(HWND overlay, const DetectionOptions &options) : m_overlay(overlay), m_options(options) {}
    ~WindowDetector() override { stop(); wait(); }
    quint64 query(const QPoint &point, const QRect &previous = {}) {
        QMutexLocker lock(&m_mutex);
        const quint64 id = ++m_id;
        m_queue.append({point, previous, id});
        while (m_queue.size() > 4) m_queue.removeFirst();
        m_wake.wakeOne();
        return id;
    }
    void cancel() { QMutexLocker lock(&m_mutex); ++m_id; m_queue.clear(); }
    void stop() { QMutexLocker lock(&m_mutex); m_stop = true; m_queue.clear(); m_wake.wakeOne(); }
signals:
    void gotTargets(const WindowTargets &targets, quint64 id);
    void gotRect(const QRect &rect, quint64 id);
protected:
    void run() override {
        const HRESULT com = CoInitialize(nullptr);
        if (SUCCEEDED(com))
            CoCreateInstance(kUiaClassId, nullptr, CLSCTX_INPROC_SERVER, kUiaIid,
                             reinterpret_cast<void **>(&m_uia));
        while (true) {
            Request request;
            {
                QMutexLocker lock(&m_mutex);
                while (!m_stop && m_queue.isEmpty()) m_wake.wait(&m_mutex);
                if (m_stop) break;
                request = m_queue.takeFirst();
                if (request.id != m_id) continue;
            }
            WindowTargets targets = m_options.windows ? detectWindowRects(m_overlay, m_options.excludedPrograms) : WindowTargets();
            {
                QMutexLocker lock(&m_mutex);
                if (!m_stop) emit gotTargets(targets, request.id);
            }
            QRect result;
            for (const DetectedWindow &win : targets) {
                if (!win.native.contains(request.point)) continue;
                result = win.native;
                if (m_options.elements) {
                    const bool blocked = !m_options.disabledUiPrograms.contains(win.program, Qt::CaseInsensitive);
                    const QRect accessible = SUCCEEDED(com) && blocked
                        ? accessibleAt(win.hwnd, request.point, win.native, request.previous) : QRect();
                    const QRect uia = m_uia && blocked
                        ? uiaRectAt(m_uia, win.hwnd, request.point, win.native, request.previous,
                                    &m_uiaSlow, win.program) : QRect();
                    const QRect child = childAt(win, request.point, request.previous);
                    QRect best = child;
                    for (const QRect &candidate : {accessible, uia})
                        if (!candidate.isEmpty() && (best.isEmpty() || rectArea(candidate) < rectArea(best)))
                            best = candidate;
                    if (!best.isEmpty()) result = best;
                }
                break;
            }
            QMutexLocker lock(&m_mutex);
            // Deliver every processed query even when a newer one is already queued: during a
            // fast slide each result is superseded before it finishes, and dropping it here
            // would freeze the hover border until the mouse stops. One worker emits in query
            // order, and the overlay rejects results that arrive out of order.
            if (!m_stop) emit gotRect(result, request.id);
        }
        if (m_uia) { m_uia->Release(); m_uia = nullptr; }
        if (SUCCEEDED(com)) CoUninitialize();
    }
private:
    struct Request { QPoint point; QRect previous; quint64 id; };
    HWND m_overlay;
    DetectionOptions m_options;
    UiaAutomation *m_uia = nullptr;
    QStringList m_uiaSlow;
    QMutex m_mutex;
    QWaitCondition m_wake;
    QVector<Request> m_queue;
    quint64 m_id = 0;
    bool m_stop = false;
};

// Map a native-pixel rect into logical coordinates via the frozen frames.
static QRect logicalFromNative(const QVector<ScreenFrame> &frames, const QRect &native)
{
    if (native.isEmpty()) return {};
    for (const ScreenFrame &f : frames)
        if (f.pixelRect.contains(native.center())) {
            const double sx = double(f.logicalRect.width()) / f.pixelRect.width();
            const double sy = double(f.logicalRect.height()) / f.pixelRect.height();
            const int x = qRound(f.logicalRect.x() + (native.x() - f.pixelRect.x()) * sx);
            const int y = qRound(f.logicalRect.y() + (native.y() - f.pixelRect.y()) * sy);
            const int right = qRound(f.logicalRect.x() + (native.x() + native.width() - f.pixelRect.x()) * sx);
            const int bottom = qRound(f.logicalRect.y() + (native.y() + native.height() - f.pixelRect.y()) * sy);
            return QRect(x, y, right - x, bottom - y);
        }
    return native;
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
    explicit CaptureOverlay(const QVector<ScreenFrame> &frames,
                            const WindowTargets &nativeWindows = {},
                            const DetectionOptions &options = DetectionOptions(), bool liveDetection = false)
        : m_frames(frames), m_detectionOptions(options)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_DeleteOnClose);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::CrossCursor);
        setGeometry(QGuiApplication::primaryScreen()->virtualGeometry());
        setWindowTargets(nativeWindows);

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

        // Frosted status bar: the overlay paints a blurred snapshot behind this transparent
        // container, so the glass effect follows the frozen frame at every position.
        m_tools = new QWidget(this);
        m_tools->setStyleSheet(
            "QToolButton{background:transparent;border:none;border-radius:3px;padding:4px;}"
            "QToolButton:hover{background:rgba(255,255,255,38);}");
        auto *save = iconButton(Icon::Save, QStringLiteral("保存到文件 (Ctrl+S)"));
        auto *copy = iconButton(Icon::Copy, QStringLiteral("复制到剪贴板 (Ctrl+C)"));
        auto *pin = iconButton(Icon::Pin, QStringLiteral("贴在屏幕上 (Enter / 双击)"));
        auto *scroll = iconButton(Icon::LongShot, QStringLiteral("长截图：手动滚动拼接长图"));
        m_toolButtons = {save, copy, pin, scroll};
        connect(save, &QToolButton::clicked, this, &CaptureOverlay::saveCapture);
        connect(copy, &QToolButton::clicked, this, &CaptureOverlay::copyCapture);
        connect(pin, &QToolButton::clicked, this, &CaptureOverlay::pinCapture);
        connect(scroll, &QToolButton::clicked, this, &CaptureOverlay::longCapture);
        buildToolLayout(Qt::Horizontal);
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
        if (liveDetection) {
            startDetector();
            requestDetection(mapFromGlobal(QCursor::pos()));
        }
    }
    ~CaptureOverlay() override { if (m_detector) m_detector->stop(); }
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
        // Magnetic edge snapping while creating, moving, and resizing a selection.
        const QRect screenGeo = QGuiApplication::primaryScreen()->geometry();
        const QPoint screenOrigin = screenGeo.topLeft() - overlay.geometry().topLeft();
        const auto mouseMove = [&overlay](const QPoint &local) {
            QMouseEvent move(QEvent::MouseMove, local, local + overlay.geometry().topLeft(),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&overlay, &move);
        };
        overlay.m_pressPos = screenOrigin + QPoint(40, 40);
        overlay.m_drag = H_New;
        overlay.m_mode = Selecting;
        mouseMove(screenOrigin + QPoint(screenGeo.width() - 6, 120));
        check(overlay.m_sel.right() + 1 == screenOrigin.x() + screenGeo.width() && overlay.m_snapX == screenOrigin.x() + screenGeo.width(),
              "new selection snaps its right edge to the screen border");
        overlay.m_sel = QRect(screenOrigin + QPoint(20, 20), QSize(60, 40));
        overlay.m_pressSel = overlay.m_sel;          // H_Move diffs against the press snapshot.
        overlay.m_pressPos = screenOrigin + QPoint(50, 40);
        overlay.m_drag = H_Move;
        overlay.m_mode = Adjusting;
        mouseMove(screenOrigin + QPoint(38, 40));   // Lands 8px from the screen border.
        check(overlay.m_sel.left() == screenOrigin.x() && overlay.m_snapX == screenOrigin.x(),
              "moving the selection snaps its left edge to the screen border");
        check(overlay.m_sel.size() == QSize(60, 40), "edge snapping preserves the selection size while moving");
        overlay.m_pressSel = overlay.m_sel;
        overlay.m_pressPos = screenOrigin + QPoint(38, 40);
        overlay.m_drag = H_R;
        mouseMove(screenOrigin + QPoint(screenGeo.width() - 3, 40));
        check(overlay.m_sel.right() + 1 == screenOrigin.x() + screenGeo.width(),
              "resizing snaps the dragged edge to the screen border");
        mouseMove(screenOrigin + QPoint(200, 40));
        check(overlay.m_sel.right() + 1 == screenOrigin.x() + 201 && overlay.m_snapX < 0,
              "no snap indicator appears away from anchor lines");
        // Click-to-select a detected window; dragging from inside draws a new selection.
        overlay.m_detectionOptions.elements = true;
        overlay.m_windowRects = { DetectedWindow{QRect(screenOrigin + QPoint(120, 60), QSize(80, 50)),
                                                 {QRect(screenOrigin + QPoint(140, 70), QSize(30, 20))}} };
        const auto mouseClick = [&overlay](const QPoint &local) {
            QMouseEvent press(QEvent::MouseButtonPress, local, local + overlay.geometry().topLeft(),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, local, local + overlay.geometry().topLeft(),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&overlay, &press);
            QApplication::sendEvent(&overlay, &release);
        };
        mouseClick(screenOrigin + QPoint(150, 80));
        check(overlay.m_sel == overlay.m_windowRects.first().nativeChildren.first() && overlay.m_mode == Adjusting,
              "clicking inside a detected child area selects the finer child rect");
        overlay.m_mode = Selecting;                  // Fresh state: a drag overrides the window hit.
        QMouseEvent dragPress(QEvent::MouseButtonPress, screenOrigin + QPoint(150, 80),
                              screenOrigin + QPoint(150, 80) + overlay.geometry().topLeft(),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &dragPress);
        mouseMove(screenOrigin + QPoint(165, 95));   // Drag before release overrides the click.
        QMouseEvent dragRelease(QEvent::MouseButtonRelease, screenOrigin + QPoint(165, 95),
                                screenOrigin + QPoint(165, 95) + overlay.geometry().topLeft(),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &dragRelease);
        check(overlay.m_sel == QRect(screenOrigin + QPoint(150, 80), QSize(16, 16)),
              "dragging from inside a window starts a new selection");
        overlay.m_sel = QRect(screenOrigin + QPoint(60, 60), QSize(40, 50));
        overlay.m_pressSel = overlay.m_sel;
        overlay.m_pressPos = screenOrigin + QPoint(100, 70);
        overlay.m_drag = H_R;
        overlay.m_mode = Adjusting;
        mouseMove(screenOrigin + QPoint(196, 70));   // 4px short of the window's right edge (200).
        check(overlay.m_sel.right() + 1 == screenOrigin.x() + 200,
              "resizing snaps the dragged edge to a window edge");
        // Hover: the mask hole follows the detected window before any selection exists.
        overlay.m_sel = QRect();
        overlay.m_mode = Selecting;
        overlay.m_drag = H_None;
        mouseMove(screenOrigin + QPoint(150, 80));   // Inside the child rect.
        check(overlay.m_hover == overlay.m_windowRects.first().nativeChildren.first() && overlay.m_sizeTip->isVisible(),
              "hovering a child area highlights the finer child rect and shows its size");
        mouseMove(screenOrigin + QPoint(125, 100));  // Inside the window, outside any child.
        check(overlay.m_hover == overlay.m_windowRects.first().native,
              "hover falls back to the whole window outside child areas");
        mouseMove(screenOrigin + QPoint(450, 300));  // Empty area away from the window.
        check(overlay.m_hover.isNull() && !overlay.m_sizeTip->isVisible(),
              "hover clears outside detected windows");
        // A committed selection stays put: hover re-detection waits until the selection is
        // cleared, so the highlight never chases the cursor after a click locks a target.
        overlay.m_sel = QRect(screenOrigin + QPoint(300, 200), QSize(50, 40));
        overlay.m_mode = Adjusting;
        overlay.m_drag = H_None;
        mouseMove(screenOrigin + QPoint(150, 80));   // Over the child, outside the selection.
        check(overlay.m_hover.isNull() && overlay.m_sel == QRect(screenOrigin + QPoint(300, 200), QSize(50, 40)),
              "a locked selection does not chase the cursor across detected areas");
        mouseMove(screenOrigin + QPoint(320, 210));  // Back inside the selection.
        check(overlay.m_hover.isNull(), "hover re-detection stays off inside the locked selection too");
        // Toolbar placement: horizontal strip flush with the selection's bottom-right corner
        // by default, flipping to a vertical strip on its right side at the screen bottom.
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry()
                               .translated(-overlay.geometry().topLeft());
        overlay.m_sel = QRect(avail.topLeft() + QPoint(60, 60), QSize(240, 120));
        overlay.m_mode = Adjusting;
        overlay.placeToolbar();
        check(overlay.m_tools->y() >= overlay.m_sel.bottom() + 1
                  && overlay.m_tools->x() + overlay.m_tools->width() == overlay.m_sel.right() + 1
                  && overlay.m_tools->width() > overlay.m_tools->height(),
              "toolbar docks below the selection, flush with its right edge");
        overlay.m_sel = QRect(avail.left() + 40, avail.bottom() + 1 - 150, 300, 150);
        overlay.placeToolbar();
        check(overlay.m_tools->x() >= overlay.m_sel.right() + 1
                  && overlay.m_tools->y() + overlay.m_tools->height() == overlay.m_sel.bottom() + 1
                  && overlay.m_tools->height() > overlay.m_tools->width(),
              "toolbar flips to a vertical strip on the selection's right side at the screen bottom");
        overlay.m_windowRects.clear();
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
    static int testWindowDetection()
    {
        const auto check = [](bool ok, const char *message) {
            if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
        };
        static_assert(DWMWA_CLOAKED == 14, "Use the cloaking attribute, not a rendering policy");
        DetectedWindow synthetic(QRect(0, 0, 100, 100), {QRect(0, 20, 100, 80), QRect(0, 20, 100, 80), QRect(0, 0, 100, 100)});
        prepareChildren(synthetic);
        check(synthetic.nativeChildren.size() == 2, "children are clipped, deduplicated, and split into uncovered strips");
        check(::childAt(synthetic, QPoint(30, 10)) == QRect(0, 0, 100, 20), "uncovered title strip is a detection target");
        check(::childAt(synthetic, QPoint(30, 40)) == QRect(0, 20, 100, 80), "content excludes the title strip exactly");
        check(::childAt(synthetic, QPoint(30, 40), QRect(0, 20, 100, 80)) == synthetic.native,
              "expanding skips the current child and returns its containing window");
        check(rectArea(QRect(0, 0, 80000, 80000)) == Q_INT64_C(6400000000), "rectangle ordering cannot overflow 32 bits");
        const QVector<ScreenFrame> scaled{{QRect(-200, 0, 200, 100), QRect(-300, 0, 300, 150), QImage()}};
        check(logicalFromNative(scaled, QRect(-300, 0, 300, 150)) == QRect(-200, 0, 200, 100),
              "native rectangles map exclusive edges without a one-pixel expansion");
        const QRect available = nativeFromLogical(QGuiApplication::primaryScreen()->availableGeometry());
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"PinshotDetectionFixture";
        check(RegisterClassW(&windowClass) != 0, "register the native fixture class");
        struct Fixture {
            HWND window = nullptr;
            QPoint cursor = QCursor::pos();
            ~Fixture() {
                if (window) DestroyWindow(window);
                UnregisterClassW(L"PinshotDetectionFixture", GetModuleHandleW(nullptr));
                QCursor::setPos(cursor);
            }
        } fixture;
        fixture.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, windowClass.lpszClassName, L"Detection test",
            WS_POPUP | WS_VISIBLE, available.center().x() - 160, available.center().y() - 100,
            320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(fixture.window != nullptr, "create a native detection fixture");
        HWND button = CreateWindowExW(0, L"BUTTON", L"Button", WS_CHILD | WS_VISIBLE,
            20, 30, 100, 32, fixture.window, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND edit = CreateWindowExW(0, L"EDIT", L"Input", WS_CHILD | WS_VISIBLE | WS_BORDER,
            20, 90, 180, 30, fixture.window, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(button && edit, "create button and input controls");
        const auto contains = [&fixture](const WindowTargets &targets) {
            for (const DetectedWindow &win : targets) if (win.hwnd == fixture.window) return true;
            return false;
        };
        check(contains(detectWindowRects()), "visible fixture is detected");
        check(!contains(detectWindowRects(fixture.window)), "the capture overlay handle is excluded");
        check(!contains(detectWindowRects(nullptr, {QFileInfo(QCoreApplication::applicationFilePath()).fileName().toUpper()})),
              "program exclusions use executable names and ignore case");
        SetWindowLongPtrW(fixture.window, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT);
        check(!contains(detectWindowRects()), "click-through windows are not capture targets");
        SetWindowLongPtrW(fixture.window, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
        EnableWindow(fixture.window, FALSE);
        check(!contains(detectWindowRects()), "disabled windows are not capture targets");
        EnableWindow(fixture.window, TRUE);
        const auto rawRect = [](HWND hwnd) {
            RECT r{};
            GetWindowRect(hwnd, &r);
            return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
        };
        const QRect buttonRect = rawRect(button), editRect = rawRect(edit);
        bool exact = false;
        for (const DetectedWindow &win : detectWindowRects())
            if (win.hwnd == fixture.window) exact = win.nativeChildren.contains(buttonRect) && win.nativeChildren.contains(editRect);
        check(exact, "native child rectangles do not grow by one pixel");
        DetectionOptions windowsOnly;
        WindowDetector coarse(nullptr, windowsOnly);
        QRect coarseResult;
        quint64 lastId = 0, returnedId = 0;
        QEventLoop coarseLoop;
        QObject::connect(&coarse, &WindowDetector::gotRect, &coarseLoop, [&](const QRect &r, quint64 id) {
            coarseResult = r; returnedId = id; coarseLoop.quit();
        });
        for (int i = 0; i < 20; ++i) lastId = coarse.query(buttonRect.center());
        coarse.start();
        QTimer::singleShot(5000, &coarseLoop, &QEventLoop::quit);
        coarseLoop.exec();
        coarse.stop();
        coarse.wait();
        check(returnedId == lastId && coarseResult == windowFrame(fixture.window),
              "UI detection defaults off and rapid requests return only the newest whole-window result");
        const QVector<ScreenFrame> frames = freezeScreens();
        check(!frames.isEmpty(), "capture fixture pixels");
        QCursor::setPos(logicalFromNative(frames, buttonRect).center());
        DetectionOptions options;
        options.elements = true;
        CaptureOverlay overlay(frames, {}, options, true);
        overlay.setAttribute(Qt::WA_DeleteOnClose, false);
        const auto waitResult = [&overlay, &check]() {
            QElapsedTimer budget;
            budget.start();
            while (overlay.m_queryPending && budget.elapsed() < 5000) {
                QEventLoop loop;
                QTimer::singleShot(10, &loop, &QEventLoop::quit);
                loop.exec();
            }
            check(!overlay.m_queryPending, "background detector replies without blocking the GUI");
        };
        waitResult();
        const QRect expectedButton = logicalFromNative(frames, buttonRect).translated(-overlay.geometry().topLeft());
        const QRect expectedEdit = logicalFromNative(frames, editRect).translated(-overlay.geometry().topLeft());
        overlay.requestDetection(expectedButton.center());
        waitResult();
        check(overlay.m_hover == expectedButton, "live MSAA/native detection highlights a button under the overlay");
        QCursor::setPos(expectedEdit.center() + overlay.geometry().topLeft());
        overlay.requestDetection(expectedEdit.center());
        waitResult();
        check(overlay.m_hover == expectedEdit, "moving queries the input control instead of retaining the button");
        const quint64 stale = overlay.m_queryId;
        overlay.cancelDetection();
        overlay.handleDetectedRect(buttonRect, stale);
        check(overlay.m_hover == expectedEdit, "cancelled or stale replies cannot replace the current target");
        // A result superseded by a newer query still paints so the border follows the cursor
        // during fast slides instead of freezing until the mouse stops.
        overlay.requestDetection(buttonRect.center());
        const quint64 superseded = overlay.m_queryId;      // First of two back-to-back queries.
        overlay.requestDetection(expectedEdit.center());
        overlay.handleDetectedRect(buttonRect, superseded);
        check(overlay.m_hover == expectedButton, "superseded hover results still follow the cursor");
        waitResult();
        check(overlay.m_hover == expectedEdit, "the newest query result wins once it arrives");
        // Probe COM directly without native child hints while the UI pumps provider messages.
        QRect accessible;
        QEventLoop comLoop;
        QThread *probe = QThread::create([&] {
            const HRESULT hr = CoInitialize(nullptr);
            if (SUCCEEDED(hr)) {
                accessible = accessibleAt(fixture.window, buttonRect.center(), rawRect(fixture.window));
                CoUninitialize();
            }
        });
        QObject::connect(probe, &QThread::finished, &comLoop, &QEventLoop::quit);
        probe->start();
        QTimer::singleShot(5000, &comLoop, &QEventLoop::quit);
        comLoop.exec();
        check(probe->isFinished(), "MSAA-only probe finishes");
        probe->wait();
        delete probe;
        if (accessible != buttonRect)
            std::fprintf(stderr, "MSAA returned (%d,%d %dx%d); button is (%d,%d %dx%d)\n",
                accessible.x(), accessible.y(), accessible.width(), accessible.height(),
                buttonRect.x(), buttonRect.y(), buttonRect.width(), buttonRect.height());
        check(accessible == buttonRect, "IAccessible accHitTest returns the button without native child hints");
        // Scoped UIA probe: walks from the window's own element, so the fullscreen capture
        // overlay above the fixture cannot hijack the result the way a global query would.
        QRect uiaHit, uiaGrow;
        QThread *uiaProbe = QThread::create([&] {
            if (SUCCEEDED(CoInitialize(nullptr))) {
                UiaAutomation *automation = nullptr;
                if (SUCCEEDED(CoCreateInstance(kUiaClassId, nullptr, CLSCTX_INPROC_SERVER, kUiaIid,
                                               reinterpret_cast<void **>(&automation))) && automation) {
                    QStringList slow;
                    const QRect window = rawRect(fixture.window);
                    uiaHit = uiaRectAt(automation, fixture.window, buttonRect.center(), window,
                                       QRect(), &slow, QStringLiteral("uia-probe"));
                    uiaGrow = uiaRectAt(automation, fixture.window, buttonRect.center(), window,
                                        buttonRect, &slow, QStringLiteral("uia-probe"));
                    automation->Release();
                }
                CoUninitialize();
            }
        });
        QEventLoop uiaLoop;
        QObject::connect(uiaProbe, &QThread::finished, &uiaLoop, &QEventLoop::quit);
        uiaProbe->start();
        QTimer::singleShot(5000, &uiaLoop, &QEventLoop::quit);
        uiaLoop.exec();
        check(uiaProbe->isFinished(), "UIA-only probe finishes");
        uiaProbe->wait();
        delete uiaProbe;
        if (uiaHit != buttonRect)
            std::fprintf(stderr, "UIA returned (%d,%d %dx%d); button is (%d,%d %dx%d)\n",
                uiaHit.x(), uiaHit.y(), uiaHit.width(), uiaHit.height(),
                buttonRect.x(), buttonRect.y(), buttonRect.width(), buttonRect.height());
        check(uiaHit == buttonRect, "scoped UIA walk returns the button without native hints");
        check(uiaGrow.contains(buttonRect) && uiaGrow != buttonRect,
              "UIA wheel expansion walks to a strictly containing ancestor");
        overlay.m_hoverHistory.clear();
        QWheelEvent expand(expectedEdit.center(), expectedEdit.center() + overlay.geometry().topLeft(),
            QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&overlay, &expand);
        waitResult();
        check(overlay.m_hover.contains(expectedEdit) && overlay.m_hover != expectedEdit,
              "element expansion returns a strictly containing region");
        QWheelEvent shrink(expectedEdit.center(), expectedEdit.center() + overlay.geometry().topLeft(),
            QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&overlay, &shrink);
        check(overlay.m_hover == expectedEdit, "wheel contraction restores the previous finer element");
        const QPointer<WindowDetector> silent = overlay.m_detector;
        silent->blockSignals(true);                  // Simulate a provider that never supplies its result.
        overlay.cancelDetection();
        overlay.requestDetection(expectedEdit.center());
        waitResult();
        check(overlay.m_detector != silent && overlay.m_hover == expectedEdit,
              "the watchdog replaces a silent worker and uses native geometry without blocking capture");
        if (silent) {
            silent->blockSignals(false);
            silent->stop();
            silent->wait();
            delete silent.data();
        }
        overlay.m_drag = H_New;
        overlay.m_pressPos = expectedButton.center();
        QCursor::setPos(expectedButton.center() + overlay.geometry().topLeft());
        overlay.requestDetection(expectedButton.center());
        overlay.m_waitingClick = true;
        overlay.m_drag = H_None;
        waitResult();
        check(overlay.m_sel == expectedButton && overlay.m_mode == Adjusting,
              "a click locks the fresh asynchronous element result");
        // Once locked, moving the mouse must not re-arm hover detection: the highlight would
        // otherwise chase the cursor and visually unlock the selection.
        const quint64 lockedQueryId = overlay.m_queryId;
        QMouseEvent slideAfterLock(QEvent::MouseMove, expectedEdit.center(),
                                   expectedEdit.center() + overlay.geometry().topLeft(),
                                   Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &slideAfterLock);
        check(overlay.m_queryId == lockedQueryId && !overlay.m_queryPending && overlay.m_hover.isNull()
                  && overlay.m_sel == expectedButton,
              "the locked selection stays put while the mouse keeps moving");
        // The status bar steps aside while the selection is dragged and returns on release.
        overlay.m_drag = H_BR;
        overlay.m_pressSel = overlay.m_sel;
        overlay.m_pressPos = expectedButton.bottomRight();
        const QPoint grown = expectedButton.bottomRight() + QPoint(40, 30);
        QMouseEvent dragMove(QEvent::MouseMove, grown, grown + overlay.geometry().topLeft(),
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &dragMove);
        check(!overlay.m_tools->isVisible() && !overlay.m_inspector->isVisible()
                  && overlay.m_sel != expectedButton,
              "the status bar and pixel inspector hide while the selection is being dragged");
        QMouseEvent dragRelease(QEvent::MouseButtonRelease, grown, grown + overlay.geometry().topLeft(),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &dragRelease);
        check(overlay.m_tools->isVisible() && overlay.m_tools->y() > overlay.m_sel.bottom(),
              "the status bar returns below the resized selection once the drag ends");
        QMouseEvent hoverAfter(QEvent::MouseMove, grown + QPoint(6, 0), grown + QPoint(6, 0) + overlay.geometry().topLeft(),
                               Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &hoverAfter);
        check(overlay.m_inspector->isVisible(),
              "the pixel inspector comes back on the next hover move after the drag");
        bool pinned = false;
        QObject::connect(&overlay, &CaptureOverlay::grabbed, &overlay, [&pinned](const QPixmap &pix) { pinned = !pix.isNull(); });
        overlay.resetSelection();
        QMouseEvent press(QEvent::MouseButtonPress, expectedButton.center(), expectedButton.center() + overlay.geometry().topLeft(),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent doubleClick(QEvent::MouseButtonDblClick, expectedButton.center(), expectedButton.center() + overlay.geometry().topLeft(),
                                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, expectedButton.center(), expectedButton.center() + overlay.geometry().topLeft(),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &press);
        QApplication::sendEvent(&overlay, &doubleClick);
        QApplication::sendEvent(&overlay, &release);
        waitResult();
        check(pinned && !overlay.isVisible(), "double-click pinning waits for the fresh element result");
        overlay.close();
        check(!overlay.isVisible() && !overlay.m_queryPending, "closing capture cancels pending detection");
        if (frames.size() > 1) {
            DetectedWindow cross(frames[0].pixelRect.united(frames[1].pixelRect));
            const QRect logical = logicalFromNative(frames, cross.native);
            if (QGuiApplication::screenAt(logical.center())) {
                overlay.setWindowTargets({cross});
                check(overlay.m_windowRects.first().native.size() == logical.size(),
                      "non-maximized windows crossing screens retain their full rectangle");
                cross.maximized = true;
                overlay.setWindowTargets({cross});
                check(rectArea(overlay.m_windowRects.first().native) < rectArea(logical),
                      "maximized detection rectangles are clipped to the center screen");
            }
        }
        std::puts("PASS: window filtering, child geometry, MSAA controls, asynchronous detection, and cancellation");
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
        // The mask hole tracks the detected target, falling back to the committed selection.
        const QRect hole = !m_hover.isNull() ? m_hover : m_sel;
        if (hole.isNull()) {
            p.fillRect(rect(), QColor(0, 0, 0, 60));
            return;
        }
        QPainterPath mask;                          // Dim outside the hole, leave the inside clear.
        mask.addRect(rect());
        mask.addRect(hole);
        p.fillPath(mask, QColor(0, 0, 0, 120));
        p.setPen(QPen(QColor("#1e90ff"), 1));
        p.drawRect(hole);
        const QColor snapColor(0, 229, 255);        // Magnetic-snap indicator lines.
        if (m_snapX >= 0) {
            p.setPen(QPen(snapColor, 1));
            p.drawLine(m_snapX, hole.top() - 10, m_snapX, hole.bottom() + 10);
        }
        if (m_snapY >= 0) {
            p.setPen(QPen(snapColor, 1));
            p.drawLine(hole.left() - 10, m_snapY, hole.right() + 10, m_snapY);
        }
        if (m_mode == Adjusting && !m_sel.isNull() && m_hover.isNull())  // Eight resize handles.
            for (const QRect &h : handleRects(m_sel)) {
                p.fillRect(h, Qt::white);
                p.drawRect(h);
            }
        if (m_tools->isVisible()) {                 // Frosted glass: blurred frozen frame plus tint.
            const QRect bar = m_tools->geometry();
            const qreal dpr = m_shot.devicePixelRatioF();
            const QRect src(qRound(bar.x() * dpr), qRound(bar.y() * dpr),
                            qMax(1, qRound(bar.width() * dpr)), qMax(1, qRound(bar.height() * dpr)));
            const QSize grain(qMax(1, bar.width() / 6), qMax(1, bar.height() / 6));
            const QPixmap blur = m_shot.copy(src)
                .scaled(grain, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .scaled(bar.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            QPainterPath frost;
            frost.addRoundedRect(bar, 8, 8);
            p.save();
            p.setClipPath(frost);
            p.drawPixmap(bar.topLeft(), blur);
            p.fillRect(bar, QColor(18, 18, 24, 168));
            p.restore();
            QPen edge(QColor(255, 255, 255, 30), 1);
            p.setPen(edge);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(bar, 8, 8);
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        m_snapX = m_snapY = -1;                     // Clear magnetic-snap indicators.
        cancelDetection();
        if (e->button() == Qt::RightButton) {       // Clear an existing selection; otherwise exit.
            if (m_mode == Adjusting) { resetSelection(); } else { close(); }
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        setFocus();                                   // Any click restores keyboard handling.
        m_pressPos = e->pos();
        m_dragStarted = false;
        if (m_mode == Adjusting) {
            m_drag = hitHandle(e->pos());
            m_pressSel = m_sel;
            if (m_drag == H_None) {                 // Clicking outside starts a new selection.
                resetSelection();
                m_drag = H_New;
            }
        } else {
            resetSelection();                       // A fresh press always starts from a clean state.
            m_drag = H_New;
        }
        m_pressWindow = m_drag == H_New ? windowAt(e->pos()) : QRect();
        if (m_drag == H_New) requestDetection(e->pos());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        updateInspector();
        const QPoint pos = e->pos();
        if (m_waitingClick) return;
        if (m_drag == H_None) {                     // Hover: cursor shape, size hint, window detection.
            if (m_mode == Adjusting) {
                setCursor(cursorForHandle(hitHandle(pos)));
                if (hitHandle(pos) != H_None) {
                    cancelDetection();
                    clearHover();                   // Repaint only the retired hover rectangle.
                }
                // Hover re-detection only runs before a selection is committed: afterwards
                // the highlight would chase the cursor and visually unlock the selection.
            } else requestDetection(pos);
            updateTip();                            // Visual state changes only with the hover rect.
            return;
        }
        if (m_drag == H_New && !m_dragStarted
            && (pos - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        m_dragStarted = true;
        cancelDetection();
        if (m_tools->isVisible()) {                 // The status bar steps aside during drags.
            update(m_tools->geometry());
            m_tools->hide();
        }
        m_inspector->hide();                        // So does the pixel inspector (this event
                                                    // ran updateInspector before the drag started).
        if (!m_hover.isNull())
            m_hover = QRect();                      // A drag replaces the hover preview with the selection.
        if (m_drag == H_New) {                      // New selection.
            if (m_mode == Selecting)
                m_mode = Adjusting;                 // Dragging enters adjustment mode.
            m_sel = QRect(m_pressPos, pos).normalized();
        } else {
            m_sel = resizeBy(m_drag, pos);
        }
        applySnap(m_drag);
        updateTip();
        update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        Q_UNUSED(e)
        if (m_drag == H_None)
            return;
        if (m_drag == H_New && !m_dragStarted) {
            m_drag = H_None;
            if (m_detector && m_queryPending) { m_waitingClick = true; return; }
            m_sel = m_pressWindow;
        }
        m_drag = H_None;
        m_snapX = m_snapY = -1;                     // Snap indicators only live during a drag.
        m_sel = m_sel.normalized();
        if (m_sel.width() < 4 || m_sel.height() < 4) {
            if (m_pressWindow.isEmpty()) {          // Plain click on empty space: nothing selected.
                resetSelection();
                return;
            }
            m_sel = m_pressWindow;                  // Click without drag: select the whole window.
        }
        m_mode = Adjusting;
        m_hover = {};
        setCursor(Qt::SizeAllCursor);
        placeToolbar();
        m_tools->show();
        updateTip();
        update();
        if (m_pinAfterDetection) { m_pinAfterDetection = false; pinCapture(); }
    }
    void wheelEvent(QWheelEvent *e) override
    {
        if (m_drag != H_None || m_mode != Selecting || !m_detectionOptions.elements) return;
        const QPoint point = e->position().toPoint();
        if (e->angleDelta().y() > 0) {
            if (m_hoverHistory.isEmpty()) m_hoverHistory.append(m_hover);
            requestDetection(point, m_hover);
        } else if (e->angleDelta().y() < 0) {
            cancelDetection();
            if (m_hoverHistory.size() > 1) {
                m_hoverHistory.removeLast();
                m_hover = m_hoverHistory.last();
                updateTip();
                update();
            } else requestDetection(point);
        }
    }
    void closeEvent(QCloseEvent *e) override
    {
        cancelDetection();
        if (m_detector) m_detector->stop();
        QWidget::closeEvent(e);
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (m_queryPending && (m_waitingClick || m_drag == H_New))
            m_pinAfterDetection = true;
        else if (m_mode == Adjusting && m_sel.contains(e->pos()))
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

    void startDetector()
    {
        qRegisterMetaType<WindowTargets>("WindowTargets");
        m_detector = new WindowDetector(reinterpret_cast<HWND>(winId()), m_detectionOptions);
        const QPointer<WindowDetector> detector = m_detector;
        connect(detector, &QThread::finished, detector, &QObject::deleteLater);
        connect(detector, &WindowDetector::gotTargets, this, [this, detector](const WindowTargets &targets, quint64 id) {
            // Arrives in query order from one worker; the newest table lands last.
            Q_UNUSED(id);
            if (detector == m_detector && isVisible()) setWindowTargets(targets);
        }, Qt::QueuedConnection);
        connect(detector, &WindowDetector::gotRect, this, [this, detector](const QRect &rect, quint64 id) {
            if (detector == m_detector) handleDetectedRect(rect, id);
        }, Qt::QueuedConnection);
        detector->start(QThread::LowestPriority);
    }
    void setWindowTargets(const WindowTargets &windows)
    {
        m_windowRects.clear();
        for (const DetectedWindow &win : windows) {
            QRect logical = logicalFromNative(m_frames, win.native);
            const QScreen *center = QGuiApplication::screenAt(logical.center());
            bool crosses = false;
            for (const QScreen *screen : QGuiApplication::screens())
                if (screen != center && screen->geometry().intersects(logical)) crosses = true;
            if (center && (win.maximized || !crosses)) logical &= center->geometry();
            if (logical.isEmpty()) continue;
            DetectedWindow target(logical.translated(-geometry().topLeft()));
            target.hwnd = win.hwnd;
            target.program = win.program;
            for (const QRect &child : win.nativeChildren) {
                const QRect clipped = logical.intersected(logicalFromNative(m_frames, child));
                if (!clipped.isEmpty()) target.nativeChildren.append(clipped.translated(-geometry().topLeft()));
            }
            m_windowRects.append(target);
        }
    }
    QRect nativeDetectionRect(const QRect &local) const
    {
        if (local.isEmpty()) return {};
        const QRect global = local.translated(geometry().topLeft());
        for (const ScreenFrame &frame : m_frames)
            if (frame.logicalRect.contains(global.center())) {
                const double sx = double(frame.pixelRect.width()) / frame.logicalRect.width();
                const double sy = double(frame.pixelRect.height()) / frame.logicalRect.height();
                return QRect(qRound(frame.pixelRect.x() + (global.x() - frame.logicalRect.x()) * sx),
                             qRound(frame.pixelRect.y() + (global.y() - frame.logicalRect.y()) * sy),
                             qRound(global.width() * sx), qRound(global.height() * sy));
            }
        return {};
    }
    void cancelDetection()
    {
        if (m_detector) m_detector->cancel();
        m_queryId = 0;
        m_queryPending = m_waitingClick = false;
        m_pinAfterDetection = false;
    }
    void requestDetection(const QPoint &point, const QRect &previous = {})
    {
        if (m_drag == H_None && point == m_detectionPoint && previous.isEmpty() && m_queryId != 0) return;
        if (previous.isEmpty()) m_hoverHistory.clear();
        m_detectionPoint = point;
        if (!m_detector) { m_hover = windowAt(point); updateTip(); return; }
        const QRect nativePoint = nativeDetectionRect(QRect(point, QSize(1, 1)));
        if (nativePoint.isEmpty()) return;
        m_queryPending = true;
        m_expanding = !previous.isEmpty();
        m_queryId = m_detector->query(nativePoint.topLeft(), nativeDetectionRect(previous));
        const quint64 id = m_queryId;
        const QPointer<WindowDetector> detector = m_detector;
        QTimer::singleShot(500, this, [this, detector, id] {
            if (!detector || detector != m_detector || id != m_queryId || !m_queryPending || !isVisible()) return;
            setWindowTargets(detectWindowRects(reinterpret_cast<HWND>(winId()), m_detectionOptions.excludedPrograms));
            bool identified = false;
            for (const DetectedWindow &win : m_windowRects)
                if (win.native.contains(m_detectionPoint) && !win.program.isEmpty()) {
                    identified = true;
                    if (!m_detectionOptions.disabledUiPrograms.contains(win.program, Qt::CaseInsensitive)) {
                        m_detectionOptions.disabledUiPrograms.append(win.program);
                        if (windowHung(win.hwnd)) {
                            QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
                            QStringList disabled = settings.value(QStringLiteral("Snip/crashes")).toStringList();
                            if (!disabled.contains(win.program, Qt::CaseInsensitive)) disabled.append(win.program);
                            settings.setValue(QStringLiteral("Snip/crashes"), disabled);
                        }
                    }
                    break;
                }
            if (!identified) m_detectionOptions.elements = false;
            // A stuck provider must not hold up a click or block the next capture session.
            detector->stop();
            handleDetectedRect(nativeDetectionRect(windowAt(m_detectionPoint)), id);
            startDetector();
            m_queryId = 0;
        });
    }
    // Hover-driven repaints stay incremental: a full-desktop repaint per follow update
    // saturates the GUI thread during fast slides, and painting is stateless (frozen frame
    // plus mask), so the union of the old and new rectangles reproduces the surface.
    void updateHoverRegion(const QRect &oldRect, const QRect &newRect)
    {
        QRect region;
        if (!newRect.isNull()) region = newRect.adjusted(-2, -2, 2, 2);
        if (!oldRect.isNull()) region |= oldRect.adjusted(-2, -2, 2, 2);
        if (!region.isNull()) update(region);
    }
    void clearHover()
    {
        if (m_hover.isNull()) return;
        const QRect old = m_hover;
        m_hover = {};
        updateHoverRegion(old, QRect());
    }
    void handleDetectedRect(const QRect &native, quint64 id)
    {
        if (!id || !isVisible()) return;
        if (id != m_queryId) {
            // Hover previews follow the cursor during fast slides: a result already superseded
            // by a newer query still paints where the cursor just was instead of freezing the
            // border until the mouse stops. Clicks and expansion keep the strict identity check.
            if (m_drag != H_None || m_waitingClick || m_expanding || id <= m_lastHoverId) return;
            m_lastHoverId = id;
            const QRect stale = logicalFromNative(m_frames, native).translated(-geometry().topLeft());
            if (stale.isEmpty()) return;
            const QRect old = m_hover;
            m_hover = stale;
            updateTip();
            updateHoverRegion(old, stale);
            return;
        }
        m_lastHoverId = id;
        m_queryPending = false;
        QRect detected = logicalFromNative(m_frames, native).translated(-geometry().topLeft());
        if (native.isEmpty() && m_detectionOptions.windows) {
            const QScreen *screen = QGuiApplication::screenAt(m_detectionPoint + geometry().topLeft());
            if (screen) detected = screen->availableGeometry().translated(-geometry().topLeft());
            if (!detected.contains(m_detectionPoint)) detected = QRect(m_detectionPoint, QSize(1, 1));
        }
        if (m_expanding && detected == m_hover) {
            const QScreen *screen = QGuiApplication::screenAt(m_detectionPoint + geometry().topLeft());
            const QRect available = screen ? screen->availableGeometry().translated(-geometry().topLeft()) : QRect();
            if (available != m_hover && available.contains(m_hover)) detected = available;
        }
        if (!detected.isEmpty() && !detected.contains(m_detectionPoint)) return;
        if (m_drag == H_New || m_waitingClick) {
            m_pressWindow = detected;
            if (m_waitingClick) {
                m_waitingClick = false;
                m_sel = detected;
                if (m_sel.width() >= 4 && m_sel.height() >= 4) {
                    m_mode = Adjusting;
                    m_tools->show();
                    placeToolbar();
                    if (m_pinAfterDetection) { m_pinAfterDetection = false; pinCapture(); return; }
                } else resetSelection();
                m_hover = {};
            }
        } else if (m_drag == H_None) {
            const QRect old = m_hover;
            m_hover = detected;
            if (m_expanding && (m_hoverHistory.isEmpty() || m_hoverHistory.last() != detected))
                m_hoverHistory.append(detected);
            updateTip();
            updateHoverRegion(old, m_hover);
            return;
        }
        updateTip();
        update();
    }

    void updateInspector()
    {
        if (m_drag != H_None && m_dragStarted) {      // The pixel inspector steps aside during drags.
            m_inspector->hide();
            return;
        }
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
        m_mode = Selecting;                         // m_hover is kept: the next mouse move refreshes it.
        m_sel = QRect();
        m_drag = H_None;
        m_tools->hide();
        m_sizeTip->hide();
        setCursor(Qt::CrossCursor);
        update();
    }
    QRect windowAt(const QPoint &p) const
    {
        if (!m_detectionOptions.windows) return {};
        for (const DetectedWindow &w : m_windowRects) {   // Top-down z-order: first hit wins.
            if (!w.native.contains(p))
                continue;
            return m_detectionOptions.elements ? ::childAt(w, p) : w.native;
        }
        return QRect();
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
    // Magnetic edges: snap selection edges to nearby screen and window anchors.
    enum { SnapThreshold = 8 };
    QVector<int> snapAnchorLines(bool horizontal) const
    {
        QVector<int> out;
        const QPoint off = -geometry().topLeft();
        for (const QScreen *s : QGuiApplication::screens()) {
            const QRect g = s->geometry().translated(off);
            if (horizontal)
                out << g.left() << g.right() + 1 << g.center().x();
            else
                out << g.top() << g.bottom() + 1 << g.center().y();
        }
        for (const DetectedWindow &w : m_windowRects) {  // Window edges as magnetic anchors.
            if (horizontal)
                out << w.native.left() << w.native.right() + 1;
            else
                out << w.native.top() << w.native.bottom() + 1;
        }
        return out;
    }
    void applySnap(Handle h)
    {
        m_snapX = m_snapY = -1;
        if (h == H_None || m_sel.isNull())
            return;
        const QVector<int> ax = snapAnchorLines(true), ay = snapAnchorLines(false);
        int (*nearest)(const QVector<int> &, int) = [](const QVector<int> &anchors, int v) {
            int best = v, dist = SnapThreshold + 1;
            for (int a : anchors) {
                const int d = qAbs(a - v);
                if (d < dist) { dist = d; best = a; }
            }
            return best;
        };
        if (h == H_New || h == H_Move) {
            // Whole rectangle: shift so that any edge lands on the nearest anchor.
            for (int axis = 0; axis < 2; ++axis) {
                const QVector<int> &anchors = axis ? ay : ax;
                const int e0 = axis ? m_sel.top() : m_sel.left();
                const int e1 = axis ? m_sel.bottom() + 1 : m_sel.right() + 1;
                int shift = 0, dist = SnapThreshold + 1, anchor = 0;
                for (int e : {e0, e1})
                    for (int a : anchors) {
                        const int d = qAbs(a - e);
                        if (d < dist) { dist = d; shift = a - e; anchor = a; }
                    }
                if (dist <= SnapThreshold) {
                    if (axis) { m_snapY = anchor; m_sel.translate(0, shift); }
                    else      { m_snapX = anchor; m_sel.translate(shift, 0); }
                }
            }
            return;
        }
        // Edge/corner resize: only the dragged edges snap.
        const bool left = h == H_L || h == H_TL || h == H_BL;
        const bool right = h == H_R || h == H_TR || h == H_BR;
        const bool top = h == H_T || h == H_TL || h == H_TR;
        const bool bottom = h == H_B || h == H_BL || h == H_BR;
        if (left) {
            const int v = nearest(ax, m_sel.left());
            if (v != m_sel.left()) { m_snapX = v; m_sel.setLeft(v); }
        }
        if (right) {
            const int v = nearest(ax, m_sel.right() + 1);
            if (v != m_sel.right() + 1) { m_snapX = v; m_sel.setRight(v - 1); }
        }
        if (top) {
            const int v = nearest(ay, m_sel.top());
            if (v != m_sel.top()) { m_snapY = v; m_sel.setTop(v); }
        }
        if (bottom) {
            const int v = nearest(ay, m_sel.bottom() + 1);
            if (v != m_sel.bottom() + 1) { m_snapY = v; m_sel.setBottom(v - 1); }
        }
        m_sel = m_sel.normalized();
    }
    void updateTip()
    {
        const QRect r = !m_hover.isNull() ? m_hover : m_sel;
        if (r.isNull()) { m_sizeTip->hide(); return; }
        m_sizeTip->setText(QString("%1 × %2").arg(r.width()).arg(r.height()));
        m_sizeTip->adjustSize();
        m_sizeTip->move(r.topLeft() - QPoint(0, m_sizeTip->height() + 4));
        m_sizeTip->show();
    }
    // The status bar holds every action icon in one of two layouts: a horizontal strip
    // flush with the selection's bottom-right corner, or a vertical strip on its right
    // side when the bottom edge has no room.
    void buildToolLayout(Qt::Orientation orientation)
    {
        if (m_toolOrientation == int(orientation)) return;
        delete m_tools->layout();
        QBoxLayout *lay = orientation == Qt::Horizontal
            ? static_cast<QBoxLayout *>(new QHBoxLayout(m_tools))
            : static_cast<QBoxLayout *>(new QVBoxLayout(m_tools));
        lay->setContentsMargins(4, 4, 4, 4);
        lay->setSpacing(2);
        for (QWidget *button : m_toolButtons) lay->addWidget(button);
        m_toolOrientation = int(orientation);
        m_tools->adjustSize();
    }
    void placeToolbar()
    {
        const QRect old = m_tools->geometry();
        const QScreen *screen = QGuiApplication::screenAt(m_sel.center() + geometry().topLeft());
        const QRect avail = (screen ? screen : QGuiApplication::primaryScreen())
                              ->availableGeometry().translated(-geometry().topLeft());
        buildToolLayout(Qt::Horizontal);
        const QSize h = m_tools->size();
        buildToolLayout(Qt::Vertical);
        const QSize v = m_tools->size();
        const int selRight = m_sel.right() + 1, selBottom = m_sel.bottom() + 1;
        const int hx = qBound(avail.left(), selRight - h.width(), avail.right() + 1 - h.width());
        if (selBottom + 6 + h.height() <= avail.bottom() + 1) {
            buildToolLayout(Qt::Horizontal);
            m_tools->move(hx, selBottom + 6);         // Default: bottom-right of the selection.
        } else if (selRight + 6 + v.width() <= avail.right() + 1) {
            buildToolLayout(Qt::Vertical);
            m_tools->move(selRight + 6,
                          qBound(avail.top(), selBottom - v.height(), avail.bottom() + 1 - v.height()));
        } else if (m_sel.top() - 6 - h.height() >= avail.top()) {
            buildToolLayout(Qt::Horizontal);
            m_tools->move(hx, m_sel.top() - 6 - h.height());
        } else {                                       // Selection covers the screen: dock inside.
            buildToolLayout(Qt::Vertical);
            m_tools->move(avail.right() + 1 - v.width() - 8, avail.bottom() + 1 - v.height() - 8);
        }
        update(m_tools->geometry().united(old).adjusted(-1, -1, 1, 1));   // Repaint both frost regions.
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
    QVector<DetectedWindow> m_windowRects;          // Detected windows (logical, overlay-local) for click-select and snapping.
    DetectionOptions m_detectionOptions;
    QPointer<WindowDetector> m_detector;
    quint64 m_queryId = 0;
    quint64 m_lastHoverId = 0;
    QPoint m_detectionPoint;
    QVector<QRect> m_hoverHistory;
    bool m_queryPending = false, m_waitingClick = false, m_expanding = false;
    bool m_pinAfterDetection = false;
    bool m_dragStarted = false;
    QRect m_hover;                                  // Detected window under the cursor; drives the mask hole while idle.
    QRect m_pressWindow;                            // Window under the press point; a click without drag selects it.
    int m_snapX = -1, m_snapY = -1;                 // Active magnetic-anchor lines while dragging.
    PixelInspector *m_inspector = nullptr;
    QPoint m_pressPos;
    QRect m_sel, m_pressSel;
    Handle m_drag = H_None;
    Mode m_mode = Selecting;
    QLabel *m_sizeTip = nullptr;
    QWidget *m_tools = nullptr;
    QVector<QWidget *> m_toolButtons;
    int m_toolOrientation = -1;
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
        QMenu *detection = m->addMenu(QStringLiteral("窗口/UI 检测"));
        QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
        for (const auto &entry : {qMakePair(QStringLiteral("自动检测窗口"), QStringLiteral("detect_windows")),
                                  qMakePair(QStringLiteral("自动检测 UI 元素"), QStringLiteral("detect_ui_elements"))}) {
            QAction *action = detection->addAction(entry.first);
            action->setCheckable(true);
            action->setChecked(settings.value(QStringLiteral("Snip/") + entry.second, entry.second == QStringLiteral("detect_windows")).toBool());
            const QString key = entry.second;
            connect(action, &QAction::toggled, this, [key](bool enabled) {
                QSettings(QStringLiteral("pinshot"), QStringLiteral("pinshot")).setValue(QStringLiteral("Snip/") + key, enabled);
            });
        }
        detection->addAction(QStringLiteral("排除程序…"), this, [] {
            QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
            bool ok = false;
            const QString text = QInputDialog::getMultiLineText(nullptr, QStringLiteral("排除窗口检测"),
                QStringLiteral("每行一个程序文件名，例如 example.exe（不区分大小写，下次截图生效）："),
                settings.value(QStringLiteral("Snip/exclude_from_detection")).toStringList().join(QLatin1Char('\n')), &ok);
            if (!ok) return;
            QStringList names;
            for (const QString &line : text.split(QLatin1Char('\n'))) {
                const QString name = line.trimmed().toLower();
                if (!name.isEmpty() && !names.contains(name)) names.append(name);
            }
            settings.setValue(QStringLiteral("Snip/exclude_from_detection"), names);
        });
        m->addSeparator();
        m->addAction(QStringLiteral("退出"), qApp, &QApplication::quit);
        m_tray.setContextMenu(m);
        m_tray.show();
    }
public slots:
    void startCapture()
    {
        if (m_capture && m_capture->isVisible()) { m_capture->raise(); m_capture->activateWindow(); return; }
        const QVector<ScreenFrame> frames = freezeScreens();   // Freeze before showing the selection overlay.
        if (frames.isEmpty()) {
            QMessageBox::warning(nullptr, QStringLiteral("pinshot"), QStringLiteral("无法读取屏幕像素"));
            return;
        }
        QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
        DetectionOptions options;
        options.windows = settings.value(QStringLiteral("Snip/detect_windows"), true).toBool();
        options.elements = settings.value(QStringLiteral("Snip/detect_ui_elements"), false).toBool();
        options.excludedPrograms = settings.value(QStringLiteral("Snip/exclude_from_detection")).toStringList();
        options.disabledUiPrograms = settings.value(QStringLiteral("Snip/crashes")).toStringList();
        auto *ov = new CaptureOverlay(frames, {}, options, true);
        m_capture = ov;
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
    QPointer<CaptureOverlay> m_capture;
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
    if (app.arguments().contains(QStringLiteral("--test-window-detection")))
        return CaptureOverlay::testWindowDetection();
    const int probe = app.arguments().indexOf(QStringLiteral("--probe-uia"));
    if (probe > 0 && probe + 2 < app.arguments().size()) {
        // Diagnostic: run the production element engines at one native-pixel screen point.
        const QPoint point(app.arguments().at(probe + 1).toInt(), app.arguments().at(probe + 2).toInt());
        const HRESULT com = CoInitialize(nullptr);
        UiaAutomation *automation = nullptr;
        if (SUCCEEDED(com))
            CoCreateInstance(kUiaClassId, nullptr, CLSCTX_INPROC_SERVER, kUiaIid,
                             reinterpret_cast<void **>(&automation));
        QElapsedTimer t;
        t.start();
        const WindowTargets targets = detectWindowRects();
        const qint64 enumMs = t.elapsed();
        std::printf("detectWindowRects %lldms (%d windows)\n", enumMs, targets.size());
        for (const DetectedWindow &win : targets) {
            if (!win.native.contains(point)) continue;
            QStringList slow;
            QElapsedTimer total, t;
            total.start();
            t.start();
            const QRect uia = uiaRectAt(automation, win.hwnd, point, win.native, QRect(), &slow, win.program);
            const qint64 uiaMs = t.restart();
            const QRect msaa = SUCCEEDED(com) ? accessibleAt(win.hwnd, point, win.native) : QRect();
            const qint64 msaaMs = t.restart();
            const QRect child = childAt(win, point);
            const qint64 childMs = t.elapsed();
            std::printf("window (%d,%d %dx%d) program=%s\nuia    (%d,%d %dx%d) %lldms\nmsaa   (%d,%d %dx%d) %lldms\nchild  (%d,%d %dx%d) %lldms\ntotal query %lldms\n",
                        win.native.x(), win.native.y(), win.native.width(), win.native.height(),
                        qPrintable(win.program),
                        uia.x(), uia.y(), uia.width(), uia.height(), uiaMs,
                        msaa.x(), msaa.y(), msaa.width(), msaa.height(), msaaMs,
                        child.x(), child.y(), child.width(), child.height(), childMs,
                        total.elapsed());
            break;
        }
        if (automation) automation->Release();
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--dump-windows"))) {
        // Diagnostic: print detected targets in global logical coordinates.
        const QVector<ScreenFrame> frames = freezeScreens();
        if (frames.isEmpty()) { std::puts("no frames"); return 1; }
        for (const DetectedWindow &w : detectWindowRects()) {
            const QRect top = logicalFromNative(frames, w.native);
            std::printf("top  (%d,%d %dx%d)", top.x(), top.y(), top.width(), top.height());
            for (const QRect &c : w.nativeChildren) {
                const QRect lc = logicalFromNative(frames, c);
                std::printf("  child (%d,%d %dx%d)", lc.x(), lc.y(), lc.width(), lc.height());
            }
            std::printf("\n");
        }
        return 0;
    }
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
