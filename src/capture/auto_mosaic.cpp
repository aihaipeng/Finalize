#include "capture/auto_mosaic.h"
#include "capture/capture_overlay.h"
#include "shared/settings.h"
#include "text/engines/barcode_engine.h"
#include "text/engines/ocr_engine.h"
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QPainter>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QtConcurrent>
#include <algorithm>

namespace {
const QRegularExpression &urlPattern()
{
    static const QRegularExpression pattern(QStringLiteral(R"(\b(?:https?://|www\.)\S+)"),
                                            QRegularExpression::CaseInsensitiveOption);
    return pattern;
}
const QRegularExpression &emailPattern()
{
    static const QRegularExpression pattern(QStringLiteral(R"(\b[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}\b)"));
    return pattern;
}
const QRegularExpression &phonePattern()
{
    // CN mobile numbers, with an optional +86/0086 prefix; separators only before
    // the subscriber digits, matching how chat lists print them.
    static const QRegularExpression pattern(QStringLiteral(R"((?:(?:\+|00)?86[\s\-]?)?1[3-9]\d{9})"));
    return pattern;
}

QString phoneKey(const QString &text)
{
    QString digits;
    for (const QChar c : text) if (c.isDigit()) digits.append(c);
    if (digits.startsWith(QStringLiteral("0086"))) digits.remove(0, 4);
    else if (digits.startsWith(QStringLiteral("86")) && digits.size() > 11) digits.remove(0, 2);
    return QStringLiteral("phone:") + digits;
}

QString keyFor(AutoMosaic::Kind kind, const QString &text)
{
    switch (kind) {
    case AutoMosaic::Kind::Email: return QStringLiteral("email:") + text.trimmed().toLower();
    case AutoMosaic::Kind::Url: return QStringLiteral("url:") + text.trimmed().toLower();
    case AutoMosaic::Kind::Phone: return phoneKey(text);
    case AutoMosaic::Kind::Qr: return QStringLiteral("qr:") + text.trimmed();
    default: return QString();
    }
}

bool overlapsMostly(const QRect &a, const QRect &b)
{
    const QRect both = a.intersected(b);
    if (both.isEmpty()) return false;
    const qint64 smaller = qMin(qint64(a.width()) * a.height(), qint64(b.width()) * b.height());
    return smaller > 0 && qint64(both.width()) * both.height() * 10 >= smaller * 7;
}

quint64 imageHash(const QImage &image, const QRect &rect)
{
    const QImage block = image.copy(rect).scaled(8, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    if (block.isNull()) return 0;
    int sum = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) sum += block.constScanLine(y)[x];
    const int average = sum / 64;
    quint64 bits = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            if (block.constScanLine(y)[x] >= average) bits |= quint64(1) << (y * 8 + x);
    return bits;
}

// Non-text visual elements (avatars, icons, colored blocks). Connected components
// against the dominant background are enough for flat UI screenshots; photos and
// full-size images are left to the manual drag.
// shortcut: components touching an edge or larger than 240 px are skipped.
QVector<AutoMosaic::Detected> imageCandidates(const QImage &image,
                                              const QVector<AutoMosaic::Detected> &known)
{
    QVector<AutoMosaic::Detected> result;
    if (image.isNull() || image.width() < 16 || image.height() < 16) return result;
    const QSize target = image.size().scaled(QSize(1600, 1600), Qt::KeepAspectRatio);
    const QImage small = (target == image.size() ? image
                                                 : image.scaled(target, Qt::IgnoreAspectRatio, Qt::FastTransformation))
                             .convertToFormat(QImage::Format_RGB32);
    const int W = small.width(), H = small.height();
    if (W < 16 || H < 16) return result;

    QHash<quint32, int> histogram;
    const int sample = qMax(1, qMin(W, H) / 200);
    for (int y = 0; y < H; y += sample)
        for (int x = 0; x < W; x += sample) {
            const QRgb p = small.pixel(x, y);
            const quint32 key = quint32(qRed(p) >> 4) << 8 | quint32(qGreen(p) >> 4) << 4
                | quint32(qBlue(p) >> 4);
            ++histogram[key];
        }
    quint32 dominant = 0;
    int best = -1;
    for (auto it = histogram.cbegin(); it != histogram.cend(); ++it)
        if (it.value() > best) { best = it.value(); dominant = it.key(); }
    const int bgR = int((dominant >> 8) & 0xF) * 17;
    const int bgG = int((dominant >> 4) & 0xF) * 17;
    const int bgB = int(dominant & 0xF) * 17;

    QVector<quint8> foreground(W * H, 0), seen(W * H, 0);
    for (int y = 0; y < H; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(small.constScanLine(y));
        for (int x = 0; x < W; ++x) {
            const QRgb p = row[x];
            const int diff = qAbs(qRed(p) - bgR) + qAbs(qGreen(p) - bgG) + qAbs(qBlue(p) - bgB);
            if (qAlpha(p) > 40 && diff > 60) foreground[y * W + x] = 1;
        }
    }

    const double sx = image.width() / double(W), sy = image.height() / double(H);
    QVector<int> stack;
    for (int index = 0; index < W * H; ++index) {
        if (!foreground[index] || seen[index]) continue;
        stack.clear();
        stack.append(index);
        seen[index] = 1;
        int minX = W, minY = H, maxX = -1, maxY = -1, count = 0;
        while (!stack.isEmpty()) {
            const int at = stack.takeLast();
            const int x = at % W, y = at / W;
            ++count;
            minX = qMin(minX, x); maxX = qMax(maxX, x);
            minY = qMin(minY, y); maxY = qMax(maxY, y);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    const int next = ny * W + nx;
                    if (foreground[next] && !seen[next]) {
                        seen[next] = 1;
                        stack.append(next);
                    }
                }
        }
        const int w = maxX - minX + 1, h = maxY - minY + 1;
        if (w < 12 || h < 12 || w > 240 || h > 240) continue;
        const double aspect = w / double(h);
        if (aspect < 0.4 || aspect > 2.5) continue;
        if (count < w * h * 0.2) continue;                 // Thin borders and hairlines.
        if (minX <= 0 || minY <= 0 || maxX >= W - 1 || maxY >= H - 1) continue;
        const QRect box(qRound(minX * sx), qRound(minY * sy),
                        qRound(w * sx), qRound(h * sy));
        if (box.width() < 8 || box.height() < 8) continue;
        bool covered = false;
        for (const AutoMosaic::Detected &other : known) {
            const QRect both = other.rect.intersected(box);
            if (!both.isEmpty() && qint64(both.width()) * both.height() * 2
                    >= qint64(box.width()) * box.height())
                { covered = true; break; }
        }
        if (covered) continue;
        bool duplicate = false;
        for (const AutoMosaic::Detected &other : result)
            if (overlapsMostly(other.rect, box)) { duplicate = true; break; }
        if (duplicate) continue;
        result.append({box, QStringLiteral("image:") + QString::number(imageHash(image, box), 16),
                       AutoMosaic::Kind::None});
        if (result.size() >= 100) break;
    }
    return result;
}
}

