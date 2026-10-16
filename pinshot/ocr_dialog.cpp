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
#include <QKeyEvent>
#include <QGridLayout>
#include <QTextEdit>
#include <tuple>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QShortcut>
#include <QKeySequence>
#include <QTextDocument>
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
    // A source still on screen (a pin) stays visible next to the card instead of under it.
    if (source && source->isVisible()) {
        const QRect avoid = source->frameGeometry();
        const QScreen *screen = QGuiApplication::screenAt(avoid.center());
        dialog->setGeometry(OcrDialog::beside(avoid,(screen ? screen : QGuiApplication::primaryScreen())->availableGeometry()));
    }
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
    // The capture keeps its selection outline, so it reads as the region just framed.
    QPen frame(QColor(0x20,0x80,0xf0), 1);
    frame.setCosmetic(true);
    scene->addRect(QRectF(QPointF(), native.size()).adjusted(-0.5,-0.5,0.5,0.5), frame);
    const qreal margin = qMax(native.width(), native.height()) * 0.04 + 8;   // Graphite breathing room.
    m_fit = QRectF(QPointF(), native.size()).adjusted(-margin,-margin,margin,margin);
    // A boundless scene lets the capture be dragged anywhere without scroll bars.
    scene->setSceneRect(m_fit.adjusted(-1e5,-1e5,1e5,1e5));
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setRenderHint(QPainter::SmoothPixmapTransform);
    setBackgroundBrush(QColor(0x2f,0x32,0x36));
    setAccessibleName(QStringLiteral("截图对照：滚轮缩放，拖动查看"));
}

void OcrImageView::resizeEvent(QResizeEvent *event)  {
    QGraphicsView::resizeEvent(event);
    if (!m_zoomed) fitInView(m_fit, Qt::KeepAspectRatio);
}

void OcrImageView::fit() {
    m_zoomed = false;
    fitInView(m_fit, Qt::KeepAspectRatio);
}

void OcrImageView::wheelEvent(QWheelEvent *event)  {
    if (!event->angleDelta().y()) return;
    const qreal factor = event->angleDelta().y() > 0 ? 1.2 : 1/1.2;
    const qreal next = transform().m11() * factor;
    if (next >= 0.02 && next <= 16) {
        // AnchorUnderMouse relies on a prior mouse move; a cursor resting since the card opened anchors at the origin.
        const QPoint pos = event->position().toPoint();
        const QPointF anchor = mapToScene(pos);
        setTransformationAnchor(NoAnchor);
        scale(factor, factor);
        centerOn(mapToScene(viewport()->rect().center()) + anchor - mapToScene(pos));
        m_zoomed = true;
    }
    event->accept();
}

