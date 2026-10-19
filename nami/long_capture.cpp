#include "long_capture.h"
#include "ocr_engine.h"
#include <QGuiApplication>
#include <QCoreApplication>
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
#include <QComboBox>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QStandardPaths>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QHash>
#include <QCursor>
#include <QPainter>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QMessageBox>
#include <QCheckBox>
#include <QSettings>
#include <QSignalBlocker>
#include <qt_windows.h>

namespace {
constexpr int kPreviewPad = 4, kPreviewGap = 15, kPreviewCross = 200, kPreviewTopMargin = 50;
constexpr int kBorder = 3;                      // Selection outline, drawn outside the region.
constexpr int kMaxGatedLength = 29001;          // Edit, OCR, pin and copy accept shorter images only.
const QColor kMatchColor(0x95, 0xec, 0x69);

QIcon rotatedIcon(const QIcon &icon, int size, int angle = -90)
{
    QIcon out;
    for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active, QIcon::Selected})
        for (QIcon::State state : {QIcon::Off, QIcon::On})
            for (qreal scale : {1.0, 1.5, 2.0}) {
                QPixmap pm = icon.pixmap(QSize(size, size), scale, mode, state).transformed(QTransform().rotate(angle));
                pm.setDevicePixelRatio(scale);
                out.addPixmap(pm, mode, state);
            }
    return out;
}

QScreen *screenFor(const QRect &region)
{
    QScreen *screen = QGuiApplication::screenAt(region.center());
    return screen ? screen : QGuiApplication::primaryScreen();
}
}

LongPreview::LongPreview()
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                           | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
}

