#include "ocr_dialog.h"
#include <QWindow>
#include <QGuiApplication>
#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QColor>
#include <QFont>
#include <QResizeEvent>
#include <QWidget>
#include <QScreen>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QLabel>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QtMath>
#include <QToolButton>
#include <QFontDatabase>
#include <QVBoxLayout>
#include <QDialog>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QCursor>
#include <QMessageBox>
#include <QTimer>
#include <QSplitter>
#include <QPlainTextEdit>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <qt_windows.h>
#include <atomic>
#include <memory>

OcrDialog *showOcr(const QImage &image, QWidget *source, const QRect &captureRect)
{
    if (image.isNull()) return nullptr;
    auto *dialog = new OcrDialog(image,captureRect);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (source) QObject::connect(dialog,&QDialog::accepted,source,[source] {
        if (source->isVisible()) { source->raise(); source->activateWindow(); source->setFocus(); }
    });
    if (source) QObject::connect(source,&QObject::destroyed,dialog,&QObject::deleteLater);
    dialog->show();
    dialog->raise(); dialog->activateWindow();
    return dialog;
}

OcrImageView::OcrImageView(const QImage &image, QWidget *parent) : QGraphicsView(parent) {
    auto *scene = new QGraphicsScene(this);
    setScene(scene);
    QImage native = image;
    native.setDevicePixelRatio(1);
    scene->addPixmap(QPixmap::fromImage(native));
    scene->setSceneRect(QRectF(QPointF(), native.size()));
    setDragMode(QGraphicsView::ScrollHandDrag);
    setRenderHint(QPainter::SmoothPixmapTransform);
    setBackgroundBrush(QColor(0xfd,0xfd,0xfd));
    setAccessibleName(QStringLiteral("截图对照：滚轮缩放，拖动查看"));
}

void OcrImageView::resizeEvent(QResizeEvent *event)  {
    QGraphicsView::resizeEvent(event);
    if (!m_zoomed) fitInView(sceneRect(), Qt::KeepAspectRatio);
}

void OcrImageView::wheelEvent(QWheelEvent *event)  {
    if (!event->angleDelta().y()) return;
    setTransformationAnchor(AnchorUnderMouse);
    const qreal factor = event->angleDelta().y() > 0 ? 1.2 : 1/1.2;
    const qreal next = transform().m11() * factor;
    if (next >= 0.02 && next <= 16) { scale(factor, factor); m_zoomed = true; }
    event->accept();
}

void OcrImageView::mouseDoubleClickEvent(QMouseEvent *event)  {
    m_zoomed = false;
    fitInView(sceneRect(), Qt::KeepAspectRatio);
    event->accept();
}