AutoMosaic::Kind AutoMosaic::classify(const QString &text)
{
    if (text.trimmed().isEmpty()) return Kind::None;
    if (urlPattern().match(text).hasMatch()) return Kind::Url;
    if (emailPattern().match(text).hasMatch()) return Kind::Email;
    if (phonePattern().match(text).hasMatch()) return Kind::Phone;
    return Kind::None;
}

AutoMosaic::Result AutoMosaic::detect(const QImage &image, const std::shared_ptr<std::atomic_bool> &cancel)
{
    Result result;
    if (image.isNull() || (cancel && cancel->load())) return result;
    const QRect bounds = image.rect();
    // The OCR engine dereferences its cancel token unconditionally.
    const auto activeCancel = cancel ? cancel : std::make_shared<std::atomic_bool>(false);

    // Text identifiers. A busy OCR card owns the engine; the QR pass still runs.
    // Large captures are tiled so the detector keeps text at native size instead
    // of shrinking a 4K window to the 1440 px limit.
    auto &engine = OcrEngine::instance();
    bool expected = false;
    if (engine.busy.compare_exchange_strong(expected, true)) {
        const auto addLines = [&result, &bounds](const OcrResult &ocr, const QPoint &offset) {
            for (const OcrLine &line : ocr.lines) {
                const QString text = line.text.trimmed();
                if (text.isEmpty()) continue;
                // Every recognized text line is a selectable element; the privacy
                // kinds only decide which lines share a sync key.
                const Kind kind = classify(text);
                const QRect box = line.box.boundingRect().toAlignedRect().translated(offset).intersected(bounds);
                if (box.width() < 6 || box.height() < 6) continue;
                const QString key = kind == Kind::None
                    ? QStringLiteral("text:") + text.toLower() : keyFor(kind, text);
                bool duplicate = false;
                for (const Detected &seen : result.blocks)
                    if (overlapsMostly(seen.rect, box)) { duplicate = true; break; }
                if (!duplicate) result.blocks.append({box, key, kind});
            }
        };
        const int maxTile = 1400, overlap = 320;
        QVector<QRect> tiles;
        if (qMax(image.width(), image.height()) > 2000) {
            for (int y = 0; y < image.height();) {
                const int h = qMin(maxTile, image.height() - y);
                for (int x = 0; x < image.width();) {
                    const int w = qMin(maxTile, image.width() - x);
                    tiles.append(QRect(x, y, w, h));
                    if (x + w >= image.width()) break;
                    x += maxTile - overlap;
                }
                if (y + h >= image.height()) break;
                y += maxTile - overlap;
            }
        } else {
            tiles.append(bounds);
        }
        for (const QRect &tile : tiles) {
            if (activeCancel->load()) break;
            QImage part = image.copy(tile);
            part.setDevicePixelRatio(1);
            try {
                const OcrResult ocr = recognizeChunked(part, activeCancel);
                if (!ocr.error.isEmpty()) { result.error = ocr.error; break; }
                addLines(ocr, tile.topLeft());
            } catch (const std::exception &error) {
                result.error = QString::fromUtf8(error.what());
                break;
            } catch (...) {
                result.error = QStringLiteral("OCR worker failed");
                break;
            }
        }
        engine.busy.store(false);
    } else {
        result.error = QStringLiteral("OCR busy");
    }
    if (cancel && cancel->load()) return result;

    // QR / matrix / 1D symbols. These are already privacy-relevant on their own.
    const BarcodeResult codes = BarcodeEngine::recognize(image, activeCancel);
    for (const BarcodeItem &item : codes.items) {
        const QRect box = item.box.toAlignedRect().intersected(bounds);
        if (box.width() < 6 || box.height() < 6) continue;
        const QString key = keyFor(Kind::Qr, item.text);
        bool duplicate = false;
        for (const Detected &seen : result.blocks)
            if (overlapsMostly(seen.rect, box)) { duplicate = true; break; }
        if (!duplicate) result.blocks.append({box, key, Kind::Qr});
    }

    // Icons, avatars and other non-text blocks. Text/QR boxes already present win.
    for (const Detected &image : imageCandidates(image, result.blocks)) {
        bool duplicate = false;
        for (const Detected &seen : result.blocks)
            if (overlapsMostly(seen.rect, image.rect)) { duplicate = true; break; }
        if (!duplicate) result.blocks.append(image);
    }

    std::sort(result.blocks.begin(), result.blocks.end(), [](const Detected &a, const Detected &b) {
        return a.rect.y() != b.rect.y() ? a.rect.y() < b.rect.y() : a.rect.x() < b.rect.x();
    });
    if (result.blocks.size() > 200) result.blocks.resize(200);
    return result;
}