void LongPreview::paintEvent(QPaintEvent *)
{
    if (image.isNull()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QImage shown = image;
    if (!atStart || !atEnd) {                    // Fades mark the ends that continue past the slice.
        const QSize logical = (QSizeF(shown.size()) / shown.devicePixelRatio()).toSize();
        const int fade = qMax(1, qRound((vertical ? logical.height() : logical.width()) * 0.2));
        const QColor dark(0, 0, 0, 0xa0), clear(0, 0, 0, 0);
        QPainter ip(&shown);
        const auto band = [&](bool start) {
            const int from = start ? 0 : qMax(0, (vertical ? logical.height() : logical.width()) - fade);
            QLinearGradient g = vertical ? QLinearGradient(0, from, 0, from + fade) : QLinearGradient(from, 0, from + fade, 0);
            g.setColorAt(0, start ? dark : clear);
            g.setColorAt(1, start ? clear : dark);
            ip.fillRect(vertical ? QRect(0, from, logical.width(), fade) : QRect(from, 0, fade, logical.height()), g);
        };
        if (!atStart) band(true);
        if (!atEnd) band(false);
    }
    p.drawImage(imageRect, shown);
    const QRect outline = imageRect.marginsAdded(QMargins(kPreviewPad / 2, kPreviewPad / 2, kPreviewPad / 2, kPreviewPad / 2));
    QPen pen(Bar::accent, kPreviewPad, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    p.setPen(pen);
    p.drawRect(outline);
    // Chevrons point toward content beyond the slice.
    const auto chevron = [&](bool start) {
        const int along = vertical ? outline.width() : outline.height();
        const int across = vertical ? outline.height() : outline.width();
        const int half = qMax(4, qMin(10, along / 14)), depth = qMax(4, qMin(10, across / 18));
        p.setPen(QPen(Qt::white, kPreviewPad, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPoint c = outline.center();
        if (vertical) {
            const int tip = start ? outline.top() + depth : outline.bottom() - depth;
            const int wing = start ? tip + depth : tip - depth;
            p.drawPolyline(QPolygon{QPoint(c.x() - half, wing), QPoint(c.x(), tip), QPoint(c.x() + half, wing)});
        } else {
            const int tip = start ? outline.left() + depth : outline.right() - depth;
            const int wing = start ? tip + depth : tip - depth;
            p.drawPolyline(QPolygon{QPoint(wing, c.y() - half), QPoint(tip, c.y()), QPoint(wing, c.y() + half)});
        }
    };
    if (!atStart) chevron(true);
    if (!atEnd) chevron(false);
    if (showFrame) {
        p.setPen(QPen(kMatchColor, kPreviewPad, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        p.drawRect(frameRect.marginsAdded(QMargins(kPreviewPad / 2, kPreviewPad / 2, kPreviewPad / 2, kPreviewPad / 2)));
    }
}

LongCapture::LongCapture(const QRect &globalRegion) : m_region(globalRegion) {}

void LongCapture::start()
{
    buildUi();
    captureFrame();                             // Capture the initial frame immediately.
}

int LongCapture::autoScrollStep(int extent)
{
    return extent < 300 ? 25 : extent < 500 ? 35 : extent < 700 ? 45 : extent < 900 ? 55 : 65;
}

void LongCapture::scrollOnce()
{
    const int step = autoScrollStep(m_vertical ? m_region.height() : m_region.width());
    // Wheel data and WM_MOUSEWHEEL share signs: negative is down, positive is up; horizontal is positive right.
    const int delta = m_autoScroll * (m_vertical ? -step : step);
    const QPoint cursor = QCursor::pos();
    // The pointer stays where the user clicked; only a pointer resting on the page itself follows the wheel.
    const bool overUi = (m_bar && m_bar->isVisible() && m_bar->geometry().contains(cursor))
        || (m_cropPanel && m_cropPanel->isVisible() && m_cropPanel->geometry().contains(cursor));
    if (m_region.contains(cursor) && !overUi) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = m_vertical ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = DWORD(delta);
        SendInput(1, &input, sizeof(input));
        return;
    }
    // The pointer left the selection: scroll the page under it directly so capturing does not stop.
    // shortcut: Direct wheel messages can miss apps that only accept hardware input; keep the pointer over the page for those.
    const QRect native = nativeFromLogical(m_region);
    const QPoint at = native.isEmpty() ? m_region.center() : native.center();
    if (HWND hwnd = WindowFromPoint(POINT{at.x(), at.y()}))
        PostMessageW(hwnd, m_vertical ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL,
                     MAKEWPARAM(0, delta), MAKELPARAM(at.x(), at.y()));
}


QRect LongCapture::editorGeometry(const QSize &pixels, qreal dpr, const QRect &available)
{
    if (pixels.isEmpty() || available.isEmpty()) return {};
    constexpr int kBarRoom = 48;                 // Pin toolbar height plus its gap under the image.
    const int margin = PinWindow::kShadowMargin;
    const QRect room = available.adjusted(margin, margin, -margin, -margin);
    const QSizeF logical = QSizeF(pixels) / qMax(1.0, dpr);
    const double fitW = room.width() / logical.width(), fitH = room.height() / logical.height();
    double scale = qMin(1.0, qMin(fitW, fitH));
    // Shrinking reserves the bar's room above and below, so the image center stays on the screen center.
    if (scale < 1.0)
        scale = qMin(scale, qMin(fitW, qMax(1, room.height() - 2 * kBarRoom) / logical.height()));
    QRect r(QPoint(), QSize(qMax(1, qFloor(logical.width() * scale)), qMax(1, qFloor(logical.height() * scale))));
    r.moveCenter(available.center());
    return r;
}

QImage LongCapture::transposed(const QImage &src)
{
    QImage dst(src.height(), src.width(), QImage::Format_RGB32);
    if (dst.isNull() || src.isNull()) return QImage();
    uchar *bits = dst.bits();
    const qsizetype bpl = dst.bytesPerLine();
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x)
            reinterpret_cast<QRgb *>(bits + x * bpl)[y] = row[x];
    }
    return dst;
}

bool LongCapture::matchFullRes(const QImage &f, int &bestK) const
{
    bestK = 0;
    if (f.size() != m_lastFrame.size()) return false;
    if (f == m_lastFrame) return true;
    const int H = f.height(), W = f.width();
    const int minOverlap = qMin(H, qMax(24, H / 8)), maxOff = H - minOverlap;
    const double dpr = crossExtent() > 0 ? double(W) / crossExtent() : 1.0;
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
    if (!m_paused && !m_suspended) {
        if (m_autoScroll && (!m_scrollClock.isValid() || m_scrollClock.elapsed() >= 100)) {
            scrollOnce();
            m_scrollClock.restart();
        }
        setUiVisible(false);
        if (m_done) return;
        const QImage raw = grabScreen(m_region).toImage().convertToFormat(QImage::Format_RGB32);
        setUiVisible(true);
        if (m_done) return;
        const int oldStart = m_rowStart, oldEnd = m_rowEnd, oldFrame = m_frameRow;
        const bool wasLost = m_alignmentLost;
        const bool aligned = ingest(raw);
        m_alignmentLost = !aligned && !m_storageFull;
        if (aligned && oldStart != oldEnd)
            autoCrop(m_rowStart < oldStart ? -1 : m_rowEnd > oldEnd ? 1 : 0);
        if (oldStart != m_rowStart || oldEnd != m_rowEnd || oldFrame != m_frameRow
            || wasLost != m_alignmentLost || m_storageFull)
            m_pending = true;
        if (m_pending && (!m_previewClock.isValid() || m_previewClock.elapsed() >= 100)) {
            updateUi();
            m_previewClock.restart();
            m_pending = false;
        }
        if (!m_alignmentLost) m_failClock.invalidate();
        else if (!m_failClock.isValid()) m_failClock.start();
        else if (!m_failWarned && m_failClock.elapsed() >= 3000) {
            m_failWarned = true;
            notice(tr("匹配失败"),
                   tr("图片匹配失败，可能原因：\n1. 滚动速度过快。\n2. 截图过程中切换了其他窗口。\n3. 截图背景为动态图像。\n\n请尝试返回最后一次成功匹配的帧后继续截图。"),
                   QStringLiteral("LongShot/MatchFailWarningNoAsk"));
        }
        if (m_storageFull && !m_storageWarned) {
            m_storageWarned = true;
            updateUi();
            notice(tr("长截图"),
                   tr("当前截图已达到最大拼接范围，无法继续拼接。\n请保存或贴图。"), QString());
        }
    }
    nextTick(int(frameClock.elapsed()));
}

bool LongCapture::ingest(const QImage &raw)
{
    return addFrame(m_vertical ? raw : transposed(raw));
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

void LongCapture::resetCapture()
{
    m_lastFrame = QImage();
    m_buf = QImage();
    m_frameRow = m_bufStart = m_rowStart = m_rowEnd = m_headPad = m_tailPad = 0;
    m_sliceStart = m_sliceFrame = 0;
    m_alignmentLost = m_storageFull = m_storageWarned = false;
    m_cropState = m_cropBase = 0;
    m_failClock.invalidate();
    if (m_preview) { m_preview->image = QImage(); m_preview->hide(); }
    updateUi();
}

bool LongCapture::cropRows(int from, int to)
{
    const int len = logicalLength();
    if (from < 0 || from >= to || to > len) return false;
    // Rows stay in the buffer; only the coverage shrinks.
    m_headPad += from;
    m_tailPad += len - to;
    m_rowEnd = m_rowStart + to;
    m_rowStart += from;
    // The reference frame must lie inside the kept rows; when it was cut away, the kept rows become the reference,
    // so frames of the removed part fail to match instead of landing at a wrong offset.
    const int frameRows = m_lastFrame.isNull() ? 0 : m_lastFrame.height();
    const int frameRow = qBound(m_rowStart, m_frameRow, qMax(m_rowStart, m_rowEnd - frameRows));
    if (frameRow != m_frameRow && frameRows && frameRows <= m_rowEnd - m_rowStart)
        m_lastFrame = m_buf.copy(0, frameRow - m_bufStart, m_width, frameRows);
    m_frameRow = frameRow;
    m_cropBase = m_frameRow - m_rowStart;
    m_alignmentLost = false;
    m_failClock.invalidate();
    if (m_storageFull) {                          // Freed room lets capturing continue.
        m_storageFull = m_storageWarned = false;
        nextTick(0);
    }
    if (m_bar) updateUi();
    return true;
}

void LongCapture::cropBefore()
{
    const int len = logicalLength(), extent = m_lastFrame.height(), offset = m_frameRow - m_rowStart;
    int from = offset;
    if (from < 1) {                               // Already at the start: keep the last frame's worth.
        if (len <= extent) return;
        from = len - extent;
    }
    cropRows(from, len);
}

void LongCapture::cropAfter()
{
    const int end = m_frameRow - m_rowStart + m_lastFrame.height();
    if (end < logicalLength()) cropRows(0, end);
}

void LongCapture::autoCrop(int growth)
{
    if (!m_autoCrop || m_lastFrame.isNull()) return;
    const int len = logicalLength(), extent = m_lastFrame.height(), offset = m_frameRow - m_rowStart;
    if (qAbs(len - extent) < 5 && m_cropState) { m_cropState = 0; m_cropBase = offset; }
    if (!m_cropState) {                           // The first growth sets the direction being captured.
        if (growth) m_cropState = growth;
        m_cropBase = offset;
        return;
    }
    const int moved = growth ? growth : (offset > m_cropBase) - (offset < m_cropBase);
    if (moved && moved != m_cropState) {          // Scrolling turned back: drop what lies past the frame.
        if (m_cropState == 1 && offset + extent < len) { cropRows(0, offset + extent); return; }
        if (m_cropState == -1 && offset > 0) { cropRows(offset, len); return; }
    }
    m_cropBase = offset;
}

void LongCapture::setAutoCrop(bool on)
{
    m_autoCrop = on;
    QSettings(QStringLiteral("Nami"), QStringLiteral("Nami")).setValue(QStringLiteral("LongShot/AutoCrop"), on);
    if (m_autoCropBox && m_autoCropBox->isChecked() != on) {
        const QSignalBlocker block(m_autoCropBox);
        m_autoCropBox->setChecked(on);
    }
    if (on) { m_cropState = 0; m_cropBase = m_frameRow - m_rowStart; }
}

void LongCapture::updateCropDirection()
{
    if (!m_cropStart || !m_cropEnd) return;
    for (QToolButton *b : {m_cropStart, m_cropEnd}) {
        const bool start = b == m_cropStart;
        const QString tip = m_vertical ? (start ? tr("裁剪顶部") : tr("裁剪底部"))
                                       : (start ? tr("裁剪左侧") : tr("裁剪右侧"));
        b->setToolTip(tip);
        b->setAccessibleName(tip);
    }
    m_cropStart->setIcon(m_vertical ? barIcon(Icon::CropStart) : rotatedIcon(barIcon(Icon::CropStart), Bar::icon));
    m_cropEnd->setIcon(m_vertical ? barIcon(Icon::CropEnd) : rotatedIcon(barIcon(Icon::CropEnd), Bar::icon));
}

void LongCapture::refreshCropPanel()
{
    // Visible changes apply at once from hidden and after a short delay from shown.
    if (m_cropTimer) m_cropTimer->start(m_cropPanel && m_cropPanel->isVisible() ? 200 : 0);
}

void LongCapture::placeCropPanel()
{
    if (!m_cropPanel || !m_crop || !m_bar) return;
    const QRect av = screenFor(m_region)->availableGeometry();
    m_cropPanel->adjustSize();
    const QSize s = m_cropPanel->size();
    const QRect bar = m_bar->geometry();
    // Centered under the crop button, above the bar when there is no room below.
    const int x = m_crop->mapToGlobal(QPoint(m_crop->width() / 2, 0)).x() - s.width() / 2;
    int y = bar.bottom() + 1 + 4;
    if (y + s.height() > av.bottom() + 1) y = bar.top() - 4 - s.height();
    m_cropPanel->move(qBound(av.left(), x, qMax(av.left(), av.right() + 1 - s.width())), y);
}

int LongCapture::logicalLength() const
{
    return m_rowEnd - m_rowStart;
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

QImage LongCapture::exportImage() const
{
    const QImage rows = toImage();
    return m_vertical || rows.isNull() ? rows : transposed(rows);
}

void LongCapture::buildUi()
{
    const QScreen *scr = screenFor(m_region);

    // Full-screen dim layer with a clear hole and an accent outline; input passes through to the page.
    m_mask = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                   | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
    m_mask->setAttribute(Qt::WA_TranslucentBackground);
    m_mask->setAttribute(Qt::WA_ShowWithoutActivating);
    m_mask->setGeometry(scr->geometry());
    m_mask->installEventFilter(this);
    m_mask->show();

    m_bar = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                  | Qt::WindowDoesNotAcceptFocus);
    m_bar->setObjectName(QStringLiteral("longCaptureBar"));
    m_bar->setAttribute(Qt::WA_TranslucentBackground);
    m_bar->setAttribute(Qt::WA_ShowWithoutActivating);
    m_bar->installEventFilter(this);
    m_bar->setStyleSheet(QStringLiteral(
        "QToolButton{color:#fcfcfc;background:transparent;border:none;border-radius:4px;padding:5px;}"
        "QToolButton:hover{background-color:#26262a;}"
        "QToolButton:pressed{background-color:#27272a;}"
        "QToolButton:checked{background:transparent;}"
        "QToolButton:checked:hover{background-color:#26262a;}"
        "QFrame#barSeparator{background-color:#27272a;margin:0px 6px;border:none;}"
        "QLabel{color:#a1a1aa;font-size:12px;background:transparent;padding:0px 4px;}"
        "QComboBox{color:#fcfcfc;background:#060607;border:none;border-radius:12px;"
        "padding:1px 4px 1px 12px;font-size:12px;}"
        "QComboBox:hover{background:#0c0c0e;}"
        "QComboBox::drop-down{border:none;width:22px;background:transparent;}"
        "QComboBox::down-arrow{image:none;width:0px;height:0px;}"
        "QComboBox QAbstractItemView{outline:0px;color:#fcfcfc;background:#18181b;border:1px solid #27272a;"
        "padding:4px;selection-background-color:#0485f7;selection-color:#fcfcfc;}"));
    auto *lay = new QHBoxLayout(m_bar);
    lay->setContentsMargins(7, 6, 7, 7);             // 42px surface inside a 1px rim.
    lay->setSpacing(4);
    m_grip = new QWidget;
    m_grip->setFixedSize(16, Bar::button);
    m_grip->setCursor(Qt::SizeAllCursor);
    m_grip->installEventFilter(this);
    m_label = new QLabel;
    m_axis = new QWidget;
    m_axis->setFixedSize(Bar::button, Bar::button);
    m_axis->setToolTip(tr("移动截图区域"));
    m_axis->setCursor(Qt::SizeVerCursor);
    m_axis->installEventFilter(this);
    m_dir = flatComboBox();
    m_dir->addItems({tr("纵向"), tr("横向")});
    m_dir->setFixedSize(qMax(98, m_dir->sizeHint().width()), 24);
    m_dir->setFocusPolicy(Qt::NoFocus);
    m_autoUp = barButton(Icon::LongShot, tr("自动向上滚动截图"));
    m_autoUp->setCheckable(true);
    m_autoUp->setIcon(rotatedIcon(barIcon(Icon::LongShot), Bar::icon, 180));
    m_auto = barButton(Icon::LongShot, tr("自动向下滚动截图"));
    m_auto->setCheckable(true);
    m_crop = barButton(Icon::Crop, tr("裁剪"));
    m_crop->installEventFilter(this);
    // Hover panel under the crop button: crop to the current frame at either end, and the auto crop switch.
    m_cropPanel = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                           | Qt::WindowDoesNotAcceptFocus);
    m_cropPanel->setObjectName(QStringLiteral("longCaptureCropPanel"));
    m_cropPanel->setAttribute(Qt::WA_TranslucentBackground);
    m_cropPanel->setAttribute(Qt::WA_ShowWithoutActivating);
    m_cropPanel->installEventFilter(this);
    m_cropPanel->setStyleSheet(QStringLiteral(
        "QToolButton{background:transparent;border:none;border-radius:4px;padding:5px;}"
        "QToolButton:hover{background-color:#26262a;}"
        "QToolButton:pressed{background-color:#27272a;}"
        "QFrame#barSeparator{background-color:#27272a;margin:0px 6px;border:none;}"
        "QCheckBox{color:#fcfcfc;font-size:12px;background:transparent;spacing:6px;}"));
    auto *cropLay = new QHBoxLayout(m_cropPanel);
    cropLay->setContentsMargins(8, 4, 8, 4);
    cropLay->setSpacing(4);
    m_cropStart = barButton(Icon::CropStart, QString());
    m_cropEnd = barButton(Icon::CropEnd, QString());
    m_autoCropBox = new QCheckBox(tr("自动裁剪"));
    m_autoCropBox->setToolTip(tr("自动裁剪"));
    m_autoCropBox->setCursor(Qt::PointingHandCursor);
    for (QWidget *w : {static_cast<QWidget *>(m_cropStart), static_cast<QWidget *>(m_cropEnd),
                       barSeparator(true), static_cast<QWidget *>(m_autoCropBox)}) {
        w->setFocusPolicy(Qt::NoFocus);
        cropLay->addWidget(w);
    }
    m_autoCrop = QSettings(QStringLiteral("Nami"), QStringLiteral("Nami"))
                     .value(QStringLiteral("LongShot/AutoCrop"), false).toBool();
    m_autoCropBox->setChecked(m_autoCrop);
    updateCropDirection();
    m_cropTimer = new QTimer(this);
    m_cropTimer->setSingleShot(true);
    connect(m_cropTimer, &QTimer::timeout, this, [this] {
        if (!m_cropPanel || m_hidCropPanel) return;
        const bool show = (m_cropHover || m_panelHover) && m_crop && m_crop->isEnabled() && !m_suspended;
        if (show == m_cropPanel->isVisible()) return;
        if (show) { placeCropPanel(); m_cropPanel->show(); m_cropPanel->raise(); }
        else m_cropPanel->hide();
    });
    connect(m_cropStart, &QToolButton::clicked, this, &LongCapture::cropBefore);
    connect(m_cropEnd, &QToolButton::clicked, this, &LongCapture::cropAfter);
    connect(m_autoCropBox, &QCheckBox::toggled, this, &LongCapture::setAutoCrop);
    auto *edit = barButton(Icon::Pen, tr("编辑"));
    m_ocr = barButton(Icon::Ocr, tr("文字识别 (OCR)"));
    m_translate = barButton(Icon::Translate, tr("识别并翻译文字"));
    m_barcode = barButton(Icon::Barcode, tr("识别二维码/条形码"));
    m_barcode->setObjectName(QStringLiteral("barcodeButton"));
    m_ocrMissing = OcrEngine::assetError(QCoreApplication::applicationDirPath());
    if (!m_ocrMissing.isEmpty()) m_ocr->setToolTip(m_ocrMissing);
    if (!m_ocrMissing.isEmpty()) m_translate->setToolTip(m_ocrMissing);
    auto *pin = barButton(Icon::Pin, tr("贴图"));
    auto *save = barButton(Icon::Save, tr("保存"));
    auto *close = barButton(Icon::Cancel, tr("关闭"));
    auto *copy = barButton(Icon::Copy, tr("复制并关闭"));
    m_gated = {edit, m_ocr, m_translate, m_barcode, pin, copy};
    m_save = save;
    lay->addWidget(m_grip);
    lay->addWidget(m_label);
    lay->addWidget(m_axis);
    lay->addWidget(barSeparator(true));
    lay->addWidget(m_dir);
    for (QToolButton *b : {m_autoUp, m_auto, m_crop, edit, m_ocr, m_translate, m_barcode, pin, save, close, copy}) {
        b->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(b);
    }
    connect(m_dir, &QComboBox::currentIndexChanged, this, [this](int index) { setVertical(index == 0); });
    connect(m_autoUp, &QToolButton::toggled, this, [this](bool on) { setAutoScroll(on ? -1 : 0); });
    connect(m_auto, &QToolButton::toggled, this, [this](bool on) { setAutoScroll(on ? 1 : 0); });
    connect(edit, &QToolButton::clicked, this, [this] { finish(true); });
    connect(m_ocr, &QToolButton::clicked, this, [this] { openTextCard(false); });
    connect(m_translate, &QToolButton::clicked, this, [this] { openTextCard(true); });
    connect(m_barcode, &QToolButton::clicked, this, [this] { openTextCard(false,true); });
    connect(pin, &QToolButton::clicked, this, [this] { finish(false); });
    connect(save, &QToolButton::clicked, this, &LongCapture::saveCapture);
    connect(close, &QToolButton::clicked, this, &LongCapture::cancel);
    connect(copy, &QToolButton::clicked, this, &LongCapture::copyCapture);
    updateLabel();
    updateButtons();
    m_bar->show();
}

void LongCapture::placeBar()
{
    if (!m_bar || m_barMoved) return;
    const QRect av = screenFor(m_region)->availableGeometry();
    m_bar->adjustSize();
    const QSize s = m_bar->size();
    // Right-aligned below the outline; above it when there is no room, inside the bottom edge as a last resort.
    const int x = qBound(av.left(), m_region.right() + 2 - s.width(), qMax(av.left(), av.right() + 1 - s.width()));
    int y = m_region.bottom() + 1 + kBorder + 4;
    if (y + s.height() > av.bottom() + 1) {
        y = m_region.top() - kBorder - 4 - s.height();
        if (y < av.top()) y = m_region.bottom() + 1 - 8 - s.height();
    }
    m_bar->move(x, qBound(av.top(), y, qMax(av.top(), av.bottom() + 1 - s.height())));
}

void LongCapture::setUiVisible(bool v)
{
    // Capture-excluded windows stay up; the rest step aside only when they overlap the grabbed region.
    bool changed = false;
    const auto apply = [&](QWidget *w, bool excluded, bool &hidden) {
        if (!w) return;
        if (!v && !excluded && w->isVisible() && w->geometry().intersects(m_region)) {
            w->hide(); hidden = true; changed = true;
        } else if (v && hidden) {
            w->show(); hidden = false; changed = true;
        }
    };
    // The mask's hole is fully transparent over the region, so it never needs to step aside.
    apply(m_bar, m_barAffinity, m_hidBar);
    apply(m_preview, m_previewAffinity, m_hidPreview);
    const bool panelAside = m_hidCropPanel;
    apply(m_cropPanel, false, m_hidCropPanel);
    if (panelAside && !m_hidCropPanel) refreshCropPanel();   // Hover may have ended during the grab.
    if (changed) QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void LongCapture::updateLabel()
{
    if (!m_label) return;
    const int rows = logicalLength();
    const QSize size = rows <= 0 ? m_region.size() : m_vertical ? QSize(m_width, rows) : QSize(rows, m_width);
    m_label->setText(QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()));
    placeBar();
}

void LongCapture::updateButtons()
{
    const int length = logicalLength();
    const bool bounded = length > 0 && length < kMaxGatedLength;
    for (QToolButton *b : m_gated)
        b->setEnabled(bounded && ((b != m_ocr && b != m_translate) || m_ocrMissing.isEmpty()));
    if (m_save) m_save->setEnabled(length > 0);
    if (m_crop) m_crop->setEnabled(length > 0);
    // The running direction owns the toggle; the other stays grey until it is stopped.
    if (m_autoUp) m_autoUp->setEnabled(m_autoScroll <= 0);
    if (m_auto) m_auto->setEnabled(m_autoScroll >= 0);
    if (m_cropPanel && m_cropPanel->isVisible()) placeCropPanel();
}

void LongCapture::updatePreview()
{
    const int total = logicalLength();
    if (m_done || total <= 0 || m_width <= 0) return;
    if (!m_preview) m_preview = new LongPreview;
    const QScreen *scr = screenFor(m_region);
    const QRect screen = scr->geometry(), av = scr->availableGeometry();
    const qreal dpr = scr->devicePixelRatio();
    // The preview sits after the selection (and the bar, when they overlap) on the cross side.
    QRect anchor = m_region;
    if (m_bar && m_bar->isVisible()) {
        const QRect b = m_bar->geometry();
        const bool overlap = m_vertical ? b.intersects(m_region)
                                        : b.left() <= m_region.right() && m_region.left() <= b.right();
        if (overlap) anchor = anchor.united(b);
    }
    const int cross = qMin(kPreviewCross - 2 * kPreviewPad, crossExtent());
    const int windowCross = cross + 2 * kPreviewPad;
    int side = 0;                                // 0 after the selection, 1 before it, 2 inside it.
    QRect win;
    if (m_vertical) {
        win = QRect(anchor.right() + kPreviewGap, screen.top() - kPreviewPad, windowCross, screen.height() + 2 * kPreviewPad);
        if (win.right() > av.right()) {
            win.moveLeft(anchor.left() - kPreviewGap - win.width());
            side = 1;
            if (win.left() < av.left()) { win = QRect(m_region.left(), m_region.top(), qMin(windowCross, m_region.width()), m_region.height()); side = 2; }
        }
    } else {
        win = QRect(screen.left() - kPreviewPad, anchor.bottom() + kPreviewGap, screen.width() + 2 * kPreviewPad, windowCross);
        if (win.bottom() > av.bottom()) {
            win.moveTop(anchor.top() - kPreviewGap - win.height());
            side = 1;
            if (win.top() < av.top()) { win = QRect(m_region.left(), m_region.top(), m_region.width(), qMin(windowCross, m_region.height())); side = 2; }
        }
    }
    // Slice length: from the selection's far edge back toward the screen edge, less a margin.
    const double scale = double(cross) / m_width;          // Preview logical pixels per stored row.
    const int avail = m_vertical ? m_region.bottom() + 1 - 2 * kPreviewPad - (kPreviewTopMargin + screen.top())
                                 : m_region.right() + 1 - 2 * kPreviewPad - (kPreviewTopMargin + screen.left());
    const int len = qMin(total, qMax(1, int(qMax(1, avail) / scale)));
    const int frameTop = m_frameRow - m_rowStart, frameRows = m_lastFrame.isNull() ? total : m_lastFrame.height();
    int start = m_sliceStart - m_rowStart;
    if (m_rowStart < m_previewHead) start = 0;                       // Grown at the head: show the head.
    else if (m_rowEnd > m_previewTail) start = total - len;          // Grown at the tail: show the tail.
    else {                                                           // Within range: follow the frame.
        const int center = frameTop + frameRows / 2, dir = m_frameRow - m_sliceFrame;
        if ((dir < 0 && center < start + len / 2) || (dir > 0 && center > start + len / 2)) start = center - len / 2;
    }
    if (start > frameTop) start = frameTop;
    if (start + len < frameTop + frameRows) start = frameTop + frameRows - len;
    start = qBound(0, start, total - len);
    m_sliceStart = m_rowStart + start;
    m_sliceFrame = m_frameRow;
    m_previewHead = m_rowStart;
    m_previewTail = m_rowEnd;

    const QImage view(m_buf.constScanLine(m_headPad + start), m_width, len, m_bytesPerLine, QImage::Format_RGB32);
    const int along = qMax(1, qRound(len * scale));
    QImage slice = (m_vertical ? view : transposed(view))
        .scaled(qMax(1, qRound((m_vertical ? cross : along) * dpr)), qMax(1, qRound((m_vertical ? along : cross) * dpr)),
                Qt::IgnoreAspectRatio, Qt::FastTransformation);
    slice.setDevicePixelRatio(dpr);
    QRect image;
    if (m_vertical) {
        const int x = side == 1 ? win.width() - kPreviewPad - cross : kPreviewPad;
        image = QRect(x, m_region.bottom() + 1 - kPreviewPad - win.top() - along, cross, along);
    } else {
        const int y = side == 1 ? win.height() - kPreviewPad - cross : kPreviewPad;
        image = QRect(m_region.right() + 1 - kPreviewPad - win.left() - along, y, along, cross);
    }
    const int frameOffset = qRound((frameTop - start) * scale), frameAlong = qMax(1, qRound(frameRows * scale));
    m_preview->vertical = m_vertical;
    m_preview->image = slice;
    m_preview->imageRect = image;
    m_preview->frameRect = m_vertical ? QRect(image.left(), image.top() + frameOffset, cross, frameAlong)
                                      : QRect(image.left() + frameOffset, image.top(), frameAlong, cross);
    m_preview->atStart = start == 0;
    m_preview->atEnd = start + len >= total;
    m_preview->showFrame = !m_alignmentLost && !m_storageFull;
    m_preview->setGeometry(win);
    if (!m_hidPreview) m_preview->show();
    // Only a preview placed inside the selection needs to stay out of the grabbed pixels.
    if (side == 2 && !m_previewTried) {
        m_previewTried = true;
        m_previewAffinity = SetWindowDisplayAffinity(reinterpret_cast<HWND>(m_preview->winId()), WDA_EXCLUDEFROMCAPTURE);
    }
    m_preview->update();
}

void LongCapture::updateUi()
{
    updateLabel();
    updateButtons();
    updatePreview();
}

void LongCapture::setVertical(bool vertical)
{
    if (vertical == m_vertical) return;
    setAutoScroll(0);
    m_vertical = vertical;
    if (m_dir && m_dir->currentIndex() != (vertical ? 0 : 1)) {
        const QSignalBlocker block(m_dir);
        m_dir->setCurrentIndex(vertical ? 0 : 1);
    }
    if (m_auto) {
        m_auto->setIcon(vertical ? barIcon(Icon::LongShot) : rotatedIcon(barIcon(Icon::LongShot), Bar::icon));
        m_auto->setToolTip(vertical ? tr("自动向下滚动截图") : tr("自动向右滚动截图"));
    }
    if (m_autoUp) {
        m_autoUp->setIcon(vertical ? rotatedIcon(barIcon(Icon::LongShot), Bar::icon, 180)
                                   : rotatedIcon(barIcon(Icon::LongShot), Bar::icon, 90));
        m_autoUp->setToolTip(vertical ? tr("自动向上滚动截图") : tr("自动向左滚动截图"));
    }
    updateCropDirection();
    if (m_axis) { m_axis->setCursor(vertical ? Qt::SizeVerCursor : Qt::SizeHorCursor); m_axis->update(); }
    resetCapture();                              // A new direction starts a new image.
}

void LongCapture::setAutoScroll(int direction)
{
    m_autoScroll = qBound(-1, direction, 1);
    if (m_auto && m_auto->isChecked() != (m_autoScroll > 0)) {
        const QSignalBlocker block(m_auto);
        m_auto->setChecked(m_autoScroll > 0);
    }
    if (m_autoUp && m_autoUp->isChecked() != (m_autoScroll < 0)) {
        const QSignalBlocker block(m_autoUp);
        m_autoUp->setChecked(m_autoScroll < 0);
    }
    if (m_autoScroll) m_scrollClock.invalidate();
    updateButtons();
}

QPoint LongCapture::moveRegion(QPoint delta)
{
    // Moving along the scroll axis reads like scrolling to the stitcher; the cross axis would change the frame.
    QPoint d = m_vertical ? QPoint(0, delta.y()) : QPoint(delta.x(), 0);
    const QRect screen = screenFor(m_region)->geometry();
    const int limit = qMax(100, (m_vertical ? screen.height() : screen.width()) / 3);
    if (qAbs(d.x()) > limit || qAbs(d.y()) > limit) return QPoint();
    const QRect next = m_region.translated(d);
    if (next.top() < screen.top()) d.ry() += screen.top() - next.top();
    if (next.bottom() > screen.bottom()) d.ry() -= next.bottom() - screen.bottom();
    if (next.left() < screen.left()) d.rx() += screen.left() - next.left();
    if (next.right() > screen.right()) d.rx() -= next.right() - screen.right();
    if (d.isNull()) return d;
    m_region.translate(d);
    if (m_mask) m_mask->update();
    if (m_bar) m_bar->move(m_bar->pos() + d);
    updatePreview();
    return d;
}

bool LongCapture::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_mask && event->type() == QEvent::Paint) {
        QPainter p(m_mask);
        const QRect hole = m_region.translated(-m_mask->geometry().topLeft());
        p.fillRect(m_mask->rect(), QColor(0, 0, 0, 120));
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(hole, Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(QPen(Bar::accent, kBorder, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.drawRect(QRectF(hole).adjusted(-kBorder / 2.0, -kBorder / 2.0, kBorder / 2.0, kBorder / 2.0));
        return true;
    }
    if (watched == m_bar && event->type() == QEvent::Paint) {   // Rounded dark surface with a faint light rim.
        QPainter p(m_bar);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 17));
        p.drawRoundedRect(QRectF(m_bar->rect()), Bar::radius + 1, Bar::radius + 1);
        p.setBrush(Bar::surface);
        p.drawRoundedRect(QRectF(m_bar->rect()).adjusted(1, 1, -1, -1), Bar::radius, Bar::radius);
        return true;
    }
    if (watched == m_cropPanel && event->type() == QEvent::Paint) {
        QPainter p(m_cropPanel);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 17));
        p.drawRoundedRect(QRectF(m_cropPanel->rect()), Bar::radius + 1, Bar::radius + 1);
        p.setBrush(Bar::surface);
        p.drawRoundedRect(QRectF(m_cropPanel->rect()).adjusted(1, 1, -1, -1), Bar::radius, Bar::radius);
        return true;
    }
    if ((watched == m_crop || watched == m_cropPanel)
        && (event->type() == QEvent::Enter || event->type() == QEvent::Leave)) {
        (watched == m_crop ? m_cropHover : m_panelHover) = event->type() == QEvent::Enter;
        refreshCropPanel();
        return QObject::eventFilter(watched, event);
    }
    if (watched != m_grip && watched != m_axis) return QObject::eventFilter(watched, event);
    auto *w = static_cast<QWidget *>(watched);
    switch (event->type()) {
    case QEvent::Paint: {
        QPainter p(w);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c = QRectF(w->rect()).center();
        if (w == m_grip) {                       // Six dots: the bar's drag handle.
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x32, 0x32, 0x36));
            for (int col : {-1, 1})
                for (int row : {-1, 0, 1})
                    p.drawRect(QRectF(c.x() + col * 3 - 1.5, c.y() + row * 7 - 1.5, 3, 3));
        } else {                                 // Double arrow along the scroll axis.
            p.setPen(QPen(Bar::ink, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const auto axis = [&](qreal a, qreal b) { return m_vertical ? QPointF(c.x() + b, c.y() + a) : QPointF(c.x() + a, c.y() + b); };
            p.drawLine(axis(-9, 0), axis(9, 0));
            p.drawPolyline(QPolygonF{axis(-5, -4), axis(-9, 0), axis(-5, 4)});
            p.drawPolyline(QPolygonF{axis(5, -4), axis(9, 0), axis(5, 4)});
        }
        return true;
    }
    case QEvent::MouseButtonPress: {
        auto *e = static_cast<QMouseEvent *>(event);
        if (e->button() == Qt::LeftButton) { m_dragging = w; m_dragAt = e->globalPosition().toPoint(); }
        return true;
    }
    case QEvent::MouseMove: {
        if (m_dragging != w) return true;
        const QPoint at = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
        if (w == m_grip) {
            m_bar->move(m_bar->pos() + at - m_dragAt);
            m_barMoved = true;
            m_dragAt = at;
            updatePreview();
        } else {
            m_dragAt += moveRegion(at - m_dragAt);
        }
        return true;
    }
    case QEvent::MouseButtonRelease:
        m_dragging = nullptr;
        return true;
    default:
        return QObject::eventFilter(watched, event);
    }
}