OcrDialog::OcrDialog(const QImage &image, const QRect &captureRect)
    : QDialog(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint),
      m_image(image) {
    setObjectName(QStringLiteral("ocrComparison"));
    setWindowTitle(QStringLiteral("Finalize OCR"));
    setWindowModality(Qt::NonModal);
    setMinimumSize(640,360);
    setMouseTracking(true);
    QFont interfaceFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    interfaceFont.setPointSizeF(qMax(10.5,interfaceFont.pointSizeF()));
    setFont(interfaceFont);
    const QString surface = QStringLiteral("#f2f2f2"), panel = QStringLiteral("#ffffff"),
                  canvas = QStringLiteral("#fdfdfd"), foreground = QStringLiteral("#323232"),
                  muted = QStringLiteral("#5b5b5b"), outline = QStringLiteral("#858585"),
                  accent = QStringLiteral("#2080f0"), selected = QStringLiteral("#a3d6ff"),
                  error = QStringLiteral("#e81123");
    setStyleSheet(QStringLiteral(
        "QDialog{background:%1;color:%4;border:1px solid %2;}"
        "QLabel{color:%5;background:transparent;} QLabel#ocrTitle{color:%4;}"
        "QWidget#ocrImagePanel,QWidget#ocrTextPanel{background:%2;border:none;border-radius:8px;}"
        "QPlainTextEdit{background:%3;color:%4;border:1px solid transparent;border-radius:6px;padding:16px;selection-background-color:%8;}"
        "QPlainTextEdit:focus{border-color:%6;}"
        "QGraphicsView{background:%3;border:1px solid transparent;} QGraphicsView:focus{border-color:%6;}"
        "QToolButton{background:transparent;color:%4;border:1px solid transparent;border-radius:6px;padding:4px;}"
        "QToolButton:hover{background:%8;} QToolButton:pressed{background:%6;}"
        "QToolButton:focus{border-color:%7;} QToolButton#ocrPin:checked{background:%8;border-color:%7;}"
        "QToolButton#ocrCopy{background:%8;} QToolButton#ocrCopy:hover{border-color:%7;}"
        "QToolButton:disabled{background:transparent;} QToolButton#ocrClose:hover{background:%9;}"
        "QSplitter::handle{background:transparent;} QSplitter::handle:hover{background:%6;}"
        "QScrollBar:vertical{background:%3;width:12px;margin:0;} QScrollBar:horizontal{background:%3;height:12px;margin:0;}"
        "QScrollBar::handle{background:%6;border-radius:4px;min-width:24px;min-height:24px;}"
        "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;}"
        "QScrollBar::add-page,QScrollBar::sub-page{background:transparent;}"
        "QToolTip{background:%2;color:%4;border:1px solid %6;padding:6px;}"
    ).arg(surface,panel,canvas,foreground,muted,outline,accent,selected,error));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12,8,12,12);
    layout->setSpacing(12);
    const auto iconButton = [this](Icon icon,const QString &name,const QString &tip) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        button->setIcon(makeIcon(icon));
        button->setIconSize(QSize(20,20));
        button->setFixedSize(40,40);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        return button;
    };
    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("ocrHeader"));
    header->installEventFilter(this);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12,0,0,0);
    headerLayout->setSpacing(4);
    auto *title = new QLabel(windowTitle());
    title->setObjectName(QStringLiteral("ocrTitle"));
    QFont titleFont = interfaceFont;
    titleFont.setPointSizeF(qMax(12.0,interfaceFont.pointSizeF()));
    titleFont.setWeight(QFont::Medium);
    title->setFont(titleFont);
    title->installEventFilter(this);
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    auto *pin = iconButton(Icon::Pin,QStringLiteral("ocrPin"),QStringLiteral("置顶 OCR 卡片"));
    pin->setCheckable(true);
    connect(pin,&QToolButton::toggled,this,[this,pin](bool checked) {
        // Change only native Z-order so geometry, focus and editing state remain intact.
        if (!SetWindowPos(reinterpret_cast<HWND>(winId()),checked ? HWND_TOPMOST : HWND_NOTOPMOST,
                          0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)) {
            const QSignalBlocker blocker(pin);
            pin->setChecked(!checked);
            m_status->setText(QStringLiteral("切换置顶失败，请重试"));
            return;
        }
        const QString tip = checked ? QStringLiteral("取消置顶 OCR 卡片") : QStringLiteral("置顶 OCR 卡片");
        pin->setToolTip(tip); pin->setAccessibleName(tip);
    });
    headerLayout->addWidget(pin);
    auto *minimize = iconButton(Icon::Minimize,QStringLiteral("ocrMinimize"),QStringLiteral("最小化"));
    connect(minimize,&QToolButton::clicked,this,&QWidget::showMinimized);
    headerLayout->addWidget(minimize);
    m_maximize = iconButton(Icon::Maximize,QStringLiteral("ocrMaximize"),QStringLiteral("最大化"));
    connect(m_maximize,&QToolButton::clicked,this,[this] { isMaximized() ? showNormal() : showMaximized(); });
    headerLayout->addWidget(m_maximize);
    auto *close = iconButton(Icon::Cancel,QStringLiteral("ocrClose"),QStringLiteral("关闭 (Esc)"));
    connect(close,&QToolButton::clicked,this,&QDialog::reject);
    headerLayout->addWidget(close);
    layout->addWidget(header);
    auto *split = new QSplitter(Qt::Horizontal);
    split->setObjectName(QStringLiteral("ocrSplit"));
    split->setChildrenCollapsible(false);
    split->setHandleWidth(8);
    auto *imagePanel = new QWidget;
    imagePanel->setObjectName(QStringLiteral("ocrImagePanel"));
    auto *imageLayout = new QVBoxLayout(imagePanel);
    imageLayout->setContentsMargins(12,12,12,12);
    imageLayout->setSpacing(12);
    auto *imageActions = new QHBoxLayout;
    auto *back = iconButton(Icon::Undo,QStringLiteral("ocrBack"),QStringLiteral("返回截图"));
    back->setText(QStringLiteral("返回截图"));
    back->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    back->setMinimumWidth(back->sizeHint().width());
    back->setMaximumWidth(QWIDGETSIZE_MAX);
    connect(back,&QToolButton::clicked,this,&QDialog::accept);
    imageActions->addWidget(back);
    imageActions->addStretch();
    imageLayout->addLayout(imageActions);
    auto *imageView = new OcrImageView(image);
    imageView->setFocusPolicy(Qt::StrongFocus);
    imagePanel->setMinimumWidth(180);
    imageView->setToolTip(QStringLiteral("滚轮缩放 · 拖动查看 · 双击适应"));
    imageLayout->addWidget(imageView,1);
    split->addWidget(imagePanel);
    auto *textPanel = new QWidget;
    textPanel->setObjectName(QStringLiteral("ocrTextPanel"));
    auto *textLayout = new QVBoxLayout(textPanel);
    textLayout->setSizeConstraint(QLayout::SetMinimumSize);
    textLayout->setContentsMargins(12,12,12,12);
    textLayout->setSpacing(12);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    m_retry = iconButton(Icon::Refresh,QStringLiteral("ocrRetry"),QStringLiteral("重新识别"));
    connect(m_retry,&QToolButton::clicked,this,&OcrDialog::start);
    actions->addWidget(m_retry);
    m_copy = iconButton(Icon::Copy,QStringLiteral("ocrCopy"),QStringLiteral("复制全部"));
    m_copy->setEnabled(false);
    actions->addWidget(m_copy);
    textLayout->addLayout(actions);
    m_text = new QPlainTextEdit;
    m_text->setObjectName(QStringLiteral("ocrText"));
    m_text->setAccessibleName(QStringLiteral("OCR 识别文本"));
    m_text->setPlaceholderText(QStringLiteral("正在识别，请稍候…"));
    m_text->setReadOnly(true);
    QFont font = interfaceFont;
    font.setPointSizeF(qMax(12.0,interfaceFont.pointSizeF()));
    m_text->setFont(font);
    textLayout->addWidget(m_text,1);
    split->addWidget(textPanel);
    split->setStretchFactor(0,1); split->setStretchFactor(1,1);
    layout->addWidget(split,1);
    m_status = new QLabel(QStringLiteral("正在识别…"));
    m_status->setObjectName(QStringLiteral("ocrStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setFocusPolicy(Qt::ClickFocus);
    QFont captionFont = interfaceFont;
    captionFont.setPointSizeF(qMax(9.0,interfaceFont.pointSizeF()/1.125/1.125));
    m_status->setFont(captionFont);
    m_status->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    m_status->setContentsMargins(4,2,4,2);
    textLayout->addWidget(m_status);
    imageLayout->addSpacing(m_status->sizeHint().height()+12);
    setTabOrder(pin,minimize); setTabOrder(minimize,m_maximize); setTabOrder(m_maximize,close);
    setTabOrder(close,back); setTabOrder(back,imageView); setTabOrder(imageView,m_retry);
    setTabOrder(m_retry,m_copy); setTabOrder(m_copy,m_text);
    connect(m_copy, &QToolButton::clicked, this, [this] {
        if (!copyTextToClipboard(m_text->toPlainText()))
            m_status->setText(QStringLiteral("复制失败，剪贴板可能被占用，请重试"));
        else m_status->setText(QStringLiteral("已复制全部文本"));
    });
    connect(m_text, &QPlainTextEdit::textChanged, this, [this] { m_copy->setEnabled(!m_text->toPlainText().isEmpty()); });
    const QRect anchor = captureRect.isEmpty() ? QRect(QCursor::pos(),QSize(1,1)) : captureRect;
    const QScreen *screen = QGuiApplication::screenAt(anchor.center());
    const QRect available = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    setGeometry(placement(anchor,available));
    split->setSizes({width()/2,width()/2});
    m_timer.setInterval(300);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        m_status->setText(QStringLiteral("正在本地识别%1").arg(QString((++m_ticks % 3)+1, QLatin1Char('.'))));
    });
    connect(&m_watcher, &QFutureWatcher<OcrResult>::finished, this, [this] {
        m_timer.stop();
        const OcrResult result = m_watcher.result();
        m_retry->setEnabled(true);
        if (m_cancel->load()) return;
        if (!result.error.isEmpty()) {
            m_status->setText(result.error);
            m_text->setReadOnly(m_text->toPlainText().isEmpty());
            m_text->setPlaceholderText(QStringLiteral("识别未完成，可重试或返回截图"));
            return;
        }
        m_text->setReadOnly(false);
        m_text->setPlainText(result.text());
        m_text->setPlaceholderText(QStringLiteral("未识别到文字。可返回截图调整选区，或在此输入文本。"));
        double confidence=0;
        for (const OcrLine &line:result.lines) confidence+=line.score;
        m_status->setText(result.lines.isEmpty() ? QStringLiteral("未识别到文字，可返回截图调整选区")
            : QStringLiteral("%1秒 · 平均置信度%2%").arg(result.elapsed/1000.0,0,'f',2)
                .arg(confidence*100/result.lines.size(),0,'f',0));
        m_text->setToolTip(QStringLiteral("置信度不等于准确率，请对照原图校对"));
        m_text->setFocus();
    });
    connect(this, &QDialog::finished, this, [this] { m_closed=true; if (m_cancel) m_cancel->store(true); });
    QTimer::singleShot(0, this, &OcrDialog::start);
}

