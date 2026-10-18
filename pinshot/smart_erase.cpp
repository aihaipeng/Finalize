#include "smart_erase.h"
#include <QPoint>
#include <QRect>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

namespace {

constexpr int Neighbours = 16;            // Known neighbours compared per pixel.
constexpr int Probes = 500;               // Random corpus probes per pixel.
constexpr double Sensitivity = 0.117;     // Outlier sensitivity of the colour metric.
constexpr int MaxPasses = 6;
constexpr double StopRatio = 0.1;         // Stop once fewer pixels than this still improve.
constexpr int WorkArea = 0x10000;         // Larger crops are synthesized downscaled.
constexpr int Unknown = 0, Corpus = 1, Target = 2;

// Context band so the crop holds about k times the hole box area.
QRect contextRect(const QRect &box, const QSize &bounds)
{
    const qint64 area = qint64(box.width()) * box.height();
    const double k = area * 8 <= 0xffff ? 16 : area * 4 <= 0xffff ? 8 : area * 2 < 0x10000 ? 6 : 4;
    const double sum = box.width() + box.height();
    const int margin = int(std::floor(std::max(0.0, (std::sqrt(sum * sum + 4 * (k - 1) * area) - sum) / 4)));
    QRect r = box.adjusted(-margin, -margin, margin, margin);
    // Shift a clipped band back inside instead of shrinking it.
    if (r.left() < 0) r.moveLeft(0);
    if (r.top() < 0) r.moveTop(0);
    if (r.right() >= bounds.width()) r.moveRight(bounds.width() - 1);
    if (r.bottom() >= bounds.height()) r.moveBottom(bounds.height() - 1);
    return r & QRect(QPoint(0, 0), bounds);
}

struct Synthesizer {
    int w = 0, h = 0;
    std::vector<QRgb> color;
    std::vector<uchar> state;
    std::vector<int> source;               // Corpus index each target borrowed from.
    std::vector<int> corpus;
    std::vector<QPoint> offsets;           // Neighbour offsets, nearest first.
    std::array<int, 511> diff{};           // Robust per-channel cost, indexed by d + 255.
    std::mt19937 rng{1};

    Synthesizer()
    {
        const double scale = Sensitivity * 256;
        const double norm = std::log((256 / scale) * (256 / scale) + 1);
        for (int d = -255; d <= 255; ++d)
            diff[d + 255] = int(std::log((d / scale) * (d / scale) + 1) / norm * 65535);
        for (int y = -7; y <= 7; ++y)
            for (int x = -7; x <= 7; ++x)
                if (x || y) offsets.push_back(QPoint(x, y));
        std::stable_sort(offsets.begin(), offsets.end(), [](const QPoint &a, const QPoint &b) {
            return a.x() * a.x() + a.y() * a.y() < b.x() * b.x() + b.y() * b.y();
        });
    }

    int at(int x, int y) const { return x < 0 || y < 0 || x >= w || y >= h ? -1 : y * w + x; }

    int cost(int target, int candidate, const std::vector<std::pair<QPoint, QRgb>> &known, int best) const
    {
        const int tx = candidate % w, ty = candidate / w;
        int sum = 0;
        for (const auto &[off, c] : known) {
            const int s = at(tx + off.x(), ty + off.y());
            if (s < 0 || state[s] == Unknown) { sum += 3 * 65535; }
            else {
                const QRgb o = color[s];
                sum += diff[qRed(c) - qRed(o) + 255] + diff[qGreen(c) - qGreen(o) + 255]
                       + diff[qBlue(c) - qBlue(o) + 255];
            }
            if (sum >= best) return sum;
        }
        Q_UNUSED(target);
        return sum;
    }

    // Returns true when the pixel picked a different source than before.
    bool synthesize(int target)
    {
        const int x = target % w, y = target / w;
        std::vector<std::pair<QPoint, QRgb>> known;
        std::vector<int> hints;
        for (const QPoint &off : offsets) {
            const int n = at(x + off.x(), y + off.y());
            if (n < 0 || state[n] == Unknown) continue;
            known.push_back({off, color[n]});
            const int from = state[n] == Corpus ? n : source[n];
            hints.push_back(at(from % w - off.x(), from / w - off.y()));
            if (int(known.size()) == Neighbours) break;
        }
        int best = INT_MAX, pick = -1;
        auto tryCandidate = [&](int c) {
            if (c < 0 || state[c] != Corpus) return;
            const int value = cost(target, c, known, best);
            if (value < best) { best = value; pick = c; }
        };
        if (source[target] >= 0) tryCandidate(source[target]);
        for (int c : hints) { if (best == 0) break; tryCandidate(c); }
        std::uniform_int_distribution<int> any(0, int(corpus.size()) - 1);
        for (int i = 0; i < Probes && best > 0; ++i) tryCandidate(corpus[any(rng)]);
        if (pick < 0) pick = corpus[any(rng)];
        const bool changed = pick != source[target];
        source[target] = pick;
        color[target] = color[pick];
        state[target] = Target;           // Now known to later pixels.
        return changed;
    }