void LongCapture::notice(const QString &title, const QString &text, const QString &noAskKey)
{
    QSettings settings(QStringLiteral("Nami"), QStringLiteral("Nami"));
    if (!noAskKey.isEmpty() && settings.value(noAskKey, false).toBool()) return;
    if (m_notice) return;
    auto *box = new QMessageBox(QMessageBox::Warning, title, text, QMessageBox::Ok);
    box->setWindowFlag(Qt::WindowStaysOnTopHint);
    box->setAttribute(Qt::WA_DeleteOnClose);
    if (!noAskKey.isEmpty()) box->setCheckBox(new QCheckBox(tr("不再提示")));
    m_notice = box;
    m_paused = true;                             // The dialog may cover the selection.
    connect(box, &QMessageBox::finished, this, [this, box, noAskKey] {
        if (!noAskKey.isEmpty() && box->checkBox() && box->checkBox()->isChecked())
            QSettings(QStringLiteral("Nami"), QStringLiteral("Nami")).setValue(noAskKey, true);
        m_paused = false;                        // The dialog closed; capturing may resume.
    });
    box->show();
}

void LongCapture::fail(const QString &message)
{
    if (!m_label) return;
    m_label->setText(message);
    placeBar();
}

void LongCapture::finish(bool edit)
{
    if (m_done)
        return;
    const QPixmap pix = QPixmap::fromImage(exportImage());
    if (pix.isNull()) {
        fail(tr("导出失败，请释放内存后重试"));
        return;
    }
    if (edit) {                                   // The editor can come back here, so the session only steps aside.
        suspend();
        emit finished(pix, true);
        return;
    }
    m_done = true;
    cleanupUi();
    emit finished(pix, false);
    deleteLater();
}