OcrDialog::~OcrDialog()  { if (m_cancel) m_cancel->store(true); }

QRect OcrDialog::placement(const QRect &capture, const QRect &available) {
    const QSize size(qMin(1000,available.width()),qMin(650,available.height()));
    return QRect(QPoint(qBound(available.left(),capture.left(),available.right()-size.width()+1),
                        qBound(available.top(),capture.top(),available.bottom()-size.height()+1)),size);
}

void OcrDialog::start() {
    if (m_closed || m_watcher.isRunning()) return;
    if (m_text->document()->isModified()
        && QMessageBox::question(this,QStringLiteral("重新识别"),QStringLiteral("重新识别会替换已修改的文本，是否继续？"),
                                 QMessageBox::Yes|QMessageBox::No,QMessageBox::No) != QMessageBox::Yes) return;
    // The confirmation pumps events and may close the card before it returns.
    if (m_closed || m_watcher.isRunning()) return;
    auto &engine = OcrEngine::instance();
    bool expected = false;
    if (!engine.busy.compare_exchange_strong(expected,true)) {
        m_status->setText(QStringLiteral("上一项识别正在结束，请稍候再点击重新识别"));
        return;
    }
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    const QImage image = m_image;
    m_retry->setEnabled(false);
    m_text->setReadOnly(true);
    m_status->setText(QStringLiteral("正在本地识别…"));
    m_timer.start();
    m_watcher.setFuture(QtConcurrent::run([&engine,image,cancel] {
        const OcrResult result = engine.recognize(image,cancel);
        engine.busy.store(false);
        return result;
    }));
}