void CaptureOverlay::selectAutoMosaic()
{
    if (m_live) { m_live = false; commitAnnotation(); }
    selectTool(ATool::Mosaic);              // Clears any previous auto state and rebuilds the regular bar.
    m_autoMosaic = true;
    m_mosaicBrush = false;
    m_autoHoverAnn = -1;
    m_autoHoverBlock = -1;
    syncToolButtons();
    updateToolOptions();
    startAutoMosaicDetection();
    update();
}

void CaptureOverlay::startAutoMosaicDetection()
{
    if (!m_autoMosaic || m_mode != Adjusting || m_sel.isNull() || m_shot.isNull()) return;
    if (m_autoWatcher && m_autoWatcher->isRunning()) return;
    ++m_autoEpoch;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_autoCancel = cancel;
    m_autoPending = true;
    m_autoSpinTimer.start();
    update();

    const qreal dpr = m_shot.devicePixelRatioF() > 0 ? m_shot.devicePixelRatioF() : 1.0;
    const QRect selection = m_sel.normalized().intersected(QRect(0, 0, width(), height()));
    const QImage shot = m_shot.toImage();
    const QRect native = QRect(qRound(selection.left() * dpr), qRound(selection.top() * dpr),
                               qRound(selection.width() * dpr), qRound(selection.height() * dpr))
                             .intersected(shot.rect());
    QImage image = shot.copy(native);
    image.setDevicePixelRatio(1);
    if (image.isNull() || image.width() < 8 || image.height() < 8) {
        m_autoPending = false;
        m_autoSpinTimer.stop();
        return;
    }
    const int epoch = m_autoEpoch;
    auto *watcher = new QFutureWatcher<AutoMosaic::Result>(this);
    m_autoWatcher = watcher;
    connect(watcher, &QFutureWatcher<AutoMosaic::Result>::finished, this,
            [this, watcher, epoch, selection, dpr] {
        const AutoMosaic::Result result = watcher->result();
        watcher->deleteLater();
        if (m_autoWatcher == watcher) m_autoWatcher = nullptr;
        if (epoch != m_autoEpoch) return;
        m_autoPending = false;
        m_autoSpinTimer.stop();
        applyAutoMosaicResult(result, selection, dpr);
        update();
    });
    watcher->setFuture(QtConcurrent::run([image, cancel] { return AutoMosaic::detect(image, cancel); }));
}