void LongCapture::suspend()
{
    if (m_done) return;
    m_suspended = true;
    setAutoScroll(0);
    m_cropHover = m_panelHover = false;
    for (QWidget *w : {m_mask, m_bar, static_cast<QWidget *>(m_preview), m_cropPanel}) if (w) w->hide();
}

void LongCapture::resume()
{
    if (m_done || !m_suspended) return;
    m_suspended = false;
    for (QWidget *w : {m_mask, m_bar}) if (w) w->show();
    if (m_preview && !m_preview->image.isNull()) m_preview->show();
    updateUi();
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
    const QImage img = exportImage();
    if (img.isNull()) {
        fail(tr("导出失败，请释放内存后重试"));
        return;
    }
    // The dialog is not topmost; the session steps aside and stops capturing while it is open.
    m_paused = true;
    setAutoScroll(0);
    for (QWidget *w : {m_mask, m_bar, static_cast<QWidget *>(m_preview), m_cropPanel}) if (w) w->hide();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getSaveFileName(
        nullptr, tr("保存图片"), dir + QStringLiteral("/longshot.png"),
        tr("PNG 图片 (*.png);;JPEG 图片 (*.jpg)"));
    if (m_done) return;
    const bool saved = !path.isEmpty() && saveImageAtomic(img, path);
    if (saved) { cancel(); return; }
    for (QWidget *w : {m_mask, m_bar}) if (w) w->show();
    if (m_preview && !m_preview->image.isNull()) m_preview->show();
    m_paused = false;
    if (!path.isEmpty()) fail(tr("保存失败，请检查路径或释放内存后重试"));
}

