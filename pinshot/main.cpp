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
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QTextDocument>
#include <QTextCursor>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QAbstractTextDocumentLayout>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <QAbstractItemView>
#include <QStylePainter>
#include <QStyleOptionComboBox>
#include <QFontDatabase>
#include <QSlider>
#include <QVBoxLayout>
#include <QDialog>
#include <QRegularExpression>
#include <QDoubleValidator>
#include <QLineEdit>
#include <QPainterPath>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QClipboard>
#include <QFileDialog>
#include <QStandardPaths>
#include <QCursor>
#include <QMessageBox>
#include <QAbstractNativeEventFilter>
#include <QTimer>
#include <QFontMetrics>
#include <QFontInfo>
#include <QMimeData>
#include <QEventLoop>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QElapsedTimer>
#include <QHash>
#include <QBuffer>
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
    explicit PinWindow(const QPixmap &pix, const QRect &displayRect)
        : m_pix(pix), m_scale(double(displayRect.width()) / pix.width())
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_DeleteOnClose);
        setCursor(Qt::SizeAllCursor);
        setGeometry(displayRect);
        show();
    }
    static QRect fittedGeometry(const QSize &imageSize, const QRect &source, const QRect &available)
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
    static int testPlacement()
    {
        auto check = [](bool ok, const char *message) {
            if (!ok) {
                std::fprintf(stderr, "FAIL: %s\n", message);
                std::exit(1);
            }
        };
        const QRect available(0, 0, 1920, 1040);
        const QRect source(300, 200, 600, 400);
        check(fittedGeometry(QSize(600, 400), source, available) == source,
              "a short capture keeps its original position and size");
        check(fittedGeometry(QSize(900, 600), source, available) == source,
              "native capture pixels map back to the original logical size at 150 percent DPI");
        check(fittedGeometry(QSize(600, 800), source, available) == QRect(300, 200, 600, 800),
              "a long capture that fits is not reduced or centered");
        const QRect tall = fittedGeometry(QSize(600, 2400), source, available);
        check(tall == QRect(300, 200, 210, 840) && available.contains(tall),
              "an oversized long capture uses the maximum height while retaining its origin");
        check(fittedGeometry(QSize(900, 3600), source, available) == tall,
              "oversized placement is independent of the native capture DPI");
        check(fittedGeometry(QSize(1200, 600), QRect(1500, 200, 1200, 300), available)
                  == QRect(1500, 200, 420, 210),
              "width overflow shrinks proportionally to the remaining screen width");
        const QPoint offset(-1920, -1080);
        check(fittedGeometry(QSize(600, 2400), source.translated(offset), available.translated(offset))
                  == tall.translated(offset),
              "secondary screens with negative coordinates retain the capture origin");
        check(fittedGeometry(QSize(600, 2400), QRect(1919, 1039, 600, 400), available)
                  == QRect(1919, 1039, 1, 1),
              "edge captures keep a nonempty display inside the screen");
        check(fittedGeometry(QSize(), source, available).isEmpty(), "empty images are rejected");
        QPixmap pixels(900, 600);
        pixels.fill(Qt::green);
        PinWindow pin(pixels, source);
        pin.setAttribute(Qt::WA_DeleteOnClose, false);
        check(pin.geometry() == source && pin.m_pix.size() == pixels.size()
                  && qAbs(pin.m_scale - 2.0 / 3.0) < 0.000001,
              "ordinary pins keep the source geometry and original pixels without automatic fitting");
        const QPoint anchor = source.center();
        QWheelEvent wheel(QPointF(anchor - source.topLeft()), QPointF(anchor), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&pin, &wheel);
        check(pin.size() == QSize(660, 440)
                  && qAbs(double(anchor.x() - pin.x()) / pin.width()
                           - double(anchor.x() - source.x()) / source.width()) < 0.003,
              "wheel zoom starts from the displayed DPI scale and retains the cursor anchor");
        pin.close();
        std::puts("PASS: original-position pins, proportional long-capture fitting, DPI, and wheel zoom");
        return 0;
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

enum class Icon { Save, Copy, Pin, LongShot, Cancel, Rect, Ellipse, Arrow, Pen, Marker, Text,
                  Number, Mosaic, Eraser, Undo, Redo, ArrowBoth, ArrowLine, ArrowHollow, Palette, Bold, Italic };

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
    case Icon::Rect:                                  // Annotation shapes.
        path.addRect(4, 4, 16, 16);
        break;
    case Icon::Ellipse:
        path.addEllipse(4, 4, 16, 16);
        break;
    case Icon::Arrow:                                 // Diagonal with arrowhead.
        path.moveTo(7, 17); path.lineTo(17, 7);
        path.moveTo(8, 7); path.lineTo(17, 7); path.lineTo(17, 16);
        break;
    case Icon::ArrowBoth:                             // Arrowheads on both ends.
        path.moveTo(8, 16); path.lineTo(16, 8);
        path.moveTo(8, 16); path.lineTo(8, 11); path.moveTo(8, 16); path.lineTo(13, 16);
        path.moveTo(16, 8); path.lineTo(11, 8); path.moveTo(16, 8); path.lineTo(16, 13);
        break;
    case Icon::ArrowLine:                             // Plain line, no heads.
        path.moveTo(5, 17); path.lineTo(17, 5);
        break;
    case Icon::ArrowHollow:                           // Open V head, unfilled.
        path.moveTo(7, 17); path.lineTo(17, 7);
        path.moveTo(8, 7); path.lineTo(17, 7); path.lineTo(17, 16);
        path.lineTo(8, 7);
        break;
    case Icon::Pen:                                   // Diagonal pencil with nib.
        path.moveTo(17, 3); path.lineTo(21, 7); path.lineTo(8, 20); path.lineTo(3, 21);
        path.lineTo(4, 16); path.closeSubpath();
        break;
    case Icon::Mosaic:                                // Block grid.
        path.addRect(3, 3, 7, 7); path.addRect(14, 3, 7, 7);
        path.addRect(14, 14, 7, 7); path.addRect(3, 14, 7, 7);
        break;
    case Icon::Marker:                                // Highlighter: slanted body plus tip.
        path.moveTo(9, 11); path.lineTo(3, 17); path.lineTo(3, 20);
        path.lineTo(12, 20); path.lineTo(15, 17);
        path.moveTo(14, 4); path.lineTo(22, 12); path.lineTo(17, 17);
        path.lineTo(9, 9); path.closeSubpath();
        break;
    case Icon::Text:                                  // Letter T.
        path.moveTo(5, 6); path.lineTo(5, 4); path.lineTo(19, 4); path.lineTo(19, 6);
        path.moveTo(12, 4); path.lineTo(12, 20);
        path.moveTo(9, 20); path.lineTo(15, 20);
        break;
    case Icon::Bold:
        path.moveTo(7, 4); path.lineTo(7, 20); path.lineTo(14, 20);
        path.cubicTo(21, 20, 21, 12, 14, 12); path.lineTo(7, 12);
        path.moveTo(7, 4); path.lineTo(13, 4); path.cubicTo(20, 4, 20, 12, 13, 12);
        break;
    case Icon::Italic:
        path.moveTo(10, 4); path.lineTo(18, 4);
        path.moveTo(14, 4); path.lineTo(10, 20);
        path.moveTo(6, 20); path.lineTo(14, 20);
        break;
    case Icon::Number:                                // Circled digit one.
        path.addEllipse(2, 2, 20, 20);
        path.moveTo(13, 7); path.lineTo(10, 9);
        path.moveTo(13, 7); path.lineTo(13, 17);
        break;
    case Icon::Eraser:                                // Slanted eraser block.
        path.moveTo(7, 21); path.lineTo(2.7, 16.7); path.lineTo(2.7, 13.3);
        path.lineTo(12.3, 3.7); path.lineTo(15.7, 3.7); path.lineTo(21.3, 9.3);
        path.lineTo(21.3, 12.7); path.lineTo(13, 21); path.closeSubpath();
        path.moveTo(22, 21); path.lineTo(7, 21);
        path.moveTo(5, 11); path.lineTo(14, 20);
        break;
    case Icon::Undo:                                  // Hooked arrow turning back.
        path.moveTo(9, 14); path.lineTo(4, 9); path.lineTo(9, 4);
        path.moveTo(4, 9); path.lineTo(15, 9); path.quadTo(20, 9, 20, 14); path.lineTo(20, 20);
        break;
    case Icon::Redo:                                  // Hooked arrow turning forward.
        path.moveTo(15, 14); path.lineTo(20, 9); path.lineTo(15, 4);
        path.moveTo(20, 9); path.lineTo(9, 9); path.quadTo(4, 9, 4, 14); path.lineTo(4, 20);
        break;
    case Icon::Palette:
        path.moveTo(12, 3);
        path.cubicTo(7, 3, 3, 7, 3, 12); path.cubicTo(3, 17, 7, 21, 12, 21);
        path.cubicTo(15, 21, 15, 18, 13.5, 17); path.cubicTo(12, 15, 14, 13, 16, 14);
        path.cubicTo(19, 15, 21, 14, 21, 11); path.cubicTo(21, 6, 17, 3, 12, 3);
        path.closeSubpath();
        for (const QPointF &dot : {QPointF(7, 10), QPointF(10, 6.5), QPointF(15, 7), QPointF(6.5, 14)})
            path.addEllipse(dot, 0.55, 0.55);
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
        QPen pen(Qt::white, 1.75 * 24.0 / 16.0);   // Renders 1.75px at the 16px size.
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPath(path);
        for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active, QIcon::Selected})
            for (QIcon::State state : {QIcon::Off, QIcon::On}) result.addPixmap(pm, mode, state);
    }
    return result;
}

static QToolButton *iconButton(Icon icon, const QString &tip)
{
    auto *button = new QToolButton;
    button->setIcon(makeIcon(icon));
    button->setIconSize(QSize(16, 16));
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

// The closed font selector shows only its icon; the popup retains readable family names.
class FontComboBox : public QComboBox
{
public:
    FontComboBox() : m_icon(makeIcon(Icon::Text)) {}
protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        option.currentText.clear();
        option.currentIcon = m_icon;
        option.iconSize = QSize(16, 16);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }
private:
    const QIcon m_icon;
};

// ---------- Compact hue/saturation wheel; brightness is controlled by a slider. ----------
class ColorWheel : public QWidget
{
    Q_OBJECT
public:
    explicit ColorWheel(const QColor &color, QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(220, 220);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::CrossCursor);
        setAccessibleName(QStringLiteral("圆形颜色选择器"));
        setToolTip(QStringLiteral("点击或拖动选色；左右调整色相，上下调整饱和度"));
        setCurrentColor(color);
    }
    QRectF wheelRect() const
    {
        const qreal side = qMin(width(), height()) - 20;
        return QRectF((width() - side) / 2, (height() - side) / 2, side, side);
    }
    QColor currentColor() const { return m_color; }
    QColor brightColor() const { return QColor::fromHsvF(m_hue, m_saturation, 1).toRgb(); }
    int brightness() const { return qRound(m_value * 255); }
    void setCurrentColor(const QColor &color)
    {
        if (!color.isValid() || color.toRgb() == m_color) return;
        m_color = color.toRgb();
        if (color.hsvHueF() >= 0) m_hue = color.hsvHueF();
        if (color.valueF() > 0) m_saturation = color.hsvSaturationF(); // Black keeps the last wheel position.
        m_value = color.valueF();
        update();
        emit colorChanged(m_color);
    }
    void setBrightness(int value)
    {
        const qreal next = qBound(0, value, 255) / 255.0;
        if (next == m_value) return;
        m_value = next;
        updateColor();
    }
    static int testRendering()
    {
        ColorWheel wheel(Qt::cyan);
        wheel.show();
        QApplication::processEvents();
        const QImage image = wheel.grab().toImage();
        const qreal dpr = image.devicePixelRatio();
        const QRectF circle = wheel.wheelRect();
        const QColor center = image.pixelColor(qRound(circle.center().x() * dpr), qRound(circle.center().y() * dpr));
        const QColor rim = image.pixelColor(qRound((circle.right() - 6) * dpr), qRound(circle.center().y() * dpr));
        if (image.width() != qRound(wheel.width() * dpr) || image.pixelColor(0, 0) != QColor(29, 31, 35)
            || center.red() < 250 || center.green() < 250 || center.blue() < 250
            || rim.red() < 250 || rim.green() >= 32 || rim.blue() >= 32) {
            std::fprintf(stderr, "FAIL: circular color rendering at DPR %.2f\n", double(dpr));
            return 1;
        }
        const QPointF point = circle.center();
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, point, wheel.mapToGlobal(point.toPoint()), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&wheel, &event);
        }
        if (wheel.currentColor() != QColor(Qt::white)) {
            std::fprintf(stderr, "FAIL: logical color-wheel hit testing at DPR %.2f\n", double(dpr));
            return 1;
        }
        std::printf("PASS: circular color rendering and hit testing at DPR %.2f\n", double(dpr));
        return 0;
    }
signals:
    void colorChanged(const QColor &color);
protected:
    void paintEvent(QPaintEvent *) override
    {
        const QRectF circle = wheelRect();
        const int side = qRound(circle.width() * devicePixelRatioF());
        if (m_disc.width() != side) {                 // Cache physical pixels; pointer drags only move the marker.
            m_disc = QImage(side, side, QImage::Format_ARGB32);
            m_disc.fill(Qt::transparent);
            const qreal radius = side / 2.0;
            for (int y = 0; y < side; ++y) {
                QRgb *row = reinterpret_cast<QRgb *>(m_disc.scanLine(y));
                for (int x = 0; x < side; ++x) {
                    const qreal dx = x + 0.5 - radius, dy = radius - y - 0.5;
                    const qreal distance = qSqrt(dx * dx + dy * dy);
                    const qreal coverage = qBound(qreal(0), radius - distance + 0.5, qreal(1));
                    if (coverage <= 0) continue;
                    qreal hue = qAtan2(dy, dx) / (2 * M_PI);
                    if (hue < 0) hue += 1;
                    QColor color = QColor::fromHsvF(hue, qMin(qreal(1), distance / radius), 1);
                    color.setAlpha(qRound(coverage * 255));
                    row[x] = color.rgba();
                }
            }
        }
        QPainter p(this);
        p.fillRect(rect(), QColor(29, 31, 35));
        p.setRenderHint(QPainter::Antialiasing);
        p.drawImage(circle, m_disc);
        p.setPen(QPen(QColor("#626468"), 1));
        p.setBrush(QColor(0, 0, 0, qRound((1 - m_value) * 255)));
        p.drawEllipse(circle);
        const qreal angle = m_hue * 2 * M_PI;
        const qreal radius = circle.width() / 2 * m_saturation;
        const QPointF marker = circle.center() + QPointF(qCos(angle) * radius, -qSin(angle) * radius);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::black, 3));
        p.drawEllipse(marker, 5, 5);
        p.setPen(QPen(Qt::white, 1.5));
        p.drawEllipse(marker, 5, 5);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        const QRectF circle = wheelRect();
        if (e->button() != Qt::LeftButton || QLineF(circle.center(), e->pos()).length() > circle.width() / 2) return;
        m_dragging = true;
        setFocus(Qt::MouseFocusReason);
        selectAt(e->pos());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_dragging && (e->buttons() & Qt::LeftButton)) selectAt(e->pos());
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) m_dragging = false;
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) {
            m_hue += e->key() == Qt::Key_Right ? 1.0 / 360 : -1.0 / 360;
            if (m_hue < 0) m_hue += 1;
            if (m_hue >= 1) m_hue -= 1;
        } else if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
            m_saturation = qBound(qreal(0), m_saturation + (e->key() == Qt::Key_Up ? 0.01 : -0.01), qreal(1));
        } else { QWidget::keyPressEvent(e); return; }
        updateColor();
    }
