#include "long_capture.h"
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QRect>
#include <QString>
#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QLabel>
#include <QtMath>
#include <QToolButton>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QStandardPaths>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QHash>
#include <qt_windows.h>

LongCapture::LongCapture(const QRect &globalRegion) : m_region(globalRegion) {}

void LongCapture::start()
{
    buildUi();
    captureFrame();                             // Capture the initial frame immediately.
}

bool LongCapture::matchFullRes(const QImage &f, int &bestK) const
{
    bestK = 0;
    if (f.size() != m_lastFrame.size()) return false;
    if (f == m_lastFrame) return true;
    const int H = f.height(), W = f.width();
    const int minOverlap = qMin(H, qMax(24, H / 8)), maxOff = H - minOverlap;
    const double dpr = m_region.width() > 0 ? double(W) / m_region.width() : 1.0;
    const int horizontal = qMax(3, qRound(3 * dpr)), vertical = qMax(2, qRound(2 * dpr));
    struct Feature { int x, y; bool repeated; };
    auto signature = [=](const QImage &image, int x, int y) {
        quint64 hash = 14695981039346656037ULL ^ quint64(x);
        for (int dy : {-vertical, 0, vertical}) {
            const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y + dy));
            for (int dx : {-4 * horizontal, -3 * horizontal, -2 * horizontal, -horizontal, 0,
                           horizontal, 2 * horizontal, 3 * horizontal, 4 * horizontal}) {
                const QRgb color = row[x + dx];
                const int packed = (qRed(color) >> 3) | ((qGreen(color) >> 3) << 5) | ((qBlue(color) >> 3) << 10);
                hash = (hash ^ quint64(packed)) * 1099511628211ULL;
            }
        }
        return hash;
    };
    auto features = [&](const QImage &image) {
        QHash<quint64, Feature> index;
        const int step = qMax(1, W / 256);
        for (int y = vertical; y < H - vertical; ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 4 * horizontal; x < W - 4 * horizontal; x += step) {
                const QRgb a = row[x], b = row[x - 1];
                if (qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b))
                    + qAbs(qBlue(a) - qBlue(b)) < 36) continue;
                const quint64 key = signature(image, x, y);
                auto found = index.find(key);
                if (found == index.end()) index.insert(key, {x, y, false});
                else if (found->x != x || y - found->y > vertical) found->repeated = true;
            }
        }
        return index;
    };
    const auto previous = features(m_lastFrame), current = features(f);
    struct Match { int x, y, offset; };
    QVector<Match> evidence;
    QVector<int> votes(2 * maxOff + 1);
    for (auto it = current.constBegin(); it != current.constEnd(); ++it) {
        if (it->repeated) continue;
        const auto before = previous.constFind(it.key());
        if (before == previous.constEnd() || before->repeated || before->x != it->x) continue;
        const int offset = before->y - it->y;
        if (qAbs(offset) > maxOff) continue;
        ++votes[offset + maxOff];
        evidence.append({it->x, it->y, offset});
    }
    int best = 0, second = 0;
    for (int cand = -maxOff; cand <= maxOff; ++cand)
        if (votes[cand + maxOff] > best
            || (votes[cand + maxOff] == best && qAbs(cand) < qAbs(bestK))) {
            best = votes[cand + maxOff];
            bestK = cand;
        }
    for (int cand = -maxOff; cand <= maxOff; ++cand)
        if (qAbs(cand - bestK) > 2) second = qMax(second, votes[cand + maxOff]);
    if (best < 12 || second * 4 >= best * 3) return false;
    const auto coveredOffset = [&](int offset, bool broad = false) {
        if (votes[offset + maxOff] < 12) return false;
        const int top = qMax(0, -offset), bottom = qMin(H, H - offset);
        int cells = 0, bandMask = 0, left = W, right = 0;
        bool covered[48] = {};
        for (const Match &match : evidence) {
            if (match.offset != offset || match.y < top || match.y >= bottom) continue;
            const int band = (match.y - top) * 6 / (bottom - top);
            const int cell = band * 8 + match.x * 8 / W;
            if (!covered[cell]) { covered[cell] = true; ++cells; }
            bandMask |= 1 << band;
            left = qMin(left, match.x);
            right = qMax(right, match.x);
        }
        return cells >= 3 && (bandMask & (bandMask - 1)) != 0 && (!broad || right - left >= W / 2);
    };
    if (!coveredOffset(bestK)) return false;
    // Unchanged edge strips with unique texture may be fixed headers/footers, not evidence of a stationary page.
    // shortcut: Narrow scroll panes can resemble local animation; add region segmentation if those must be supported.
    if (coveredOffset(0)) {
        const auto sameRow = [&](int y) {
            return memcmp(f.constScanLine(y), m_lastFrame.constScanLine(y), size_t(W) * 4) == 0;
        };
        int head = 0, tail = H;
        while (head < H && sameRow(head)) ++head;
        while (tail > head && sameRow(tail - 1)) --tail;
        int fixedVotes = 0;
        for (const Match &match : evidence)
            if (match.offset == 0 && ((head >= minOverlap && match.y + vertical < head)
                || (H - tail >= minOverlap && match.y - vertical >= tail))) ++fixedVotes;
        if (fixedVotes >= 12)
            for (int cand = -maxOff; cand <= maxOff; ++cand)
                if (qAbs(cand) > 2 && coveredOffset(cand, true)) return false;
    }
    return true;
}

