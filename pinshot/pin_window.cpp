#include "pin_window.h"
#include "ocr_dialog.h"
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QColor>
#include <QAction>
#include <QWidgetAction>
#include <QPaintEvent>
#include <QContextMenuEvent>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QSlider>
#include <QLabel>
#include <QHBoxLayout>
#include <QWidgetAction>
#include <QTransform>
#include <QtMath>
#include <QFileDialog>
#include <QStandardPaths>
#include <QCursor>
#include <QMessageBox>
#include <QCoreApplication>
#include <QApplication>
#include <QScreen>
#include <QSizePolicy>
#include <QLayout>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <qt_windows.h>

PinWindow::PinWindow(const QPixmap &pix, const QRect &displayRect)
    : m_originalPix(pix), m_pix(pix), m_scale(pix.isNull() ? 1 : qMin(double(displayRect.width()) / pix.width(), double(displayRect.height()) / pix.height()))
{
    if (pix.isNull() || displayRect.isEmpty()) return;
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_DeleteOnClose);
    setCursor(Qt::SizeAllCursor);
    setAccessibleName(QStringLiteral("贴图"));
    setMouseTracking(true);
    setGeometry(displayRect);
    setShadowEnabled(m_shadowOn);
    show();
}

void PinWindow::setShadowEnabled(bool enabled)
{
    m_shadowOn = enabled;
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd) {
        const ULONG_PTR style = GetClassLongPtrW(hwnd, GCL_STYLE);
        if (enabled) SetClassLongPtrW(hwnd, GCL_STYLE, style | CS_DROPSHADOW);
        else SetClassLongPtrW(hwnd, GCL_STYLE, style & ~CS_DROPSHADOW);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void PinWindow::applyProcessedImage()
{
    if (m_originalPix.isNull()) return;
    QImage img = m_originalPix.toImage().convertToFormat(QImage::Format_ARGB32);

    // 1. Invert colors if requested
    if (m_inverted) img.invertPixels(QImage::InvertRgb);

    // 2. Brightness adjustment (0%..200%, default 100%)
    if (m_brightnessPercent != 100) {
        const double factor = m_brightnessPercent / 100.0;
        uchar *bits = img.bits();
        const qsizetype totalBytes = img.sizeInBytes();
        for (qsizetype i = 0; i < totalBytes; i += 4) {
            bits[i]   = static_cast<uchar>(qBound(0, qRound(bits[i] * factor), 255));     // B
            bits[i+1] = static_cast<uchar>(qBound(0, qRound(bits[i+1] * factor), 255));   // G
            bits[i+2] = static_cast<uchar>(qBound(0, qRound(bits[i+2] * factor), 255));   // R
            // bits[i+3] is Alpha, preserved
        }
    }

    // 3. Rotation and flipping
    QTransform transform;
    if (m_flipH) transform.scale(-1, 1);
    if (m_flipV) transform.scale(1, -1);
    if (m_rotation != 0) transform.rotate(m_rotation);

    if (!transform.isIdentity()) {
        img = img.transformed(transform, Qt::SmoothTransformation);
    }

    m_pix = QPixmap::fromImage(img);

    // Invalidate OCR cache on image transformation
    m_charBoxes.clear();
    m_ocrLines.clear();
    m_selAnchor = -1;
    m_selLead = -1;
    if (m_textSelectable) {
        runOcrForTextSelection();
    }

    // Recalculate window size to preserve logical scale and aspect ratio
    const QPoint c = geometry().center();
    const QSize newSize(qMax(1, qRound(m_pix.width() * m_scale)),
                        qMax(1, qRound(m_pix.height() * m_scale)));
    QRect r(0, 0, newSize.width(), newSize.height());
    r.moveCenter(c);
    setGeometry(r);
    update();
}

void PinWindow::rotateBy(int degrees)
{
    m_rotation = (m_rotation + degrees) % 360;
    if (m_rotation < 0) m_rotation += 360;
    applyProcessedImage();
}

void PinWindow::flipHorizontal()
{
    m_flipH = !m_flipH;
    applyProcessedImage();
}

void PinWindow::flipVertical()
{
    m_flipV = !m_flipV;
    applyProcessedImage();
}

void PinWindow::invertColors()
{
    m_inverted = !m_inverted;
    applyProcessedImage();
}

void PinWindow::setOpacityPercent(int percent)
{
    m_opacityPercent = qBound(10, percent, 100);
    setWindowOpacity(m_opacityPercent / 100.0);
}

void PinWindow::setBrightnessPercent(int percent)
{
    m_brightnessPercent = qBound(0, percent, 200);
    applyProcessedImage();
}

void PinWindow::resetProcessing()
{
    m_rotation = 0;
    m_flipH = false;
    m_flipV = false;
    m_inverted = false;
    m_brightnessPercent = 100;
    setOpacityPercent(100);
    applyProcessedImage();
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

    // Draw text selection highlight if text is selectable and selected
    if (m_textSelectable && m_selAnchor >= 0 && m_selLead >= 0
            && !m_charBoxes.isEmpty() && !m_ocrImgSize.isEmpty()) {
        const int start = qMin(m_selAnchor, m_selLead);
        const int end = qMax(m_selAnchor, m_selLead);
        const double scaleX = double(width()) / m_ocrImgSize.width();
        const double scaleY = double(height()) / m_ocrImgSize.height();

        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(QColor(32, 128, 240, 80)); // Semi-transparent theme blue highlight
        p.setPen(Qt::NoPen);

        for (int i = start; i <= end && i < m_charBoxes.size(); ++i) {
            const QRectF cr = m_charBoxes.at(i).rect;
            const QRectF vr(cr.x() * scaleX, cr.y() * scaleY, cr.width() * scaleX, cr.height() * scaleY);
            p.drawRect(vr);
        }
        p.restore();
    }

    if (m_borderOn) {                             // Highlight border, toggled through the context menu.
        p.setPen(QPen(QColor("#2080f0"), 2));
        p.drawRect(rect().adjusted(1, 1, -1, -1));
    } else {
        p.setPen(QPen(QColor(0, 0, 0, 90), 1));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

// Flyout row for opacity/brightness: slider and percentage only. The row lives
// inside the flyout that opens when hovering the parent item.
// Place a flyout fully outside its parent. Windows menus overlap the parent by
// a few pixels by default, and a same-width flyout then covers the parent row.
QPoint flyoutOutsideParent(const QMenu *subMenu, const QMenu *parent, const QRect &item)
{
    const int w = qMax(subMenu->width(), subMenu->sizeHint().width());
    const int h = subMenu->actions().size() == 1 ? item.height() : subMenu->sizeHint().height();
    const QRect screen = parent->screen()
        ? parent->screen()->availableGeometry()
        : QRect(0, 0, 1920, 1080);
    const bool toLeft = parent->geometry().right() + w > screen.right()
        && parent->geometry().left() - w >= screen.left();
    const int x = toLeft ? parent->geometry().left() - w - 1 : parent->geometry().right() + 1;
    const QPoint rowTop = parent->mapToGlobal(item.topLeft());
    // A one-row slider flyout is exactly as tall as the parent row and shares its top.
    const int y = subMenu->actions().size() == 1 ? rowTop.y()
        : qBound(screen.top(), rowTop.y(), qMax(screen.top(), screen.bottom() - h));
    return QPoint(x, y);
}

void separateFlyoutEdge(QMenu *menu)
{
    auto *owner = qobject_cast<QMenu *>(menu->parentWidget());
    if (!owner) return;
    const QRect item = owner->actionGeometry(menu->menuAction());
    if (!item.isValid()) return;
    if (menu->actions().size() == 1) {
        // The shared menu padding would push a one-row slider against the top edge.
        menu->setStyleSheet(menu->styleSheet() + QStringLiteral("QMenu{padding:0px;border:none;}"));
        menu->setFixedHeight(item.height());
        if (auto *rowAction = qobject_cast<QWidgetAction *>(menu->actions().first())) {
            if (QWidget *row = rowAction->defaultWidget()) {
                row->setFixedHeight(item.height());
                if (QLayout *lay = row->layout()) lay->activate();
            }
        }
    }
    const QPoint next = flyoutOutsideParent(menu, owner, item);
    if (menu->pos() != next) menu->move(next);
}

class MenuEdgeFix : public QObject {
public:
    explicit MenuEdgeFix(QMenu *menu) : QObject(menu), m_menu(menu)
    {
        menu->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if ((event->type() == QEvent::Show || event->type() == QEvent::Resize) && watched == m_menu)
            separateFlyoutEdge(m_menu);
        return false;
    }

private:
    QMenu *m_menu;
};

class MenuSliderRow : public QWidget {
public:
    MenuSliderRow(int minVal, int maxVal, int initialVal,
                  std::function<void(int)> onValueChanged, QWidget *parent = nullptr)
        : QWidget(parent), m_onValueChanged(std::move(onValueChanged))
    {
        setMouseTracking(true);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 0, 12, 0);
        layout->setSpacing(8);

        m_valLabel = new QLabel(QString::number(initialVal) + QStringLiteral("%"), this);
        m_valLabel->setFixedWidth(40);
        m_valLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_valLabel->setStyleSheet(QStringLiteral("color:#323232;font-size:13px;"));

        m_slider = new QSlider(Qt::Horizontal, this);
        m_slider->setRange(minVal, maxVal);
        m_slider->setValue(initialVal);
        m_slider->setFixedHeight(22);
        m_slider->setStyleSheet(lightSliderQss);

        layout->addWidget(m_slider, 1, Qt::AlignVCenter);
        layout->addWidget(m_valLabel, 0, Qt::AlignVCenter);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

        connect(m_slider, &QSlider::valueChanged, this, [this](int val) {
            m_valLabel->setText(QString::number(val) + QStringLiteral("%"));
            if (m_onValueChanged) m_onValueChanged(val);
        });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *menu = qobject_cast<QMenu *>(watched);
        auto *flyout = qobject_cast<QMenu *>(parentWidget());
        if (!menu || !flyout) return QWidget::eventFilter(watched, event);
        if (event->type() != QEvent::Wheel) return QWidget::eventFilter(watched, event);
        // The highlighted row is the parent entry; once the flyout itself has
        // the pointer, its own row handles the wheel.
        if (menu != flyout && menu->activeAction() != flyout->menuAction())
            return QWidget::eventFilter(watched, event);
        wheelEvent(static_cast<QWheelEvent *>(event));
        return true;
    }
    void wheelEvent(QWheelEvent *e) override
    {
        if (!e->angleDelta().y()) { QWidget::wheelEvent(e); return; }
        // Match QSlider's own wheel handling: scroll lines * single step.
        const int unit = QApplication::wheelScrollLines() * qMax(1, m_slider->singleStep());
        m_slider->setValue(m_slider->value() + (e->angleDelta().y() > 0 ? unit : -unit));
        e->accept();
    }

private:
    QSlider *m_slider = nullptr;
    QLabel *m_valLabel = nullptr;
    std::function<void(int)> m_onValueChanged;
};

static QMenu *createSliderSubMenu(QMenu *parent, const QString &title,
                                  int minVal, int maxVal, int initialVal,
                                  std::function<void(int)> onValueChanged)
{
    auto *subMenu = parent->addMenu(title);
    subMenu->setStyleSheet(parent->styleSheet());
    auto *row = new MenuSliderRow(minVal, maxVal, initialVal, std::move(onValueChanged), subMenu);
    auto *action = new QWidgetAction(subMenu);
    action->setDefaultWidget(row);
    subMenu->addAction(action);
    // The wheel lands on the highlighted menu row, not on the flyout widget.
    parent->installEventFilter(row);
    subMenu->installEventFilter(row);
    new MenuEdgeFix(subMenu);
    return subMenu;
}

void PinWindow::toggleTextSelectable(bool enabled)
{
    m_textSelectable = enabled;
    m_selAnchor = -1;
    m_selLead = -1;
    m_selecting = false;
    if (m_textSelectable) {
        setCursor(Qt::IBeamCursor);
        if (m_charBoxes.isEmpty() && !m_ocrRunning) {
            runOcrForTextSelection();
        }
    } else {
        setCursor(Qt::SizeAllCursor);
    }
    update();
}

void PinWindow::runOcrForTextSelection()
{
    if (m_pix.isNull() || m_ocrRunning) return;
    m_ocrRunning = true;
    m_ocrCancel = std::make_shared<std::atomic_bool>(false);

    const QImage img = m_pix.toImage();
    // Boxes come back in this raw pixel space (recognize resets DPR), not the pixmap's logical size.
    m_ocrImgSize = QSizeF(img.size());
    const auto cancel = m_ocrCancel;

    auto watcher = new QFutureWatcher<OcrResult>(this);
    connect(watcher, &QFutureWatcher<OcrResult>::finished, this, [this, watcher] {
        const OcrResult res = watcher->result();
        watcher->deleteLater();
        m_ocrRunning = false;
        if (m_ocrCancel && m_ocrCancel->load()) return;

        m_ocrLines = res.lines;
        m_charBoxes.clear();

        for (int l = 0; l < m_ocrLines.size(); ++l) {
            const OcrLine &line = m_ocrLines.at(l);
            const QString text = line.text;
            if (text.isEmpty()) continue;

            const QRectF box = line.box.boundingRect();
            const double charWidth = box.width() / qMax(1, text.size());
            for (int c = 0; c < text.size(); ++c) {
                CharBox cb;
                cb.ch = text.at(c);
                cb.rect = QRectF(box.left() + c * charWidth, box.top(), charWidth, box.height());
                cb.lineIdx = l;
                m_charBoxes.append(cb);
            }
        }
        if (m_textSelectable) {
            setCursor(Qt::IBeamCursor);
        }
        update();
    });

    watcher->setFuture(QtConcurrent::run([img, cancel]() -> OcrResult {
        return OcrEngine::instance().recognize(img, cancel);
    }));
}

int PinWindow::charIndexAt(const QPoint &pos) const
{
    if (m_pix.isNull() || m_charBoxes.isEmpty() || m_ocrImgSize.isEmpty()) return -1;
    const double scaleX = m_ocrImgSize.width() / width();
    const double scaleY = m_ocrImgSize.height() / height();
    const QPointF imgPt(pos.x() * scaleX, pos.y() * scaleY);

    // 1. Direct hit with generous view padding (5px in view coordinates)
    const double padX = 5.0 * scaleX;
    const double padY = 5.0 * scaleY;
    for (int i = 0; i < m_charBoxes.size(); ++i) {
        const QRectF &r = m_charBoxes.at(i).rect;
        const QRectF expanded = r.adjusted(-padX, -padY, padX, padY);
        if (expanded.contains(imgPt)) return i;
    }

    // 2. Proximity search: find the closest character within reasonable distance
    int bestIdx = -1;
    double bestDistSq = std::numeric_limits<double>::max();
    for (int i = 0; i < m_charBoxes.size(); ++i) {
        const QRectF &r = m_charBoxes.at(i).rect;
        const QPointF c = r.center();
        // Give higher weight to vertical distance to prefer characters on the same line
        const double dy = (c.y() - imgPt.y()) * 1.5;
        const double dx = (c.x() - imgPt.x());
        const double distSq = dx * dx + dy * dy;

        const double maxDist = qMax(30.0 * scaleY, r.height() * 1.5);
        if (distSq < (maxDist * maxDist) && distSq < bestDistSq) {
            bestDistSq = distSq;
            bestIdx = i;
        }
    }

    return bestIdx;
}

QString PinWindow::selectedText() const
{
    if (m_selAnchor < 0 || m_selLead < 0 || m_charBoxes.isEmpty()) return {};
    const int start = qMin(m_selAnchor, m_selLead);
    const int end = qMax(m_selAnchor, m_selLead);
    QString out;
    int lastLine = -1;
    for (int i = start; i <= end && i < m_charBoxes.size(); ++i) {
        if (lastLine != -1 && m_charBoxes.at(i).lineIdx != lastLine) {
            out += QLatin1Char('\n');
        }
        out += m_charBoxes.at(i).ch;
        lastLine = m_charBoxes.at(i).lineIdx;
    }
    return out;
}

void PinWindow::toggleToolbar(bool enabled)
{
    m_toolbarOn = enabled;
    if (m_toolbarOn) {
        if (!m_toolbar) {
            m_toolbar = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
            m_toolbar->setAttribute(Qt::WA_DeleteOnClose, false);
            m_toolbar->setStyleSheet(QStringLiteral(
                "QWidget { background: #f2f2f2; border: 1px solid #c8c8c8; border-radius: 4px; }"
                "QToolButton { color: #323232; background: transparent; border: none; border-radius: 2px; padding: 4px; }"
                "QToolButton:hover { background-color: #d4d4d4; }"
                "QToolButton:checked { background-color: #2080f0; }"
            ));
            auto *lay = new QHBoxLayout(m_toolbar);
            lay->setContentsMargins(4, 4, 4, 4);
            lay->setSpacing(2);

            auto *copyBtn = iconButton(Icon::Copy, QStringLiteral("复制图像"));
            connect(copyBtn, &QToolButton::clicked, this, [this] {
                if (!copyImageToClipboard(m_pix.toImage()))
                    QMessageBox::warning(this, QStringLiteral("pinshot"), QStringLiteral("复制失败，剪贴板可能被占用，请重试"));
            });
            lay->addWidget(copyBtn);

            auto *saveBtn = iconButton(Icon::Save, QStringLiteral("保存图像"));
            connect(saveBtn, &QToolButton::clicked, this, [this] {
                const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
                const QString path = QFileDialog::getSaveFileName(
                    this, QStringLiteral("保存图片"), dir + QStringLiteral("/pin.png"),
                    QStringLiteral("PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)"));
                if (!path.isEmpty() && !saveImageAtomic(m_pix.toImage(), path))
                    QMessageBox::warning(this, QStringLiteral("pinshot"), QStringLiteral("保存失败，请检查路径或磁盘空间后重试"));
            });
            lay->addWidget(saveBtn);

            auto *ocrBtn = iconButton(Icon::Ocr, QStringLiteral("文字识别 (OCR)"));
            connect(ocrBtn, &QToolButton::clicked, this, [this] {
                showOcr(m_pix.toImage(), this, geometry());
            });
            lay->addWidget(ocrBtn);

            auto *closeBtn = iconButton(Icon::Cancel, QStringLiteral("关闭贴图 (Esc)"));
            connect(closeBtn, &QToolButton::clicked, this, &PinWindow::close);
            lay->addWidget(closeBtn);

            m_toolbar->adjustSize();
        }
        updateToolbarPosition();
        m_toolbar->show();
    } else {
        if (m_toolbar) m_toolbar->hide();
    }
}

void PinWindow::updateToolbarPosition()
{
    if (!m_toolbar || !m_toolbarOn) return;
    const QRect geom = geometry();
    const QSize tSize = m_toolbar->sizeHint();
    // Position toolbar centered below the pinned window
    const int x = geom.left() + (geom.width() - tSize.width()) / 2;
    const int y = geom.bottom() + 6;
    m_toolbar->move(x, y);
}

QMenu *PinWindow::createContextMenu()
{
    auto *m = new QMenu(this);
    m->setStyleSheet(QStringLiteral(
        "QMenu { background: #f2f2f2; color: #323232; border: 1px solid #c8c8c8; padding: 4px; border-radius: 4px; }"
        "QMenu::item { padding: 6px 24px 6px 20px; border-radius: 2px; }"
        "QMenu::item:selected { background: #e0e0e0; color: #323232; }"
        "QMenu::item:disabled { color: #858585; }"
        "QMenu::separator { height: 1px; background: #dcdcdc; margin: 4px 8px; }"
    ));

    // Section 1
    QAction *textSelect = m->addAction(QStringLiteral("文本可选择"));
    textSelect->setCheckable(true);
    textSelect->setChecked(m_textSelectable);
    connect(textSelect, &QAction::toggled, this, [this](bool checked) { toggleTextSelectable(checked); });

    QAction *translate = m->addAction(QStringLiteral("翻译为"));
    translate->setEnabled(false); // Placeholder for translation functionality (not implemented yet).

    m->addSeparator();

    // Section 2: Image processing submenu
    QMenu *imgProc = m->addMenu(QStringLiteral("图像处理"));
    imgProc->setStyleSheet(m->styleSheet());
    new MenuEdgeFix(imgProc);

    QAction *rotLeft = imgProc->addAction(QStringLiteral("向左旋转"));
    connect(rotLeft, &QAction::triggered, this, [this] { rotateBy(-90); });

    QAction *rotRight = imgProc->addAction(QStringLiteral("向右旋转"));
    connect(rotRight, &QAction::triggered, this, [this] { rotateBy(90); });

    QAction *flipH = imgProc->addAction(QStringLiteral("水平翻转"));
    connect(flipH, &QAction::triggered, this, [this] { flipHorizontal(); });

    QAction *flipV = imgProc->addAction(QStringLiteral("垂直翻转"));
    connect(flipV, &QAction::triggered, this, [this] { flipVertical(); });

    QAction *invert = imgProc->addAction(QStringLiteral("反色"));
    connect(invert, &QAction::triggered, this, [this] { invertColors(); });

    imgProc->addSeparator();

    // Slider flyouts: hovering 不透明度/亮度 opens an aligned sub-column with a
    // progress-bar slider (wheel and drag adjust, value shown in percent).
    createSliderSubMenu(imgProc, QStringLiteral("不透明度"), 10, 100, m_opacityPercent,
                        [this](int val) { setOpacityPercent(val); });
    createSliderSubMenu(imgProc, QStringLiteral("亮度"), 0, 200, m_brightnessPercent,
                        [this](int val) { setBrightnessPercent(val); });

    imgProc->addSeparator();

    QAction *resetProc = imgProc->addAction(QStringLiteral("重置所有处理"));
    connect(resetProc, &QAction::triggered, this, [this] { resetProcessing(); });

    QAction *copy = m->addAction(QStringLiteral("复制图像"));
    connect(copy, &QAction::triggered, this, [this] {
        if (!copyImageToClipboard(m_pix.toImage()))
            QMessageBox::warning(this, QStringLiteral("pinshot"), QStringLiteral("复制失败，剪贴板可能被占用，请重试"));
    });

    QAction *save = m->addAction(QStringLiteral("保存图像"));
    connect(save, &QAction::triggered, this, [this] {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存图片"), dir + QStringLiteral("/pin.png"),
            QStringLiteral("PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)"));
        if (!path.isEmpty() && !saveImageAtomic(m_pix.toImage(), path))
            QMessageBox::warning(this, QStringLiteral("pinshot"), QStringLiteral("保存失败，请检查路径或磁盘空间后重试"));
    });

    m->addSeparator();

    // Section 3
    QAction *showTools = m->addAction(QStringLiteral("显示工具条"));
    showTools->setCheckable(true);
    showTools->setChecked(m_toolbarOn);
    connect(showTools, &QAction::toggled, this, [this](bool checked) { toggleToolbar(checked); });

    QAction *shadow = m->addAction(QStringLiteral("窗口阴影"));
    shadow->setCheckable(true);
    shadow->setChecked(m_shadowOn);
    connect(shadow, &QAction::toggled, this, [this](bool checked) { setShadowEnabled(checked); });

    return m;
}

void PinWindow::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu *m = createContextMenu();
    m->setAttribute(Qt::WA_DeleteOnClose);
    m->exec(e->globalPos());
}