private:
    void selectAt(const QPoint &point)
    {
        const QRectF circle = wheelRect();
        const QPointF delta = point - circle.center();
        const qreal distance = qSqrt(delta.x() * delta.x() + delta.y() * delta.y());
        if (distance > 0.01) {
            m_hue = qAtan2(-delta.y(), delta.x()) / (2 * M_PI);
            if (m_hue < 0) m_hue += 1;
        }
        m_saturation = qMin(qreal(1), distance / (circle.width() / 2));
        updateColor();
    }
    void updateColor()
    {
        m_color = QColor::fromHsvF(m_hue, m_saturation, m_value).toRgb();
        update();
        emit colorChanged(m_color);
    }
    QImage m_disc;
    QColor m_color;
    qreal m_hue = 0, m_saturation = 1, m_value = 1;
    bool m_dragging = false;
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
                const bool aligned = lc.addFrame(page.copy(0, pos, W, H));
                if (!aligned) std::fprintf(stderr, "SCROLL position=%d reference=%d\n", pos, lc.m_frameRow);
                check(aligned, "scroll frame must align");
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
        int failures = 0;
        auto regressionCheck = [&failures](bool ok, const char *message) {
            if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
        };
        QImage motionPage(W, 3600, QImage::Format_RGB32);
        motionPage.fill(Qt::white);
        QPainter motionPainter(&motionPage);
        motionPainter.setPen(Qt::black);
        for (int y = 0; y < motionPage.height(); y += 48) {
            motionPainter.drawText(24, y + 20, QStringLiteral("Scrolling capture: stable text and distinct rows"));
            motionPainter.drawText(790, y + 20, QString::number(y * 7919, 16));
            motionPainter.drawText(24, y + 40, QStringLiteral("Page row %1 with content that must stay in order").arg(y));
        }
        motionPainter.end();
        LongCapture fast{QRect()};
        regressionCheck(fast.addFrame(motionPage.copy(0, 0, W, H)), "fast-scroll initial frame");
        int firstFast = 0, lastFast = H;
        for (int pos : {281, 570, 865, 600, 300, 40, 0}) {
            const bool accepted = fast.addFrame(motionPage.copy(0, pos, W, H));
            regressionCheck(accepted && fast.m_frameRow == pos,
                            "fast scrolling with narrow overlap keeps its exact displacement");
            if (accepted) {
                firstFast = qMin(firstFast, pos);
                lastFast = qMax(lastFast, pos + H);
                regressionCheck(fast.toImage() == motionPage.copy(0, firstFast, W, lastFast - firstFast),
                                "fast scrolling never skips, repeats, or reorders image rows");
            }
        }
        for (double dpr : {1.5, 2.0}) {
            const QImage nativePage = motionPage.scaled(qRound(W * dpr), qRound(motionPage.height() * dpr),
                                                       Qt::IgnoreAspectRatio, Qt::FastTransformation);
            LongCapture nativeCapture{QRect(0, 0, W, H)};
            int nativeEnd = qRound(H * dpr);
            for (int pos : {0, 281, 570, 865, 600, 300, 40, 0}) {
                const int y = qRound(pos * dpr);
                const bool accepted = nativeCapture.addFrame(nativePage.copy(0, y, nativePage.width(), qRound(H * dpr)));
                if (!accepted || nativeCapture.m_frameRow != y)
                    std::fprintf(stderr, "DPI %.1f position=%d accepted=%d actual=%d\n", dpr, y, int(accepted), nativeCapture.m_frameRow);
                regressionCheck(accepted && nativeCapture.m_frameRow == y,
                                "native 150/200 percent DPI frames retain exact fast-scroll positions");
                if (accepted) {
                    nativeEnd = qMax(nativeEnd, y + qRound(H * dpr));
                    regressionCheck(nativeCapture.toImage() == nativePage.copy(0, 0, nativePage.width(), nativeEnd),
                                    "native high-DPI scrolling preserves every original output pixel");
                }
            }
            const QImage beforeJump = nativeCapture.toImage();
            regressionCheck(!nativeCapture.addFrame(nativePage.copy(0, qRound(3000 * dpr), nativePage.width(), qRound(H * dpr)))
                                && nativeCapture.toImage() == beforeJump,
                            "native high-DPI non-overlapping jumps cannot create plausible but incorrect splices");
        }
        auto animatedFrame = [&motionPage](int pos, int phase) {
            QImage frame = motionPage.copy(0, pos, W, H);
            for (int y = 72; y < 224; ++y) {
                QRgb *row = reinterpret_cast<QRgb *>(frame.scanLine(y));
                for (int x = 250; x < 730; ++x)
                    row[x] = ((x / 7 + y / 9 + phase) % 3) ? qRgb(225, 35, 55) : qRgb(20, 60, 235);
            }
            return frame;
        };
        LongCapture animated{QRect()};
        regressionCheck(animated.addFrame(animatedFrame(0, 0)), "animated initial frame");
        regressionCheck(animated.addFrame(animatedFrame(0, 1)) && animated.m_frameRow == 0
                            && animated.m_rowEnd == H,
                        "stationary local animation must not lose alignment or extend the output");
        int phase = 2;
        for (int pos : {80, 160, 300, 590, 390, 150, 0}) {
            regressionCheck(animated.addFrame(animatedFrame(pos, phase++)) && animated.m_frameRow == pos,
                            "local animation is excluded while stable content establishes the scroll offset");
        }
        regressionCheck(animated.toImage().copy(0, 0, 200, animated.m_rowEnd) == motionPage.copy(0, 0, 200, animated.m_rowEnd),
                        "animation handling preserves every stable output pixel during down/up/revisit");
        QImage unrelated(W, H, QImage::Format_RGB32);
        for (int y = 0; y < H; ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(unrelated.scanLine(y));
            for (int x = 0; x < W; ++x)
                row[x] = qRgb((x * 73 + y * 17) % 256, (x * 11 + y * 67) % 256, (x * 53 + y * 31) % 256);
        }
        const QImage animatedBefore = animated.toImage();
        regressionCheck(!animated.addFrame(unrelated) && animated.toImage() == animatedBefore,
                        "an unrelated animated frame never replaces the reference or changes the output");
        QImage longRows(16, 70000, QImage::Format_RGB32);
        for (int y = 0; y < longRows.height(); ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(longRows.scanLine(y));
            for (int x = 0; x < longRows.width(); ++x)
                row[x] = qRgb(y % 256, (y / 256) % 256, x * 13);
        }
        LongCapture large{QRect()};
        large.initTiles(longRows.width());
        large.m_tileMaxRows = 4096;
        for (int from = 0; from < longRows.height(); from += 2048)
            large.appendRows(longRows, from, qMin(2048, longRows.height() - from));
        regressionCheck(large.m_rowEnd == 70000 && large.toImage() == longRows,
                        "buffer growth preserves every pixel beyond 32767 and 65000 rows");
        QImage prefix(16, 1024, QImage::Format_RGB32);
        prefix.fill(Qt::green);
        large.prependRows(prefix, 0, prefix.height());
        const QImage extended = large.toImage();
        regressionCheck(extended.copy(0, 0, 16, prefix.height()) == prefix
                            && extended.copy(0, prefix.height(), 16, longRows.height()) == longRows,
                        "prepending an extra head preserves the complete very long image");
        QByteArray png;
        QBuffer pngBuffer(&png);
        pngBuffer.open(QIODevice::WriteOnly);
        regressionCheck(extended.save(&pngBuffer, "PNG") && QImage::fromData(png, "PNG") == extended,
                        "PNG export beyond 65000 rows round-trips every pixel");
        regressionCheck(!large.reserveRows(1 << 25, 0) && large.m_storageFull && large.toImage() == extended,
                        "memory budget rejection leaves the complete capture intact");
        QLabel preview;
        preview.resize(120, 240);
        large.m_preview = &preview;
        large.updateUi();
        large.m_preview = nullptr;
        regressionCheck(!preview.pixmap(Qt::ReturnByValue).isNull() && preview.pixmap(Qt::ReturnByValue).toImage()
                            == extended.scaled(1, 240, Qt::IgnoreAspectRatio, Qt::FastTransformation),
                        "very long preview scales the buffer view without copying the whole result first");
        auto *exportCapture = new LongCapture{QRect()};
        exportCapture->initTiles(16);
        regressionCheck(exportCapture->appendRows(extended, 0, extended.height()),
                        "a first frame larger than the growth step reserves enough storage");
        QPixmap exported;
        QObject::connect(exportCapture, &LongCapture::finished, [&exported](const QPixmap &pix) { exported = pix; });
        exportCapture->finish();
        regressionCheck(exported.toImage() == extended, "very long pinned output retains the original pixels");
        LongCapture exportFailure{QRect()};
        exportFailure.finish();
        regressionCheck(!exportFailure.m_done, "failed export keeps the capture session open");
        QImage performancePage(1280, 3600, QImage::Format_RGB32);
        performancePage.fill(Qt::white);
        QPainter performancePainter(&performancePage);
        performancePainter.setPen(Qt::black);
        for (int y = 0; y < performancePage.height(); y += 36) {
            performancePainter.drawText(24, y + 24, QStringLiteral("Row %1").arg(y * 7919));
            performancePainter.drawText(1100, y + 24, QString::number(y * 271, 16));
        }
        performancePainter.end();
        LongCapture denseAnimation{QRect()};
        qint64 maxMatchNs = 0;
        for (int pos : {0, 250, 500, 750, 1000}) {
            QImage frame = performancePage.copy(0, pos, 1280, 720);
            for (int y = 100; y < 620; ++y) {
                QRgb *row = reinterpret_cast<QRgb *>(frame.scanLine(y));
                for (int x = 260; x < 1000; ++x) {
                    quint32 value = quint32(x) * 747796405u ^ quint32(y) * 2891336453u ^ quint32(pos + 1) * 277803737u;
                    value ^= value >> 16;
                    value *= 2246822519u;
                    row[x] = qRgb(value & 255, (value >> 8) & 255, (value >> 16) & 255);
                }
            }
            QElapsedTimer timer;
            timer.start();
            const bool accepted = denseAnimation.addFrame(frame);
            if (pos) maxMatchNs = qMax(maxMatchNs, timer.nsecsElapsed());
            regressionCheck(accepted && denseAnimation.m_frameRow == pos,
                            "dense video-like animation does not dominate the static texture evidence");
        }
        std::printf("PERF: 1280x720 dense-animation match maximum %.2f ms\n", double(maxMatchNs) / 1000000.0);
        check(failures == 0, "fast-scroll, animation, and very-long-buffer regression checks");
        std::puts("PASS: long-capture stitching regression checks");
        return 0;
    }
signals:
    void finished(const QPixmap &pix);