void LongCapture::nextTick(int elapsedMs)
{
    if (!m_done && !m_storageFull)
        QTimer::singleShot(qMax(1, 50 - elapsedMs), this, &LongCapture::captureFrame);
}

void LongCapture::captureFrame()
{
    if (m_done || m_storageFull)
        return;
    QElapsedTimer frameClock;
    frameClock.start();
    setUiVisible(false);
    QApplication::processEvents();
    if (m_done) return;
    const QImage f = grabScreen(m_region).toImage().convertToFormat(QImage::Format_RGB32);
    setUiVisible(true);
    if (m_done) return;
    const bool wasLost = m_alignmentLost;
    const int oldRows = m_rowEnd - m_rowStart;
    m_alignmentLost = !addFrame(f) && !m_storageFull;
    if (oldRows != m_rowEnd - m_rowStart
        && (!m_previewClock.isValid() || m_previewClock.elapsed() >= 100)) {
        updateUi();
        m_previewClock.restart();
    } else if (oldRows != m_rowEnd - m_rowStart || wasLost != m_alignmentLost || m_storageFull) {
        updateLabel();
    }
    nextTick(int(frameClock.elapsed()));
}

bool LongCapture::addFrame(const QImage &f)
{
    if (f.isNull() || f.format() != QImage::Format_RGB32)
        return false;
    m_storageFull = false;
    if (m_rowStart == m_rowEnd) {                 // First frame.
        initTiles(f.width());
        if (!appendRows(f, 0, f.height())) return false;
        m_lastFrame = f;
        return true;
    }
    int k = 0;
    if (!matchFullRes(f, k))
        return false;
    const int row = m_frameRow + k;
    if (row < m_rowStart && !prependRows(f, 0, m_rowStart - row))
        return false;
    if (row + f.height() > m_rowEnd) {
        const int from = m_rowEnd - row;
        if (!appendRows(f, from, f.height() - from)) return false;
    }
    m_frameRow = row;
    m_lastFrame = f;
    return true;
}

void LongCapture::initTiles(int width)
{
    m_width = width;
    m_bytesPerLine = width * 4;
    m_tileMaxRows = qMax(64, (64 << 20) / m_bytesPerLine);  // Allocate up to 64 MB of rows per growth step.
}

bool LongCapture::reserveRows(int head, int tail)
{
    if (!m_buf.isNull() && m_headPad >= head && m_tailPad >= tail)
        return true;
    const int used = m_rowEnd - m_rowStart;
    const qint64 needed = qint64(used) + head + tail;
    const int maxRows = int((qint64(1) << 30) / m_bytesPerLine);
    // Bound buffer memory, not image height; leave the existing capture intact if allocation fails.
    if (needed > maxRows) { m_storageFull = true; return false; }
    const int height = int(qMin(qint64(maxRows), qMax(needed, qint64(m_buf.height()) + m_tileMaxRows)));
    const int newHead = int(qMin(qint64(height - used - tail),
                                head ? qint64(head) + m_tileMaxRows : qint64(m_headPad)));
    QImage next(m_width, height, QImage::Format_RGB32);
    if (next.isNull()) { m_storageFull = true; return false; }
    if (used)
        memcpy(next.scanLine(newHead), m_buf.constScanLine(m_headPad), size_t(used) * m_bytesPerLine);
    m_buf = next;
    m_headPad = newHead;
    m_tailPad = height - newHead - used;
    m_bufStart = m_rowStart - newHead;
    return true;
}

bool LongCapture::appendRows(const QImage &f, int from, int count)
{
    if (count <= 0) return true;
    if (from < 0 || count > f.height() - from || !reserveRows(0, count)) return false;
    const int dstY = m_rowEnd - m_bufStart;
    memcpy(m_buf.scanLine(dstY), f.constScanLine(from), size_t(count) * m_bytesPerLine);
    m_rowEnd += count;
    m_tailPad -= count;
    return true;
}