void OcrImageView::mouseDoubleClickEvent(QMouseEvent *event)  {
    fit();
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
    // Chrome matches the capture bar; the screenshot sits on a graphite light table, the text on white.
    const QString surface = QStringLiteral("#f2f2f2"), panel = QStringLiteral("#ffffff"),
                  table = QStringLiteral("#2f3236"), foreground = QStringLiteral("#323232"),
                  muted = QStringLiteral("#5b5b5b"), outline = QStringLiteral("#858585"),
                  accent = QStringLiteral("#2080f0"), selected = QStringLiteral("#a3d6ff"),
                  error = QStringLiteral("#e81123");
    setStyleSheet(QStringLiteral(
        "QDialog{background:%1;color:%4;border:1px solid #c8c8c8;}"
        "QLabel{color:%5;background:transparent;} QLabel#ocrTitle{color:%4;}"
        "QWidget#ocrImageBar,QWidget#ocrTextBar{background:%1;}"
        "QWidget#ocrImagePanel{background:%3;} QWidget#ocrTextPanel{background:%2;}"
        "QWidget#ocrFind{background:%1;}"
        "QPlainTextEdit{background:%2;color:%4;border:none;padding:12px 16px;selection-background-color:%8;}"
        "QGraphicsView{background:%3;border:none;}"
        "QLineEdit{background:%2;color:%4;border:1px solid #c8c8c8;border-radius:4px;padding:3px 6px;selection-background-color:%8;}"
        "QLineEdit:focus{border-color:%7;}"
        "QToolButton{background:transparent;color:%4;border:1px solid transparent;border-radius:4px;padding:4px;}"
        "QToolButton:hover{background:#e0e0e0;} QToolButton:pressed{background:#d4d4d4;}"
        "QToolButton:focus{border-color:%7;} QToolButton#ocrPin:checked{background:%7;}"
        "QToolButton::menu-indicator{image:none;width:0;}"
        "QToolButton:disabled{background:transparent;} QToolButton#ocrClose:hover{background:%9;}"
        // The divider matches the bars, so it only shows between the two contents.
        "QSplitter::handle{background:%1;} QSplitter::handle:hover{background:%7;}"
        "QMenu{background:%2;color:%4;border:1px solid #c8c8c8;padding:4px;}"
        "QMenu::item{padding:6px 20px;} QMenu::item:selected{background:#e0e0e0;color:%4;}"
        "QScrollBar:vertical{background:transparent;width:12px;margin:0;} QScrollBar:horizontal{background:transparent;height:12px;margin:0;}"
        "QScrollBar::handle{background:%6;border-radius:4px;min-width:24px;min-height:24px;}"
        "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;}"
        "QScrollBar::add-page,QScrollBar::sub-page{background:transparent;}"
        "QToolTip{background:%2;color:%4;border:1px solid %6;padding:6px;}"
    ).arg(surface,panel,table,foreground,muted,outline,accent,selected,error));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1,1,1,1);         // Inside the 1px window border.
    layout->setSpacing(0);
    const auto iconButton = [this](Icon icon,const QString &name,const QString &tip) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        button->setIcon(makeIcon(icon));
        button->setIconSize(QSize(20,20));
        button->setFixedSize(40,40);
        // Clicks must not take focus (no outline); Tab still reaches and outlines the button.
        button->setFocusPolicy(Qt::TabFocus);
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
    // Qt flags the frameless card maximized without Windows zooming it, so drive the native show state.
    connect(m_maximize,&QToolButton::clicked,this,[this] {
        ShowWindow(reinterpret_cast<HWND>(winId()), isMaximized() ? SW_RESTORE : SW_MAXIMIZE);
    });
    headerLayout->addWidget(m_maximize);
    auto *close = iconButton(Icon::Cancel,QStringLiteral("ocrClose"),QStringLiteral("关闭 (Esc)"));
    connect(close,&QToolButton::clicked,this,&QDialog::reject);
    headerLayout->addWidget(close);
    layout->addWidget(header);
    m_split = new QSplitter(Qt::Horizontal);
    m_split->setObjectName(QStringLiteral("ocrSplit"));
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(1);
    // Each pane: an action bar (top in columns, right edge in rows) and its content edge to edge.
    const auto actionBar = [](const QString &name) {
        auto *bar = new QWidget;
        bar->setObjectName(name);
        bar->setAttribute(Qt::WA_StyledBackground);
        auto *row = new QBoxLayout(QBoxLayout::LeftToRight,bar);
        row->setContentsMargins(8,4,8,4);
        row->setSpacing(4);
        return std::pair<QWidget *,QBoxLayout *>(bar,row);
    };
    m_imagePanel = new QWidget;
    m_imagePanel->setObjectName(QStringLiteral("ocrImagePanel"));
    m_imagePanel->setAttribute(Qt::WA_StyledBackground);
    m_imagePane = new QBoxLayout(QBoxLayout::TopToBottom,m_imagePanel);
    m_imagePane->setContentsMargins(0,0,0,0);
    m_imagePane->setSpacing(0);
    QWidget *imageBar;
    std::tie(imageBar,m_imageActions) = actionBar(QStringLiteral("ocrImageBar"));
    auto *back = iconButton(Icon::Undo,QStringLiteral("ocrBack"),QStringLiteral("返回截图"));
    connect(back,&QToolButton::clicked,this,&QDialog::accept);
    m_imageActions->addWidget(back);
    auto *imageView = new OcrImageView(image);
    auto *fit = iconButton(Icon::FitView,QStringLiteral("ocrFit"),QStringLiteral("完整显示截图"));
    connect(fit,&QToolButton::clicked,imageView,&OcrImageView::fit);
    m_imageActions->addWidget(fit);
    m_imageActions->addStretch();
    m_imagePane->addWidget(imageBar);
    imageView->setFocusPolicy(Qt::StrongFocus);
    m_imagePanel->setMinimumSize(180,120);
    imageView->setToolTip(QStringLiteral("滚轮缩放 · 拖动查看 · 双击适应"));
    m_imagePane->addWidget(imageView,1);
    m_split->addWidget(m_imagePanel);
    auto *textPanel = new QWidget;
    textPanel->setObjectName(QStringLiteral("ocrTextPanel"));
    textPanel->setAttribute(Qt::WA_StyledBackground);
    m_textPane = new QBoxLayout(QBoxLayout::TopToBottom,textPanel);
    m_textPane->setContentsMargins(0,0,0,0);
    m_textPane->setSpacing(0);
    QWidget *textBar;
    std::tie(textBar,m_textActions) = actionBar(QStringLiteral("ocrTextBar"));
    m_layout = iconButton(Icon::SplitRows,QStringLiteral("ocrLayout"),QStringLiteral("切换为上下分屏"));
    connect(m_layout,&QToolButton::clicked,this,[this] { setStacked(!m_rows); });
    m_textActions->addWidget(m_layout);
    m_format = iconButton(Icon::TextFormat,QStringLiteral("ocrFormat"),QStringLiteral("排版"));
    auto *formatMenu = new QMenu(m_format);
    auto *formatGroup = new QActionGroup(formatMenu);
    const std::pair<const char16_t *,TextFormat> formats[] = {
        {u"自动",TextFormat::Auto},{u"移除换行符",TextFormat::JoinLines},{u"移除空格",TextFormat::RemoveSpaces},
        {u"全角符号",TextFormat::FullWidth},{u"半角符号",TextFormat::HalfWidth}};
    for (const auto &[label,kind] : formats) {
        QAction *action = formatMenu->addAction(QString::fromUtf16(label));
        action->setCheckable(true);
        formatGroup->addAction(action);
        if (kind==TextFormat::Auto) { m_autoFormat = action; action->setChecked(true); }
        connect(action,&QAction::triggered,this,[this,kind = kind] {
            if (m_text->isReadOnly()) return;
            // Auto restores the recognized text; other modes reformat the current, possibly corrected, text.
            const QString next = kind==TextFormat::Auto ? m_raw : formatText(m_text->toPlainText(),kind);
            if (next==m_text->toPlainText()) return;
            QTextCursor cursor(m_text->document());   // One undoable edit instead of setPlainText.
            cursor.select(QTextCursor::Document);
            cursor.insertText(next);
        });
    }
    m_format->setMenu(formatMenu);
    m_format->setPopupMode(QToolButton::InstantPopup);
    m_textActions->addWidget(m_format);
    auto *search = iconButton(Icon::Search,QStringLiteral("ocrSearch"),QStringLiteral("查找 (Ctrl+F)"));
    connect(search,&QToolButton::clicked,this,[this] { m_find->isVisible() ? hideFind() : showFind(false); });
    m_textActions->addWidget(search);
    m_textActions->addStretch();
    m_retry = iconButton(Icon::Refresh,QStringLiteral("ocrRetry"),QStringLiteral("重新识别"));
    connect(m_retry,&QToolButton::clicked,this,&OcrDialog::start);
    m_textActions->addWidget(m_retry);
    m_copy = iconButton(Icon::Copy,QStringLiteral("ocrCopy"),QStringLiteral("复制全部"));
    m_copy->setEnabled(false);
    m_textActions->addWidget(m_copy);
    m_textPane->addWidget(textBar);
    auto *body = new QWidget;
    auto *textLayout = new QVBoxLayout(body);
    textLayout->setSizeConstraint(QLayout::SetMinimumSize);
    textLayout->setContentsMargins(0,0,0,0);
    textLayout->setSpacing(0);
    m_textPane->addWidget(body,1);
    // Find / replace strip under the bar, shown by the search button, Ctrl+F or Ctrl+H.
    m_find = new QWidget;
    m_find->setObjectName(QStringLiteral("ocrFind"));
    m_find->setAttribute(Qt::WA_StyledBackground);
    auto *findLayout = new QGridLayout(m_find);
    findLayout->setContentsMargins(8,4,8,6);
    findLayout->setHorizontalSpacing(4); findLayout->setVerticalSpacing(4);
    const auto smallButton = [&](Icon icon,const QString &name,const QString &tip) {
        auto *button = iconButton(icon,name,tip);
        button->setIconSize(QSize(14,14)); button->setFixedSize(28,28);
        return button;
    };
    m_replaceToggle = smallButton(Icon::ChevronRight,QStringLiteral("ocrReplaceToggle"),QStringLiteral("切换替换"));
    connect(m_replaceToggle,&QToolButton::clicked,this,[this] { showFind(!m_replaceRow->isVisible()); });
    findLayout->addWidget(m_replaceToggle,0,0);
    m_findEdit = new QLineEdit;
    m_findEdit->setObjectName(QStringLiteral("ocrFindEdit"));
    m_findEdit->setPlaceholderText(QStringLiteral("查找"));
    m_findEdit->setAccessibleName(QStringLiteral("查找"));
    m_findEdit->installEventFilter(this);
    connect(m_findEdit,&QLineEdit::textChanged,this,[this] { m_match = 0; highlightMatches(true); });
    findLayout->addWidget(m_findEdit,0,1);
    m_findCount = new QLabel;
    m_findCount->setObjectName(QStringLiteral("ocrFindCount"));
    m_findCount->setMinimumWidth(56);
    m_findCount->setAlignment(Qt::AlignCenter);
    findLayout->addWidget(m_findCount,0,2);
    auto *previous = smallButton(Icon::ChevronUp,QStringLiteral("ocrFindPrevious"),QStringLiteral("上一个 (Shift+Enter)"));
    connect(previous,&QToolButton::clicked,this,[this] { findStep(-1); });
    findLayout->addWidget(previous,0,3);
    auto *next = smallButton(Icon::ChevronDown,QStringLiteral("ocrFindNext"),QStringLiteral("下一个 (Enter)"));
    connect(next,&QToolButton::clicked,this,[this] { findStep(1); });
    findLayout->addWidget(next,0,4);
    auto *closeFind = smallButton(Icon::Cancel,QStringLiteral("ocrFindClose"),QStringLiteral("关闭查找 (Esc)"));
    connect(closeFind,&QToolButton::clicked,this,&OcrDialog::hideFind);
    findLayout->addWidget(closeFind,0,5);
    m_replaceRow = new QWidget;
    auto *replaceLayout = new QHBoxLayout(m_replaceRow);
    replaceLayout->setContentsMargins(0,0,0,0); replaceLayout->setSpacing(4);
    m_replaceEdit = new QLineEdit;
    m_replaceEdit->setObjectName(QStringLiteral("ocrReplaceEdit"));
    m_replaceEdit->setPlaceholderText(QStringLiteral("替换"));
    m_replaceEdit->setAccessibleName(QStringLiteral("替换为"));
    m_replaceEdit->installEventFilter(this);
    replaceLayout->addWidget(m_replaceEdit,1);
    for (const auto &[label,name,slot] : {std::tuple(u"替换",u"ocrReplace",&OcrDialog::replaceOne),
                                          std::tuple(u"全部替换",u"ocrReplaceAll",&OcrDialog::replaceAll)}) {
        auto *button = new QToolButton;
        button->setObjectName(QString::fromUtf16(name));
        button->setText(QString::fromUtf16(label));
        button->setFixedHeight(28);
        button->setFocusPolicy(Qt::TabFocus);
        connect(button,&QToolButton::clicked,this,slot);
        replaceLayout->addWidget(button);
    }
    findLayout->addWidget(m_replaceRow,1,1,1,5);
    m_find->hide();
    textLayout->addWidget(m_find);
    m_text = new QPlainTextEdit;
    m_text->setObjectName(QStringLiteral("ocrText"));
    m_text->setAccessibleName(QStringLiteral("OCR 识别文本"));
    m_text->setReadOnly(true);
    QFont font = interfaceFont;
    font.setPointSizeF(qMax(12.0,interfaceFont.pointSizeF()));
    m_text->setFont(font);
    m_busy = new QLabel(QStringLiteral("· · ·"));
    m_busy->setObjectName(QStringLiteral("ocrBusy"));
    QFont busyFont = font; busyFont.setPointSizeF(font.pointSizeF()*1.5);
    m_busy->setFont(busyFont);
    m_busy->setAlignment(Qt::AlignCenter);
    m_busy->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_busy->hide();
    (new QVBoxLayout(m_text->viewport()))->addWidget(m_busy);
    textLayout->addWidget(m_text,1);
    m_split->addWidget(textPanel);
    m_split->setStretchFactor(0,1); m_split->setStretchFactor(1,1);
    layout->addWidget(m_split,1);
    m_status = new QLabel(QStringLiteral("识别中 · · ·"));
    m_status->setObjectName(QStringLiteral("ocrStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setFocusPolicy(Qt::ClickFocus);
    QFont captionFont = interfaceFont;
    captionFont.setPointSizeF(qMax(9.0,interfaceFont.pointSizeF()/1.125/1.125));
    m_status->setFont(captionFont);
    m_status->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    m_status->setContentsMargins(12,4,12,8);
    textLayout->addWidget(m_status);
    setTabOrder(pin,minimize); setTabOrder(minimize,m_maximize); setTabOrder(m_maximize,close);
    setTabOrder(close,back); setTabOrder(back,imageView); setTabOrder(imageView,m_retry);
    setTabOrder(m_retry,m_copy); setTabOrder(m_copy,m_text);
    setTabOrder(back,fit); setTabOrder(fit,imageView); setTabOrder(imageView,m_layout);
    setTabOrder(m_layout,m_format); setTabOrder(m_format,search); setTabOrder(search,m_retry);
    connect(new QShortcut(QKeySequence::Find,this),&QShortcut::activated,this,[this] { showFind(false); });
    connect(new QShortcut(QKeySequence(Qt::CTRL|Qt::Key_H),this),&QShortcut::activated,this,[this] { showFind(true); });
    connect(m_text,&QPlainTextEdit::textChanged,this,[this] { if (m_find->isVisible()) highlightMatches(); });
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
    setStacked(prefersRows(image.size(),size()));
    connect(&m_watcher, &QFutureWatcher<OcrResult>::finished, this, [this] {
        const OcrResult result = m_watcher.result();
        m_busy->hide();
        m_retry->setEnabled(true);
        if (m_cancel->load()) return;
        if (!result.error.isEmpty()) {
            // A failed retry brings the previous text back instead of leaving the pane empty.
            m_text->setPlainText(m_previous);
            m_text->document()->setModified(m_previousModified);
            m_status->setText(result.error);
            m_text->setReadOnly(m_text->toPlainText().isEmpty());
            m_text->setPlaceholderText(QStringLiteral("识别未完成，可重试或返回截图"));
            return;
        }
        m_text->setReadOnly(false);
        m_raw = result.text();
        m_text->setPlainText(m_raw);
        m_autoFormat->setChecked(true);
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

QRect OcrDialog::beside(const QRect &avoid, const QRect &available) {
    const QRect card = placement(avoid,available);
    const QSize s = card.size();
    const int gap = 8;
    // Right, left, below, above, slid inside the work area; the first clear spot wins, else the least covering one.
    QRect best = card;
    qint64 bestArea = qint64(avoid.width())*avoid.height()+1;
    for (const QPoint p : {QPoint(avoid.right()+1+gap,avoid.top()),QPoint(avoid.left()-gap-s.width(),avoid.top()),
                           QPoint(avoid.left(),avoid.bottom()+1+gap),QPoint(avoid.left(),avoid.top()-gap-s.height())}) {
        const QRect r(QPoint(qBound(available.left(),p.x(),available.right()-s.width()+1),
                             qBound(available.top(),p.y(),available.bottom()-s.height()+1)),s);
        const QRect hit = r.intersected(avoid);
        const qint64 area = hit.isEmpty() ? 0 : qint64(hit.width())*hit.height();
        if (area < bestArea) { best = r; bestArea = area; }
        if (!area) break;
    }
    return best;
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
    // Disabling the focused retry would hand focus to copy and outline it; park focus on the text.
    if (m_retry->hasFocus()) m_text->setFocus();
    m_retry->setEnabled(false);
    m_previous = m_text->toPlainText();
    m_previousModified = m_text->document()->isModified();
    m_text->clear();
    m_text->setReadOnly(true);
    m_busy->show();
    m_status->setText(QStringLiteral("识别中 · · ·"));
    m_watcher.setFuture(QtConcurrent::run([&engine,image,cancel] {
        const OcrResult result = engine.recognize(image,cancel);
        engine.busy.store(false);
        return result;
    }));
}

bool OcrDialog::prefersRows(const QSize &image, const QSize &card) {
    // Pick the split that shows the capture larger: wide strips stack, tall or square ones sit beside.
    if (image.isEmpty() || card.isEmpty()) return false;
    const qreal columns = qMin(card.width()/2.0/image.width(), qreal(card.height())/image.height());
    const qreal rows = qMin(qreal(card.width())/image.width(), card.height()/2.0/image.height());
    return rows > columns;
}

void OcrDialog::setStacked(bool rows) {
    m_rows = rows;
    m_split->setOrientation(rows ? Qt::Vertical : Qt::Horizontal);
    // Columns: bar above content. Rows: bar on the content's right edge.
    for (QBoxLayout *pane : {m_imagePane,m_textPane})
        pane->setDirection(rows ? QBoxLayout::RightToLeft : QBoxLayout::TopToBottom);
    for (QBoxLayout *bar : {m_imageActions,m_textActions})
        bar->setDirection(rows ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    // Five stacked 40px actions need more height than the side-by-side minimum.
    setMinimumSize(640, rows ? 440 : 360);
    const int span = rows ? m_split->height() : m_split->width();
    m_split->setSizes({span/2,span/2});
    m_layout->setIcon(makeIcon(rows ? Icon::SplitColumns : Icon::SplitRows));
    const QString tip = rows ? QStringLiteral("切换为左右分屏") : QStringLiteral("切换为上下分屏");
    m_layout->setToolTip(tip); m_layout->setAccessibleName(tip);
}

QString OcrDialog::formatText(const QString &text, TextFormat kind) {
    const auto alnum = [](QChar c) { return c.unicode()<0x80 && c.isLetterOrNumber(); };
    QString out;
    out.reserve(text.size());
    for (int i=0;i<text.size();++i) {
        const QChar c = text.at(i);
        const QChar before = i ? text.at(i-1) : QChar(), after = i+1<text.size() ? text.at(i+1) : QChar();
        switch (kind) {
        case TextFormat::Auto:
            break;
        case TextFormat::JoinLines:
            if (c==QLatin1Char('\n')) {
                // Latin words split across lines keep one space; CJK lines join directly.
                if (alnum(before) && alnum(after)) out += QLatin1Char(' ');
                continue;
            }
            break;
        case TextFormat::RemoveSpaces:
            if (c!=QLatin1Char('\n') && c.isSpace()) continue;
            break;
        case TextFormat::FullWidth:
            // Punctuation only; marks inside numbers, versions and URLs (3.14, a-b) stay half width.
            if (c.unicode()>0x20 && c.unicode()<0x7f && !alnum(c) && !(alnum(before) && alnum(after))) {
                out += c==QLatin1Char('.') ? QChar(0x3002) : QChar(c.unicode()+0xfee0);
                continue;
            }
            break;
        case TextFormat::HalfWidth:
            if (c.unicode()==0x3002) { out += QLatin1Char('.'); continue; }
            if (c.unicode()==0x3001) { out += QLatin1Char(','); continue; }
            if (c.unicode()>0xff00 && c.unicode()<0xff5f && !QChar(c.unicode()-0xfee0).isLetterOrNumber()) {
                out += QChar(c.unicode()-0xfee0);
                continue;
            }
            break;
        }
        out += c;
    }
    return out;
}

QList<QTextCursor> OcrDialog::matches() const {
    QList<QTextCursor> found;
    const QString needle = m_findEdit->text();
    if (needle.isEmpty()) return found;
    QTextCursor cursor = m_text->document()->find(needle,0);
    while (!cursor.isNull() && found.size()<10000) {
        found.append(cursor);
        cursor = m_text->document()->find(needle,cursor);
    }
    return found;
}

void OcrDialog::highlightMatches(bool reveal) {
    const QList<QTextCursor> found = matches();
    m_match = found.isEmpty() ? -1 : qBound(0,m_match,int(found.size())-1);
    QList<QTextEdit::ExtraSelection> marks;
    for (int i=0;i<found.size();++i) {
        QTextEdit::ExtraSelection mark;
        mark.cursor = found[i];
        mark.format.setBackground(i==m_match ? QColor(0xff,0x9f,0x1a) : QColor(0xff,0xe5,0x4c));
        marks.append(mark);
    }
    m_text->setExtraSelections(marks);
    m_findCount->setText(found.isEmpty() ? (m_findEdit->text().isEmpty() ? QString() : QStringLiteral("无结果"))
                                         : QStringLiteral("%1/%2").arg(m_match+1).arg(found.size()));
    if (reveal && m_match>=0) {
        QTextCursor caret = found[m_match];
        caret.clearSelection();
        m_text->setTextCursor(caret);
        m_text->ensureCursorVisible();
    }
}

void OcrDialog::showFind(bool replace) {
    const QString selected = m_text->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator)) m_findEdit->setText(selected);
    m_find->show();
    m_replaceRow->setVisible(replace);
    m_replaceToggle->setIcon(makeIcon(replace ? Icon::ChevronDown : Icon::ChevronRight));
    m_findEdit->setFocus();
    m_findEdit->selectAll();
    highlightMatches(true);
}

void OcrDialog::hideFind() {
    m_find->hide();
    m_text->setExtraSelections({});
    m_text->setFocus();
}

void OcrDialog::findStep(int direction) {
    const int count = matches().size();
    if (!count) return;
    m_match = ((m_match<0 ? 0 : m_match+direction)%count+count)%count;
    highlightMatches(true);
}

void OcrDialog::replaceOne() {
    const QList<QTextCursor> found = matches();
    if (m_text->isReadOnly() || m_match<0 || m_match>=found.size()) return;
    QTextCursor cursor = found[m_match];
    cursor.insertText(m_replaceEdit->text());   // m_match now points at the following hit.
    highlightMatches(true);
}

void OcrDialog::replaceAll() {
    const QList<QTextCursor> found = matches();
    if (m_text->isReadOnly() || found.isEmpty()) return;
    QTextCursor edit(m_text->document());
    edit.beginEditBlock();                       // One undo step, back to front so offsets stay valid.
    for (int i=found.size()-1;i>=0;--i) {
        edit.setPosition(found[i].selectionStart());
        edit.setPosition(found[i].selectionEnd(),QTextCursor::KeepAnchor);
        edit.insertText(m_replaceEdit->text());
    }
    edit.endEditBlock();
    highlightMatches();
}

bool OcrDialog::eventFilter(QObject *object,QEvent *event)  {
    if (object==m_findEdit || object==m_replaceEdit) {
        if (event->type()==QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key()==Qt::Key_Escape) { hideFind(); return true; }
            if (key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter) {
                if (object==m_replaceEdit) replaceOne();
                else findStep(key->modifiers().testFlag(Qt::ShiftModifier) ? -1 : 1);
                return true;
            }
        }
        return QDialog::eventFilter(object,event);
    }
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