bool OcrDialog::eventFilter(QObject *object,QEvent *event)  {
    if (event->type()==QEvent::MouseButtonDblClick) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button()==Qt::LeftButton) { m_maximize->click(); return true; }
    } else if (event->type()==QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button()==Qt::LeftButton && windowHandle()) {
            windowHandle()->startSystemMove(); return true;
        }
    }
    return QDialog::eventFilter(object,event);
}

void OcrDialog::changeEvent(QEvent *event)  {
    QDialog::changeEvent(event);
    if (event->type()==QEvent::WindowStateChange && m_maximize) {
        m_maximize->setIcon(makeIcon(isMaximized() ? Icon::Restore : Icon::Maximize));
        const QString tip = isMaximized() ? QStringLiteral("还原") : QStringLiteral("最大化");
        m_maximize->setToolTip(tip); m_maximize->setAccessibleName(tip);
    }
}

void OcrDialog::mousePressEvent(QMouseEvent *event)  {
    const Qt::Edges edges = resizeEdges(event->position().toPoint());
    if (event->button()==Qt::LeftButton && edges && windowHandle() && windowHandle()->startSystemResize(edges)) {
        event->accept(); return;
    }
    QDialog::mousePressEvent(event);
}

void OcrDialog::mouseMoveEvent(QMouseEvent *event)  {
    const Qt::Edges edges = resizeEdges(event->position().toPoint());
    const bool horizontal = edges.testFlag(Qt::LeftEdge)||edges.testFlag(Qt::RightEdge);
    const bool vertical = edges.testFlag(Qt::TopEdge)||edges.testFlag(Qt::BottomEdge);
    if (horizontal && vertical)
        setCursor((edges.testFlag(Qt::LeftEdge)==edges.testFlag(Qt::TopEdge)) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
    else if (horizontal) setCursor(Qt::SizeHorCursor);
    else if (vertical) setCursor(Qt::SizeVerCursor);
    else unsetCursor();
    QDialog::mouseMoveEvent(event);
}

void OcrDialog::leaveEvent(QEvent *event)  { unsetCursor(); QDialog::leaveEvent(event); }

Qt::Edges OcrDialog::resizeEdges(const QPoint &point) const {
    Qt::Edges edges;
    if (isMaximized()) return edges;
    if (point.x()<6) edges|=Qt::LeftEdge;
    else if (point.x()>=width()-6) edges|=Qt::RightEdge;
    if (point.y()<6) edges|=Qt::TopEdge;
    else if (point.y()>=height()-6) edges|=Qt::BottomEdge;
    return edges;
}