void PinWindow::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        if (m_textSelectable) {
            if (m_charBoxes.isEmpty() && !m_ocrRunning && !m_pix.isNull()
                    && OcrEngine::assetError(QCoreApplication::applicationDirPath()).isEmpty()) {
                // A press before/after a failed OCR run retries instead of failing silently.
                runOcrForTextSelection();
            }
            const int idx = charIndexAt(e->pos());
            if (idx >= 0) {
                m_selAnchor = idx;
                m_selLead = idx;
                m_selecting = true;
                update();
                return;
            } else {
                m_selAnchor = -1;
                m_selLead = -1;
                m_selecting = false;
                update();
            }
        }
        m_dragOff = e->globalPosition().toPoint() - pos();
    }
}

void PinWindow::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton) {
        if (m_textSelectable && m_selecting) {
            const int idx = charIndexAt(e->pos());
            if (idx >= 0 && idx != m_selLead) {
                m_selLead = idx;
                update();
            }
            return;
        }
        move(e->globalPosition().toPoint() - m_dragOff);
        updateToolbarPosition();
    } else {
        if (m_textSelectable) {
            // Keep IBeam cursor when selectable to clearly signal text selection mode
            setCursor(Qt::IBeamCursor);
        }
    }
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
    updateToolbarPosition();
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent *)  { close(); }

PinWindow::~PinWindow()
{
    if (m_toolbar) {
        m_toolbar->close();
        delete m_toolbar;
        m_toolbar = nullptr;
    }
}

void PinWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->matches(QKeySequence::Copy)) {
        if (m_textSelectable && m_selAnchor >= 0 && m_selLead >= 0) {
            const QString text = selectedText();
            if (!text.isEmpty()) {
                copyTextToClipboard(text);
                return;
            }
        }
    }
    if (e->key() == Qt::Key_Escape)
        close();
}