void CaptureOverlay::cancelAutoMosaicDetection()
{
    ++m_autoEpoch;
    if (m_autoCancel) m_autoCancel->store(true);
    m_autoCancel.reset();
    m_autoPending = false;
    m_autoSpinTimer.stop();
    if (m_autoWatcher) {
        m_autoWatcher->disconnect(this);
        m_autoWatcher->deleteLater();
        m_autoWatcher = nullptr;
    }
}

CaptureOverlay::Annotation *CaptureOverlay::autoMosaicAnnotation()
{
    for (int i = m_anns.size() - 1; i >= 0; --i)
        if (m_anns[i].autoMosaic) return &m_anns[i];
    return nullptr;
}

const CaptureOverlay::Annotation *CaptureOverlay::autoMosaicAnnotation() const
{
    for (int i = m_anns.size() - 1; i >= 0; --i)
        if (m_anns[i].autoMosaic) return &m_anns[i];
    return nullptr;
}

void CaptureOverlay::applyAutoMosaicResult(const AutoMosaic::Result &result, const QRect &selection, qreal dpr)
{
    if (result.blocks.isEmpty() || dpr <= 0) return;
    Annotation *ann = autoMosaicAnnotation();
    QVector<Annotation::AutoBlock> fresh;
    for (const AutoMosaic::Detected &detected : result.blocks) {
        QRect logical(qRound(selection.left() + detected.rect.left() / dpr),
                      qRound(selection.top() + detected.rect.top() / dpr),
                      qRound(detected.rect.width() / dpr), qRound(detected.rect.height() / dpr));
        logical = logical.normalized().intersected(selection);
        if (logical.width() < 4 || logical.height() < 4) continue;
        const auto covered = [&logical](const QVector<Annotation::AutoBlock> &blocks) {
            for (const auto &block : blocks) {
                const QRect both = block.rect.intersected(logical);
                if (!both.isEmpty() && qint64(both.width()) * both.height() * 10
                        >= qint64(qMin(block.rect.width() * block.rect.height(),
                                       logical.width() * logical.height())) * 7) return true;
            }
            return false;
        };
        if (ann && covered(ann->autoBlocks)) continue;
        if (covered(fresh)) continue;
        // Detection only proposes candidates; the user clicks a block to cover it.
        fresh.append({logical, detected.key, false});
    }
    if (fresh.isEmpty()) return;

    pushUndo();
    if (!ann) {
        Annotation created;
        created.tool = ATool::Mosaic;
        created.autoMosaic = true;
        created.mosaicBlur = m_autoMosaicBlur;
        created.mosaicStrength = m_autoMosaicStrength;
        created.mosaicLevels[0] = m_autoMosaicLevels[0];
        created.mosaicLevels[1] = m_autoMosaicLevels[1];
        created.mosaicSeed = QRandomGenerator::global()->generate();
        m_anns.append(created);
        ann = &m_anns.last();
    }
    ann->autoBlocks += fresh;
    QRect bounds;
    for (const auto &block : ann->autoBlocks) bounds = bounds.united(block.rect);
    ann->rect = bounds;
    m_selAnn = -1;                          // Blocks are toggled, never moved by the regular handles.
    update();
}