void LongCapture::copyCapture()
{
    const QImage img = exportImage();
    if (img.isNull()) {
        fail(tr("导出失败，请释放内存后重试"));
        return;
    }
    if (!copyImageToClipboard(img)) {
        fail(tr("复制失败，剪贴板可能被占用，请重试"));
        return;
    }
    cancel();
}

void LongCapture::openTextCard(bool translate, bool barcode)
{
    const QImage img = exportImage();
    if (img.isNull()) {
        fail(tr("导出失败，请释放内存后重试"));
        return;
    }
    // The card replaces the session; back resumes it, while closing ends the capture.
    suspend();
    auto *dialog = barcode ? showBarcode(img, m_bar, m_region)
                           : translate ? showTranslate(img, m_bar, m_region) : showOcr(img, m_bar, m_region);
    m_ocrDialog = dialog;
    if (!dialog) {
        resume();
        fail(barcode ? tr("无法打开二维码识别窗口") : tr("无法打开识别窗口"));
        return;
    }
    connect(dialog, &QDialog::finished, this, [this,dialog] {
        m_ocrDialog=nullptr;   // The card deletes itself; cancel must not close it a second time.
        if (dialog->returnedToSource()) {
            resume();
            return;
        }
        QTimer::singleShot(0, this, &LongCapture::cancel);
    });
}

void LongCapture::cleanupUi()
{
    if (m_ocrDialog) m_ocrDialog->close();
    if (m_notice) m_notice->close();
    if (m_cropTimer) m_cropTimer->stop();
    for (QWidget **w : {&m_mask, &m_bar, &m_cropPanel}) if (*w) { (*w)->hide(); (*w)->deleteLater(); *w = nullptr; }
    if (m_preview) { m_preview->hide(); m_preview->deleteLater(); m_preview = nullptr; }
    m_label = nullptr; m_dir = nullptr; m_auto = m_autoUp = nullptr; m_save = nullptr;
    m_ocr = m_translate = m_barcode = nullptr;
    m_crop = m_cropStart = m_cropEnd = nullptr; m_autoCropBox = nullptr;
    m_grip = m_axis = nullptr;
    m_gated.clear();
}