    void run(std::vector<int> targets)
    {
        // Fill from the outside in so early pixels have the most known context.
        double cx = 0, cy = 0;
        for (int t : targets) { cx += t % w; cy += t / w; }
        cx /= targets.size(); cy /= targets.size();
        std::stable_sort(targets.begin(), targets.end(), [&](int a, int b) {
            const double da = std::hypot(a % w - cx, a / w - cy), db = std::hypot(b % w - cx, b / w - cy);
            return da > db;
        });
        int count = int(targets.size());
        for (int pass = 0; pass < MaxPasses && count > 0; ++pass) {
            int better = 0;
            for (int i = 0; i < count; ++i) better += synthesize(targets[i]);
            if (pass > 0 && double(better) / targets.size() < StopRatio) break;
            if (pass > 0) count = count * 3 / 4;   // Later passes refine the earliest, least-informed pixels.
        }
    }
};

int luma(QRgb c) { return (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114) / 1000; }

// Flat-background case: when one Otsu class covers >= 90% of the hole border,
// hole pixels already close to a border background colour are snapped to it and
// kept, so only the foreground strokes are synthesized from that background.
void keepBackground(Synthesizer &s, std::vector<int> &targets)
{
    std::vector<QRgb> ring;
    for (int t : targets) {
        const int x = t % s.w, y = t / s.w;
        for (const QPoint d : {QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)}) {
            const int n = s.at(x + d.x(), y + d.y());
            if (n >= 0 && s.state[n] == Corpus) ring.push_back(s.color[n]);
        }
    }
    if (ring.empty()) return;
    std::array<int, 256> hist{};
    for (QRgb c : ring) ++hist[luma(c)];
    double total = 0, sumAll = 0;
    for (int i = 0; i < 256; ++i) { total += hist[i]; sumAll += double(i) * hist[i]; }
    double wB = 0, sumB = 0, bestVar = -1; int threshold = 0;
    for (int i = 0; i < 256; ++i) {
        wB += hist[i]; sumB += double(i) * hist[i];
        if (wB == 0 || wB == total) continue;
        const double mB = sumB / wB, mF = (sumAll - sumB) / (total - wB);
        const double var = wB * (total - wB) * (mB - mF) * (mB - mF);
        if (var > bestVar) { bestVar = var; threshold = i; }
    }
    int low = 0;
    for (QRgb c : ring) low += luma(c) <= threshold;
    const bool lowWins = low * 2 >= int(ring.size());
    if (std::max(low, int(ring.size()) - low) < 0.9 * ring.size()) return;
    std::vector<QRgb> background;
    for (QRgb c : ring)
        if ((luma(c) <= threshold) == lowWins && std::find(background.begin(), background.end(), c) == background.end()) {
            background.push_back(c);
            if (background.size() >= 64) break;   // UI borders hold few colours; cap the scan.
        }
    std::vector<int> rest;
    for (int t : targets) {
        const QRgb c = s.color[t];
        int bestD = INT_MAX; QRgb pick = 0;
        for (QRgb b : background) {
            const int d = qAbs(qRed(c) - qRed(b)) + qAbs(qGreen(c) - qGreen(b)) + qAbs(qBlue(c) - qBlue(b));
            if (d < bestD) { bestD = d; pick = b; }
        }
        if (bestD < 31) { s.color[t] = pick; s.state[t] = Corpus; }
        else rest.push_back(t);
    }
    targets.swap(rest);
}