private:
    // Unique local texture votes for a displacement; animation and repeated rows do not vote.
    // shortcut: Overlaps need stable unique texture; add another evidence source for textureless or fully animated pages.
    bool matchFullRes(const QImage &f, int &bestK) const
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
        const int top = qMax(0, -bestK), bottom = qMin(H, H - bestK);
        int cells = 0, bandMask = 0;
        bool covered[48] = {};
        for (const Match &match : evidence) {
            if (match.offset != bestK || match.y < top || match.y >= bottom) continue;
            const int band = (match.y - top) * 6 / (bottom - top);
            const int cell = band * 8 + match.x * 8 / W;
            if (!covered[cell]) { covered[cell] = true; ++cells; }
            bandMask |= 1 << band;
        }
        return cells >= 3 && (bandMask & (bandMask - 1)) != 0;
    }
    // ---- Capture loop: the user scrolls manually; polling captures and stitches frames. ----
    void nextTick(int elapsedMs)
    {
        if (!m_done && !m_storageFull)
            QTimer::singleShot(qMax(1, 50 - elapsedMs), this, &LongCapture::captureFrame);
    }
    void captureFrame()
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
    bool addFrame(const QImage &f)
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
    // ---- Growable row buffer with contiguous append/prepend. ----
    void initTiles(int width)
    {
        m_width = width;
        m_bytesPerLine = width * 4;
        m_tileMaxRows = qMax(64, (64 << 20) / m_bytesPerLine);  // Allocate up to 64 MB of rows per growth step.
    }
    bool reserveRows(int head, int tail)
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
    bool appendRows(const QImage &f, int from, int count)
    {
        if (count <= 0) return true;
        if (from < 0 || count > f.height() - from || !reserveRows(0, count)) return false;
        const int dstY = m_rowEnd - m_bufStart;
        memcpy(m_buf.scanLine(dstY), f.constScanLine(from), size_t(count) * m_bytesPerLine);
        m_rowEnd += count;
        m_tailPad -= count;
        return true;
    }
    bool prependRows(const QImage &f, int from, int count)
    {
        if (count <= 0) return true;
        if (from < 0 || count > f.height() - from || !reserveRows(count, 0)) return false;
        const int dstY = m_rowStart - m_bufStart - count;
        memcpy(m_buf.scanLine(dstY), f.constScanLine(from), size_t(count) * m_bytesPerLine);
        m_rowStart -= count;
        m_headPad -= count;
        return true;
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
            "QWidget{background:rgb(29,31,35);border-radius:4px;}"
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
            m_label->setText(m_storageFull ? QStringLiteral("内存不足或达到安全上限，已暂停延伸，请保存或贴图")
                : m_alignmentLost ? QStringLiteral("无法对齐，请缓慢滚回上一位置")
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
            const QImage view(m_buf.constScanLine(m_headPad), m_width, rows, m_bytesPerLine, QImage::Format_RGB32);
            const QImage scaled = view.scaled(qMax(1, qRound(m_width * s)), qMax(1, qRound(rows * s)),
                                             Qt::IgnoreAspectRatio, Qt::FastTransformation);
            m_preview->setPixmap(QPixmap::fromImage(scaled));
        }
    }
    void finish()
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
        if (!img.save(path)) {
            if (m_label) m_label->setText(QStringLiteral("保存失败，请检查路径或释放内存后重试"));
            return;
        }
        cancel();
    }
    void copyCapture()
    {
        const QImage img = toImage();
        if (img.isNull()) {
            if (m_label) m_label->setText(QStringLiteral("导出失败，请释放内存后重试"));
            return;
        }
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
    bool m_storageFull = false;
    QElapsedTimer m_previewClock;
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

        // The overlay paints matching solid backgrounds for both toolbar containers.
        m_tools = new QWidget(this);
        m_tools->setStyleSheet(
            "QToolButton{color:#fff;background:transparent;border:none;border-radius:3px;padding:4px;}"
            "QToolButton:hover,QToolButton:checked{background:rgba(255,255,255,38);}");
        auto *save = iconButton(Icon::Save, QStringLiteral("保存到文件 (Ctrl+S)"));
        auto *copy = iconButton(Icon::Copy, QStringLiteral("复制到剪贴板 (Ctrl+C)"));
        auto *pin = iconButton(Icon::Pin, QStringLiteral("贴在屏幕上 (Enter / 双击)"));
        auto *scroll = iconButton(Icon::LongShot, QStringLiteral("长截图：手动滚动拼接长图"));
        connect(save, &QToolButton::clicked, this, &CaptureOverlay::saveCapture);
        connect(copy, &QToolButton::clicked, this, &CaptureOverlay::copyCapture);
        connect(pin, &QToolButton::clicked, this, &CaptureOverlay::pinCapture);
        connect(scroll, &QToolButton::clicked, this, &CaptureOverlay::longCapture);

        // Instant annotation tools: draw inside the committed selection; every capture
        // action composes them in. One shape button covers rect / ellipse; per-tool
        // options (sub-shape, arrow style, colors, text weight) live on the strip below.
        auto *undo = iconButton(Icon::Undo, QStringLiteral("撤销 (Ctrl+Z)"));
        connect(undo, &QToolButton::clicked, this, &CaptureOverlay::undoAnnotation);
        m_undoButton = undo;
        auto *redo = iconButton(Icon::Redo, QStringLiteral("重做 (Ctrl+Y)"));
        connect(redo, &QToolButton::clicked, this, &CaptureOverlay::redoAnnotation);
        m_redoButton = redo;
        m_shapeButton = new QToolButton;               // One button for both shapes.
        m_shapeButton->setIcon(makeIcon(Icon::Rect));
        m_shapeButton->setIconSize(QSize(16, 16));
        m_shapeButton->setToolTip(QStringLiteral("形状：矩形/椭圆 (R=矩形 / O=椭圆)"));
        m_shapeButton->setCheckable(true);
        m_shapeButton->setCursor(Qt::PointingHandCursor);
        connect(m_shapeButton, &QToolButton::clicked, this, [this] {
            selectTool(m_shapeEllipse ? ATool::Ellipse : ATool::Rect);
            setCursor(Qt::CrossCursor);
            update();
        });
        m_toolBtn.fill(nullptr, int(ATool::Eraser) + 1);
        m_toolBtn[int(ATool::Rect)] = m_toolBtn[int(ATool::Ellipse)] = m_shapeButton;
        struct { ATool tool; Icon icon; const char *tip; } const tools[] = {
            {ATool::Arrow, Icon::Arrow, "箭头 (A)"},
            {ATool::Pen, Icon::Pen, "画笔 (P)"},
            {ATool::Marker, Icon::Marker, "荧光笔 (H)"},
            {ATool::Text, Icon::Text, "文字 (T)"},
            {ATool::Number, Icon::Number, "序号 (N)"},
            {ATool::Mosaic, Icon::Mosaic, "马赛克 (M)"},
            {ATool::Eraser, Icon::Eraser, "橡皮 (E)"},
        };
        for (const auto &t : tools) {
            auto *button = iconButton(t.icon, QString::fromUtf8(t.tip));
            button->setCheckable(true);
            const ATool tool = t.tool;
            connect(button, &QToolButton::clicked, this, [this, tool] {
                selectTool(tool);
                setCursor(Qt::CrossCursor);
                update();
            });
            m_toolBtn[int(tool)] = button;
        }
        m_toolButtons = {undo, redo, m_shapeButton};
        for (int t = int(ATool::Arrow); t < m_toolBtn.size(); ++t) m_toolButtons.append(m_toolBtn[t]);
        m_toolButtons.append(save); m_toolButtons.append(copy);
        m_toolButtons.append(pin); m_toolButtons.append(scroll);
        for (QWidget *button : m_toolButtons) button->setFocusPolicy(Qt::NoFocus);

        m_toolOptions = new QWidget(this);
        m_toolOptions->setStyleSheet(QStringLiteral(
            "QToolButton{color:#fff;background:transparent;border:none;border-radius:3px;padding:3px;}"
            "QToolButton:hover,QToolButton:checked{background:rgba(255,255,255,38);}"
            "QLabel{color:#fff;font-size:12px;border:none;}"
            "QComboBox{color:#fff;background:rgb(29,31,35);border:1px solid #626468;"
            "padding:1px 3px;font-size:12px;}"
            "QComboBox QAbstractItemView{color:#fff;background:rgb(29,31,35);selection-background-color:#007aff;}"
            "QLineEdit{color:#fff;background:transparent;border:none;selection-background-color:#007aff;}"));
        m_toolOptions->hide();
        buildToolLayout();
        m_tools->adjustSize();
        m_tools->hide();

        show();
        // Claim foreground synchronously (hotkey handling grants activation rights)
        // and detach the IME so plain C/Shift always reach this window.
        HWND handle = reinterpret_cast<HWND>(winId());
        SetForegroundWindow(handle);
        SetFocus(handle);
        m_imeCtx = ImmAssociateContext(handle, nullptr);   // Detached; re-attached for text editing.
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
        // Both rows stay horizontal and move above the selection when the screen bottom is full.
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry()
                               .translated(-overlay.geometry().topLeft());
        overlay.m_sel = QRect(avail.topLeft() + QPoint(60, 60), QSize(640, 120));
        overlay.m_mode = Adjusting;
        overlay.placeToolbar();
        check(overlay.m_tools->y() >= overlay.m_sel.bottom() + 1
                  && overlay.m_tools->x() + overlay.m_tools->width() == overlay.m_sel.right() + 1
                  && overlay.m_tools->width() > overlay.m_tools->height(),
              "toolbar docks below the selection, flush with its right edge");
        overlay.m_sel = QRect(avail.left() + 40, avail.bottom() + 1 - 150, 300, 150);
        overlay.placeToolbar();
        check(overlay.m_tools->y() + overlay.m_tools->height() <= overlay.m_sel.top()
                  && overlay.m_tools->width() > overlay.m_tools->height(),
              "the horizontal toolbar moves above the selection at the screen bottom");
        // Annotation editing: shortcuts, shift constraints, selection, re-tune, delete,
        // undo/redo, marker, the text editor, sequence numbers, blur, and the eraser.
        auto mouse = [&overlay](QEvent::Type type, const QPoint &local,
                                Qt::KeyboardModifiers mods = Qt::NoModifier) {
            QMouseEvent ev(type, local, local + overlay.geometry().topLeft(), Qt::LeftButton,
                           type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                           mods);
            QApplication::sendEvent(&overlay, &ev);
        };
        const QPoint annOrigin = screenOrigin + QPoint(20, 20);
        overlay.m_sel = QRect(annOrigin, QSize(110, 80));
        overlay.m_mode = Adjusting;
        QKeyEvent keyR(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &keyR);
        check(overlay.m_atool == ATool::Rect, "single-letter shortcuts arm annotation tools");
        const QPoint squareA = annOrigin + QPoint(10, 10);
        mouse(QEvent::MouseButtonPress, squareA);
        mouse(QEvent::MouseMove, squareA + QPoint(40, 20), Qt::ShiftModifier);
        mouse(QEvent::MouseButtonRelease, squareA + QPoint(40, 20), Qt::ShiftModifier);
        check(overlay.m_anns.size() == 1 && overlay.m_anns[0].rect.size() == QSize(41, 41),
              "shift constrains rectangles to squares");
        overlay.setToolNone();
        const QPoint edge(overlay.m_anns[0].rect.center().x(), overlay.m_anns[0].rect.top());
        mouse(QEvent::MouseButtonPress, edge);
        check(overlay.m_selAnn == 0 && overlay.m_itemDrag == H_Move,
              "clicking an annotation selects it");
        mouse(QEvent::MouseMove, edge + QPoint(12, 0));
        mouse(QEvent::MouseButtonRelease, edge + QPoint(12, 0));
        check(overlay.m_anns[0].rect == QRect(squareA + QPoint(12, 0), QSize(41, 41)),
              "dragging moves the selected annotation");
        const QPoint corner = overlay.m_anns[0].rect.bottomRight();
        mouse(QEvent::MouseButtonPress, corner);
        mouse(QEvent::MouseMove, corner + QPoint(25, 5));
        mouse(QEvent::MouseButtonRelease, corner + QPoint(25, 5));
        check(overlay.m_anns[0].rect.size() == QSize(66, 46),
              "corner handles resize the selected annotation");
        const QPoint wheelAt = overlay.m_sel.center();
        QWheelEvent tune(QPointF(wheelAt), QPointF(wheelAt + overlay.geometry().topLeft()),
                         QPoint(0, 8), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                         Qt::ScrollUpdate, false);
        QApplication::sendEvent(&overlay, &tune);
        check(overlay.m_anns[0].pen == 4, "the wheel retunes the selected annotation");
        const QColor originalShapeColor = overlay.m_anns[0].color;
        const int beforeColorUndo = overlay.m_undoStack.size();
        overlay.pickToolColor(ATool::Rect, QColor("#1c1c1e"));
        check(overlay.m_anns[0].color == originalShapeColor
                  && overlay.m_undoStack.size() == beforeColorUndo
                  && overlay.m_colorShape == QColor("#1c1c1e"),
              "color selection updates the tool default without recoloring a committed selection");
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &del);
        check(overlay.m_anns.isEmpty(), "delete removes the selected annotation");
        overlay.undoAnnotation();
        check(overlay.m_anns.size() == 1, "undo restores the deleted annotation");
        overlay.redoAnnotation();
        check(overlay.m_anns.isEmpty(), "redo replays the deletion");
        overlay.selectTool(ATool::Marker);
        const QPoint strokeAt = annOrigin + QPoint(12, 40);
        mouse(QEvent::MouseButtonPress, strokeAt);
        mouse(QEvent::MouseMove, strokeAt + QPoint(15, 5));
        mouse(QEvent::MouseMove, strokeAt + QPoint(18, 18));
        mouse(QEvent::MouseButtonRelease, strokeAt + QPoint(18, 18));
        check(overlay.m_anns.size() == 1 && overlay.m_anns[0].tool == ATool::Marker
                  && overlay.m_anns[0].stroke.size() >= 3,
              "the marker commits a decimated translucent stroke");
        const QImage beforeText = overlay.cropped().toImage();
        overlay.selectTool(ATool::Text);
        mouse(QEvent::MouseButtonPress, annOrigin + QPoint(60, 20));
        check(overlay.editorActive(), "the text tool opens an inline editor");
        overlay.m_editor->setPlainText(QStringLiteral("标注"));
        overlay.commitTextEdit();
        check(overlay.m_anns.size() == 2 && overlay.m_anns[1].tool == ATool::Text
                  && overlay.m_anns[1].text == QStringLiteral("标注")
                  && overlay.m_anns[1].font == overlay.m_afont,
              "text commits into an annotation");
        check(overlay.cropped().toImage() != beforeText, "text composes into the capture");
        overlay.setToolNone();                       // Re-editing works in selection mode.
        mouse(QEvent::MouseButtonDblClick, overlay.m_anns[1].rect.center());
        check(overlay.editorActive()
                  && overlay.m_editor->toPlainText() == QStringLiteral("标注"),
              "double-click reopens a text annotation for editing");
        overlay.m_editor->setPlainText(QStringLiteral("改"));
        overlay.commitTextEdit();
        check(overlay.m_anns.size() == 2 && overlay.m_anns[1].text == QStringLiteral("改"),
              "re-editing replaces the annotation in place");
        overlay.selectTool(ATool::Number);
        const QPoint n1 = annOrigin + QPoint(15, 15), n2 = annOrigin + QPoint(70, 50);
        mouse(QEvent::MouseButtonPress, n1);
        mouse(QEvent::MouseButtonRelease, n1);
        mouse(QEvent::MouseButtonPress, n2);
        mouse(QEvent::MouseButtonRelease, n2);
        check(overlay.m_anns.size() == 4 && overlay.m_anns[2].text == QStringLiteral("1")
                  && overlay.m_anns[3].text == QStringLiteral("2"),
              "number clicks place incrementing sequence digits");
        overlay.setToolNone();
        mouse(QEvent::MouseButtonPress, n1);
        mouse(QEvent::MouseButtonRelease, n1);
        QApplication::sendEvent(&overlay, &del);
        check(overlay.m_anns.size() == 3 && overlay.m_anns[2].text == QStringLiteral("1"),
              "numbers renumber after a deletion");
        const QImage beforeMosaic = overlay.cropped().toImage();
        overlay.selectTool(ATool::Mosaic);
        mouse(QEvent::MouseButtonPress, annOrigin + QPoint(50, 10));
        mouse(QEvent::MouseMove, annOrigin + QPoint(100, 60));
        mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(100, 60));
        check(overlay.m_anns.size() == 4 && overlay.m_anns[3].tool == ATool::Mosaic
                  && overlay.cropped().toImage() != beforeMosaic,
              "mosaic pixelates the frozen frame in its rectangle");
        overlay.selectTool(ATool::Eraser);
        mouse(QEvent::MouseButtonPress, strokeAt + QPoint(9, 9));
        mouse(QEvent::MouseMove, strokeAt + QPoint(10, 10));
        mouse(QEvent::MouseButtonRelease, strokeAt + QPoint(10, 10));
        bool markerGone = true;
        for (const Annotation &a : overlay.m_anns)
            if (a.tool == ATool::Marker) markerGone = false;
        check(overlay.m_anns.size() == 3 && markerGone,
              "the eraser removes whole annotations under it");
        overlay.setToolNone();
        mouse(QEvent::MouseButtonPress, n2);
        mouse(QEvent::MouseButtonRelease, n2);
        QKeyEvent escSel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &escSel);
        check(overlay.m_selAnn == -1 && overlay.isVisible(),
              "escape deselects the annotation before closing");
        // Behavior parity round: right click finishes input, +/- width, WASD nudge,
        // rotation, and undo/redo button enable state.
        const int annCount = overlay.m_anns.size();
        overlay.selectTool(ATool::Marker);
        const QPoint fin = annOrigin + QPoint(16, 12);
        mouse(QEvent::MouseButtonPress, fin);
        mouse(QEvent::MouseMove, fin + QPoint(14, 6));
        QMouseEvent rightClick(QEvent::MouseButtonPress, fin + QPoint(14, 6),
                               fin + QPoint(14, 6) + overlay.geometry().topLeft(),
                               Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &rightClick);
        check(overlay.m_anns.size() == annCount + 1
                  && overlay.m_anns.last().tool == ATool::Marker && !overlay.m_live,
              "right click finishes a live stroke instead of discarding it");
        QKeyEvent plus(QEvent::KeyPress, Qt::Key_Plus, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &plus);
        check(overlay.m_anns.last().pen == 4, "the plus key retunes the selected annotation");
        overlay.setToolNone();
        mouse(QEvent::MouseButtonPress, fin + QPoint(7, 3));   // Re-select the stroke.
        mouse(QEvent::MouseButtonRelease, fin + QPoint(7, 3));
        const QPointF beforeNudge = overlay.m_anns.last().stroke.first();
        QKeyEvent keyA(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &keyA);
        QKeyEvent keyW(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &keyW);
        check(overlay.m_anns.last().stroke.first() == beforeNudge + QPointF(-1, -1),
              "WASD keys nudge the selected annotation one pixel");
        const QPoint knob = CaptureOverlay::rotateAnchor(overlay.m_anns.last());
        mouse(QEvent::MouseButtonPress, knob);
        mouse(QEvent::MouseMove, knob + QPoint(30, 0));
        mouse(QEvent::MouseButtonRelease, knob + QPoint(30, 0));
        check(!qFuzzyIsNull(overlay.m_anns.last().rot), "the rotate handle spins the annotation");
        const QPoint inside = CaptureOverlay::annotationRect(overlay.m_anns.last()).center()
                              + QPoint(2, 3);
        mouse(QEvent::MouseButtonPress, inside);
        check(overlay.m_selAnn == overlay.m_anns.size() - 1,
              "rotated annotations stay selectable through the inverse transform");
        mouse(QEvent::MouseButtonRelease, inside);
        QKeyEvent keyDel(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &keyDel);
        while (!overlay.m_undoStack.isEmpty()) overlay.undoAnnotation();
        check(!overlay.m_undoButton->isEnabled(), "the undo button disables on an empty stack");
        overlay.m_redoStack.clear();
        overlay.syncUndoButtons();
        check(!overlay.m_redoButton->isEnabled(), "the redo button disables on an empty stack");
        overlay.selectTool(ATool::Rect);
        QKeyEvent fillKey(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &fillKey);
        check(overlay.m_afill, "the F key toggles fill mode for the shape tools");
        mouse(QEvent::MouseButtonPress, annOrigin + QPoint(60, 20));
        mouse(QEvent::MouseMove, annOrigin + QPoint(95, 55));
        mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(95, 55));
        check(overlay.m_anns.last().fill, "fill mode carries into fresh shapes");
        overlay.m_afill = false;
        // Per-tool option strip round: merged shape button, arrow styles, per-tool
        // colors, text weight, and eraser reach.
        overlay.setToolNone();
        overlay.selectTool(ATool::Rect);
        check(overlay.m_shapeButton->isChecked() && !overlay.m_shapeEllipse
                  && overlay.m_acolor == overlay.m_colorShape
                  && overlay.m_toolOptions->isVisible(),
              "the merged shape button arms the rectangle with its option strip");
        overlay.selectTool(ATool::Ellipse);
        check(overlay.m_shapeEllipse && overlay.m_shapeButton->isChecked(),
              "arming the ellipse flips the shared button's sub-shape");
        overlay.pickToolColor(ATool::Marker, QColor("#34c759"));
        overlay.selectTool(ATool::Marker);
        check(overlay.m_acolor == QColor("#34c759"), "each tool remembers its own color");
        overlay.selectTool(ATool::Arrow);
        overlay.pickArrowStyle(3);
        const QPoint a1 = annOrigin + QPoint(60, 5), a2 = annOrigin + QPoint(60, 55);
        mouse(QEvent::MouseButtonPress, a1);
        mouse(QEvent::MouseMove, a2);
        mouse(QEvent::MouseButtonRelease, a2);
        check(overlay.m_anns.last().tool == ATool::Arrow
                  && overlay.m_anns.last().arrowStyle == 3,
              "the arrow style carries into fresh arrows");
        overlay.setToolNone();
        mouse(QEvent::MouseButtonPress, a1 + QPoint(0, 25));
        mouse(QEvent::MouseButtonRelease, a1 + QPoint(0, 25));
        check(overlay.m_selAnn == overlay.m_anns.size() - 1
                  && overlay.m_anns[overlay.m_selAnn].tool == ATool::Arrow,
              "the arrow stays selectable");
        overlay.pickArrowStyle(1);
        check(overlay.m_anns[overlay.m_selAnn].arrowStyle == 1,
              "the options strip restyles the selected arrow");
        overlay.setTextBold(true);
        overlay.setTextItalic(true);
        overlay.selectTool(ATool::Text);
        check(overlay.m_abold && overlay.m_aitalic, "text toggles persist per tool");
        mouse(QEvent::MouseButtonPress, annOrigin + QPoint(30, 30));
        overlay.m_editor->setPlainText(QStringLiteral("粗"));
        overlay.commitTextEdit();
        check(overlay.m_anns.last().bold && overlay.m_anns.last().italic,
              "bold and italic commit into text annotations");
        overlay.selectTool(ATool::Eraser);
        overlay.m_apen = 12;                        // A fat eraser reaches farther.
        const int beforeErase = overlay.m_anns.size();
        mouse(QEvent::MouseButtonPress, annOrigin + QPoint(60, 75));
        mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(60, 75));
        check(overlay.m_anns.size() < beforeErase, "the eraser's reach tracks its thickness");
        overlay.m_anns.clear();
        overlay.m_selAnn = -1;
        // Every tool exposes an editable size, and clicking it again keeps its options open.
        QVector<QImage> toolbarPreviews;
        for (ATool tool : {ATool::Rect, ATool::Ellipse, ATool::Arrow, ATool::Pen, ATool::Marker,
                           ATool::Text, ATool::Number, ATool::Mosaic, ATool::Eraser}) {
            overlay.selectTool(tool);
            overlay.m_toolBtn[int(tool)]->click();
            overlay.m_toolBtn[int(tool)]->click();
            check(overlay.m_atool == tool && overlay.m_toolOptions->isVisible() && overlay.m_sizeInput,
                  "repeated toolbar clicks keep the matching options and numeric control visible");
            const bool textSize = tool == ATool::Text || tool == ATool::Number;
            check(overlay.m_sizeInput->width() == (textSize ? 62 : 40)
                      && overlay.m_sizeInput->lineEdit()->alignment() == Qt::AlignCenter,
                  "numeric controls use a compact fixed width with centered values");
            check(overlay.m_sizeInput->lineEdit()->contentsRect().width()
                      >= QFontMetrics(overlay.m_sizeInput->lineEdit()->font()).horizontalAdvance(
                          textSize ? QStringLiteral("1637.5") : QStringLiteral("12")),
                  "compact numeric controls still fit the complete supported values");
            check(overlay.m_toolOptions->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly).isEmpty(),
                  "option strips contain no visible thickness labels or wheel instructions");
            for (QWidget *bar : {overlay.m_tools, overlay.m_toolOptions})
                for (QToolButton *button : bar->findChildren<QToolButton *>())
                    check(button->text().isEmpty(), "toolbar actions render icons or swatches rather than text");
            if (!previewPath.isEmpty()) {
                QImage bars = overlay.grab(overlay.m_tools->geometry().united(overlay.m_toolOptions->geometry())).toImage();
                bars.setDevicePixelRatio(1);
                toolbarPreviews.append(bars);
            }
            const bool colored = tool != ATool::Mosaic && tool != ATool::Eraser;
            const QList<QToolButton *> swatches = overlay.m_toolOptions->findChildren<QToolButton *>(QStringLiteral("presetColor"));
            check((overlay.m_colorButton != nullptr) == colored && swatches.size() == (colored ? 8 : 0),
                  "colored tools expose eight common colors and a palette button, not a dense color grid");
            if (colored)
                check(overlay.m_colorButton->text().isEmpty() && !overlay.m_colorButton->icon().isNull()
                          && overlay.m_colorButton->toolButtonStyle() == Qt::ToolButtonIconOnly
                          && overlay.m_colorButton->styleSheet().isEmpty(),
                      "every palette button uses a white icon without a text label or custom border");
            for (QToolButton *swatch : swatches)
                check(swatch->y() == swatches.first()->y(), "common color presets stay in one spacious row");
        }
        if (!toolbarPreviews.isEmpty()) {
            int width = 0, height = 0;
            for (const QImage &bars : toolbarPreviews) { width = qMax(width, bars.width()); height += bars.height() + 8; }
            QImage sheet(width, height, QImage::Format_RGB32);
            sheet.fill(QColor(15, 17, 20));
            QPainter painter(&sheet);
            int y = 0;
            for (const QImage &bars : toolbarPreviews) { painter.drawImage(0, y, bars); y += bars.height() + 8; }
            painter.end();
            check(sheet.save(previewPath + QStringLiteral(".toolbars.png")), "save toolbar visual regression sheet");
        }
        check(parseAnnotationColor(QStringLiteral("rgb(0, 128,255)")) == QColor(0, 128, 255)
                  && parseAnnotationColor(QStringLiteral("#A1b2C3")) == QColor("#a1b2c3")
                  && !parseAnnotationColor(QStringLiteral("rgb(256,0,0)")).isValid()
                  && !parseAnnotationColor(QStringLiteral("rgb(-1,0,0)")).isValid()
                  && !parseAnnotationColor(QStringLiteral("#12xz56")).isValid()
                  && !parseAnnotationColor(QStringLiteral("#12345678")).isValid(),
              "manual colors validate channel bounds and exactly six hexadecimal digits");
        overlay.selectTool(ATool::Pen);
        QLineEdit *sizeEdit = overlay.m_sizeInput->findChild<QLineEdit *>();
        sizeEdit->setText(QStringLiteral("6"));
        QKeyEvent sizeEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(sizeEdit, &sizeEnter);
        check(overlay.m_apen == 6 && overlay.m_sizeInput->currentText().toDouble() == 6 && overlay.isVisible(),
              "typing a width and pressing Enter changes the tool without pinning the capture");
        QWheelEvent sizeWheel(QPointF(10, 10), QPointF(sizeEdit->mapToGlobal(QPoint(10, 10))),
                             QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                             Qt::ScrollUpdate, false);
        QApplication::sendEvent(sizeEdit, &sizeWheel);
        check(overlay.m_apen == 7 && overlay.m_sizeInput->currentText().toDouble() == 7,
              "wheel input over the numeric field uses the same width and updates the displayed value");
        const QPoint sizeStroke = annOrigin + QPoint(15, 15);
        mouse(QEvent::MouseButtonPress, sizeStroke);
        overlay.tuneWidth(1, sizeStroke);
        mouse(QEvent::MouseMove, sizeStroke + QPoint(20, 10));
        overlay.pickToolColor(ATool::Pen, QColor("#007aff"));
        check(overlay.m_tools->isVisible() && overlay.m_toolOptions->isVisible()
                  && overlay.m_sizeInput->currentText().toDouble() == 8 && overlay.m_ann.pen == 8,
              "drawing keeps both bars visible and wheel changes update the live stroke and number");
        check(overlay.m_ann.color == QColor("#007aff"), "color changes apply to the in-progress stroke");
        mouse(QEvent::MouseButtonRelease, sizeStroke + QPoint(20, 10));
        overlay.m_sizeInput->lineEdit()->setText(QStringLiteral("9"));
        QApplication::sendEvent(overlay.m_sizeInput->lineEdit(), &sizeEnter);
        check(overlay.m_anns.last().pen == 9 && overlay.m_sizeInput->currentText().toDouble() == 9,
              "the numeric field retunes the selected object");
        overlay.undoAnnotation();
        check(overlay.m_anns.last().pen == 8, "one undo restores the width before a numeric edit");
        overlay.redoAnnotation();
        check(overlay.m_anns.last().pen == 9, "redo reapplies the numeric width edit");
        overlay.selectTool(ATool::Eraser);
        overlay.setToolSize(99);
        check(overlay.toolSize() == 12 && overlay.m_sizeInput->currentText().toDouble() == 12,
              "width input stays within the supported range");
        overlay.setToolSize(0);
        check(overlay.toolSize() == 1 && overlay.m_sizeInput->currentText().toDouble() == 1,
              "width input cannot become zero or negative");
        overlay.placeToolbar();
        overlay.showToolBars();
        check(overlay.m_toolOptions->x() == overlay.m_tools->x()
                  && overlay.m_toolOptions->y() == overlay.m_tools->geometry().bottom() + 1
                  && overlay.m_toolOptions->width() == overlay.m_tools->width(),
              "the main toolbar and its options form aligned, gapless horizontal rows");
        for (QWidget *bar : {overlay.m_tools, overlay.m_toolOptions}) {
            const QImage painted = overlay.grab(bar->geometry()).toImage();
            const qreal dpr = painted.devicePixelRatio();
            const QColor background = painted.pixelColor(qRound(10 * dpr), qRound((bar->height() - 2) * dpr));
            if (background != QColor(29, 31, 35))
                std::fprintf(stderr, "bar background %d,%d,%d size=%dx%d dpr=%.2f main=%d\n",
                             background.red(), background.green(), background.blue(),
                             bar->width(), bar->height(), double(dpr), bar == overlay.m_tools);
            check(background == QColor(29, 31, 35),
                  "main and option bars render the exact solid background color");
        }
        for (QWidget *widget : overlay.m_toolButtons) {
            const QIcon icon = static_cast<QToolButton *>(widget)->icon();
            for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled}) {
                const QImage pixels = icon.pixmap(QSize(16, 16), mode).toImage();
                bool white = true, visible = false;
                for (int y = 0; y < pixels.height(); ++y)
                    for (int x = 0; x < pixels.width(); ++x) {
                        const QColor color = pixels.pixelColor(x, y);
                        if (!color.alpha()) continue;
                        visible = true;
                        if (color.red() != 255 || color.green() != 255 || color.blue() != 255) white = false;
                    }
                check(visible && white, "toolbar icons stay white in normal and disabled states");
            }
        }
        overlay.m_anns.clear();
        overlay.m_undoStack.clear();
        overlay.m_redoStack.clear();
        overlay.m_atool = ATool::None;
        overlay.m_selAnn = -1;
        overlay.syncUndoButtons();
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
        check(!overlay.m_inspector->isVisible(),
              "the color value card stays hidden inside a locked selection");
        // Instant annotation: a rectangle stroke commits, composes, and undoes.
        overlay.selectTool(ATool::Rect);
        const QRect annRect = overlay.m_sel.adjusted(12, 12, -12, -12);
        const QPoint annA = annRect.topLeft(), annB = annRect.bottomRight();
        QMouseEvent annPress(QEvent::MouseButtonPress, annA, annA + overlay.geometry().topLeft(),
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &annPress);
        check(overlay.m_live && overlay.m_tools->isVisible() && overlay.m_toolOptions->isVisible(),
              "an annotation drag keeps the active toolbar and options visible");
        QMouseEvent annMove(QEvent::MouseMove, annB, annB + overlay.geometry().topLeft(),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &annMove);
        QMouseEvent annRelease(QEvent::MouseButtonRelease, annB, annB + overlay.geometry().topLeft(),
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &annRelease);
        check(!overlay.m_live && overlay.m_anns.size() == 1 && overlay.m_anns.first().rect == annRect
                  && overlay.m_tools->isVisible(),
              "the annotation commits with the dragged geometry and the tool stays ready");
        const QImage composed = overlay.cropped().toImage();
        overlay.undoAnnotation();
        check(overlay.m_anns.isEmpty() && overlay.cropped().toImage() != composed,
              "undo removes the annotation from the composed capture");
        QKeyEvent escTool(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &escTool);
        check(overlay.m_atool == ATool::None && overlay.isVisible(),
              "escape drops the annotation tool before closing the overlay");
        bool pinned = false;
        QRect pinnedRegion;
        QObject::connect(&overlay, &CaptureOverlay::grabbed, &overlay,
                         [&pinned, &pinnedRegion](const QPixmap &pix, const QRect &region) {
            pinned = !pix.isNull();
            pinnedRegion = region;
        });
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
        check(pinnedRegion == overlay.m_sel.translated(overlay.geometry().topLeft()),
              "double-click pinning reports the locked selection's original global geometry");
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
    static int testTextAnnotation()
    {
        auto check = [](bool ok, const char *message) {
            if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
        };
        const QRect screen = QGuiApplication::primaryScreen()->geometry();
        QImage image(1350, 750, QImage::Format_RGB32);
        image.fill(QColor(30, 40, 50));
        CaptureOverlay overlay({ScreenFrame{QRect(screen.topLeft(), QSize(900, 500)),
                                            QRect(0, 0, 1350, 750), image}});
        overlay.setAttribute(Qt::WA_DeleteOnClose, false);
        const QPoint origin = screen.topLeft() - overlay.geometry().topLeft() + QPoint(60, 60);
        overlay.m_mode = Adjusting;
        overlay.m_sel = QRect(origin, QSize(800, 300));
        overlay.placeToolbar();
        overlay.showToolBars();
        overlay.selectTool(ATool::Text);
        QLineEdit *fontEdit = overlay.m_sizeInput->findChild<QLineEdit *>();
        fontEdit->setText(QStringLiteral("10.5"));
        QKeyEvent fontEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(fontEdit, &fontEnter);
        check(overlay.m_afont == 10.5 && overlay.m_sizeInput->currentText().toDouble() == 10.5
                  && static_cast<const QDoubleValidator *>(overlay.m_sizeInput->validator())->top() == 1638
                  && overlay.m_sizeInput->findText(QStringLiteral("12")) >= 0 && overlay.isVisible(),
              "the editable font control accepts fractional point sizes without pinning the capture");
        overlay.beginTextEdit(origin + QPoint(20, 20));
        QApplication::processEvents();
        check(overlay.m_editor->font().pointSizeF() == 10.5 && overlay.m_editor->font().pixelSize() == -1
                  && qAbs(QFontInfo(overlay.m_editor->font()).pixelSize() - 10.5 * overlay.logicalDpiY() / 72.0) <= 1,
              "font sizes use standard points and the screen DPI, not pixels mislabeled as points");
        auto key = [&overlay](int code, Qt::KeyboardModifiers mods = Qt::NoModifier,
                              const QString &text = QString()) {
            QKeyEvent ev(QEvent::KeyPress, code, mods, text);
            QApplication::sendEvent(overlay.m_textView, &ev);
        };
        QInputMethodEvent ime;
        ime.setCommitString(QStringLiteral("中文标注"));
        QApplication::sendEvent(overlay.m_textView, &ime);
        check(overlay.m_editor->toPlainText() == QStringLiteral("中文标注"),
              "native scene input accepts a committed Chinese composition");
        const qreal singleHeight = overlay.m_editor->boundingRect().height();
        key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
        key(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        check(overlay.editorActive() && overlay.m_editor->toPlainText() == QStringLiteral("中文标注\nx")
                  && overlay.m_editor->boundingRect().height() > singleHeight,
              "Enter inserts a new line and grows the text item without committing or pinning");
        const QString typed = overlay.m_editor->toPlainText();
        key(Qt::Key_Z, Qt::ControlModifier);
        check(overlay.editorActive() && overlay.m_anns.isEmpty() && overlay.m_editor->toPlainText() != typed,
              "Ctrl+Z first undoes typing inside the text document");
        key(Qt::Key_Y, Qt::ControlModifier);
        check(overlay.m_editor->toPlainText() == typed, "Ctrl+Y restores local typing without committing");
        key(Qt::Key_C, Qt::ControlModifier);
        check(overlay.editorActive() && overlay.isVisible(),
              "copy with no text selection cannot copy and close the whole capture");
        QTextCursor cur = overlay.m_editor->textCursor();
        cur.setPosition(1);
        cur.setPosition(3, QTextCursor::KeepAnchor);
        overlay.m_editor->setTextCursor(cur);
        const int anchor = cur.anchor(), position = cur.position();
        QComboBox *fontFamily = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("annotationFont"));
        check(fontFamily && fontFamily->focusPolicy() == Qt::NoFocus,
              "the text options include a font-family selector without stealing input focus");
        const QImage fontIcon = fontFamily->grab().toImage();
        fontFamily->setCurrentText(QStringLiteral("Arial"));
        check(fontFamily->currentText() == QStringLiteral("Arial") && fontFamily->findText(QStringLiteral("Arial")) >= 0
                  && dynamic_cast<FontComboBox *>(fontFamily) && fontFamily->width() == 40
                  && fontFamily->view()->minimumWidth() >= 200
                  && fontFamily->grab().toImage() == fontIcon && fontFamily->toolTip().contains(QStringLiteral("Arial")),
              "the closed font selector stays icon-only while its popup and tooltip retain the selected family");
        fontEdit = overlay.m_sizeInput->findChild<QLineEdit *>();
        fontEdit->setFocus();
        fontEdit->setText(QStringLiteral("12"));
        QApplication::sendEvent(fontEdit, &fontEnter);
        check(overlay.editorActive() && overlay.m_editor->font().pointSizeF() == 12
                  && overlay.m_editor->font().family() == QStringLiteral("Arial")
                  && overlay.m_editor->textCursor().anchor() == anchor
                  && overlay.m_editor->textCursor().position() == position
                  && overlay.m_textView->hasFocus(),
              "editing the size keeps live text and its selection, then returns keyboard focus");
        for (QToolButton *button : overlay.m_toolOptions->findChildren<QToolButton *>()) {
            check(button->focusPolicy() == Qt::NoFocus, "text options do not steal keyboard focus");
            if (button->toolTip() == QStringLiteral("粗体") || button->toolTip() == QStringLiteral("斜体"))
                button->click();
            if (button->toolTip() == QStringLiteral("#007aff")) button->click();
        }
        overlay.editorFontStep(1);
        check(overlay.editorActive() && overlay.m_editor->font().bold() && overlay.m_editor->font().italic()
                  && overlay.m_editFont == 12.5 && overlay.m_sizeInput->currentText().toDouble() == 12.5
                  && overlay.m_editor->defaultTextColor() == QColor("#007aff")
                  && overlay.m_editor->textCursor().anchor() == anchor
                  && overlay.m_editor->textCursor().position() == position,
              "live color, bold, italic, and font size preserve input and selection");
        overlay.m_editor->setPlainText(QString(80, QLatin1Char('W')));
        check(overlay.m_editor->boundingRect().width() > 420
                  && overlay.m_editor->document()->blockCount() == 1,
              "long text grows naturally without a fixed-width wrapping box");
        overlay.m_editor->setPlainText(QStringLiteral("中文标注\nsecond line"));
        const QPointF beforeMove = overlay.m_editor->pos();
        const QPoint border = overlay.m_textView->mapFromScene(
            overlay.m_editor->mapToScene(QPointF(1, overlay.m_editor->boundingRect().height() / 2)));
        const QPoint delta(24, 12);
        auto editorMouse = [&overlay](QEvent::Type type, const QPoint &at) {
            QWidget *viewport = overlay.m_textView->viewport();
            QMouseEvent ev(type, at, viewport->mapToGlobal(at),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          Qt::NoModifier);
            QApplication::sendEvent(viewport, &ev);
        };
        editorMouse(QEvent::MouseButtonPress, border);
        editorMouse(QEvent::MouseMove, border + delta);
        editorMouse(QEvent::MouseButtonRelease, border + delta);
        check(overlay.editorActive() && overlay.m_editor->pos() == beforeMove + delta,
              "dragging the text margin moves the item and keeps text editing active");
        const QImage liveCapture = overlay.cropped().toImage();
        overlay.m_editor->clearFocus();
        const qreal dpr = overlay.m_shot.devicePixelRatioF();
        const QRect crop(qRound(overlay.m_sel.x() * dpr), qRound(overlay.m_sel.y() * dpr),
                         qRound(overlay.m_sel.width() * dpr), qRound(overlay.m_sel.height() * dpr));
        QPixmap nativeCapture = overlay.m_shot.copy(crop);
        nativeCapture.setDevicePixelRatio(1);
        {
            QPainter painter(&nativeCapture);
            overlay.m_textView->scene()->render(&painter, QRectF(nativeCapture.rect()), QRectF(overlay.m_sel));
        }
        overlay.m_editor->setFocus();
        check(nativeCapture.toImage() == liveCapture,
              "native scene text and capture text have identical pixels at 150 percent DPI");
        key(Qt::Key_Escape);
        check(!overlay.editorActive() && overlay.isVisible() && overlay.m_anns.size() == 1
                  && overlay.m_atool == ATool::Text && overlay.m_anns[0].bold && overlay.m_anns[0].italic
                  && overlay.m_anns[0].family == QStringLiteral("Arial")
                  && overlay.m_anns[0].color == QColor("#007aff"),
              "Escape finishes text input in one transaction and keeps the capture open");
        check(overlay.cropped().toImage() == liveCapture,
              "text pixels do not jump or wrap differently when editing ends at 150 percent DPI");
        const Annotation saved = overlay.m_anns[0];
        const int undoCount = overlay.m_undoStack.size();
        overlay.beginTextEdit(saved.rect.center(), 0);
        overlay.commitTextEdit();
        check(overlay.m_undoStack.size() == undoCount && overlay.m_anns[0].rect == saved.rect,
              "opening and closing unchanged text adds no phantom undo step or geometry change");
        const QPoint click = saved.rect.topLeft() + QPoint(12, 12);
        QMouseEvent press(QEvent::MouseButtonPress, click, overlay.mapToGlobal(click),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, click, overlay.mapToGlobal(click),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &press);
        QApplication::sendEvent(&overlay, &release);
        check(overlay.editorActive() && overlay.m_editAnn == 0 && overlay.m_anns.size() == 1,
              "the text tool edits an existing item instead of creating an overlapping duplicate");
        cur = overlay.m_editor->textCursor();
        cur.setPosition(1);
        overlay.m_editor->setTextCursor(cur);
        key(Qt::Key_Return, Qt::ControlModifier);
        check(overlay.m_editor->toPlainText().startsWith(QStringLiteral("中文标注\n\n")),
              "Ctrl+Enter starts a new line at the end of the current paragraph");
        overlay.commitTextEdit();
        overlay.undoAnnotation();
        check(overlay.m_anns[0].text == saved.text, "one undo restores the entire text-edit transaction");
        overlay.redoAnnotation();
        check(overlay.m_anns[0].text != saved.text, "redo restores the edited multiline text");
        const Annotation beforeResize = overlay.m_anns[0];
        overlay.pushUndo();
        overlay.m_selAnn = 0;
        overlay.m_pressAnn = beforeResize;
        overlay.m_itemDrag = H_BR;
        overlay.applyItemDrag(beforeResize.rect.bottomRight() + QPoint(50, 20));
        Annotation fitted = overlay.m_anns[0];
        fitTextAnnotation(fitted);
        check(overlay.m_anns[0].font > beforeResize.font && overlay.m_anns[0].text == beforeResize.text
                  && overlay.m_anns[0].rect.size() == fitted.rect.size(),
              "text corner resizing scales the letters and keeps content-sized bounds");
        overlay.m_itemDrag = H_None;
        overlay.undoAnnotation();
        overlay.m_anns[0].rot = 30;
        const QPoint rotatedOrigin = mapFromLocal(overlay.m_anns[0], overlay.m_anns[0].rect.topLeft());
        overlay.beginTextEdit(mapFromLocal(overlay.m_anns[0], overlay.m_anns[0].rect.center()), 0);
        overlay.m_editor->setPlainText(QStringLiteral("旋转文字"));
        check(overlay.m_editor->rotation() == 30,
              "re-editing rotated text retains its native scene rotation");
        overlay.commitTextEdit();
        check(QLineF(rotatedOrigin, mapFromLocal(overlay.m_anns[0], overlay.m_anns[0].rect.topLeft())).length() < 2,
              "changing rotated text keeps its scene origin stable");
        overlay.beginTextEdit(mapFromLocal(overlay.m_anns[0], overlay.m_anns[0].rect.center()), 0);
        overlay.m_editor->setPlainText(QString());
        overlay.commitTextEdit();
        check(overlay.m_anns.isEmpty(), "clearing an existing text item deletes it");
        overlay.undoAnnotation();
        check(overlay.m_anns.size() == 1 && overlay.m_anns[0].text == QStringLiteral("旋转文字"),
              "undo restores deleted text with its formatting and rotation");
        for (ATool tool : {ATool::Arrow, ATool::Marker}) {
            Annotation previous;
            previous.tool = tool;
            previous.color = tool == ATool::Arrow ? QColor("#34c759") : QColor("#ffcc00");
            previous.a = origin + QPoint(450, 200);
            previous.b = origin + QPoint(520, 220);
            previous.stroke << previous.a << previous.b;
            overlay.m_anns.append(previous);
        }
        QVector<QColor> previousColors;
        for (const Annotation &ann : overlay.m_anns) previousColors.append(ann.color);
        overlay.selectTool(ATool::Text);
        overlay.beginTextEdit(origin + QPoint(300, 40));
        overlay.m_editor->setPlainText(QStringLiteral("未结束也能输出"));
        cur = overlay.m_editor->textCursor();
        cur.setPosition(1);
        cur.setPosition(3, QTextCursor::KeepAnchor);
        overlay.m_editor->setTextCursor(cur);
        const int paletteUndoCount = overlay.m_undoStack.size();
        overlay.m_colorButton->click();
        QApplication::processEvents();
        check(overlay.m_colorDialog && overlay.m_colorDialog->isVisible() && overlay.editorActive(),
              "opening the non-modal palette keeps native text editing live");
        auto typeColor = [](QLineEdit *field, const QString &text) {
            field->setFocus();
            field->selectAll();
            QKeyEvent input(QEvent::KeyPress, 0, Qt::NoModifier, text);
            QApplication::sendEvent(field, &input);
        };
        typeColor(overlay.m_rgbInput, QStringLiteral("rgb(255,0,0)"));
        check(overlay.m_editor->defaultTextColor() == QColor(Qt::red)
                  && overlay.m_colorWheel->currentColor() == QColor(Qt::red)
                  && overlay.m_hexInput->text() == QStringLiteral("#ff0000")
                  && overlay.m_editor->textCursor().anchor() == cur.anchor()
                  && overlay.m_editor->textCursor().position() == cur.position(),
              "RGB typing synchronizes the live text, picker, HEX value, and text selection");
        QKeyEvent colorEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(overlay.m_rgbInput, &colorEnter);
        check(overlay.editorActive() && overlay.m_colorDialog->isVisible() && overlay.isVisible(),
              "Enter in a color field does not commit text or pin the capture");
        typeColor(overlay.m_hexInput, QStringLiteral("#123ABC"));
        check(overlay.m_editor->defaultTextColor() == QColor("#123abc")
                  && overlay.m_rgbInput->text() == QStringLiteral("rgb(18,58,188)")
                  && overlay.m_colorWheel->currentColor() == QColor("#123abc")
                  && overlay.m_colorBrightness->value() == 188,
              "HEX typing synchronizes RGB, the picker, and the current text");
        typeColor(overlay.m_rgbInput, QStringLiteral("rgb(300,0,0)"));
        typeColor(overlay.m_hexInput, QStringLiteral("#nothex"));
        check(overlay.m_editor->defaultTextColor() == QColor("#123abc")
                  && overlay.m_colorWheel->currentColor() == QColor("#123abc"),
              "invalid or incomplete manual colors never change the annotation or picker");
        ColorWheel *picker = overlay.m_colorWheel;
        check(picker && picker->width() == picker->height()
                  && overlay.m_colorDialog->findChildren<QLineEdit *>().size() == 2
                  && overlay.m_colorDialog->findChildren<QToolButton *>().isEmpty()
                  && overlay.m_colorDialog->width() < 300 && overlay.m_colorDialog->height() < 350,
              "the compact palette only contains a circular wheel, brightness, preview, and two color fields");
        const QColor beforeDrag = picker->currentColor();
        const QPoint pickerStart(picker->width() / 3, picker->height() / 3);
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
            const QPoint at = type == QEvent::MouseButtonPress ? pickerStart : pickerStart + QPoint(30, 15);
            QMouseEvent mouse(type, at, picker->mapToGlobal(at),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(picker, &mouse);
        }
        const QColor draggedColor = picker->currentColor();
        check(draggedColor != beforeDrag && overlay.m_editor->defaultTextColor() == draggedColor
                  && overlay.m_rgbInput->text() == rgbColorText(draggedColor)
                  && overlay.m_hexInput->text() == draggedColor.name(),
              "clicking and dragging the circular color wheel updates live text and both manual fields");
        auto wheelClick = [picker](const QPointF &point) {
            for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent event(type, point, picker->mapToGlobal(point.toPoint()), Qt::LeftButton,
                                  type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(picker, &event);
            }
        };
        overlay.m_colorBrightness->setValue(255);
        const QRectF circle = picker->wheelRect();
        wheelClick(QPointF(circle.right(), circle.center().y()));
        check(picker->currentColor() == QColor(Qt::red), "the wheel's right edge selects fully saturated red");
        wheelClick(QPointF(circle.left(), circle.center().y()));
        check(picker->currentColor() == QColor(Qt::cyan), "the opposite wheel edge selects cyan");
        wheelClick(circle.center());
        check(picker->currentColor() == QColor(Qt::white), "the wheel center selects neutral white");
        wheelClick(circle.topLeft());
        check(picker->currentColor() == QColor(Qt::white), "clicking outside the circle does not select a color");
        overlay.m_colorBrightness->setValue(128);
        check(picker->currentColor() == QColor(128, 128, 128)
                  && overlay.m_rgbInput->text() == QStringLiteral("rgb(128,128,128)")
                  && overlay.m_hexInput->text() == QStringLiteral("#808080"),
              "brightness adjusts grayscale and synchronizes both color fields");
        overlay.m_colorBrightness->setValue(0);
        wheelClick(QPointF(circle.left(), circle.center().y()));
        check(picker->currentColor() == QColor(Qt::black), "wheel selection at zero brightness stays black");
        overlay.m_colorBrightness->setValue(255);
        check(picker->currentColor() == QColor(Qt::cyan), "raising brightness preserves the color chosen while black");
        QKeyEvent wheelKey(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QApplication::sendEvent(picker, &wheelKey);
        check(picker->currentColor() != QColor(Qt::cyan)
                  && overlay.m_editor->defaultTextColor() == picker->currentColor(),
              "keyboard wheel adjustment remains accessible and updates the active text");
        const QImage renderedWheel = picker->grab().toImage();
        const qreal wheelDpr = renderedWheel.devicePixelRatio();
        const QColor centerPixel = renderedWheel.pixelColor(qRound(circle.center().x() * wheelDpr),
                                                            qRound(circle.center().y() * wheelDpr));
        const QColor rimPixel = renderedWheel.pixelColor(qRound((circle.right() - 6) * wheelDpr),
                                                         qRound(circle.center().y() * wheelDpr));
        if (renderedWheel.pixelColor(0, 0) != QColor(29, 31, 35)
            || centerPixel.red() < 250 || centerPixel.green() < 250 || centerPixel.blue() < 250
            || rimPixel.red() < 250 || rimPixel.green() >= 32 || rimPixel.blue() >= 32)
            std::fprintf(stderr, "wheel pixels corner=%s center=%s rim=%s dpr=%.2f\n",
                         qPrintable(renderedWheel.pixelColor(0, 0).name()), qPrintable(centerPixel.name()),
                         qPrintable(rimPixel.name()), double(wheelDpr));
        check(renderedWheel.pixelColor(0, 0) == QColor(29, 31, 35)
                  && centerPixel.red() >= 250 && centerPixel.green() >= 250 && centerPixel.blue() >= 250
                  && rimPixel.red() >= 250 && rimPixel.green() < 32 && rimPixel.blue() < 32,
              "the rendered wheel has a neutral center, a red rim, and dark corners at the active DPI");
        for (int i = 0; i < previousColors.size(); ++i)
            check(overlay.m_anns[i].color == previousColors[i],
                  "palette changes leave earlier text, arrows, and marker strokes unchanged");
        check(overlay.m_undoStack.size() == paletteUndoCount, "palette changes stay inside the current text transaction");
        QKeyEvent paletteEscape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(overlay.m_colorDialog, &paletteEscape);
        check(overlay.editorActive() && overlay.isVisible() && overlay.m_textView->hasFocus(),
              "closing the palette returns keyboard focus without closing or committing the text");
        check(overlay.m_colorButton->styleSheet().isEmpty(), "live color changes do not restore a palette button border");
        QPixmap pinned;
        QRect pinnedRegion;
        QObject::connect(&overlay, &CaptureOverlay::grabbed,
                         [&pinned, &pinnedRegion](const QPixmap &pix, const QRect &region) {
            pinned = pix;
            pinnedRegion = region;
        });
        const QImage expected = overlay.cropped().toImage();
        const QRect expectedRegion = overlay.m_sel.translated(overlay.geometry().topLeft());
        overlay.pinCapture();
        check(!pinned.isNull() && pinned.toImage() == expected && overlay.m_anns.size() == 4,
              "capture actions commit the live text and output it exactly once");
        check(pinnedRegion == expectedRegion, "pinning preserves the annotated selection's original global geometry");
        std::puts("PASS: native text input, multiline layout, live formatting, re-editing, rotation, undo, and output");
        return 0;
    }
signals:
    void grabbed(const QPixmap &pix, const QRect &globalRegion);
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
        if (m_mode == Adjusting && !m_sel.isNull())   // Annotations clip to the selection.
            paintAnnotations(p, QPointF(0, 0), 1.0, QRectF(m_sel), false);
        if (m_mode == Adjusting && !m_live
            && (editorActive() || (m_selAnn >= 0 && m_selAnn < m_anns.size()))) {
            const Annotation ann = editorActive() ? editorAnnotation() : m_anns[m_selAnn];
            p.save();
            if (!qFuzzyIsNull(ann.rot)) p.setTransform(annTransform(ann), true);
            const QRect box = annotationRect(ann);
            p.setPen(QPen(QColor(30, 144, 255), 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(box.adjusted(-3, -3, 3, 3));
            p.setBrush(Qt::white);
            for (const QPoint &anchor : annAnchors(ann))
                p.drawRect(QRect(anchor - QPoint(4, 4), QSize(8, 8)));
            {                                        // Rotate stalk below the item.
                const QPoint base(box.center().x(), box.bottom() + 2);
                const QPoint knob(box.center().x(), box.bottom() + 22);
                p.drawLine(base, knob);
                p.drawEllipse(QRect(knob - QPoint(5, 5), QSize(10, 10)));
            }
            p.restore();
        }
        if (m_mode == Adjusting && !m_sel.isNull() && m_hover.isNull())  // Eight resize handles.
            for (const QRect &h : handleRects(m_sel)) {
                p.fillRect(h, Qt::white);
                p.drawRect(h);
            }
        for (QWidget *glass : {m_tools, m_toolOptions}) {
            if (!glass->isVisible()) continue;
            const QRect bar = glass->geometry();
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(29, 31, 35));
            p.drawRect(bar);
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        m_snapX = m_snapY = -1;                     // Clear magnetic-snap indicators.
        cancelDetection();
        m_nudging = false;
        const bool wasEditing = editorActive();
        if (wasEditing) commitTextEdit();           // Canvas clicks finish input; controls keep it live.
        if (e->button() == Qt::RightButton) {       // Right click ends input, then the ladder.
            if (m_live) { m_live = false; commitAnnotation(); }
            else if (wasEditing) return;
            else if (m_atool != ATool::None) setToolNone();
            else if (m_selAnn >= 0) { m_selAnn = -1; updateToolOptions(); update(); }
            else if (m_mode == Adjusting) { resetSelection(); } else { close(); }
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        setFocus();                                   // Any click restores keyboard handling.
        m_pressPos = e->pos();
        m_dragStarted = false;
        if (m_mode == Adjusting && m_atool == ATool::Text && !m_spaceHeld) {
            if (m_selAnn >= 0 && m_selAnn < m_anns.size()
                && m_anns[m_selAnn].tool == ATool::Text) {
                const Handle h = annHandleAt(m_anns[m_selAnn], e->pos());
                if (h != H_None) {
                    m_itemDrag = h;
                    m_pressAnn = m_anns[m_selAnn];
                    return;
                }
            }
            const int hit = annotationAt(e->pos());
            if (hit >= 0 && m_anns[hit].tool == ATool::Text) {
                beginTextEdit(e->pos(), hit);
                return;
            }
        }
        if (m_mode == Adjusting && m_atool != ATool::None
            && !m_spaceHeld && hitHandle(e->pos()) == H_Move) { // A tool acts inside the selection.
            if (m_atool == ATool::Text) { beginTextEdit(e->pos()); return; }
            if (m_atool == ATool::Eraser) {          // Drag deletes whole annotations touched.
                m_erasing = true;
                m_eraseSnapshotted = false;
                eraseAt(e->pos());
                return;
            }
            if (m_atool == ATool::Number) {          // Click drops the next sequence number.
                pushUndo();
                Annotation ann;
                ann.tool = ATool::Number;
                ann.color = m_acolor;
                ann.font = m_afont;
                const int s = numberDiameter(ann);
                ann.rect = QRect(e->pos() - QPoint(s / 2, s / 2), QSize(s, s));
                m_anns.append(ann);
                normalizeNumbers();
                m_selAnn = m_anns.size() - 1;        // Auto-select fresh annotations.
                syncSizeInput();
                update(ann.rect.adjusted(-8, -8, 8, 8));
                return;
            }
            beginAnnotation(e->pos());
            return;
        }
        if (m_mode == Adjusting && (m_atool == ATool::None || m_spaceHeld)) {   // Editing mode.
            if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
                const Handle h = annHandleAt(m_anns[m_selAnn], e->pos());
                if (h != H_None) {                   // Corner/endpoint handles resize in place.
                    m_itemDrag = h;
                    m_pressAnn = m_anns[m_selAnn];
                    return;
                }
            }
            const int hit = annotationAt(e->pos());
            if (hit >= 0) {                          // Click selects and drags the annotation.
                m_selAnn = hit;
                m_itemDrag = H_Move;
                m_pressAnn = m_anns[hit];
                updateToolOptions();
                update();
                return;
            }
            m_selAnn = -1;
            updateToolOptions();
        }
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
        if (m_live) {                               // Live annotation follows the cursor.
            const QRect dirty = annotationRect(m_ann);
            updateAnnotation(pos, e->modifiers());
            update(dirty.united(annotationRect(m_ann)).adjusted(-24, -24, 24, 24));
            if (m_ann.rect.isValid() && m_ann.tool != ATool::Pen
                && m_ann.tool != ATool::Marker && m_ann.tool != ATool::Arrow) {
                m_sizeTip->setText(QString("%1 × %2").arg(m_ann.rect.width()).arg(m_ann.rect.height()));
                m_sizeTip->adjustSize();
                m_sizeTip->move(m_ann.rect.topLeft() - QPoint(0, m_sizeTip->height() + 4));
                m_sizeTip->show();
            } else {
                m_sizeTip->hide();
            }
            return;
        }
        if (m_erasing) {                             // The eraser removes whole annotations.
            eraseAt(pos);
            return;
        }
        if (m_itemDrag != H_None) {                  // Move or resize the selected annotation.
            if (!m_dragStarted) {
                if ((pos - m_pressPos).manhattanLength() < QApplication::startDragDistance())
                    return;
                m_dragStarted = true;
                pushUndo();                          // Snapshot before the first modification.
                hideToolBars();
                m_inspector->hide();
            }
            const QRect dirty = rotatedBounds(m_anns[m_selAnn]).adjusted(-24, -24, 24, 24);
            applyItemDrag(pos, e->modifiers());
            update(dirty.united(rotatedBounds(m_anns[m_selAnn])).adjusted(-24, -24, 24, 24));
            return;
        }
        if (m_drag == H_None) {                     // Hover: cursor shape, size hint, window detection.
            if (m_mode == Adjusting) {
                const Handle h = hitHandle(pos);
                setCursor(m_atool != ATool::None && h == H_Move ? Qt::CrossCursor
                                                                : cursorForHandle(h));
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
        hideToolBars();                              // The status bar steps aside during drags.
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
        if (m_live) {                               // Commit the annotation; the tool stays active.
            m_live = false;
            commitAnnotation();
            return;
        }
        if (m_erasing) { m_erasing = false; return; }
        if (m_itemDrag != H_None) {                 // A click only selects; a drag edits.
            const bool moved = m_dragStarted;
            m_itemDrag = H_None;
            m_dragStarted = false;
            if (moved) {
                placeToolbar();
                showToolBars();
            }
            update();
            return;
        }
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
        showToolBars();
        updateTip();
        update();
        if (m_pinAfterDetection) { m_pinAfterDetection = false; pinCapture(); }
    }
    void wheelEvent(QWheelEvent *e) override
    {
        if (!e->angleDelta().y()) return;
        if (editorActive()) {                        // Wheel retunes the live text editor.
            editorFontStep(e->angleDelta().y() > 0 ? 1 : -1);
            return;
        }
        if (m_mode == Adjusting && m_drag == H_None && m_itemDrag == H_None) {
            tuneWidth(e->angleDelta().y() > 0 ? 1 : -1, e->position().toPoint());
            if (m_selAnn >= 0 || m_atool != ATool::None) return;
        }
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
        if (m_colorDialog) m_colorDialog->close();
        cancelDetection();
        if (m_detector) m_detector->stop();
        QWidget::closeEvent(e);
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (editorActive()) return;                  // A text double-click never pins the capture.
        if (m_mode == Adjusting && (m_atool == ATool::None || m_atool == ATool::Text)) {
            const int hit = annotationAt(e->pos());  // Double-click re-opens a text annotation.
            if (hit >= 0 && m_anns[hit].tool == ATool::Text) {
                m_selAnn = hit;
                m_itemDrag = H_None;
                beginTextEdit(e->pos(), hit);
                return;
            }
        }
        if (m_queryPending && (m_waitingClick || m_drag == H_New))
            m_pinAfterDetection = true;
        else if (m_mode == Adjusting && m_atool == ATool::None && m_sel.contains(e->pos()))
            pinCapture();
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (editorActive()) {                        // Unhandled editing keys must not trigger capture actions.
            if (e->key() == Qt::Key_Escape) commitTextEdit();
            else if (e->modifiers() & Qt::ControlModifier) {
                if (e->key() == Qt::Key_S) saveCapture();
                else if (e->key() == Qt::Key_T) pinCapture();
                else if (e->key() == Qt::Key_Z) {
                    if (e->modifiers() & Qt::ShiftModifier) redoAnnotation();
                    else undoAnnotation();
                } else if (e->key() == Qt::Key_Y) redoAnnotation();
            }
            return;
        }
        if ((e->modifiers() & Qt::ControlModifier)
            && !(e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier))) {
            if (e->key() == Qt::Key_T && m_mode == Adjusting && !m_live && !editorActive()) {
                pinCapture();                        // Pin to screen (Ctrl+T).
                return;
            }
        }
        switch (e->key()) {
        case Qt::Key_Escape:
            if (m_live) { m_live = false; endAnnotationDrag(); }
            else if (editorActive()) commitTextEdit();
            else if (m_atool != ATool::None) setToolNone();
            else if (m_selAnn >= 0) { m_selAnn = -1; updateToolOptions(); update(); }
            else close();
            break;
        case Qt::Key_Space:
            if (!e->isAutoRepeat()) m_spaceHeld = true;   // Space turns drags into moves.
            break;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            if (!m_live && !editorActive() && m_mode == Adjusting)
                tuneWidth(1, mapFromGlobal(QCursor::pos()));
            break;
        case Qt::Key_Minus:
            if (!m_live && !editorActive() && m_mode == Adjusting)
                tuneWidth(-1, mapFromGlobal(QCursor::pos()));
            break;
        case Qt::Key_F:                              // Fill mode for shapes and the selection.
            if (e->modifiers() == Qt::NoModifier && !e->isAutoRepeat()
                && !m_live && !editorActive() && m_mode == Adjusting) {
                if (m_selAnn >= 0 && m_selAnn < m_anns.size()
                    && (m_anns[m_selAnn].tool == ATool::Rect
                        || m_anns[m_selAnn].tool == ATool::Ellipse)) {
                    pushUndo();
                    m_anns[m_selAnn].fill = !m_anns[m_selAnn].fill;
                    update();
                } else if (m_atool == ATool::Rect || m_atool == ATool::Ellipse) {
                    m_afill = !m_afill;
                    m_sizeTip->setText(m_afill ? QStringLiteral("填充") : QStringLiteral("描边"));
                    tipPosition(mapFromGlobal(QCursor::pos()));
                    m_sizeTip->show();
                }
            }
            break;
        case Qt::Key_Delete:
            if (m_selAnn >= 0) deleteSelected();
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (m_mode == Adjusting && !m_live) pinCapture();
            break;
        case Qt::Key_C:
            if (e->modifiers() & Qt::ControlModifier) {
                if (m_mode == Adjusting && !m_live) copyCapture();
            } else if (!(e->modifiers() & (Qt::AltModifier | Qt::MetaModifier))
                       && m_inspector->isVisible() && !m_inspector->colorText().isEmpty()) {
                QGuiApplication::clipboard()->setText(m_inspector->colorText());
            }
            break;
        case Qt::Key_Z:
            if (e->modifiers() & Qt::ControlModifier) {
                if (e->modifiers() & Qt::ShiftModifier) redoAnnotation();
                else undoAnnotation();
            }
            break;
        case Qt::Key_Y:
            if (e->modifiers() & Qt::ControlModifier) redoAnnotation();
            break;
        case Qt::Key_Shift:
            if (!e->isAutoRepeat()) m_inspector->toggleFormat();
            break;
        case Qt::Key_S:
            if (e->modifiers() & Qt::ControlModifier && !m_live) saveCapture();
            else if (e->modifiers() == Qt::NoModifier && !e->isAutoRepeat())
                nudgeSelected(Qt::Key_S);            // WASD nudges the selection.
            break;
        default:
            if (e->modifiers() == Qt::NoModifier && !e->isAutoRepeat()
                && !m_live && !editorActive()) {
                if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
                    nudgeSelected(e->key());        // Arrows / WASD move the selected item.
                    return;
                }
                const int key = e->key();           // Single letters arm annotation tools.
                const ATool tool = key == Qt::Key_R ? ATool::Rect
                    : key == Qt::Key_O ? ATool::Ellipse
                    : key == Qt::Key_A ? ATool::Arrow
                    : key == Qt::Key_P ? ATool::Pen
                    : key == Qt::Key_H ? ATool::Marker
                    : key == Qt::Key_T ? ATool::Text
                    : key == Qt::Key_N ? ATool::Number
                    : key == Qt::Key_M ? ATool::Mosaic
                    : key == Qt::Key_E ? ATool::Eraser : ATool::None;
                if (tool != ATool::None)
                    selectTool(tool == m_atool ? ATool::None : tool);   // Same key toggles off.
            }
            break;
        }
    }
    void keyReleaseEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Space) m_spaceHeld = false;
    }