bool LongCapture::prependRows(const QImage &f, int from, int count)
{
    if (count <= 0) return true;
    if (from < 0 || count > f.height() - from || !reserveRows(count, 0)) return false;
    const int dstY = m_rowStart - m_bufStart - count;
    memcpy(m_buf.scanLine(dstY), f.constScanLine(from), size_t(count) * m_bytesPerLine);
    m_rowStart -= count;
    m_headPad -= count;
    return true;
}

QImage LongCapture::toImage() const
{
    if (m_buf.isNull() || m_rowEnd <= m_rowStart)
        return QImage();
    return m_buf.copy(0, m_rowStart - m_bufStart, m_width, m_rowEnd - m_rowStart);
}

void LongCapture::buildUi()
{
    const QString barQss =
        "QWidget{background:#f2f2f2;border:none;}"
        "QLabel{color:#323232;font-size:13px;padding:0 8px;}"
        "QToolButton{background:transparent;border:none;border-radius:0px;padding:4px;}"
        "QToolButton:hover{background-color:#d4d4d4;}";
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
    m_preview->setStyleSheet("background:#f2f2f2;border:1px solid #2080f0;");
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
    m_frame->setStyleSheet("background:#2080f0;");
    m_frame->setGeometry(m_region.adjusted(-2, -2, 2, 2));
    const QRect fr = m_frame->rect();
    m_frame->setMask(QRegion(fr) - QRegion(fr.adjusted(2, 2, -2, -2)));
    m_frame->show();
}

void LongCapture::setUiVisible(bool v)
{
    m_bar->setVisible(v);
    if (!m_affinityOk) m_preview->setVisible(v);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void LongCapture::updateLabel()
{
    if (m_label)
        m_label->setText(m_storageFull ? QStringLiteral("内存不足或达到安全上限，已暂停延伸，请保存或贴图")
            : m_alignmentLost ? QStringLiteral("无法对齐，请缓慢滚回上一位置")
                              : QStringLiteral("● 长截图 · %1 px").arg(m_rowEnd - m_rowStart));
    if (m_bar)
        m_bar->adjustSize();
}

void LongCapture::updateUi()
{
    updateLabel();
    if (m_preview && m_rowEnd > m_rowStart) {
        const int rows = m_rowEnd - m_rowStart;
        const double s = qMin(double(m_preview->width()) / m_width,
                              double(m_preview->height()) / rows);
        const QImage view(m_buf.constScanLine(m_headPad), m_width, rows, m_bytesPerLine, QImage::Format_RGB32);
        const QImage scaled = view.scaled(qMax(1, qRound(m_width * s)), qMax(1, qRound(rows * s)),
                                         Qt::IgnoreAspectRatio, Qt::FastTransformation);
        m_preview->setPixmap(QPixmap::fromImage(scaled));
    }
}

void LongCapture::finish()
{
    if (m_done)
        return;
    const QPixmap pix = QPixmap::fromImage(toImage());
    if (pix.isNull()) {
        if (m_label) m_label->setText(QStringLiteral("导出失败，请释放内存后重试"));
        return;
    }
    m_done = true;
    cleanupUi();
    emit finished(pix);
    deleteLater();
}

void LongCapture::cancel()
{
    if (m_done)
        return;
    m_done = true;
    cleanupUi();
    deleteLater();
}

void LongCapture::saveCapture()
{
    const QImage img = toImage();
    if (img.isNull()) {
        if (m_label) m_label->setText(QStringLiteral("导出失败，请释放内存后重试"));
        return;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getSaveFileName(
        nullptr, QStringLiteral("保存长截图"), dir + QStringLiteral("/longshot.png"),
        QStringLiteral("PNG (*.png)"));            // Save long captures as PNG.
    if (path.isEmpty())
        return;                                    // Cancelled dialog: keep the session active.
    if (!saveImageAtomic(img, path)) {
        if (m_label) m_label->setText(QStringLiteral("保存失败，请检查路径或释放内存后重试"));
        return;
    }
    cancel();
}

void LongCapture::copyCapture()
{
    const QImage img = toImage();
    if (img.isNull()) {
        if (m_label) m_label->setText(QStringLiteral("导出失败，请释放内存后重试"));
        return;
    }
    if (!copyImageToClipboard(img)) {
        if (m_label) m_label->setText(QStringLiteral("复制失败，剪贴板可能被占用，请重试"));
        return;
    }
    cancel();
}

void LongCapture::cleanupUi()
{
    if (m_bar) { m_bar->hide(); m_bar->deleteLater(); m_bar = nullptr; }
    if (m_preview) { m_preview->hide(); m_preview->deleteLater(); m_preview = nullptr; }
    if (m_frame) { m_frame->hide(); m_frame->deleteLater(); m_frame = nullptr; }
}