QImage gaussian9(const QImage &mask)
{
    // Separable 9-tap Gaussian (sigma 1.7) for a soft edge after upscaling.
    static const std::array<double, 9> k = [] {
        std::array<double, 9> v{}; double s = 0;
        for (int i = 0; i < 9; ++i) s += v[i] = std::exp(-(i - 4) * (i - 4) / (2 * 1.7 * 1.7));
        for (double &x : v) x /= s;
        return v;
    }();
    const int w = mask.width(), h = mask.height();
    std::vector<double> tmp(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double s = 0;
            for (int i = 0; i < 9; ++i) s += k[i] * mask.constScanLine(y)[qBound(0, x + i - 4, w - 1)];
            tmp[size_t(y) * w + x] = s;
        }
    QImage out(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double s = 0;
            for (int i = 0; i < 9; ++i) s += k[i] * tmp[size_t(qBound(0, y + i - 4, h - 1)) * w + x];
            out.scanLine(y)[x] = uchar(qBound(0, qRound(s), 255));
        }
    return out;
}

} // namespace

QImage SmartErase::erase(const QImage &image, const QImage &mask, QString *error)
{
    if (error) error->clear();
    if (image.isNull() || mask.isNull() || image.size() != mask.size()) {
        if (error) *error = QStringLiteral("智能擦除需要尺寸一致的有效图像与掩模");
        return {};
    }
    const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8);
    QImage result = image.convertToFormat(QImage::Format_RGB32);
    int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
    for (int y = 0; y < gray.height(); ++y)
        for (int x = 0; x < gray.width(); ++x)
            if (gray.constScanLine(y)[x] < 128) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    if (x1 < 0) return result;                     // Nothing to erase.
    const QRect crop = contextRect(QRect(QPoint(x0, y0), QPoint(x1, y1)), image.size());
    QImage work = result.copy(crop), workMask = gray.copy(crop);
    const qint64 area = qint64(crop.width()) * crop.height();
    const bool scaled = area > WorkArea;
    if (scaled) {
        const double f = std::sqrt(double(WorkArea) / area);
        const QSize small(std::max(1, int(crop.width() * f)), std::max(1, int(crop.height() * f)));
        work = work.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        workMask = workMask.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    Synthesizer s;
    s.w = work.width(); s.h = work.height();
    s.color.resize(size_t(s.w) * s.h);
    s.state.resize(s.color.size());
    s.source.assign(s.color.size(), -1);
    std::vector<int> targets;
    for (int y = 0; y < s.h; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(work.constScanLine(y));
        for (int x = 0; x < s.w; ++x) {
            const int i = y * s.w + x;
            s.color[i] = row[x];
            if (workMask.constScanLine(y)[x] < 128) { s.state[i] = Unknown; targets.push_back(i); }
            else { s.state[i] = Corpus; s.corpus.push_back(i); }
        }
    }
    if (s.corpus.empty()) {
        if (error) *error = QStringLiteral("擦除区域周围没有可参考的像素");
        return {};
    }
    keepBackground(s, targets);
    if (!targets.empty()) s.run(targets);
    QImage filled(s.w, s.h, QImage::Format_RGB32);
    for (int y = 0; y < s.h; ++y)
        std::copy_n(s.color.data() + size_t(y) * s.w, s.w, reinterpret_cast<QRgb *>(filled.scanLine(y)));
    QImage alpha;
    if (scaled) {
        filled = filled.scaled(crop.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QImage hole = gray.copy(crop);
        for (int y = 0; y < hole.height(); ++y)
            for (int x = 0; x < hole.width(); ++x) hole.scanLine(y)[x] = hole.scanLine(y)[x] < 128 ? 255 : 0;
        alpha = gaussian9(hole);
    }
    // Only hole pixels change; the upscaled fill blends with the original near the edge.
    for (int y = 0; y < crop.height(); ++y) {
        QRgb *out = reinterpret_cast<QRgb *>(result.scanLine(crop.top() + y)) + crop.left();
        const QRgb *in = reinterpret_cast<const QRgb *>(filled.constScanLine(y));
        const uchar *m = gray.constScanLine(crop.top() + y) + crop.left();
        for (int x = 0; x < crop.width(); ++x) {
            if (m[x] >= 128) continue;
            if (!scaled) { out[x] = in[x]; continue; }
            const int a = alpha.constScanLine(y)[x], b = 255 - a;
            out[x] = qRgb((qRed(in[x]) * a + qRed(out[x]) * b) / 255, (qGreen(in[x]) * a + qGreen(out[x]) * b) / 255,
                          (qBlue(in[x]) * a + qBlue(out[x]) * b) / 255);
        }
    }
    return result;
}