private:
    enum Handle { H_None = 0, H_New, H_Move, H_TL, H_TR, H_BL, H_BR, H_L, H_R, H_T, H_B, H_Rotate };

    // ---------- instant annotation ----------
    enum class ATool { None, Rect, Ellipse, Arrow, Pen, Marker, Text, Number, Mosaic, Eraser };
    struct Annotation {
        ATool tool = ATool::None;
        QColor color;
        int pen = 3;                                 // Stroke width / mosaic block / blur grain.
        QRect rect;                                  // Rect / Ellipse / Mosaic / Blur / Text / Number box.
        QPoint a, b;                                 // Arrow endpoints.
        QPolygonF stroke;                            // Pen / Marker polyline.
        QString text;                                // Text / Number content.
        qreal font = 15;                             // Text / Number point size (1/72 inch).
        QString family = QStringLiteral("Microsoft YaHei UI");
        int arrowStyle = 0;                          // 0 single, 1 double, 2 none, 3 hollow.
        bool bold = false;                           // Text weight / slant.
        bool italic = false;
        qreal rot = 0;                               // Rotation around the box center (degrees).
        bool fill = false;                           // Rect / Ellipse translucent fill mode.
    };
    static QRect annotationRect(const Annotation &ann)
    {
        switch (ann.tool) {
        case ATool::Arrow:  return QRect(ann.a, ann.b).normalized();
        case ATool::Pen:    return ann.stroke.boundingRect().toRect();
        case ATool::Marker: {                          // Fat translucent stroke: include its width.
            const int w = ann.pen * 5 / 2 + 2;
            return ann.stroke.boundingRect().toRect().adjusted(-w, -w, w, w);
        }
        default:            return ann.rect;
        }
    }
    // Rotation helpers: item geometry stays in local space, the angle only transforms paint and hit tests.
    static QTransform annTransform(const Annotation &ann)
    {
        QTransform t;
        if (qFuzzyIsNull(ann.rot)) return t;
        const QPointF c = QPointF(annotationRect(ann).center());
        t.translate(c.x(), c.y());
        t.rotate(ann.rot);
        t.translate(-c.x(), -c.y());
        return t;
    }
    static QRect rotatedBounds(const Annotation &ann)
    {
        const QRect r = annotationRect(ann);
        return qFuzzyIsNull(ann.rot) ? r : annTransform(ann).mapToPolygon(r).boundingRect();
    }
    static QPoint mapToLocal(const Annotation &ann, const QPoint &pos)
    {
        return qFuzzyIsNull(ann.rot) ? pos : annTransform(ann).inverted().map(QPointF(pos)).toPoint();
    }
    static QPoint mapFromLocal(const Annotation &ann, const QPoint &pos)
    {
        return qFuzzyIsNull(ann.rot) ? pos : annTransform(ann).map(QPointF(pos)).toPoint();
    }
    // ---- object editing: hit tests, selection anchors, move/resize, undo/redo ----
    static qreal distToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
    {
        const qreal len = QLineF(a, b).length();
        if (len < 0.001) return QLineF(a, p).length();
        const qreal t = qBound(0.0, ((p.x() - a.x()) * (b.x() - a.x())
                                   + (p.y() - a.y()) * (b.y() - a.y())) / (len * len), 1.0);
        return QLineF(p, a + (b - a) * t).length();
    }
    static bool annotationHit(const Annotation &ann, const QPoint &pos, int tolBoost = 0)
    {
        const int tol = qMax(5, ann.pen / 2 + 4);    // Areas keep the base reach; the boost
        const int lineTol = tol + tolBoost;          // only widens outline / stroke hits.
        switch (ann.tool) {
        case ATool::Rect:                            // Outlines select on their border only.
            return ann.rect.adjusted(-lineTol, -lineTol, lineTol, lineTol).contains(pos)
                && !ann.rect.adjusted(tol, tol, -tol, -tol).contains(pos);
        case ATool::Ellipse: {
            const QPointF d = pos - ann.rect.center();
            const qreal rx = ann.rect.width() / 2.0, ry = ann.rect.height() / 2.0;
            if (rx < 1 || ry < 1) return ann.rect.contains(pos);
            const qreal outer = qPow(d.x() / (rx + lineTol), 2) + qPow(d.y() / (ry + lineTol), 2);
            const qreal inner = qPow(d.x() / qMax(1.0, rx - tol), 2)
                              + qPow(d.y() / qMax(1.0, ry - tol), 2);
            return outer <= 1.0 && inner >= 1.0;
        }
        case ATool::Arrow:
            return distToSegment(QPointF(pos), ann.a, ann.b) <= lineTol + ann.pen / 2;
        case ATool::Pen:
        case ATool::Marker:
            if (ann.stroke.isEmpty()) return false;
            for (int i = 1; i < ann.stroke.size(); ++i)
                if (distToSegment(QPointF(pos), ann.stroke[i - 1], ann.stroke[i])
                    <= lineTol + ann.pen / 2)
                    return true;
            return distToSegment(QPointF(pos), ann.stroke.first(), ann.stroke.first())
                <= lineTol + ann.pen / 2;
        default:                                     // Area objects select anywhere inside.
            return ann.rect.adjusted(-tol, -tol, tol, tol).contains(pos);
        }
    }
    int annotationAt(const QPoint &pos) const        // Topmost annotation first.
    {
        for (int i = m_anns.size() - 1; i >= 0; --i)
            if (annotationHit(m_anns[i], mapToLocal(m_anns[i], pos))) return i;
        return -1;
    }
    // Corner anchors for area shapes, endpoint anchors for arrows; strokes and text move only.
    QVector<QPoint> annAnchors(const Annotation &ann) const
    {
        if (ann.tool == ATool::Arrow) return {ann.a, ann.b};
        switch (ann.tool) {
        case ATool::Rect: case ATool::Ellipse: case ATool::Mosaic:
        case ATool::Text: case ATool::Number: {
            const QRect r = ann.rect.normalized();
            return {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()};
        }
        default:
            return {};
        }
    }
    Handle annHandleAt(const Annotation &ann, const QPoint &pos) const
    {
        if (QRect(rotateAnchor(ann) - QPoint(6, 6), QSize(12, 12)).contains(pos)) return H_Rotate;
        const QVector<QPoint> anchors = annAnchors(ann);
        static const Handle ids[] = {H_TL, H_TR, H_BL, H_BR, H_TL, H_BR};
        for (int i = 0; i < anchors.size(); ++i)
            if (QRect(mapFromLocal(ann, anchors[i]) - QPoint(5, 5), QSize(10, 10)).contains(pos))
                return ids[i];
        return H_None;
    }
    static QPoint rotateAnchor(const Annotation &ann)  // Rotate stalk sits below the item.
    {
        const QRect r = annotationRect(ann);
        return mapFromLocal(ann, QPoint(r.center().x(), r.bottom() + 22));
    }
    static void translateAnnotation(Annotation &ann, const QPoint &d)
    {
        ann.rect.translate(d);
        ann.a += d;
        ann.b += d;
        ann.stroke.translate(d);
    }
    void applyItemDrag(const QPoint &pos, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        Annotation ann = m_pressAnn;                 // Always computed from the press snapshot.
        if (m_itemDrag == H_Move) translateAnnotation(ann, pos - m_pressPos);
        else if (m_itemDrag == H_Rotate) {           // Drag the stalk to rotate around the center.
            const QPointF c = QPointF(annotationRect(ann).center());
            qreal delta = QLineF(c, QPointF(m_pressPos)).angle()
                        - QLineF(c, QPointF(pos)).angle();
            qreal rot = ann.rot + delta;
            if (mods & Qt::ShiftModifier) rot = qRound(rot / 15.0) * 15.0;   // Snap to 15 degrees.
            ann.rot = rot;
        } else {
            const QPoint local = mapToLocal(ann, pos);   // Handle math runs in item space.
            if (ann.tool == ATool::Arrow) {
                if (m_itemDrag == H_TL) ann.a = local; else ann.b = local;
            } else {
                const QRect r = m_pressAnn.rect.normalized();
                const QPoint fixed = m_itemDrag == H_TL ? r.bottomRight()
                    : m_itemDrag == H_TR ? r.bottomLeft()
                    : m_itemDrag == H_BL ? r.topRight() : r.topLeft();
                ann.rect = QRect(fixed, local).normalized();
                if (ann.tool == ATool::Text) {       // Resize letters, not an invisible wrapping box.
                    const qreal scale = qMax(qreal(ann.rect.width()) / qMax(1, r.width()),
                                             qreal(ann.rect.height()) / qMax(1, r.height()));
                    ann.font = qBound(qreal(1), qRound(m_pressAnn.font * scale * 2) / 2.0, qreal(1638));
                    fitTextAnnotation(ann);
                    if (m_itemDrag == H_TL) ann.rect.moveBottomRight(fixed);
                    else if (m_itemDrag == H_TR) ann.rect.moveBottomLeft(fixed);
                    else if (m_itemDrag == H_BL) ann.rect.moveTopRight(fixed);
                    else ann.rect.moveTopLeft(fixed);
                } else if (ann.tool == ATool::Number) { // Circles stay square while resizing.
                    const int s = qMax(ann.rect.width(), ann.rect.height());
                    ann.rect = QRect(ann.rect.topLeft(), QSize(s, s));
                }
            }
        }
        m_anns[m_selAnn] = ann;
        syncSizeInput();
    }
    void pushUndo()                                  // Snapshot stacks: one list per step.
    {
        m_undoStack.append(m_anns);
        if (m_undoStack.size() > 100) m_undoStack.removeFirst();
        m_redoStack.clear();
        syncUndoButtons();
    }
    void syncUndoButtons()                           // Buttons follow the stacks, like the bar.
    {
        if (m_undoButton) m_undoButton->setEnabled(!m_undoStack.isEmpty());
        if (m_redoButton) m_redoButton->setEnabled(!m_redoStack.isEmpty());
    }
    void normalizeNumbers()                          // Sequence numbers follow list order.
    {
        int n = 0;
        for (Annotation &ann : m_anns)
            if (ann.tool == ATool::Number) ann.text = QString::number(++n);
    }
    void eraseAt(const QPoint &pos)
    {
        const int tolBoost = m_apen * 2;              // Eraser thickness widens the hit zone.
        for (int i = m_anns.size() - 1; i >= 0; --i)
            if (annotationHit(m_anns[i], mapToLocal(m_anns[i], pos), tolBoost)) {
                if (!m_eraseSnapshotted) {           // One snapshot covers the whole drag.
                    pushUndo();
                    m_eraseSnapshotted = true;
                }
                const QRect dirty = rotatedBounds(m_anns[i]).adjusted(-24, -24, 24, 24);
                m_anns.removeAt(i);
                if (m_selAnn == i) m_selAnn = -1;
                else if (m_selAnn > i) --m_selAnn;
                normalizeNumbers();
                update(dirty);
            }
    }
    void deleteSelected()
    {
        if (m_selAnn < 0 || m_selAnn >= m_anns.size()) { m_selAnn = -1; return; }
        pushUndo();
        m_anns.removeAt(m_selAnn);
        m_selAnn = -1;
        normalizeNumbers();
        updateToolOptions();
        update();
    }
    static QFont textFont(const Annotation &ann)
    {
        QFont font(ann.family);
        font.setPointSizeF(ann.font);
        font.setBold(ann.bold);
        font.setItalic(ann.italic);
        return font;
    }
    static int numberDiameter(const Annotation &ann)
    {
        return qMax(1, QFontInfo(textFont(ann)).pixelSize()) * 2;
    }
    static void setupTextDocument(QTextDocument &doc, const Annotation &ann)
    {
        doc.setDefaultFont(textFont(ann));
        doc.setDocumentMargin(6);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        doc.setDefaultTextOption(option);
        doc.setPlainText(ann.text);
        doc.setTextWidth(-1);
    }
    static void fitTextAnnotation(Annotation &ann)
    {
        const QPoint origin = mapFromLocal(ann, ann.rect.topLeft());
        QTextDocument doc;
        setupTextDocument(doc, ann);
        const QSizeF size = doc.size();
        ann.rect.setSize(QSize(qCeil(size.width()), qCeil(size.height())));
        ann.rect.translate(origin - mapFromLocal(ann, ann.rect.topLeft()));
    }
    // ---- native scene text editing: one layout for input, selection, and capture ----
    bool editorActive() const { return m_textView && m_textView->isVisible(); }
    Annotation editorAnnotation() const
    {
        Annotation ann;
        ann.tool = ATool::Text;
        ann.text = m_editor->toPlainText();
        ann.font = m_editFont;
        ann.family = m_editFamily;
        ann.color = m_editColor;
        ann.bold = m_editBold;
        ann.italic = m_editItalic;
        ann.rot = m_editor->rotation();
        const QSizeF size = m_editor->boundingRect().size();
        // shortcut: Text geometry rounds to logical pixels; use fractional bounds for exact subpixel rotation.
        ann.rect = QRect(m_editor->pos().toPoint(), QSize(qCeil(size.width()), qCeil(size.height())));
        return ann;
    }
    void syncTextEditorGeometry()
    {
        if (!editorActive()) return;
        const QPointF origin = m_editor->mapToScene(QPointF());
        m_editor->setTransformOriginPoint(m_editor->boundingRect().center());
        m_editor->setPos(m_editor->pos() + origin - m_editor->mapToScene(QPointF()));
        const QPolygon mask = m_textView->mapFromScene(
            m_editor->mapToScene(m_editor->boundingRect()));
        m_textView->setMask(QRegion(mask).intersected(QRegion(m_sel)));
        update();
    }
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (m_colorDialog && (obj == m_rgbInput || obj == m_hexInput) && ev->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(ev)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                const QColor color = parseAnnotationColor(static_cast<QLineEdit *>(obj)->text());
                if (color.isValid()) pickToolColor(m_colorDialogTool, color);
                return true;                         // Enter applies a color; it never pins the capture.
            }
        }
        if (m_sizeInput && (obj == m_sizeInput || obj == m_sizeInput->findChild<QLineEdit *>())) {
            if (ev->type() == QEvent::Wheel) {
                const int delta = static_cast<QWheelEvent *>(ev)->angleDelta().y();
                if (delta) tuneWidth(delta > 0 ? 1 : -1, mapFromGlobal(QCursor::pos()));
                ev->accept();
                return true;
            }
            if (ev->type() == QEvent::KeyPress) {
                const int key = static_cast<QKeyEvent *>(ev)->key();
                if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) {
                    if (key == Qt::Key_Escape) syncSizeInput();
                    else commitSizeInput();
                    if (editorActive()) {
                        m_textView->setFocus();
                        m_editor->setFocus();
                    } else setFocus();
                    return true;
                }
            }
        }
        if (editorActive() && (obj == m_textView || obj == m_textView->viewport())) {
            if (ev->type() == QEvent::InputMethod) {
                m_textPreedit = !static_cast<QInputMethodEvent *>(ev)->preeditString().isEmpty();
            } else if (ev->type() == QEvent::KeyPress) {
                auto *ke = static_cast<QKeyEvent *>(ev);
                if (ke->key() == Qt::Key_Escape) {
                    if (m_textPreedit) QGuiApplication::inputMethod()->reset();
                    else commitTextEdit();
                    m_textPreedit = false;
                    return true;
                }
                if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
                    && ke->modifiers() == Qt::ControlModifier && !m_textPreedit) {
                    QTextCursor cur = m_editor->textCursor();
                    cur.movePosition(QTextCursor::EndOfBlock);
                    cur.insertText(QStringLiteral("\n"));
                    m_editor->setTextCursor(cur);
                    return true;
                }
                if (ke->key() == Qt::Key_V && ke->modifiers() == Qt::ControlModifier) {
                    QTextCursor cur = m_editor->textCursor();
                    cur.insertText(QGuiApplication::clipboard()->text());
                    m_editor->setTextCursor(cur);
                    return true;
                }
            } else if (ev->type() == QEvent::Wheel) {
                const int delta = static_cast<QWheelEvent *>(ev)->angleDelta().y();
                if (delta) editorFontStep(delta > 0 ? 1 : -1);
                return true;
            } else if (ev->type() == QEvent::ContextMenu) {
                return true;
            } else if (obj == m_textView->viewport() && ev->type() == QEvent::MouseButtonPress) {
                auto *me = static_cast<QMouseEvent *>(ev);
                if (me->button() == Qt::RightButton) { commitTextEdit(); return true; }
                if (me->button() == Qt::MiddleButton) {
                    QTextCursor cur = m_editor->textCursor();
                    cur.insertText(QGuiApplication::clipboard()->text());
                    m_editor->setTextCursor(cur);
                    return true;
                }
                const QPointF local = m_editor->mapFromScene(m_textView->mapToScene(me->pos()));
                if (me->button() == Qt::LeftButton
                    && !m_editor->boundingRect().adjusted(6, 6, -6, -6).contains(local)) {
                    m_textMoving = true;
                    m_textMovePress = me->globalPos();
                    m_textMoveOrigin = m_editor->pos();
                    m_textView->viewport()->grabMouse();
                    return true;
                }
            } else if (m_textMoving && ev->type() == QEvent::MouseMove) {
                auto *me = static_cast<QMouseEvent *>(ev);
                const QPoint delta = me->globalPos() - m_textMovePress;
                if (m_editor->pos() == m_textMoveOrigin
                    && delta.manhattanLength() < QApplication::startDragDistance()) return true;
                m_editor->setPos(m_textMoveOrigin + delta);
                syncTextEditorGeometry();
                return true;
            } else if (m_textMoving && ev->type() == QEvent::MouseButtonRelease) {
                m_textMoving = false;
                m_textView->viewport()->releaseMouse();
                return true;
            }
        }
        return QWidget::eventFilter(obj, ev);
    }
    void beginTextEdit(const QPoint &pos, int existing = -1)
    {
        commitTextEdit();
        m_editAnn = existing;
        m_editFont = existing >= 0 ? m_anns[existing].font : m_afont;
        m_editFamily = existing >= 0 ? m_anns[existing].family : m_afamily;
        m_editColor = existing >= 0 ? m_anns[existing].color : m_acolor;
        m_editBold = existing >= 0 ? m_anns[existing].bold : m_abold;
        m_editItalic = existing >= 0 ? m_anns[existing].italic : m_aitalic;
        m_atool = ATool::Text;
        m_selAnn = existing;
        m_acolor = m_colorText = m_editColor;
        m_afont = m_editFont;
        m_afamily = m_editFamily;
        m_abold = m_editBold;
        m_aitalic = m_editItalic;
        syncToolButtons();
        updateToolOptions();
        if (!m_editor) {
            m_textView = new QGraphicsView(this);
            auto *scene = new QGraphicsScene(m_textView);
            m_textView->setScene(scene);
            m_textView->setFrameShape(QFrame::NoFrame);
            m_textView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            m_textView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            m_textView->setAlignment(Qt::AlignLeft | Qt::AlignTop);
            m_textView->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
            m_textView->setRenderHint(QPainter::Antialiasing);
            m_textView->setGeometry(rect());
            m_textView->setSceneRect(QRectF(rect()));
            m_textView->hide();
            m_editor = new QGraphicsTextItem;
            scene->addItem(m_editor);
            m_editor->setTextInteractionFlags(Qt::TextEditorInteraction);
            m_editor->setTextWidth(-1);
            m_editor->document()->setDocumentMargin(6);
            QTextOption option;
            option.setWrapMode(QTextOption::NoWrap);
            m_editor->document()->setDefaultTextOption(option);
            m_textView->installEventFilter(this);
            m_textView->viewport()->installEventFilter(this);
            connect(m_editor->document()->documentLayout(),
                    &QAbstractTextDocumentLayout::documentSizeChanged,
                    this, [this] { syncTextEditorGeometry(); });
        }
        applyEditorFont();
        m_editor->setRotation(0);
        m_editor->setPlainText(existing >= 0 ? m_anns[existing].text : QString());
        m_editor->setTransformOriginPoint(m_editor->boundingRect().center());
        m_editor->setPos(existing >= 0 ? m_anns[existing].rect.topLeft() : pos - QPoint(6, 6));
        if (existing >= 0) m_editor->setRotation(m_anns[existing].rot);
        QTextCursor cur = m_editor->textCursor();
        const int hit = m_editor->document()->documentLayout()->hitTest(
            m_editor->mapFromScene(QPointF(pos)), Qt::FuzzyHit);
        cur.setPosition(qMax(0, hit));
        m_editor->setTextCursor(cur);
        ImmAssociateContext(reinterpret_cast<HWND>(winId()), m_imeCtx);
        m_textView->show();
        m_textView->raise();
        m_tools->raise();
        m_toolOptions->raise();
        m_sizeTip->hide();
        m_inspector->hide();
        m_textView->setFocus();
        m_editor->setFocus(Qt::MouseFocusReason);
        syncTextEditorGeometry();
        update();
    }
    void commitTextEdit()
    {
        if (m_colorDialog) m_colorDialog->close();
        if (m_committingText || !editorActive()) return;
        m_committingText = true;
        QGuiApplication::inputMethod()->commit();
        const Annotation edited = editorAnnotation();
        const QString text = edited.text;
        m_textView->hide();
        m_textMoving = m_textPreedit = false;
        ImmAssociateContext(reinterpret_cast<HWND>(winId()), nullptr); // Back to hotkey mode.
        const bool replace = m_editAnn >= 0 && m_editAnn < m_anns.size();
        bool changed = !text.trimmed().isEmpty();
        if (replace) {
            const Annotation &old = m_anns[m_editAnn];
            changed = old.text != text || old.rect != edited.rect || old.font != edited.font || old.family != edited.family
                || old.color != edited.color || old.bold != edited.bold || old.italic != edited.italic
                || old.rot != edited.rot;
        }
        if (changed) {
            pushUndo();
            if (text.trimmed().isEmpty()) {         // Clearing the text deletes the annotation.
                m_anns.removeAt(m_editAnn);
                if (m_selAnn == m_editAnn) m_selAnn = -1;
                else if (m_selAnn > m_editAnn) --m_selAnn;
            } else if (replace) {
                m_anns[m_editAnn] = edited;
                m_selAnn = m_editAnn;
            } else {
                m_anns.append(edited);
                m_selAnn = m_anns.size() - 1;        // Auto-select fresh annotations.
            }
        }
        m_editAnn = -1;
        m_committingText = false;
        setFocus();
        syncSizeInput();
        update();
    }
    void applyEditorFont()                          // Size, weight, and slant in one place.
    {
        if (!m_editor) return;
        Annotation ann;
        ann.font = m_editFont;
        ann.family = m_editFamily;
        ann.bold = m_editBold;
        ann.italic = m_editItalic;
        m_editor->setFont(textFont(ann));
        m_editor->setDefaultTextColor(m_editColor);
        syncTextEditorGeometry();
    }
    void editorFontStep(int dir)
    {
        setToolSize(m_editFont + dir * 0.5);
    }
    void tipPosition(const QPoint &at)
    {
        m_sizeTip->adjustSize();
        m_sizeTip->move(qBound(0, at.x() - m_sizeTip->width() / 2, width() - m_sizeTip->width()),
                        qMax(0, at.y() - m_sizeTip->height() - 16));
    }
    void nudgeSelected(int key)                       // Arrows / WASD move the selection 1px.
    {
        if (m_selAnn < 0 || m_selAnn >= m_anns.size() || m_live || editorActive())
            return;
        const QPoint d = key == Qt::Key_Left || key == Qt::Key_A ? QPoint(-1, 0)
            : key == Qt::Key_Right || key == Qt::Key_D ? QPoint(1, 0)
            : key == Qt::Key_Up || key == Qt::Key_W ? QPoint(0, -1)
            : key == Qt::Key_Down || key == Qt::Key_S ? QPoint(0, 1) : QPoint();
        if (d.isNull()) return;
        if (!m_nudging) {
            pushUndo();                               // Coalesce a key burst into one step.
            m_nudging = true;
        }
        translateAnnotation(m_anns[m_selAnn], d);
        update();
    }
    ATool optionsTool() const
    {
        if (editorActive()) return ATool::Text;
        if (m_live) return m_ann.tool;
        if (m_atool != ATool::Eraser && m_selAnn >= 0 && m_selAnn < m_anns.size())
            return m_anns[m_selAnn].tool;
        return m_atool;
    }
    qreal toolSize() const
    {
        if (editorActive()) return m_editFont;
        const ATool tool = optionsTool();
        const bool texty = tool == ATool::Text || tool == ATool::Number;
        if (m_live) return texty ? m_ann.font : m_ann.pen;
        if (m_atool != ATool::Eraser && m_selAnn >= 0 && m_selAnn < m_anns.size())
            return texty ? m_anns[m_selAnn].font : m_anns[m_selAnn].pen;
        return texty ? m_afont : m_apen;
    }
    void syncSizeInput()
    {
        if (!m_sizeInput) return;
        const QSignalBlocker lock(m_sizeInput);
        m_sizeInput->setEditText(QString::number(toolSize()));
    }
    void commitSizeInput()
    {
        if (!m_sizeInput) return;
        bool ok = false;
        const qreal value = m_sizeInput->currentText().toDouble(&ok);
        if (ok && m_sizeInput->lineEdit()->hasAcceptableInput()) setToolSize(value);
        syncSizeInput();
    }
    void setToolSize(qreal value)                    // Numeric edits and wheel input share validation and undo.
    {
        const ATool tool = optionsTool();
        if (tool == ATool::None || !qIsFinite(value)) return;
        const bool texty = tool == ATool::Text || tool == ATool::Number;
        value = texty ? qBound(qreal(1), value, qreal(1638)) : qBound(1, qRound(value), 12);
        if (value == toolSize()) { syncSizeInput(); return; }
        if (editorActive()) {
            m_afont = m_editFont = value;
            applyEditorFont();
        } else if (m_live) {
            m_ann.pen = m_apen = qRound(value);
        } else if (m_atool != ATool::Eraser && m_selAnn >= 0 && m_selAnn < m_anns.size()) {
            pushUndo();
            Annotation &ann = m_anns[m_selAnn];
            if (texty) ann.font = value; else ann.pen = qRound(value);
            if (ann.tool == ATool::Text) fitTextAnnotation(ann);
            if (ann.tool == ATool::Number) {          // Circles stay square and wrap the digits.
                const int s = numberDiameter(ann);
                ann.rect = QRect(ann.rect.center() - QPoint(s / 2, s / 2), QSize(s, s));
            }
        } else if (texty) m_afont = value;
        else m_apen = qRound(value);
        syncSizeInput();
        update();
    }
    void tuneWidth(int dir, const QPoint &at)
    {
        const ATool tool = optionsTool();
        if (tool == ATool::None) return;
        const bool texty = tool == ATool::Text || tool == ATool::Number;
        setToolSize(toolSize() + dir * (texty ? 0.5 : 1));
        m_sizeTip->setText(texty ? QStringLiteral("字号 %1 pt").arg(toolSize())
                               : QStringLiteral("粗细 %1").arg(toolSize()));
        tipPosition(at);
        m_sizeTip->show();
    }
    void beginAnnotation(const QPoint &pos)
    {
        m_live = true;
        m_ann = Annotation();
        m_ann.tool = m_atool;
        m_ann.color = m_acolor;
        m_ann.pen = m_apen;
        if (m_atool == ATool::Rect || m_atool == ATool::Ellipse) m_ann.fill = m_afill;
        if (m_atool == ATool::Arrow) m_ann.arrowStyle = m_arrowStyle;
        if (m_atool == ATool::Pen || m_atool == ATool::Marker) m_ann.stroke << pos;
        else if (m_atool == ATool::Arrow) m_ann.a = m_ann.b = pos;
        else m_ann.rect = QRect(pos, pos);
        syncSizeInput();                             // Keep the active options visible while drawing.
        m_inspector->hide();
        m_sizeTip->hide();
    }
    void updateAnnotation(const QPoint &pos, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        switch (m_atool) {
        case ATool::Pen:
        case ATool::Marker:
            if ((pos - m_ann.stroke.last().toPoint()).manhattanLength() >= 2)
                m_ann.stroke << pos;                 // Decimate near-duplicate samples.
            break;
        case ATool::Arrow: {
            QPointF b = pos;
            if (mods & Qt::ShiftModifier) {          // Snap the direction to 45-degree steps.
                const QLineF line(QPointF(m_ann.a), QPointF(pos));
                if (line.length() > 1) {
                    const qreal angle = qRound(qAtan2(-(pos.y() - m_ann.a.y()),
                                                        pos.x() - m_ann.a.x())
                                               / (M_PI / 4)) * (M_PI / 4);
                    b = QPointF(m_ann.a.x() + qCos(angle) * line.length(),
                                m_ann.a.y() - qSin(angle) * line.length());
                }
            }
            m_ann.b = b.toPoint();
            break;
        }
        default: {
            QPoint end = pos;
            if (mods & Qt::ShiftModifier) {          // Shift keeps squares and circles true.
                const int dx = pos.x() - m_pressPos.x(), dy = pos.y() - m_pressPos.y();
                const int s = qMax(qAbs(dx), qAbs(dy));
                end = QPoint(m_pressPos.x() + (dx < 0 ? -s : s),
                             m_pressPos.y() + (dy < 0 ? -s : s));
            }
            m_ann.rect = QRect(m_pressPos, end).normalized();
            break;
        }
        }
    }
    void commitAnnotation()
    {
        const bool meaningful = m_ann.tool == ATool::Pen || m_ann.tool == ATool::Marker
            ? m_ann.stroke.size() >= 2
            : m_ann.tool == ATool::Arrow
                  ? (m_ann.a - m_ann.b).manhattanLength() >= 6
                  : m_ann.rect.width() >= 3 && m_ann.rect.height() >= 3;
        if (meaningful) {                            // Discard accidental tiny strokes.
            pushUndo();
            m_anns.append(m_ann);
            normalizeNumbers();
            m_selAnn = m_anns.size() - 1;            // Auto-select fresh annotations for editing.
        }
        endAnnotationDrag();
    }
    void endAnnotationDrag()                         // Restore what stepped aside.
    {
        m_sizeTip->hide();
        placeToolbar();
        showToolBars();
        syncSizeInput();
        update();
    }
    void setToolNone()
    {
        commitTextEdit();
        m_atool = ATool::None;
        syncToolButtons();
        updateToolOptions();
        update();
    }
    void selectTool(ATool tool)                      // Shared by toolbar toggles and keyboard.
    {
        commitTextEdit();
        m_atool = tool;
        if (tool == ATool::Rect) m_shapeEllipse = false;        // R/O/popup pick the sub-shape.
        else if (tool == ATool::Ellipse) m_shapeEllipse = true;
        m_selAnn = -1;                               // Tools and item selection exclude each other.
        m_acolor = toolColor(tool);
        syncToolButtons();
        updateToolOptions();
        setCursor(Qt::CrossCursor);
        update();
    }
    void syncToolButtons()                           // Rect and Ellipse share one button.
    {
        const QToolButton *active = m_atool == ATool::None ? nullptr : m_toolBtn[int(m_atool)];
        for (int t = 1; t < m_toolBtn.size(); ++t) {
            QSignalBlocker lock(m_toolBtn[t]);
            m_toolBtn[t]->setChecked(m_toolBtn[t] == active);
        }
        const QSignalBlocker lock(m_shapeButton);
        m_shapeButton->setIcon(makeIcon(m_shapeEllipse ? Icon::Ellipse : Icon::Rect));
    }
    QColor toolColor(ATool tool) const               // Each tool remembers its own color.
    {
        switch (tool) {
        case ATool::Rect: case ATool::Ellipse: return m_colorShape;
        case ATool::Arrow: return m_colorArrow;
        case ATool::Pen: return m_colorPen;
        case ATool::Marker: return m_colorMarker;
        case ATool::Text: return m_colorText;
        case ATool::Number: return m_colorNumber;
        default: return m_acolor;
        }
    }
    void pickToolColor(ATool family, const QColor &color, QLineEdit *source = nullptr)
    {
        if (!color.isValid()) return;
        switch (family) {
        case ATool::Rect: case ATool::Ellipse: m_colorShape = color; break;
        case ATool::Arrow: m_colorArrow = color; break;
        case ATool::Pen: m_colorPen = color; break;
        case ATool::Marker: m_colorMarker = color; break;
        case ATool::Text: m_colorText = color; break;
        case ATool::Number: m_colorNumber = color; break;
        default: return;
        }
        const bool sameFamily = [](ATool a, ATool b) {
            const bool shapes = (a == ATool::Rect || a == ATool::Ellipse)
                             && (b == ATool::Rect || b == ATool::Ellipse);
            return a == b || shapes;
        }(m_atool, family);
        if (sameFamily) m_acolor = color;
        if (editorActive() && family == ATool::Text) {
            m_editColor = color;
            applyEditorFont();
        } else if (m_live && sameFamily) {
            m_ann.color = color;                    // Committed annotations retain their original colors.
            update();
        }
        if (m_colorDialog && m_colorDialogTool == family) {
            const QSignalBlocker lock(m_colorWheel);
            m_colorWheel->setCurrentColor(color);
            const QSignalBlocker sliderLock(m_colorBrightness);
            m_colorBrightness->setValue(m_colorWheel->brightness());
            m_colorBrightness->setStyleSheet(QStringLiteral(
                "QSlider::groove:vertical{width:12px;border:1px solid #626468;"
                "background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %1,stop:1 #000000);}"
                "QSlider::handle:vertical{background:#fff;border:1px solid #626468;height:8px;margin:0 -3px;}")
                .arg(m_colorWheel->brightColor().name()));
            m_colorPreview->setStyleSheet(QStringLiteral("background:%1;border:1px solid #929498;border-radius:3px;").arg(color.name()));
            for (QLineEdit *field : {m_rgbInput.data(), m_hexInput.data()}) {
                if (!field || field == source) continue;
                const QSignalBlocker fieldLock(field);
                field->setText(field == m_rgbInput ? rgbColorText(color) : color.name());
                field->setStyleSheet(QString());
            }
        }
    }
    static QString rgbColorText(const QColor &color)
    {
        return QStringLiteral("rgb(%1,%2,%3)").arg(color.red()).arg(color.green()).arg(color.blue());
    }
    static QColor parseAnnotationColor(const QString &text)
    {
        const QString value = text.trimmed();
        static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
        if (hex.match(value).hasMatch()) return QColor(value);
        static const QRegularExpression rgb(QStringLiteral("^rgb\\(\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*\\)$"),
                                             QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = rgb.match(value);
        if (!match.hasMatch()) return {};
        const int r = match.captured(1).toInt(), g = match.captured(2).toInt(), b = match.captured(3).toInt();
        return r <= 255 && g <= 255 && b <= 255 ? QColor(r, g, b) : QColor();
    }
    void showColorPalette(ATool family, QToolButton *anchor)
    {
        if (m_colorDialog) m_colorDialog->close();
        auto *dialog = new QDialog(this, Qt::Popup | Qt::FramelessWindowHint);
        m_colorDialog = dialog;
        m_colorDialogTool = family;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("调色板"));
        dialog->setStyleSheet(QStringLiteral(
            "QDialog{background:rgb(29,31,35);border:1px solid #626468;}"
            "QLineEdit{color:#fff;background:rgb(29,31,35);border:1px solid #626468;"
            "padding:3px;font-size:12px;selection-background-color:#007aff;}"));
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);
        auto *top = new QHBoxLayout;
        top->setSpacing(8);
        m_colorWheel = new ColorWheel(toolColor(family), dialog);
        top->addWidget(m_colorWheel);
        m_colorBrightness = new QSlider(Qt::Vertical, dialog);
        m_colorBrightness->setRange(0, 255);
        m_colorBrightness->setFixedSize(18, 200);
        m_colorBrightness->setAccessibleName(QStringLiteral("明暗"));
        m_colorBrightness->setToolTip(QStringLiteral("拖动调整明暗"));
        top->addWidget(m_colorBrightness);
        layout->addLayout(top);
        auto *bottom = new QHBoxLayout;
        bottom->setSpacing(8);
        m_colorPreview = new QLabel(dialog);
        m_colorPreview->setFixedSize(32, 54);
        m_colorPreview->setToolTip(QStringLiteral("当前颜色"));
        bottom->addWidget(m_colorPreview);
        auto *fields = new QVBoxLayout;
        fields->setSpacing(4);
        m_rgbInput = new QLineEdit(dialog);
        m_hexInput = new QLineEdit(dialog);
        m_rgbInput->setPlaceholderText(QStringLiteral("rgb(255,0,0)"));
        m_hexInput->setPlaceholderText(QStringLiteral("#ff0000"));
        m_rgbInput->setToolTip(QStringLiteral("RGB 分量为 0–255，例如 rgb(255,0,0)"));
        m_hexInput->setToolTip(QStringLiteral("六位 HEX 颜色，例如 #ff0000"));
        m_rgbInput->setAccessibleName(QStringLiteral("RGB 颜色"));
        m_hexInput->setAccessibleName(QStringLiteral("HEX 颜色"));
        for (QLineEdit *field : {m_rgbInput.data(), m_hexInput.data()}) {
            field->installEventFilter(this);
            fields->addWidget(field);
            connect(field, &QLineEdit::textEdited, this, [this, family, field](const QString &text) {
                const QColor color = parseAnnotationColor(text);
                field->setStyleSheet(color.isValid() ? QString() : QStringLiteral("border:1px solid #ff3b30;"));
                if (color.isValid()) pickToolColor(family, color, field);
            });
        }
        bottom->addLayout(fields);
        layout->addLayout(bottom);
        connect(m_colorBrightness, &QSlider::valueChanged, m_colorWheel, &ColorWheel::setBrightness);
        connect(m_colorWheel, &ColorWheel::colorChanged, this,
                [this, family](const QColor &color) { pickToolColor(family, color); });
        connect(dialog, &QDialog::finished, this, [this] {
            if (!isVisible()) return;
            if (editorActive()) { m_textView->setFocus(); m_editor->setFocus(); }
            else setFocus();
        });
        pickToolColor(family, toolColor(family));
        dialog->show();
        dialog->adjustSize();
        const QPoint below = anchor->mapToGlobal(QPoint(0, anchor->height()));
        const QScreen *screen = QGuiApplication::screenAt(below);
        const QRect available = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
        const int y = below.y() + dialog->height() <= available.bottom() + 1 ? below.y()
            : qMax(available.top(), anchor->mapToGlobal(QPoint()).y() - dialog->height());
        dialog->move(qBound(available.left(), below.x(), available.right() + 1 - dialog->width()), y);
    }
    void pickArrowStyle(int style)
    {
        m_arrowStyle = style;
        if (m_selAnn >= 0 && m_selAnn < m_anns.size()
            && m_anns[m_selAnn].tool == ATool::Arrow) {
            pushUndo();
            m_anns[m_selAnn].arrowStyle = style;
            update();
        }
    }
    void setTextBold(bool on)
    {
        m_abold = on;
        if (editorActive()) { m_editBold = on; applyEditorFont(); }
        else if (m_selAnn >= 0 && m_selAnn < m_anns.size()
                 && m_anns[m_selAnn].tool == ATool::Text) {
            pushUndo();
            m_anns[m_selAnn].bold = on;
            fitTextAnnotation(m_anns[m_selAnn]);
            update();
        }
    }
    void setTextItalic(bool on)
    {
        m_aitalic = on;
        if (editorActive()) { m_editItalic = on; applyEditorFont(); }
        else if (m_selAnn >= 0 && m_selAnn < m_anns.size()
                 && m_anns[m_selAnn].tool == ATool::Text) {
            pushUndo();
            m_anns[m_selAnn].italic = on;
            fitTextAnnotation(m_anns[m_selAnn]);
            update();
        }
    }
    void setTextFamily(const QFont &font)
    {
        m_afamily = font.family();
        if (editorActive()) {
            m_editFamily = m_afamily;
            applyEditorFont();
        } else if (m_selAnn >= 0 && m_selAnn < m_anns.size() && m_anns[m_selAnn].tool == ATool::Text
                   && m_anns[m_selAnn].family != m_afamily) {
            pushUndo();
            m_anns[m_selAnn].family = m_afamily;
            fitTextAnnotation(m_anns[m_selAnn]);
            update();
        }
    }
    // Options strip under the toolbar: contents follow the armed tool.
    void updateToolOptions()
    {
        if (m_colorDialog) m_colorDialog->close();
        const QRect old = m_toolOptions->geometry();
        const ATool tool = optionsTool();
        if (tool == ATool::None) {
            m_toolOptions->hide();
            update(old);
            return;
        }
        m_sizeInput = nullptr;
        m_colorButton = nullptr;
        delete m_toolOptions->layout();              // Drops the previous row.
        const QList<QWidget *> stale = m_toolOptions->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
        for (QWidget *w : stale) delete w;
        auto *lay = new QHBoxLayout(m_toolOptions);
        lay->setContentsMargins(4, 2, 4, 2);
        lay->setSpacing(2);
        lay->setAlignment(Qt::AlignLeft);
        auto addOption = [this](QHBoxLayout *l, Icon icon, const QString &tip, bool on,
                                const std::function<void()> &apply) {
            auto *b = iconButton(icon, tip);
            b->setFocusPolicy(Qt::NoFocus);
            b->setCheckable(true);
            b->setChecked(on);
            connect(b, &QToolButton::clicked, this, [this, apply, on] { apply(); Q_UNUSED(on); });
            l->addWidget(b);
        };
        auto addSeparator = [](QHBoxLayout *l) {
            auto *line = new QFrame;
            line->setFrameShape(QFrame::VLine);
            line->setFixedHeight(20);
            l->addWidget(line);
        };
        auto addSwatches = [this](QHBoxLayout *l, ATool family) {
            static const QColor swatches[] = {"#ff3b30", "#ff9500", "#ffcc00", "#34c759",
                                              "#007aff", "#af52de", "#ffffff", "#1c1c1e"};
            for (const QColor &color : swatches) {
                auto *swatch = new QToolButton;
                swatch->setObjectName(QStringLiteral("presetColor"));
                swatch->setFixedSize(18, 18);
                swatch->setFocusPolicy(Qt::NoFocus);
                swatch->setCursor(Qt::PointingHandCursor);
                swatch->setToolTip(color.name());
                swatch->setStyleSheet(QString("QToolButton{background:%1;border:1px solid #929498;}")
                                          .arg(color.name()));
                connect(swatch, &QToolButton::clicked, this,
                        [this, family, color] { pickToolColor(family, color); });
                l->addWidget(swatch);
            }
            m_colorButton = iconButton(Icon::Palette, QStringLiteral("调色板：点击或拖动选色，或输入 RGB / HEX"));
            m_colorButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
            m_colorButton->setFocusPolicy(Qt::NoFocus);
            m_colorButton->setCursor(Qt::PointingHandCursor);
            QToolButton *button = m_colorButton;
            connect(button, &QToolButton::clicked, this, [this, family, button] { showColorPalette(family, button); });
            l->addWidget(button);
        };
        const bool texty = tool == ATool::Text || tool == ATool::Number;
        auto addSize = [this, lay, texty] {
            m_sizeInput = new QComboBox;
            m_sizeInput->setEditable(true);
            m_sizeInput->setInsertPolicy(QComboBox::NoInsert);
            m_sizeInput->setCompleter(nullptr);
            auto *validator = new QDoubleValidator(1, texty ? 1638 : 12, texty ? 1 : 0, m_sizeInput);
            validator->setNotation(QDoubleValidator::StandardNotation);
            validator->setLocale(QLocale::c());
            m_sizeInput->setValidator(validator);
            if (texty) {
                for (qreal size : {5.5, 6.5, 7.5, 8., 9., 10., 10.5, 11., 12., 14., 15., 16.,
                                   18., 20., 22., 24., 26., 28., 36., 42., 48., 54., 72.})
                    m_sizeInput->addItem(QString::number(size));
            } else {
                for (int size = 1; size <= 12; ++size) m_sizeInput->addItem(QString::number(size));
            }
            m_sizeInput->setToolTip(texty ? QStringLiteral("标准字号 (pt)：小四=12，五号=10.5")
                                        : QStringLiteral("输入、下拉选择或滚轮调节粗细"));
            m_sizeInput->setFocusPolicy(Qt::ClickFocus);
            m_sizeInput->setFixedWidth(texty ? 62 : 40);
            m_sizeInput->setStyleSheet(QStringLiteral("QComboBox{padding:1px 2px;}QComboBox::drop-down{width:12px;}"));
            m_sizeInput->lineEdit()->setAlignment(Qt::AlignCenter);
            m_sizeInput->setAccessibleName(texty ? QStringLiteral("字号") : QStringLiteral("粗细"));
            m_sizeInput->installEventFilter(this);
            m_sizeInput->lineEdit()->installEventFilter(this);
            syncSizeInput();
            connect(m_sizeInput->lineEdit(), &QLineEdit::editingFinished, this, &CaptureOverlay::commitSizeInput);
            connect(m_sizeInput, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
                commitSizeInput();
                if (editorActive()) { m_textView->setFocus(); m_editor->setFocus(); }
                else setFocus();
            });
            lay->addWidget(m_sizeInput);
        };
        switch (tool) {
        case ATool::Rect:
        case ATool::Ellipse:
            addOption(lay, Icon::Rect, QStringLiteral("矩形"), !m_shapeEllipse,
                      [this] { selectTool(ATool::Rect); });
            addOption(lay, Icon::Ellipse, QStringLiteral("椭圆"), m_shapeEllipse,
                      [this] { selectTool(ATool::Ellipse); });
            addSize();
            addSeparator(lay);
            addSwatches(lay, ATool::Rect);
            break;
        case ATool::Arrow: {
            struct { Icon icon; const char *tip; } const styles[] = {
                {Icon::Arrow, "单向"}, {Icon::ArrowBoth, "双向"},
                {Icon::ArrowLine, "无箭头"}, {Icon::ArrowHollow, "镂空"},
            };
            for (int k = 0; k < 4; ++k)
                addOption(lay, styles[k].icon, QString::fromUtf8(styles[k].tip),
                          m_arrowStyle == k, [this, k] { pickArrowStyle(k); });
            addSize();
            addSeparator(lay);
            addSwatches(lay, ATool::Arrow);
            break;
        }
        case ATool::Text: {
            auto *bold = iconButton(Icon::Bold, QStringLiteral("粗体"));
            auto *italic = iconButton(Icon::Italic, QStringLiteral("斜体"));
            for (auto *b : {bold, italic}) {
                b->setFocusPolicy(Qt::NoFocus);
                b->setCheckable(true);
                b->setCursor(Qt::PointingHandCursor);
            }
            bold->setChecked(editorActive() ? m_editBold : m_abold);
            italic->setChecked(editorActive() ? m_editItalic : m_aitalic);
            bold->setToolTip(QStringLiteral("粗体"));
            italic->setToolTip(QStringLiteral("斜体"));
            connect(bold, &QToolButton::toggled, this, &CaptureOverlay::setTextBold);
            connect(italic, &QToolButton::toggled, this, &CaptureOverlay::setTextItalic);
            lay->addWidget(bold);
            lay->addWidget(italic);
            auto *font = new FontComboBox;
            font->setObjectName(QStringLiteral("annotationFont"));
            font->setEditable(false);
            font->addItems(QFontDatabase().families());
            font->setFocusPolicy(Qt::NoFocus);
            font->setFixedWidth(40);
            font->view()->setMinimumWidth(200);
            font->setAccessibleName(QStringLiteral("字体"));
            font->setCurrentText(editorActive() ? m_editFamily
                : m_selAnn >= 0 && m_selAnn < m_anns.size() ? m_anns[m_selAnn].family : m_afamily);
            font->setToolTip(QStringLiteral("字体：%1").arg(font->currentText()));
            connect(font, &QComboBox::currentTextChanged, this, [this, font](const QString &family) {
                font->setToolTip(QStringLiteral("字体：%1").arg(family));
                setTextFamily(QFont(family));
            });
            lay->addWidget(font);
            addSize();
            addSeparator(lay);
            addSwatches(lay, ATool::Text);
            break;
        }
        case ATool::Mosaic:
        case ATool::Eraser: {
            addSize();
            m_sizeInput->setToolTip(tool == ATool::Mosaic ? QStringLiteral("输入、下拉选择或滚轮调节块大小")
                                                       : QStringLiteral("输入、下拉选择或滚轮调节粗细"));
            break;
        }
        default:
            addSize();
            addSeparator(lay);
            addSwatches(lay, tool);                 // Pen, Marker, Number.
            break;
        }
        for (int i = 0; i < lay->count(); ++i) lay->itemAt(i)->widget()->show();
        m_toolOptions->show();
        placeToolbar();
        m_toolOptions->raise();
        update(old.united(m_toolOptions->geometry()).adjusted(-1, -1, 1, 1));
    }
    // Shared by the overlay (logical coords, origin 0, scale 1) and cropped() (native crop
    // coords): annotations are stored overlay-local and mapped through this transform.
    void paintAnnotations(QPainter &p, const QPointF &origin, qreal scale, const QRectF &clip,
                          bool includeEditor = true) const
    {
        if (m_anns.isEmpty() && !m_live && !editorActive()) return;
        p.save();
        p.translate(origin);
        p.scale(scale, scale);
        p.setClipRect(clip, Qt::IntersectClip);
        for (int i = 0; i < m_anns.size(); ++i)
            if (!editorActive() || i != m_editAnn) drawAnnotation(p, m_anns[i]);
        if (includeEditor && editorActive()) drawAnnotation(p, editorAnnotation());
        if (m_live) drawAnnotation(p, m_ann);
        p.restore();
    }
    void drawAnnotation(QPainter &p, const Annotation &ann) const
    {
        QPen pen(ann.color, ann.pen);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setBrush(Qt::NoBrush);
        p.save();
        if (!qFuzzyIsNull(ann.rot)) {                // Rotate around the item's box center.
            const QTransform t = annTransform(ann);
            p.setTransform(t, true);
        }
        switch (ann.tool) {
        case ATool::Rect:
        case ATool::Ellipse:
            if (ann.fill) {                          // Fill mode: translucent wash plus outline.
                QColor wash = ann.color;
                wash.setAlpha(70);
                p.setBrush(wash);
            }
            p.setPen(pen);
            if (ann.tool == ATool::Rect) p.drawRect(ann.rect);
            else p.drawEllipse(ann.rect);
            break;
        case ATool::Pen:
            p.setPen(pen);
            p.drawPolyline(ann.stroke);
            break;
        case ATool::Marker: {                        // Translucent fat stroke, marker style.
            QColor wash = ann.color;
            wash.setAlpha(110);
            QPen washPen(wash, ann.pen * 5);
            washPen.setCapStyle(Qt::RoundCap);
            washPen.setJoinStyle(Qt::RoundJoin);
            p.setPen(washPen);
            p.drawPolyline(ann.stroke);
            break;
        }
        case ATool::Text: {
            if (ann.text.isEmpty()) break;
            QTextDocument doc;
            setupTextDocument(doc, ann);
            QAbstractTextDocumentLayout::PaintContext context;
            context.palette.setColor(QPalette::Text, ann.color);
            p.translate(ann.rect.topLeft());
            doc.documentLayout()->draw(&p, context);
            break;
        }
        case ATool::Number: {                        // Filled circle with a sequence digit.
            if (ann.text.isEmpty()) break;
            QFont font = textFont(ann);
            font.setBold(true);
            p.setPen(Qt::NoPen);
            p.setBrush(ann.color);
            p.drawEllipse(ann.rect);
            p.setPen(ann.color.lightness() > 165 ? Qt::black : Qt::white);
            p.setFont(font);
            p.drawText(ann.rect, Qt::AlignCenter, ann.text);
            break;
        }
        case ATool::Arrow: {
            p.setPen(pen);
            const QLineF shaft(ann.a, ann.b);
            if (shaft.length() < 1) break;
            if (ann.arrowStyle == 2) {               // Plain line, no heads.
                p.drawLine(shaft);
                break;
            }
            const qreal head = qMax(qreal(ann.pen * 2 + 8), qreal(11));
            const QPointF dir = (shaft.p2() - shaft.p1()) / shaft.length();
            const QPointF norm(-dir.y(), dir.x());
            QLineF line = shaft;
            line.setLength(qMax(qreal(1), shaft.length() - head * 0.55));
            if (ann.arrowStyle == 1) {               // Double: pull the shaft back twice.
                QLineF mid(line.p1() + dir * head * 0.55, line.p2() - dir * head * 0.55);
                if (mid.length() < 1) break;
                p.drawLine(mid);
            } else {
                p.drawLine(line);
            }
            if (ann.arrowStyle != 3) p.setBrush(ann.color);   // Hollow keeps the outline only.
            QPolygonF tip;
            tip << shaft.p2() << shaft.p2() - dir * head + norm * head * 0.45
                << shaft.p2() - dir * head - norm * head * 0.45;
            p.drawPolygon(tip);
            if (ann.arrowStyle == 1) {               // Second head at the start.
                QPolygonF tail;
                tail << shaft.p1() << shaft.p1() + dir * head + norm * head * 0.45
                     << shaft.p1() + dir * head - norm * head * 0.45;
                p.drawPolygon(tail);
            }
            break;
        }
        case ATool::Mosaic: {                        // Block-average the frozen frame in place.
            const qreal dpr = m_shot.devicePixelRatioF();
            const QRect src(qRound(ann.rect.x() * dpr), qRound(ann.rect.y() * dpr),
                            qMax(1, qRound(ann.rect.width() * dpr)),
                            qMax(1, qRound(ann.rect.height() * dpr)));
            const int block = qMax(4, qRound(ann.pen * 2 * dpr));  // Granularity tracks pen width.
            QPixmap tiles = m_shot.copy(src)
                .scaled(QSize(qMax(1, src.width() / block), qMax(1, src.height() / block)),
                        Qt::IgnoreAspectRatio, Qt::FastTransformation)
                .scaled(src.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
            tiles.setDevicePixelRatio(1);
            p.drawPixmap(QRect(ann.rect.topLeft(), ann.rect.size()), tiles);
            break;
        }
        default:
            break;
        }
        p.restore();
    }

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
                    showToolBars();
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
        if (m_mode == Adjusting) {                    // A locked selection edits, not samples.
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
        if (m_colorDialog) m_colorDialog->close();
        m_mode = Selecting;                         // m_hover is kept: the next mouse move refreshes it.
        m_sel = QRect();
        m_drag = H_None;
        m_tools->hide();
        m_toolOptions->hide();
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
    void buildToolLayout()
    {
        if (m_tools->layout()) return;
        auto *lay = new QHBoxLayout(m_tools);
        lay->setContentsMargins(4, 4, 4, 4);
        lay->setSpacing(2);
        for (QWidget *button : m_toolButtons) lay->addWidget(button);
        m_tools->adjustSize();
    }
    void placeToolbar()
    {
        const QRect old = m_tools->geometry().united(m_toolOptions->geometry());
        const QScreen *screen = QGuiApplication::screenAt(m_sel.center() + geometry().topLeft());
        const QRect avail = (screen ? screen : QGuiApplication::primaryScreen())
                              ->availableGeometry().translated(-geometry().topLeft());
        const QSize main = m_tools->sizeHint();
        const QSize options = optionsTool() == ATool::None ? QSize(0, 0) : m_toolOptions->sizeHint();
        const int width = qMax(main.width(), options.width());
        const int height = main.height() + options.height();
        const int selRight = m_sel.right() + 1, selBottom = m_sel.bottom() + 1;
        const int x = qBound(avail.left(), selRight - width, avail.right() + 1 - width);
        const int y = selBottom + 6 + height <= avail.bottom() + 1 ? selBottom + 6
            : m_sel.top() - 6 - height >= avail.top() ? m_sel.top() - 6 - height
            : qMax(avail.top(), avail.bottom() + 1 - height - 8);
        m_tools->setGeometry(x, y, width, main.height());
        positionToolOptions();
        const QRect glass = m_toolOptions->isVisible()
            ? m_tools->geometry().united(m_toolOptions->geometry()) : m_tools->geometry();
        update(glass.united(old).adjusted(-1, -1, 1, 1));
    }
    void positionToolOptions()                      // Option strip hugs the toolbar's bottom edge.
    {
        if (!m_toolOptions->isVisible() && optionsTool() == ATool::None) return;
        const QRect bar = m_tools->geometry();
        m_toolOptions->setGeometry(bar.left(), bar.bottom() + 1, bar.width(), m_toolOptions->sizeHint().height());
    }
    void hideToolBars()
    {
        if (optionsTool() != ATool::None) return;     // Active tool options remain available throughout a drag.
        if (m_toolOptions->isVisible()) {
            update(m_toolOptions->geometry());
            m_toolOptions->hide();
        }
        if (m_tools->isVisible()) {
            update(m_tools->geometry());
            m_tools->hide();
        }
    }
    void showToolBars()
    {
        m_tools->show();
        if (optionsTool() != ATool::None) {
            positionToolOptions();
            m_toolOptions->show();
        }
    }
    QPixmap cropped() const
    {
        const qreal scale = m_shot.devicePixelRatio();
        const QRect r = m_sel.normalized();
        QPixmap pix = m_shot.copy(QRect(qRound(r.x() * scale), qRound(r.y() * scale),
                                       qRound(r.width() * scale), qRound(r.height() * scale)));
        pix.setDevicePixelRatio(1);
        if (!m_anns.isEmpty() || editorActive()) {   // Unfinished text is also included in previews.
            QPainter p(&pix);
            paintAnnotations(p, -QPointF(r.topLeft()) * scale, scale, QRectF(r));
        }
        return pix;
    }
private slots:
    void undoAnnotation()
    {
        commitTextEdit();
        if (m_undoStack.isEmpty()) return;
        m_redoStack.append(m_anns);
        m_anns = m_undoStack.takeLast();
        m_selAnn = -1;
        normalizeNumbers();
        syncUndoButtons();
        updateToolOptions();
        update();
    }
    void redoAnnotation()
    {
        commitTextEdit();
        if (m_redoStack.isEmpty()) return;
        m_undoStack.append(m_anns);
        m_anns = m_redoStack.takeLast();
        m_selAnn = -1;
        normalizeNumbers();
        syncUndoButtons();
        updateToolOptions();
        update();
    }
    void saveCapture()
    {
        commitTextEdit();
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
        commitTextEdit();
        const QPixmap pix = cropped();
        QClipboard *clip = QGuiApplication::clipboard();
        clip->setPixmap(pix);
        // IME/clipboard helpers may hold the clipboard open for a moment after an IME
        // context switch; retry briefly instead of silently losing the copy.
        QElapsedTimer retry;
        retry.start();
        while (retry.elapsed() < 300 && clip->image() != pix.toImage()) {
            QEventLoop loop;
            QTimer::singleShot(15, &loop, &QEventLoop::quit);
            loop.exec();
            clip->setPixmap(pix);
        }
        close();
    }
    void pinCapture()
    {
        commitTextEdit();
        const QPixmap pix = cropped();
        const QRect global = m_sel.normalized().translated(geometry().topLeft());
        close();
        emit grabbed(pix, global);
    }
    void longCapture()
    {
        commitTextEdit();
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
    ATool m_atool = ATool::None;
    QColor m_acolor = QColor("#ff3b30");             // Red first: the loudest marking color.
    int m_apen = 3;
    qreal m_afont = 15;                              // Base text/number font size (pt).
    QString m_afamily = QStringLiteral("Microsoft YaHei UI");
    bool m_afill = false;                            // Default fill mode for rect / ellipse.
    bool m_spaceHeld = false;                        // Space turns the next drag into a move.
    bool m_nudging = false;                          // Coalesces WASD bursts into one undo step.
    QVector<Annotation> m_anns;
    Annotation m_ann;                                // Annotation under construction.
    bool m_live = false;
    int m_selAnn = -1;                               // Annotation selected for editing.
    Annotation m_pressAnn;                           // Snapshot for move/resize drags.
    Handle m_itemDrag = H_None;
    bool m_erasing = false;
    bool m_eraseSnapshotted = false;
    QVector<QVector<Annotation>> m_undoStack, m_redoStack;
    QToolButton *m_undoButton = nullptr;             // Enabled state follows the stacks.
    QToolButton *m_redoButton = nullptr;
    QGraphicsTextItem *m_editor = nullptr;           // Native editable text with content-sized bounds.
    QGraphicsView *m_textView = nullptr;             // Masked to the text item; canvas stays clickable.
    bool m_textPreedit = false, m_textMoving = false;
    QPoint m_textMovePress;
    QPointF m_textMoveOrigin;
    int m_editAnn = -1;                              // Annotation being re-edited.
    QColor m_editColor = QColor("#ff3b30");
    qreal m_editFont = 15;
    QString m_editFamily;
    bool m_committingText = false;
    HIMC m_imeCtx = nullptr;                         // Detached IME context for text editing.
    QToolButton *m_shapeButton = nullptr;            // Merged rect / ellipse tool button.
    QVector<QToolButton *> m_toolBtn;                // Tool button per ATool id (shape shared).
    QWidget *m_toolOptions = nullptr;                // Per-tool option strip under the toolbar.
    QComboBox *m_sizeInput = nullptr;                 // Editable width / point-size presets.
    QToolButton *m_colorButton = nullptr;
    QPointer<QDialog> m_colorDialog;
    QPointer<ColorWheel> m_colorWheel;
    QPointer<QSlider> m_colorBrightness;
    QPointer<QLabel> m_colorPreview;
    QPointer<QLineEdit> m_rgbInput, m_hexInput;
    ATool m_colorDialogTool = ATool::None;
    bool m_shapeEllipse = false;                     // Current sub-shape of the shape button.
    int m_arrowStyle = 0;                            // 0 single, 1 double, 2 none, 3 hollow.
    bool m_abold = false, m_aitalic = false;         // Text tool defaults.
    bool m_editBold = false, m_editItalic = false;   // Live editor weight / slant.
    QColor m_colorShape = "#ff3b30", m_colorArrow = "#ff3b30", m_colorPen = "#ff3b30",
           m_colorMarker = "#ffcc00", m_colorText = "#ff3b30", m_colorNumber = "#ff3b30";
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
        connect(ov, &CaptureOverlay::grabbed, this, [](const QPixmap &pix, const QRect &region) {
            new PinWindow(pix, region);
        });
        connect(ov, &CaptureOverlay::longRequested, this, [](const QRect &region) {
            auto *lc = new LongCapture(region);
            connect(lc, &LongCapture::finished, lc, [region](const QPixmap &pix) {
                const QScreen *scr = QGuiApplication::screenAt(region.topLeft());
                if (!scr) scr = QGuiApplication::primaryScreen();
                const QRect av = scr->availableGeometry().contains(region.topLeft())
                    ? scr->availableGeometry() : scr->geometry();
                new PinWindow(pix, PinWindow::fittedGeometry(pix.size(), region, av));
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
    if (app.arguments().contains(QStringLiteral("--test-pin-placement")))
        return PinWindow::testPlacement();
    if (app.arguments().contains(QStringLiteral("--test-color-picker")))
        return CaptureOverlay::testColorPicker(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-color-picker")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-window-detection")))
        return CaptureOverlay::testWindowDetection();
    if (app.arguments().contains(QStringLiteral("--test-text-annotation")))
        return CaptureOverlay::testTextAnnotation();
    if (app.arguments().contains(QStringLiteral("--test-color-wheel")))
        return ColorWheel::testRendering();
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