bool CaptureOverlay::autoMosaicAt(const QPoint &pos, int *annIndex, int *blockIndex) const
{
    for (int i = m_anns.size() - 1; i >= 0; --i) {
        const Annotation &ann = m_anns[i];
        if (!ann.autoMosaic) continue;
        for (int b = ann.autoBlocks.size() - 1; b >= 0; --b) {
            if (ann.autoBlocks[b].rect.adjusted(-2, -2, 2, 2).contains(pos)) {
                if (annIndex) *annIndex = i;
                if (blockIndex) *blockIndex = b;
                return true;
            }
        }
    }
    return false;
}

void CaptureOverlay::toggleAutoMosaicBlock(int annIndex, int blockIndex)
{
    if (annIndex < 0 || annIndex >= m_anns.size()) return;
    Annotation &ann = m_anns[annIndex];
    if (!ann.autoMosaic || blockIndex < 0 || blockIndex >= ann.autoBlocks.size()) return;
    pushUndo();
    const bool included = !ann.autoBlocks[blockIndex].included;
    const QString key = ann.autoBlocks[blockIndex].key;
    QRect dirty;
    if (m_autoMosaicSync && !key.isEmpty()) {
        for (auto &block : ann.autoBlocks)
            if (block.key == key) {
                block.included = included;
                dirty |= block.rect;
            }
    } else {
        ann.autoBlocks[blockIndex].included = included;
        dirty = ann.autoBlocks[blockIndex].rect;
    }
    if (!dirty.isNull()) update(dirty.adjusted(-4, -4, 4, 4));
}

void CaptureOverlay::addAutoMosaicBlock(const QRect &rect)
{
    const QRect block = rect.normalized().intersected(m_sel);
    if (block.width() < 4 || block.height() < 4) return;
    Annotation *ann = autoMosaicAnnotation();
    if (ann) {
        for (const auto &existing : ann->autoBlocks)
            if (existing.rect.contains(block.center())) return;   // Already covered.
    }
    pushUndo();
    if (!ann) {
        Annotation created;
        created.tool = ATool::Mosaic;
        created.autoMosaic = true;
        created.mosaicBlur = m_autoMosaicBlur;
        created.mosaicStrength = m_autoMosaicStrength;
        created.mosaicLevels[0] = m_autoMosaicLevels[0];
        created.mosaicLevels[1] = m_autoMosaicLevels[1];
        created.mosaicSeed = QRandomGenerator::global()->generate();
        m_anns.append(created);
        ann = &m_anns.last();
    }
    ann->autoBlocks.append({block, QString(), true});
    ann->rect = ann->rect.united(block);
    m_selAnn = -1;
    update(block.adjusted(-4, -4, 4, 4));
}

void CaptureOverlay::setAutoMosaicMode(bool blur)
{
    if (m_autoMosaicBlur == blur) return;
    m_autoMosaicLevels[m_autoMosaicBlur ? 1 : 0] = m_autoMosaicStrength;
    m_autoMosaicBlur = blur;
    m_autoMosaicStrength = m_autoMosaicLevels[blur ? 1 : 0];
    Annotation *ann = autoMosaicAnnotation();
    QRect dirty;
    if (ann) {
        pushUndo();
        ann->mosaicBlur = blur;
        ann->mosaicStrength = m_autoMosaicStrength;
        ann->mosaicCacheStrength = 0;
        for (const auto &block : ann->autoBlocks)
            if (block.included) dirty |= block.rect;
    }
    if (m_autoStrengthSlider) {
        const QSignalBlocker lock(m_autoStrengthSlider);   // The combo is mid-click; do not rebuild the strip.
        m_autoStrengthSlider->setValue(m_autoMosaicStrength);
    }
    LensSnipSettings settings;
    settings.setValue(QStringLiteral("Capture/AutoMosaicBlur"), blur);
    settings.setValue(QStringLiteral("Capture/AutoMosaicStrength"), m_autoMosaicLevels[0]);
    settings.setValue(QStringLiteral("Capture/AutoMosaicBlurStrength"), m_autoMosaicLevels[1]);
    if (!dirty.isNull()) update(dirty.adjusted(-4, -4, 4, 4));
}

void CaptureOverlay::setAutoMosaicStrength(int value)
{
    value = qBound(3, value, 24);
    m_autoMosaicLevels[m_autoMosaicBlur ? 1 : 0] = value;
    if (value == m_autoMosaicStrength) return;
    m_autoMosaicStrength = value;
    QRect dirty;
    Annotation *ann = autoMosaicAnnotation();
    if (ann) {
        const bool dragging = m_autoStrengthSlider && m_autoStrengthSlider->isSliderDown();
        if (!dragging || !m_autoDragUndo) pushUndo();
        m_autoDragUndo = dragging;
        ann->mosaicStrength = value;
        ann->mosaicSeed = QRandomGenerator::global()->generate();
        ann->mosaicCacheStrength = 0;
        for (const auto &block : ann->autoBlocks)
            if (block.included) dirty |= block.rect;
    }
    if (m_autoStrengthSlider) {
        const QSignalBlocker lock(m_autoStrengthSlider);
        m_autoStrengthSlider->setValue(value);
    }
    LensSnipSettings settings;
    settings.setValue(QStringLiteral("Capture/AutoMosaicStrength"), m_autoMosaicLevels[0]);
    settings.setValue(QStringLiteral("Capture/AutoMosaicBlurStrength"), m_autoMosaicLevels[1]);
    if (!dirty.isNull()) update(dirty.adjusted(-4, -4, 4, 4));
}

void CaptureOverlay::paintAutoMosaicOverlay(QPainter &p) const
{
    if (!m_autoMosaic || m_atool != ATool::Mosaic || m_mode != Adjusting) return;
    if (m_autoHoverAnn < 0 || m_autoHoverAnn >= m_anns.size()) return;
    const Annotation &ann = m_anns[m_autoHoverAnn];
    if (!ann.autoMosaic || m_autoHoverBlock < 0 || m_autoHoverBlock >= ann.autoBlocks.size()) return;
    // Only the block under the pointer is outlined; the rest stay invisible until
    // hovered so the capture is not covered by a wall of candidate frames.
    const Annotation::AutoBlock &block = ann.autoBlocks[m_autoHoverBlock];
    const QRect r = block.rect.normalized();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(r, block.included ? QColor(80, 190, 120, 70) : QColor(229, 57, 53, 55));
    const QColor edge = block.included ? QColor(96, 220, 140) : QColor(255, 110, 120);
    p.setPen(QPen(edge, 1.6, Qt::SolidLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(r).adjusted(0, 0, -1, -1));
    if (r.width() >= 10 && r.height() >= 10) {
        const int tick = qMin(6, qMin(r.width(), r.height()) / 3);
        p.drawLine(r.topLeft(), r.topLeft() + QPoint(tick, 0));
        p.drawLine(r.topLeft(), r.topLeft() + QPoint(0, tick));
        p.drawLine(r.topRight(), r.topRight() + QPoint(-tick, 0));
        p.drawLine(r.topRight(), r.topRight() + QPoint(0, tick));
        p.drawLine(r.bottomLeft(), r.bottomLeft() + QPoint(tick, 0));
        p.drawLine(r.bottomLeft(), r.bottomLeft() + QPoint(0, -tick));
        p.drawLine(r.bottomRight(), r.bottomRight() + QPoint(-tick, 0));
        p.drawLine(r.bottomRight(), r.bottomRight() + QPoint(0, -tick));
    }
    p.restore();
}

void CaptureOverlay::paintAutoMosaicStatus(QPainter &p) const
{
    if (!m_autoPending || m_sel.isNull()) return;
    QFont font = p.font();
    font.setPointSizeF(10.5);
    const QFontMetrics metrics(font);
    const QString text = tr("正在识别…");
    const int width = metrics.horizontalAdvance(text) + 48;
    QRect box(m_sel.center().x() - width / 2, m_sel.bottom() + 14, width, 28);
    if (box.bottom() > height() - 4) box.moveBottom(m_sel.top() - 10);
    box.moveTop(qBound(4, box.top(), qMax(4, height() - box.height() - 4)));
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(24, 25, 29, 235));
    p.drawRoundedRect(box, 14, 14);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(143, 155, 255), 2));
    const QRect spinner(box.left() + 13, box.center().y() - 6, 12, 12);
    p.drawArc(spinner, m_autoSpinAngle * 16, 260 * 16);
    p.setPen(QColor(244, 245, 248));
    p.setFont(font);
    p.drawText(box.adjusted(33, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    p.restore();
}
