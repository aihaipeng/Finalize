#include "ocr_dialog.h"
#include "translate_gateway.h"
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
#include <QComboBox>
#include <QSettings>
#include <qt_windows.h>
#include <QApplication>
#include <QPushButton>
#include <QFrame>
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAbstractSlider>
#include <QAbstractScrollArea>
#include <QSplitterHandle>
#include <QStylePainter>
#include <QStyleOptionComboBox>
#include <atomic>
#include <memory>

// Child-free padding along every card edge; its outer half grabs a resize.
constexpr int ResizeBand = 16;

static OcrDialog *presentCard(OcrDialog *dialog, QWidget *source)
{
    dialog->setAttribute(Qt::WA_DeleteOnClose);
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

OcrDialog *showOcr(const QImage &image, QWidget *source, const QRect &captureRect)
{
    if (image.isNull()) return nullptr;
    return presentCard(new OcrDialog(image,captureRect), source);
}

OcrDialog *showTranslate(const QImage &image, QWidget *source, const QRect &captureRect)
{
    if (image.isNull()) return nullptr;
    return presentCard(new OcrDialog(image,captureRect,true), source);
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
    setBackgroundBrush(QColor(0x06,0x06,0x07));   // Same near-black as the block it sits on.
    setAccessibleName(QStringLiteral("截图对照：滚轮缩放，拖动查看"));
}

void OcrImageView::resizeEvent(QResizeEvent *event)  {
    QGraphicsView::resizeEvent(event);
    if (!m_zoomed) fitInView(m_fit, Qt::KeepAspectRatio);
    if (zoomed) zoomed();
}

void OcrImageView::fit() {
    m_zoomed = false;
    fitInView(m_fit, Qt::KeepAspectRatio);
    if (zoomed) zoomed();
}

int OcrImageView::percent() const {
    // The scene holds native capture pixels; a scale of 1/DPR shows them one-to-one on screen.
    return qRound(transform().m11() * devicePixelRatioF() * 100);
}

void OcrImageView::zoomTo(int value) {
    const qreal target = qBound(2,value,1600) / 100.0 / devicePixelRatioF();
    const QPointF centre = mapToScene(viewport()->rect().center());
    setTransform(QTransform::fromScale(target,target));
    centerOn(centre);
    m_zoomed = true;
    if (zoomed) zoomed();
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
        if (zoomed) zoomed();
    }
    event->accept();
}

void OcrImageView::mouseDoubleClickEvent(QMouseEvent *event)  {
    // On text a double click selects the whole recognized line; elsewhere it fits the capture.
    const int hit = event->button()==Qt::LeftButton ? glyphAt(event->position().toPoint(),false) : -1;
    if (hit >= 0) {
        int first = hit, last = hit;
        while (first > 0 && m_glyphs[first-1].line == m_glyphs[hit].line) --first;
        while (last+1 < m_glyphs.size() && m_glyphs[last+1].line == m_glyphs[hit].line) ++last;
        m_press = -1; m_selecting = false;
        selectRange(first,last);
        event->accept(); return;
    }
    fit();
    event->accept();
}

void OcrImageView::setTextLines(const QVector<OcrLine> &lines) {
    m_textData.clear(); m_glyphs.clear(); m_highlighted.clear();
    m_anchor = m_lead = m_press = -1; m_selecting = false;
    viewport()->setMouseTracking(true);   // Hover alone must switch the cursor over text.
    for (int l = 0; l < lines.size(); ++l) {
        const OcrLine &line = lines[l];
        if (!m_textData.isEmpty()) m_textData += QLatin1Char('\n');
        const int base = int(m_textData.size());
        m_textData += line.text;
        if (line.glyphs.size() != line.text.size()) continue;   // Text without geometry stays unselectable.
        for (int c = 0; c < line.text.size(); ++c)
            if (!line.text[c].isSpace() && !line.glyphs[c].boundingRect().isEmpty())
                m_glyphs.append(Glyph{line.glyphs[c], base+c, l});
    }
    viewport()->update();
}

int OcrImageView::glyphAt(const QPoint &viewPos, bool nearest) const {
    // A glyph under the point wins; while dragging the closest glyph box follows the cursor.
    const QPointF at = mapToScene(viewPos);
    int best = -1;
    double bestDistance = std::numeric_limits<double>::max();
    for (int i = 0; i < m_glyphs.size(); ++i) {
        if (m_glyphs[i].polygon.containsPoint(at,Qt::OddEvenFill)) return i;
        if (!nearest) continue;
        const QRectF r = m_glyphs[i].polygon.boundingRect();
        const double dx = at.x()<r.left() ? r.left()-at.x() : at.x()>r.right() ? at.x()-r.right() : 0;
        const double dy = at.y()<r.top() ? r.top()-at.y() : at.y()>r.bottom() ? at.y()-r.bottom() : 0;
        if (dx*dx+dy*dy < bestDistance) { bestDistance = dx*dx+dy*dy; best = i; }
    }
    return best;
}

void OcrImageView::selectRange(int anchor, int lead) {
    m_anchor = anchor; m_lead = lead;
    viewport()->update();
}

QString OcrImageView::selectedText() const {
    if (m_anchor < 0 || m_lead < 0 || m_glyphs.isEmpty()) return {};
    const int first = qMin(m_anchor,m_lead), last = qMin(qMax(m_anchor,m_lead),int(m_glyphs.size())-1);
    if (first > last) return {};
    const int from = m_glyphs[first].textIndex, to = m_glyphs[last].textIndex;
    return m_textData.mid(from,to-from+1);
}

void OcrImageView::highlightText(const QString &selection) {
    m_highlighted.clear();
    // Compatibility-folded and blank-free on both sides, so reflowed or width-converted text still finds its glyphs.
    QString needle;
    for (const QChar ch : selection.normalized(QString::NormalizationForm_KC))
        if (!ch.isSpace() && ch != QChar(0x2029) && ch != QChar(0x2028)) needle += ch;
    if (!needle.isEmpty() && !m_glyphs.isEmpty()) {
        QHash<int,int> glyphOf;
        for (int i = 0; i < m_glyphs.size(); ++i) glyphOf.insert(m_glyphs[i].textIndex,i);
        QString hay;
        QVector<int> source;
        for (int i = 0; i < m_textData.size(); ++i) {
            if (m_textData[i].isSpace()) continue;
            for (const QChar ch : QString(m_textData[i]).normalized(QString::NormalizationForm_KC)) {
                hay += ch; source.append(i);
            }
        }
        QSet<int> marked;
        for (int at = hay.indexOf(needle); at >= 0; at = hay.indexOf(needle,at+needle.size()))
            for (int k = at; k < at+needle.size(); ++k)
                if (glyphOf.contains(source[k])) marked.insert(glyphOf.value(source[k]));
        m_highlighted = QVector<int>(marked.begin(),marked.end());
        std::sort(m_highlighted.begin(),m_highlighted.end());
    }
    viewport()->update();
}

void OcrImageView::copySelection() {
    const QString text = selectedText();
    if (text.isEmpty()) return;
    const bool ok = copyTextToClipboard(text);
    if (copied) copied(ok);
}

void OcrImageView::mousePressEvent(QMouseEvent *event) {
    if (event->button()==Qt::LeftButton && !m_glyphs.isEmpty()) {
        setFocus(Qt::MouseFocusReason);
        const int hit = glyphAt(event->position().toPoint(),false);
        if (hit >= 0) {   // Text starts a selection, not a pan; the text cursor stays for the whole drag.
            m_press = hit;
            viewport()->setCursor(Qt::IBeamCursor);
            event->accept(); return;
        }
        selectRange(-1,-1);
    }
    QGraphicsView::mousePressEvent(event);
}

void OcrImageView::mouseMoveEvent(QMouseEvent *event) {
    if (m_press >= 0 && (event->buttons() & Qt::LeftButton)) {
        if (!m_selecting) { m_selecting = true; selectRange(m_press,m_press); }
        const int lead = glyphAt(event->position().toPoint(),true);
        if (lead >= 0 && lead != m_lead) selectRange(m_anchor,lead);
        event->accept(); return;
    }
    if (event->buttons()==Qt::NoButton && !m_glyphs.isEmpty())
        viewport()->setCursor(glyphAt(event->position().toPoint(),false) >= 0 ? Qt::IBeamCursor : Qt::OpenHandCursor);
    QGraphicsView::mouseMoveEvent(event);
}

void OcrImageView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button()==Qt::LeftButton && m_press >= 0) {
        // A click on text without a drag drops the selection.
        if (!m_selecting) selectRange(-1,-1);
        m_press = -1; m_selecting = false;
        viewport()->setCursor(glyphAt(event->position().toPoint(),false) >= 0 ? Qt::IBeamCursor : Qt::OpenHandCursor);
        event->accept(); return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void OcrImageView::keyPressEvent(QKeyEvent *event) {
    if (event->matches(QKeySequence::Copy) && !selectedText().isEmpty()) { copySelection(); event->accept(); return; }
    QGraphicsView::keyPressEvent(event);
}

void OcrImageView::contextMenuEvent(QContextMenuEvent *event) {
    if (selectedText().isEmpty()) { QGraphicsView::contextMenuEvent(event); return; }
    QMenu menu(this);
    QAction *copy = menu.addAction(QStringLiteral("复制"));
    if (menu.exec(event->globalPos()) == copy) copySelection();
    event->accept();
}

void OcrImageView::leaveEvent(QEvent *event) {
    if (!m_glyphs.isEmpty()) viewport()->setCursor(Qt::OpenHandCursor);
    QGraphicsView::leaveEvent(event);
}

void OcrImageView::drawForeground(QPainter *painter, const QRectF &rect) {
    QGraphicsView::drawForeground(painter,rect);
    if (m_glyphs.isEmpty()) return;
    // Consecutive glyphs of one line paint as one quad: first glyph's leading edge to the last one's trailing edge.
    const auto paintRuns = [&](const QVector<int> &indices) {
        for (int i = 0; i < indices.size();) {
            int j = i;
            while (j+1 < indices.size() && indices[j+1] == indices[j]+1
                   && m_glyphs[indices[j+1]].line == m_glyphs[indices[i]].line) ++j;
            const QPolygonF &a = m_glyphs[indices[i]].polygon, &b = m_glyphs[indices[j]].polygon;
            const QPolygonF run{a[0],b[1],b[2],a[3]};
            if (run.boundingRect().intersects(rect)) painter->drawPolygon(run);
            i = j+1;
        }
    };
    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setRenderHint(QPainter::Antialiasing);
    if (!m_highlighted.isEmpty()) {
        painter->setBrush(QColor(0xff,0xc4,0x00,110));
        paintRuns(m_highlighted);
    }
    if (m_anchor >= 0 && m_lead >= 0) {
        QVector<int> range;
        for (int i = qMin(m_anchor,m_lead); i <= qMin(qMax(m_anchor,m_lead),int(m_glyphs.size())-1); ++i) range.append(i);
        painter->setBrush(QColor(0x16,0x83,0xff,80));
        paintRuns(range);
    }
    painter->restore();
}

QString PickerCombo::displayText() const
{
    const QString choice = currentIndex() == 0 && !m_detected.isEmpty() ? m_detected : currentText();
    return m_prefix.isEmpty() ? choice : QStringLiteral("%1 (%2)").arg(m_prefix,choice);
}

QSize PickerCombo::sizeHint() const
{
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const QSize text(fontMetrics().horizontalAdvance(displayText()),fontMetrics().height());
    return style()->sizeFromContents(QStyle::CT_ComboBox,&option,text,this);
}

void PickerCombo::paintEvent(QPaintEvent *)
{
    QStylePainter painter(this);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    option.currentText = displayText();
    painter.drawComplexControl(QStyle::CC_ComboBox,option);
    painter.drawControl(QStyle::CE_ComboBoxLabel,option);
    // The sheet hides the native arrow; draw the card chevron in the drop-down area instead.
    const QRect arrow = style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxArrow,this);
    const QIcon chevron = cardIcon(Icon::CardDown);
    chevron.paint(&painter,QRect(arrow.center()-QPoint(7,7),QSize(14,14)),Qt::AlignCenter,
                  underMouse() ? QIcon::Active : QIcon::Normal);
}

OcrDialog::OcrDialog(const QImage &image, const QRect &captureRect, bool translate)
    : QDialog(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint),
      m_image(image), m_translate(translate) {
    setObjectName(QStringLiteral("ocrComparison"));
    setWindowTitle(m_translate ? QStringLiteral("Finalize 翻译") : QStringLiteral("Finalize OCR"));
    setWindowModality(Qt::NonModal);
    setMinimumSize(640,360);
    setMouseTracking(true);
    QFont interfaceFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    interfaceFont.setPointSizeF(qMax(10.5,interfaceFont.pointSizeF()));
    setFont(interfaceFont);
    // Dark card: zinc chrome, each content on its own near-black rounded block, actions under it.
    const QString surface = QStringLiteral("#18181b"), panel = QStringLiteral("#060607"),
                  hover = QStringLiteral("rgba(39,39,42,204)"), foreground = QStringLiteral("#fcfcfc"),
                  muted = QStringLiteral("#a1a1aa"), outline = QStringLiteral("#27272a"),
                  accent = QStringLiteral("#0485f7"), selected = QStringLiteral("#264f78"),
                  thumb = QStringLiteral("#555555");
    // Natively drawn bits (combo arrows, message box text) follow the palette, not the sheet.
    QPalette dark = palette();
    for (const auto role : {QPalette::Window, QPalette::Base, QPalette::Button, QPalette::AlternateBase})
        dark.setColor(role, QColor(panel));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        dark.setColor(role, QColor(foreground));
    dark.setColor(QPalette::PlaceholderText, QColor(0x80,0x80,0x80));
    dark.setColor(QPalette::Highlight, QColor(selected));
    dark.setColor(QPalette::HighlightedText, QColor(foreground));
    setPalette(dark);
    setStyleSheet(QStringLiteral(
        "QDialog{background:%1;color:%4;border:1px solid %6;}"
        "QLabel{color:%5;background:transparent;} QLabel#ocrTitle{color:%4;}"
        "QFrame#ocrImagePanel,QFrame#ocrTextPanel,QFrame#translateSourceBlock,QFrame#translateResultBlock"
            "{background:%2;border:none;border-radius:12px;}"
        "QWidget#ocrFind{background:transparent;border-bottom:1px solid %6;}"
        // Pickers: rounded fields on the chrome; lists drop below.
        "QComboBox{background:%1;color:%4;border:1px solid %6;border-radius:12px;padding:0 8px 0 12px;"
            "min-height:32px;combobox-popup:0;}"
        "QComboBox:hover{background:%3;} QComboBox:focus{border-color:%7;}"
        "QComboBox::drop-down{width:24px;border:none;background:transparent;} QComboBox::down-arrow{image:none;width:0;height:0;}"
        "QComboBox QAbstractItemView{background:%1;color:%4;border:1px solid %6;outline:none;padding:4px;}"
        "QComboBox QAbstractItemView::item{min-height:24px;padding:0 12px;border-radius:8px;}"
        "QComboBox QAbstractItemView::item:selected{background:%7;color:white;}"
        "QPlainTextEdit{background:%2;color:%4;border:none;padding:8px 12px;selection-background-color:%8;}"
        "QGraphicsView{background:%2;border:none;}"
        "QLineEdit{background:%1;color:%4;border:1px solid %6;border-radius:6px;padding:3px 6px;selection-background-color:%8;}"
        "QLineEdit:focus{border-color:%7;}"
        "QPushButton{background:%6;color:%4;border:1px solid transparent;border-radius:8px;padding:4px 14px;min-width:56px;}"
        "QPushButton:hover{background:#3f3f46;} QPushButton:focus{border-color:%7;}"
        // Stale translation: a grey haze over the pane, one rounded button in the middle.
        "QWidget#retranslateMask{background:rgba(128,128,128,120);border-radius:12px;}"
        "QPushButton#retranslateButton{min-width:150px;max-width:150px;min-height:30px;max-height:30px;padding:0;"
            "border:none;border-radius:12px;background:%1;color:%4;}"
        "QPushButton#retranslateButton:hover,QPushButton#retranslateButton:pressed{background:%7;color:white;}"
        // Icon buttons carry no fill: the glyph itself turns accent on hover (cardIcon); Tab focus keeps an outline.
        "QToolButton{background:transparent;color:%4;border:1px solid transparent;border-radius:6px;padding:3px;}"
        "QToolButton:focus{border-color:%7;}"
        "QToolButton::menu-indicator{image:none;width:0;}"
        "QToolButton#ocrReplace,QToolButton#ocrReplaceAll{background:%6;padding:0 10px;}"
        "QToolButton#ocrReplace:hover,QToolButton#ocrReplaceAll:hover{background:#3f3f46;}"
        // Splitters: a hairline in the gap between two blocks, accent while dragged or hovered.
        "QSplitter::handle{background:transparent;}"
        "QSplitter::handle:horizontal{border-left:1px solid %6;margin:0 6px;}"
        "QSplitter::handle:vertical{border-top:1px solid %6;margin:6px 0;}"
        "QSplitter::handle:hover,QSplitter::handle:pressed{border-color:%7;}"
        "QMenu{background:%1;color:%4;border:1px solid %6;padding:4px;}"
        "QMenu::item{padding:6px 20px;border-radius:6px;} QMenu::item:selected{background:%7;color:white;}"
        "QMenu::item:disabled{color:#52525b;} QMenu::separator{height:1px;background:%6;margin:4px 8px;}"
        "QScrollBar:vertical{background:transparent;width:8px;margin:0;} QScrollBar:horizontal{background:transparent;height:8px;margin:0;}"
        "QScrollBar::handle{background:%9;border-radius:4px;min-width:16px;min-height:16px;}"
        "QScrollBar::handle:hover{background:#797979;}"
        "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;}"
        "QScrollBar::add-page,QScrollBar::sub-page{background:transparent;}"
        "QToolTip{background:%1;color:%4;border:1px solid %6;padding:6px;}"
    ).arg(surface,panel,hover,foreground,muted,outline,accent,selected,thumb));
    auto *layout = new QVBoxLayout(this);
    // Child-free padding along every edge: the card itself sees the cursor there and resizes.
    layout->setContentsMargins(ResizeBand,ResizeBand,ResizeBand,ResizeBand);
    layout->setSpacing(0);
    const auto iconButton = [this](Icon icon,const QString &name,const QString &tip) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        // Rasterized on its own 16px grid, never rescaled; hover tints the glyph instead of filling the button.
        button->setIcon(cardIcon(icon));
        button->setIconSize(QSize(16,16));
        button->setAutoRaise(true);              // The style only reports hover (QIcon::Active) for auto-raise buttons.
        button->setFixedSize(28,28);
        // Clicks must not take focus (no outline); Tab still reaches and outlines the button.
        button->setFocusPolicy(Qt::TabFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        return button;
    };
    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("ocrHeader"));
    m_header = header;
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0,0,0,0);
    headerLayout->setSpacing(6);
    auto *title = new QLabel(windowTitle());
    title->setObjectName(QStringLiteral("ocrTitle"));
    QFont titleFont = interfaceFont;
    titleFont.setPointSizeF(qMax(12.0,interfaceFont.pointSizeF()));
    titleFont.setWeight(QFont::Medium);
    title->setFont(titleFont);
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    auto *pin = iconButton(Icon::CardPin,QStringLiteral("ocrPin"),QStringLiteral("置顶 OCR 卡片"));
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
    auto *minimize = iconButton(Icon::CardMinimize,QStringLiteral("ocrMinimize"),QStringLiteral("最小化"));
    connect(minimize,&QToolButton::clicked,this,&QWidget::showMinimized);
    headerLayout->addWidget(minimize);
    m_maximize = iconButton(Icon::CardMaximize,QStringLiteral("ocrMaximize"),QStringLiteral("最大化"));
    connect(m_maximize,&QToolButton::clicked,this,&OcrDialog::toggleMaximize);
    headerLayout->addWidget(m_maximize);
    auto *close = iconButton(Icon::CardClose,QStringLiteral("ocrClose"),QStringLiteral("关闭 (Esc)"));
    connect(close,&QToolButton::clicked,this,&QDialog::reject);
    headerLayout->addWidget(close);
    header->setFixedHeight(28);
    layout->addWidget(header);
    // Translate card: one toolbar under the title for the pickers and the view controls.
    QHBoxLayout *tools = nullptr;
    if (m_translate) {
        auto *toolbar = new QWidget;
        toolbar->setObjectName(QStringLiteral("translateToolbar"));
        tools = new QHBoxLayout(toolbar);
        tools->setContentsMargins(0,8,0,8);
        tools->setSpacing(8);
        layout->addWidget(toolbar);
    }
    m_split = new QSplitter(Qt::Horizontal);
    m_split->setObjectName(QStringLiteral("ocrSplit"));
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(13);             // 6px gap, 1px hairline, 6px gap.
    // Each content sits on a rounded block; its actions run along the block's bottom edge.
    const auto block = [](const QString &name, int margin) {
        auto *frame = new QFrame;
        frame->setObjectName(name);
        auto *column = new QVBoxLayout(frame);
        column->setContentsMargins(margin,margin,margin,8);
        column->setSpacing(6);
        return std::pair<QFrame *,QVBoxLayout *>(frame,column);
    };
    const auto actionRow = [](const QString &name) {
        auto *bar = new QWidget;
        bar->setObjectName(name);
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(0,0,0,0);
        row->setSpacing(4);
        return std::pair<QWidget *,QHBoxLayout *>(bar,row);
    };
    QVBoxLayout *imageColumn;
    std::tie(m_imagePanel,imageColumn) = block(QStringLiteral("ocrImagePanel"),8);
    auto *imageView = new OcrImageView(image);
    imageView->setFocusPolicy(Qt::StrongFocus);
    imageView->setToolTip(QStringLiteral("滚轮缩放 · 拖动查看 · 双击适应"));
    imageColumn->addWidget(imageView,1);
    QWidget *imageBar;
    QHBoxLayout *imageActions;
    std::tie(imageBar,imageActions) = actionRow(QStringLiteral("ocrImageBar"));
    auto *copyImage = iconButton(Icon::CardCopy,QStringLiteral("ocrCopyImage"),QStringLiteral("复制图片"));
    connect(copyImage,&QToolButton::clicked,this,[this] {
        m_status->setText(copyImageToClipboard(m_image) ? QStringLiteral("已复制图片")
                                                        : QStringLiteral("复制失败，剪贴板可能被占用，请重试"));
    });
    // Ten percentage points a click.
    auto *zoomIn = iconButton(Icon::CardZoomIn,QStringLiteral("ocrZoomIn"),QStringLiteral("放大"));
    connect(zoomIn,&QToolButton::clicked,imageView,[imageView] { imageView->zoomTo(imageView->percent()+10); });
    auto *zoomOut = iconButton(Icon::CardZoomOut,QStringLiteral("ocrZoomOut"),QStringLiteral("缩小"));
    connect(zoomOut,&QToolButton::clicked,imageView,[imageView] { imageView->zoomTo(imageView->percent()-10); });
    auto *zoomLevel = new QLabel;
    zoomLevel->setObjectName(QStringLiteral("ocrZoomLevel"));
    zoomLevel->setAlignment(Qt::AlignCenter);
    zoomLevel->setMinimumWidth(48);
    // One button toggles: fitted shows 1:1, zoomed shows fit; the glyph names the click's result.
    auto *fit = iconButton(Icon::CardActual,QStringLiteral("ocrFit"),QString());
    connect(fit,&QToolButton::clicked,imageView,[imageView] { imageView->fitted() ? imageView->zoomTo(100) : imageView->fit(); });
    imageView->zoomed = [imageView,zoomLevel,fit] {
        zoomLevel->setText(QStringLiteral("%1%").arg(imageView->percent()));
        const bool fitted = imageView->fitted();
        fit->setIcon(cardIcon(fitted ? Icon::CardActual : Icon::CardFit));
        const QString tip = fitted ? QStringLiteral("原始大小 (1:1)") : QStringLiteral("适应窗口");
        fit->setToolTip(tip); fit->setAccessibleName(tip);
    };
    imageView->zoomed();
    m_imageView = imageView;
    imageView->copied = [this](bool ok) {
        m_status->setText(ok ? QStringLiteral("已复制选中文字") : QStringLiteral("复制失败，剪贴板可能被占用，请重试"));
    };
    for (QWidget *widget : {static_cast<QWidget *>(copyImage),static_cast<QWidget *>(zoomIn),
                            static_cast<QWidget *>(zoomOut),static_cast<QWidget *>(zoomLevel),static_cast<QWidget *>(fit)})
        imageActions->addWidget(widget);
    imageActions->addStretch();
    imageColumn->addWidget(imageBar);
    m_imagePanel->setMinimumSize(180,120);
    m_split->addWidget(m_imagePanel);
    QWidget *textBar;
    QHBoxLayout *textActions;
    std::tie(textBar,textActions) = actionRow(m_translate ? QStringLiteral("translateResultBar") : QStringLiteral("ocrTextBar"));
    m_layout = iconButton(Icon::CardSplit,QStringLiteral("ocrLayout"),QStringLiteral("切换为上下分屏"));
    connect(m_layout,&QToolButton::clicked,this,[this] {
        if (m_mode != 1) { setStacked(!m_rows); return; }
        // Without the image the button splits the two texts instead: original first, translation after.
        m_textRows = !m_textRows;
        m_texts->setOrientation(m_textRows ? Qt::Vertical : Qt::Horizontal);
        const int span = m_textRows ? m_texts->height() : m_texts->width();
        m_texts->setSizes({span*2/5,span*3/5});
        syncLayoutButton();
    });
    m_format = iconButton(Icon::CardFormat,QStringLiteral("ocrFormat"),QStringLiteral("排版"));
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
    QToolButton *search = nullptr;
    if (m_translate) {
        // One row for the whole card: language › target and the engine on the left, actions on the right.
        // Translate card: no format or search buttons; find lives in each pane's context menu and Ctrl+F/H.
        m_format->hide();
        const auto combo = [&](const QString &name, const QString &tip) {
            auto *box = new PickerCombo;
            box->setObjectName(name);
            box->setToolTip(tip);
            box->setAccessibleName(tip);
            box->setFocusPolicy(Qt::TabFocus);
            box->setCursor(Qt::PointingHandCursor);
            box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
            box->setMaxVisibleItems(18);
            return box;
        };
        m_source = combo(QStringLiteral("translateSource"),QStringLiteral("原文语言"));
        m_target = combo(QStringLiteral("translateTarget"),QStringLiteral("译文语言"));
        m_provider = combo(QStringLiteral("translateProvider"),QStringLiteral("翻译引擎：自动会在失败时换下一家"));
        const std::pair<const char16_t *,TranslateGateway::Provider> providers[] = {
            {u"自动选择",TranslateGateway::Provider::Auto},{u"Edge",TranslateGateway::Provider::Edge},
            {u"Google",TranslateGateway::Provider::Google},{u"有道",TranslateGateway::Provider::Youdao},
            {u"搜狗",TranslateGateway::Provider::Sogou},{u"腾讯交互翻译",TranslateGateway::Provider::Transmart},
            {u"火山",TranslateGateway::Provider::Volcengine},{u"DeepL",TranslateGateway::Provider::Deepl}};
        for (const auto &[label,provider] : providers)
            m_provider->addItem(QString::fromUtf16(label),int(provider));
        // Every card starts on Auto (fallback across engines); a chosen engine lasts for this card only.
        const QSettings settings(QStringLiteral("pinshot"),QStringLiteral("pinshot"));
        populateLanguages(settings.value(QStringLiteral("Translate/source")).toString(),
                          settings.value(QStringLiteral("Translate/target"),QStringLiteral("zh")).toString());
        syncPickers();
        auto *arrow = new QLabel;
        arrow->setPixmap(cardIcon(Icon::CardRight).pixmap(QSize(16,16),devicePixelRatioF()));
        arrow->setAccessibleName(QStringLiteral("翻译为"));
        connect(m_provider,&QComboBox::currentIndexChanged,this,[this] {
            populateLanguages(m_source->currentData().toString(),m_target->currentData().toString());
            syncPickers();
            saveTranslateChoice();
            translateLines();
        });
        connect(m_source,&QComboBox::activated,this,[this] { syncPickers(); saveTranslateChoice(); translateLines(); });
        connect(m_target,&QComboBox::activated,this,[this] { saveTranslateChoice(); translateLines(); });
        tools->addWidget(m_source);
        tools->addWidget(arrow);
        tools->addWidget(m_target);
        tools->addWidget(m_provider);
        tools->addStretch();
        tools->addWidget(m_layout);
        m_view = iconButton(Icon::View,QStringLiteral("translateView"),QStringLiteral("显示内容"));
        auto *viewMenu = new QMenu(m_view);
        auto *viewGroup = new QActionGroup(viewMenu);
        const char16_t *const views[] = {u"图片、原文和译文",u"原文和译文",u"仅译文"};
        for (int mode = 0; mode < 3; ++mode) {
            QAction *action = viewMenu->addAction(QString::fromUtf16(views[mode]));
            action->setCheckable(true);
            action->setData(mode);
            viewGroup->addAction(action);
            connect(action,&QAction::triggered,this,[this,mode] {
                setDisplay(mode);
                QSettings(QStringLiteral("pinshot"),QStringLiteral("pinshot")).setValue(QStringLiteral("Translate/view"),mode);
            });
        }
        m_view->setMenu(viewMenu);
        m_view->setPopupMode(QToolButton::InstantPopup);
        tools->addWidget(m_view);
        m_again = iconButton(Icon::CardTranslate,QStringLiteral("translateAgain"),QStringLiteral("重新翻译 (Ctrl+Enter)"));
        m_again->setAccessibleName(QStringLiteral("重新翻译"));
        connect(m_again,&QToolButton::clicked,this,[this] { translateLines(); });
        tools->addWidget(m_again);
    }
    // Translate card: recognize the capture again, then translate it with the chosen engine.
    m_retry = iconButton(Icon::CardReload,QStringLiteral("ocrRetry"),
                         m_translate ? QStringLiteral("重新识别并翻译") : QStringLiteral("重新识别"));
    connect(m_retry,&QToolButton::clicked,this,&OcrDialog::start);
    m_copy = iconButton(Icon::CardCopy,QStringLiteral("ocrCopy"),m_translate ? QStringLiteral("复制译文") : QStringLiteral("复制全部"));
    m_copy->setEnabled(false);
    const auto characters = [] {
        auto *label = new QLabel;
        label->setObjectName(QStringLiteral("ocrCharacterCount"));
        label->setMinimumWidth(40);
        label->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);
        return label;
    };
    // Bottom row of the (translation) text block: copy, count, then the card's other actions.
    textActions->addWidget(m_copy);
    QLabel *textCount = characters();
    textActions->addWidget(textCount);
    if (m_translate) {
        tools->insertWidget(tools->indexOf(m_again)+1,m_retry);
    } else {
        textActions->addWidget(m_layout);
        textActions->addWidget(m_format);
        search = iconButton(Icon::CardSearch,QStringLiteral("ocrSearch"),QStringLiteral("查找 (Ctrl+F)"));
        connect(search,&QToolButton::clicked,this,[this] { m_find->isVisible() ? hideFind() : showFind(false); });
        textActions->addWidget(search);
        textActions->addWidget(m_retry);
    }
    textActions->addStretch();
    auto *body = new QWidget;
    auto *textLayout = new QVBoxLayout(body);
    textLayout->setContentsMargins(0,0,0,0);
    textLayout->setSpacing(0);
    // Find / replace strip under the bar, shown by the search button, Ctrl+F or Ctrl+H.
    m_find = new QWidget;
    m_find->setObjectName(QStringLiteral("ocrFind"));
    m_find->setAttribute(Qt::WA_StyledBackground);
    auto *findLayout = new QGridLayout(m_find);
    findLayout->setContentsMargins(8,4,8,6);
    findLayout->setHorizontalSpacing(4); findLayout->setVerticalSpacing(4);
    const auto smallButton = [&](Icon icon,const QString &name,const QString &tip) {
        return iconButton(icon,name,tip);
    };
    m_replaceToggle = smallButton(Icon::CardRight,QStringLiteral("ocrReplaceToggle"),QStringLiteral("切换替换"));
    connect(m_replaceToggle,&QToolButton::clicked,this,[this] { showFind(!m_replaceRow->isVisible(),m_findTarget); });
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
    auto *previous = smallButton(Icon::CardUp,QStringLiteral("ocrFindPrevious"),QStringLiteral("上一个 (Shift+Enter)"));
    connect(previous,&QToolButton::clicked,this,[this] { findStep(-1); });
    findLayout->addWidget(previous,0,3);
    auto *next = smallButton(Icon::CardDown,QStringLiteral("ocrFindNext"),QStringLiteral("下一个 (Enter)"));
    connect(next,&QToolButton::clicked,this,[this] { findStep(1); });
    findLayout->addWidget(next,0,4);
    auto *closeFind = smallButton(Icon::CardClose,QStringLiteral("ocrFindClose"),QStringLiteral("关闭查找 (Esc)"));
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
    QFont font = interfaceFont;
    font.setPointSizeF(qMax(12.0,interfaceFont.pointSizeF()));
    QWidget *sourceBox = nullptr;
    if (m_translate) {
        // Recognized text sits above the translation, quieter, and stays editable:
        // fixing an OCR slip re-translates after a short pause, Ctrl+Enter at once.
        m_sourcePane = new QPlainTextEdit;
        m_sourcePane->setObjectName(QStringLiteral("translateSourceText"));
        m_sourcePane->setAccessibleName(QStringLiteral("识别原文，可修改后重新翻译"));
        m_sourcePane->setPlaceholderText(QStringLiteral("识别中…"));
        m_sourcePane->setReadOnly(true);
        QFont sourceFont = font;
        sourceFont.setPointSizeF(qMax(10.5,font.pointSizeF()-1.5));
        m_sourcePane->setFont(sourceFont);
        // Editing the original never calls the engine by itself: the stale translation is hazed
        // over with one button, and only a click (or Ctrl+Enter) translates again.
        connect(m_sourcePane,&QPlainTextEdit::textChanged,this,&OcrDialog::updateMask);
        auto *now = new QShortcut(QKeySequence(Qt::CTRL|Qt::Key_Return),m_sourcePane,nullptr,nullptr,Qt::WidgetShortcut);
        connect(now,&QShortcut::activated,this,[this] { translateLines(); });
        sourceBox = m_sourcePane;
    }
    m_text = new QPlainTextEdit;
    m_text->setObjectName(QStringLiteral("ocrText"));
    m_text->setAccessibleName(m_translate ? QStringLiteral("译文") : QStringLiteral("OCR 识别文本"));
    m_text->setReadOnly(true);
    m_findTarget = m_text;
    // Selecting recognized text marks the same characters on the capture.
    if (!m_translate)
        connect(m_text,&QPlainTextEdit::selectionChanged,this,[this] {
            const QTextCursor cursor = m_text->textCursor();
            m_imageView->highlightText(cursor.hasSelection() ? cursor.selectedText() : QString());
        });
    if (m_translate) font.setPointSizeF(font.pointSizeF()+1);   // The translation is what the card is for.
    m_text->setFont(font);
    m_busy = new QLabel(QStringLiteral("· · ·"));
    m_busy->setObjectName(QStringLiteral("ocrBusy"));
    QFont busyFont = font; busyFont.setPointSizeF(font.pointSizeF()*1.5);
    m_busy->setFont(m_translate ? font : busyFont);   // The translate card says what it is doing in words.
    m_busy->setAlignment(Qt::AlignCenter);
    m_busy->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_busy->hide();
    (new QVBoxLayout(m_text->viewport()))->addWidget(m_busy);
    m_status = new QLabel(QStringLiteral("识别中 · · ·"));
    m_status->setObjectName(QStringLiteral("ocrStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setFocusPolicy(Qt::ClickFocus);
    QFont captionFont = interfaceFont;
    captionFont.setPointSizeF(qMax(9.0,interfaceFont.pointSizeF()/1.125/1.125));
    m_status->setFont(captionFont);
    m_status->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    m_status->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);   // Long advice never widens the card.
    textActions->addWidget(m_status,1);
    const auto countOf = [](QLabel *label, QPlainTextEdit *pane) {
        // Code points, so an emoji or a rare CJK character counts once. The document signal still
        // fires when recognized text is filled in with the pane's own signals blocked.
        connect(pane->document(),&QTextDocument::contentsChanged,label,[label,pane] {
            label->setText(QString::number(pane->toPlainText().toUcs4().size()));
        });
    };
    countOf(textCount,m_text);
    QFrame *textBlock;
    QVBoxLayout *textColumn;
    std::tie(textBlock,textColumn) = block(m_translate ? QStringLiteral("translateResultBlock") : QStringLiteral("ocrTextPanel"),4);
    textColumn->addWidget(m_find);
    textColumn->addWidget(m_text,1);
    textColumn->addWidget(textBar);
    QToolButton *sourceCopy = nullptr;
    if (sourceBox) {
        // Original and translation each sit on their own block with their own bottom row.
        QVBoxLayout *sourceColumn;
        QFrame *sourceBlock;
        std::tie(sourceBlock,sourceColumn) = block(QStringLiteral("translateSourceBlock"),4);
        m_sourceBox = sourceBlock;
        m_sourceLayout = sourceColumn;
        m_resultLayout = textColumn;
        sourceColumn->addWidget(sourceBox,1);
        QWidget *sourceBar;
        QHBoxLayout *sourceActions;
        std::tie(sourceBar,sourceActions) = actionRow(QStringLiteral("translateSourceBar"));
        sourceCopy = iconButton(Icon::CardCopy,QStringLiteral("translateSourceCopy"),QStringLiteral("复制原文"));
        sourceCopy->setEnabled(false);
        connect(sourceCopy,&QToolButton::clicked,this,[this] {
            m_status->setText(copyTextToClipboard(m_sourcePane->toPlainText()) ? QStringLiteral("已复制原文")
                                                                               : QStringLiteral("复制失败，剪贴板可能被占用，请重试"));
        });
        connect(m_sourcePane->document(),&QTextDocument::contentsChanged,sourceCopy,[this,sourceCopy] {
            sourceCopy->setEnabled(!m_sourcePane->toPlainText().isEmpty());
        });
        QLabel *sourceCount = characters();
        countOf(sourceCount,m_sourcePane);
        sourceActions->addWidget(sourceCopy);
        sourceActions->addWidget(sourceCount);
        sourceActions->addStretch();
        sourceColumn->addWidget(sourceBar);
        m_texts = new QSplitter(Qt::Vertical);
        auto *texts = m_texts;
        texts->setObjectName(QStringLiteral("translateSplit"));
        texts->setChildrenCollapsible(false);
        texts->setHandleWidth(13);
        texts->addWidget(m_sourceBox);
        texts->addWidget(textBlock);
        m_sourceBox->setMinimumSize(120,80);
        texts->setStretchFactor(0,2); texts->setStretchFactor(1,3);
        textLayout->addWidget(texts,1);
        // Haze over the translation pane once the original differs from what it translated.
        m_mask = new QWidget(m_text);
        m_mask->setObjectName(QStringLiteral("retranslateMask"));
        m_mask->setAttribute(Qt::WA_StyledBackground);
        auto *maskLayout = new QVBoxLayout(m_mask);
        auto *maskButton = new QPushButton(QStringLiteral("重新翻译"));
        maskButton->setObjectName(QStringLiteral("retranslateButton"));
        maskButton->setFocusPolicy(Qt::NoFocus);
        maskButton->setCursor(Qt::PointingHandCursor);
        maskButton->setToolTip(QStringLiteral("原文已修改，点击重新翻译 (Ctrl+Enter)"));
        connect(maskButton,&QPushButton::clicked,this,[this] { translateLines(); });
        maskLayout->addWidget(maskButton,0,Qt::AlignCenter);
        m_mask->hide();
        m_text->installEventFilter(this);   // Keeps the haze covering the pane as it resizes.
        // Find / replace from either pane: its context menu, Ctrl+F or Ctrl+H, scoped to that pane.
        for (QPlainTextEdit *pane : {m_sourcePane,m_text}) {
            pane->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(pane,&QWidget::customContextMenuRequested,this,[this,pane](const QPoint &at) {
                QMenu *menu = pane->createStandardContextMenu(at);
                menu->setAttribute(Qt::WA_DeleteOnClose);
                menu->addSeparator();
                QAction *find = menu->addAction(QStringLiteral("查找"),this,[this,pane] { showFind(false,pane); });
                find->setShortcut(QKeySequence::Find);
                QAction *replace = menu->addAction(QStringLiteral("替换"),this,[this,pane] { showFind(true,pane); });
                replace->setShortcut(QKeySequence(Qt::CTRL|Qt::Key_H));
                replace->setEnabled(!pane->isReadOnly());
                menu->popup(pane->viewport()->mapToGlobal(at));
            });
        }
    } else {
        textLayout->addWidget(textBlock,1);
    }
    m_split->addWidget(body);
    m_split->setStretchFactor(0,1); m_split->setStretchFactor(1,1);
    auto *content = new QWidget;
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0,m_translate ? 0 : 8,0,0);
    contentLayout->addWidget(m_split);
    layout->addWidget(content,1);
    // Tab follows the visual order: window actions, toolbar, image block, then text block(s).
    QList<QWidget *> order{pin,minimize,m_maximize,close};
    if (m_translate) order << m_source << m_target << m_provider << m_layout << m_view << m_again << m_retry;
    order << imageView << copyImage << zoomIn << zoomOut << fit;
    if (m_translate) order << m_sourcePane << sourceCopy << m_text << m_copy;
    else order << m_text << m_copy << m_layout << m_format << search << m_retry;
    for (int i = 1; i < order.size(); ++i) setTabOrder(order[i-1],order[i]);
    // Ctrl+F / Ctrl+H search the focused pane (the source pane in the translate card), else the main text.
    const auto findHere = [this](bool replace) {
        // Focus inside the strip keeps searching the pane it already serves.
        QPlainTextEdit *target = m_find->isAncestorOf(QApplication::focusWidget()) ? m_findTarget
            : m_sourcePane && m_sourcePane->hasFocus() ? m_sourcePane : m_text;
        showFind(replace,target);
    };
    connect(new QShortcut(QKeySequence::Find,this),&QShortcut::activated,this,[findHere] { findHere(false); });
    connect(new QShortcut(QKeySequence(Qt::CTRL|Qt::Key_H),this),&QShortcut::activated,this,[findHere] { findHere(true); });
    for (QPlainTextEdit *pane : {m_text,m_sourcePane})
        if (pane) connect(pane,&QPlainTextEdit::textChanged,this,[this,pane] {
            if (m_find->isVisible() && pane==m_findTarget) highlightMatches();
        });
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
    if (m_translate)
        setDisplay(QSettings(QStringLiteral("pinshot"),QStringLiteral("pinshot")).value(QStringLiteral("Translate/view"),0).toInt());
    connect(&m_watcher, &QFutureWatcher<OcrResult>::finished, this, [this] {
        const OcrResult result = m_watcher.result();
        m_busy->hide();
        // Translate mode re-enables retry when the gateway answers, unless recognition stops here.
        if (!m_translate) m_retry->setEnabled(true);
        else if (!result.error.isEmpty() || result.lines.isEmpty() || m_cancel->load()) setActionsEnabled(true);
        if (m_cancel->load()) return;
        if (result.error.isEmpty()) m_imageView->setTextLines(result.lines);
        if (m_translate && (!result.error.isEmpty() || result.lines.isEmpty())) {
            // Recognition stopped: the user can still type the original and translate it.
            m_sourcePane->setReadOnly(false);
            m_sourcePane->setPlaceholderText(QStringLiteral("未识别到文字，可在此输入原文"));
            m_text->setReadOnly(false);
            m_text->setPlaceholderText(QStringLiteral("在上方输入原文后点击重新翻译"));
            m_status->setText(result.error.isEmpty() ? QStringLiteral("未识别到文字") : result.error);
            return;
        }
        if (!result.error.isEmpty()) {
            // A failed retry brings the previous text back instead of leaving the pane empty.
            m_text->setPlainText(m_previous);
            m_text->document()->setModified(m_previousModified);
            m_status->setText(result.error);
            m_text->setReadOnly(m_text->toPlainText().isEmpty());
            m_text->setPlaceholderText(QStringLiteral("识别未完成，可点击重新识别"));
            return;
        }
        if (m_translate) {
            // The pane stays busy: recognition lines now go to the gateway.
            translateResult(result);
            return;
        }
        m_text->setReadOnly(false);
        m_raw = result.text();
        m_text->setPlainText(m_raw);
        m_autoFormat->setChecked(true);
        m_text->setPlaceholderText(QStringLiteral("未识别到文字，可在此输入文本。"));
        double confidence=0;
        for (const OcrLine &line:result.lines) confidence+=line.score;
        m_status->setText(result.lines.isEmpty() ? QStringLiteral("未识别到文字")
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

QRect OcrDialog::centered(const QRect &anchor, const QRect &available) {
    // Default card size, centred on the anchor and slid inside the work area.
    const QSize size = placement(anchor,available).size();
    const QPoint corner = anchor.center() - QPoint(size.width()/2,size.height()/2);
    return QRect(QPoint(qBound(available.left(),corner.x(),available.right()-size.width()+1),
                        qBound(available.top(),corner.y(),available.bottom()-size.height()+1)),size);
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
    const bool userEdit = m_text->document()->isModified() || (m_sourcePane && m_sourcePane->document()->isModified());
    if (userEdit && QMessageBox::question(this,QStringLiteral("重新识别"),
            QStringLiteral("重新识别会替换已修改的文本，是否继续？"),
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
    if (m_retry->hasFocus() || (m_again && m_again->hasFocus())) m_text->setFocus();
    m_previous = m_text->toPlainText();
    m_previousModified = m_text->document()->isModified();
    m_text->clear();
    m_text->setReadOnly(true);
    if (m_translate) {
        m_sourcePane->setReadOnly(true);
        {   const QSignalBlocker blocker(m_sourcePane);   // Not a user edit.
            m_sourcePane->clear();
        }
        m_translatedSource.clear();
        setActionsEnabled(false);
    } else {
        m_retry->setEnabled(false);
    }
    if (m_translate) {
        m_sourcePane->setPlaceholderText(QStringLiteral("识别中…"));
        m_text->setPlaceholderText(QString());
    }
    m_busy->setText(m_translate ? QStringLiteral("识别中…") : QStringLiteral("· · ·"));
    m_busy->show();
    m_status->setText(m_translate ? QString() : QStringLiteral("识别中 · · ·"));
    m_watcher.setFuture(QtConcurrent::run([&engine,image,cancel] {
        const OcrResult result = recognizeChunked(image,cancel);
        engine.busy.store(false);
        return result;
    }));
}

void OcrDialog::translateResult(const OcrResult &result)
{
    // Recognized text goes into the editable source pane; typing there re-translates.
    QString joined;
    for (const OcrLine &line : result.lines) {
        if (!joined.isEmpty()) joined += QLatin1Char('\n');
        joined += line.text;
    }
    {   const QSignalBlocker blocker(m_sourcePane);
        m_sourcePane->setPlainText(joined);
        m_sourcePane->document()->setModified(false);   // Recognized text is not a user edit.
    }
    m_sourcePane->setReadOnly(false);
    translateLines();
}

void OcrDialog::translateLines()
{
    if (m_closed || m_watcher.isRunning()) return;   // Recognition finishing will translate.
    if (!m_gateway) m_gateway = new TranslateGateway(this);   // Child: aborted with the card.
    m_gateway->abort();          // A new choice supersedes the running request.
    const QStringList lines = m_sourcePane->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        m_text->clear();
        m_text->setReadOnly(false);
        m_text->setPlaceholderText(QStringLiteral("在上方输入原文后点击重新翻译"));
        m_status->clear();
        m_busy->hide();
        m_translatedSource = m_sourcePane->toPlainText();
        setActionsEnabled(true);
        return;
    }
    // Re-translating replaces the pane; a failure brings this text back.
    m_previous = m_text->toPlainText();
    m_previousModified = m_text->document()->isModified();
    if (m_retry->hasFocus() || m_again->hasFocus()) m_text->setFocus();
    const QString requested = m_sourcePane->toPlainText();
    setActionsEnabled(false);
    m_text->clear();
    m_text->setReadOnly(true);
    m_text->setPlaceholderText(QString());
    m_busy->setText(QStringLiteral("翻译中…"));
    m_busy->show();
    m_status->clear();
    const auto provider = TranslateGateway::Provider(m_provider->currentData().toInt());
    // Auto walks the engines inside the gateway: one engine failing never reaches the card.
    m_gateway->translate(lines, m_source->currentData().toString(), m_target->currentData().toString(),
                         provider, [this, provider, requested](const TranslateResult &t) {
        if (m_closed || t.code == TranslateResult::Error::Cancelled) return;
        m_busy->hide();
        if (t.code == TranslateResult::Error::None) m_translatedSource = requested;
        setActionsEnabled(true);   // Also brings the haze back if this run failed or the original changed meanwhile.
        if (t.code != TranslateResult::Error::None) {
            // Keep the previous translation; explain in plain words, never the raw engine error.
            m_text->setPlainText(m_previous);
            m_text->document()->setModified(m_previousModified);
            m_text->setReadOnly(m_text->toPlainText().isEmpty());
            QString message;
            if (t.code == TranslateResult::Error::Unsupported)
                message = QStringLiteral("暂不支持这组语言，请更换原文或译文语言");
            else if (provider == TranslateGateway::Provider::Auto)
                message = QStringLiteral("翻译服务暂时不可用，请检查网络后重试");
            else
                message = QStringLiteral("%1暂时无法翻译，请稍后重试或切换为其他翻译引擎").arg(m_provider->currentText());
            m_text->setPlaceholderText(message);
            m_status->setText(message);
            return;
        }
        m_text->setReadOnly(false);
        m_raw = t.lines.join(QLatin1Char('\n'));
        m_text->setPlainText(m_raw);
        m_autoFormat->setChecked(true);
        // Automatic pickers name what this run actually used: "语言检测 (韩语)", "翻译引擎 (Edge)".
        bool known = false;
        const int engine = m_provider->findData(int(TranslateGateway::providerFromName(t.provider,&known)));
        const QString engineName = known && engine >= 0 ? m_provider->itemText(engine) : t.provider;
        if (m_source->currentIndex() == 0) {
            const int at = t.detectedSource.isEmpty() ? -1 : m_source->findData(t.detectedSource);
            m_source->setDetected(at > 0 ? m_source->itemText(at) : QString());
        }
        if (m_provider->currentIndex() == 0) m_provider->setDetected(engineName);
        m_status->setText(QStringLiteral("翻译服务：%1 · %2秒").arg(engineName).arg(t.elapsed / 1000.0, 0, 'f', 2));
        // Someone typing in the source keeps the caret there; otherwise the result takes focus.
        if (!m_sourcePane->hasFocus()) m_text->setFocus();
    });
}

void OcrDialog::populateLanguages(const QString &source, const QString &target)
{
    // Lists follow the chosen engine's language matrix; a choice it lacks falls back.
    const auto provider = TranslateGateway::Provider(m_provider->currentData().toInt());
    const QSignalBlocker blockSource(m_source), blockTarget(m_target);
    m_source->clear();
    m_source->addItem(QStringLiteral("自动检测"),QString());
    for (const TranslateLanguage &language : TranslateGateway::supportedSourceLanguages(provider))
        m_source->addItem(language.name,language.code);
    const int s = m_source->findData(source);
    m_source->setCurrentIndex(s < 0 ? 0 : s);
    m_target->clear();
    for (const TranslateLanguage &language : TranslateGateway::supportedTargetLanguages(provider))
        m_target->addItem(language.name,language.code);
    int t = m_target->findData(target);
    if (t < 0) t = m_target->findData(QStringLiteral("zh"));
    m_target->setCurrentIndex(t < 0 ? 0 : t);
}

void OcrDialog::saveTranslateChoice() const
{
    QSettings settings(QStringLiteral("pinshot"),QStringLiteral("pinshot"));
    settings.remove(QStringLiteral("Translate/provider"));   // Engine is per card; drop the old stored choice.
    settings.setValue(QStringLiteral("Translate/source"),m_source->currentData().toString());
    settings.setValue(QStringLiteral("Translate/target"),m_target->currentData().toString());
}

void OcrDialog::setDisplay(int mode)
{
    // 0: image, original and translation; 1: original and translation; 2: translation only.
    m_mode = mode = qBound(0,mode,2);
    m_imagePanel->setVisible(mode == 0);
    m_layout->setVisible(mode < 2);            // Splits image|texts in mode 0, original|translation in mode 1.
    m_sourceBox->setVisible(mode < 2);
    if (mode == 2 && m_findTarget == m_sourcePane && m_find->isVisible()) hideFind();
    for (QAction *action : m_view->menu()->actions())
        action->setChecked(action->data().toInt() == mode);
    syncLayoutButton();
}

void OcrDialog::syncPickers()
{
    // A hand-picked item drops what an earlier automatic run detected.
    m_source->setPrefix(QStringLiteral("语言检测"));
    m_provider->setPrefix(QStringLiteral("翻译引擎"));
    for (PickerCombo *box : {m_source,m_provider})
        if (box->currentIndex() != 0) box->setDetected(QString());
}

void OcrDialog::syncLayoutButton()
{
    // The split button always offers the other direction of whatever split it currently drives.
    const bool rows = m_mode == 1 ? m_textRows : m_rows;
    m_layout->setIcon(cardIcon(rows ? Icon::CardColumns : Icon::CardSplit));   // Shows the split it switches to.
    const QString tip = rows ? QStringLiteral("切换为左右分屏") : QStringLiteral("切换为上下分屏");
    m_layout->setToolTip(tip); m_layout->setAccessibleName(tip);
}

void OcrDialog::setActionsEnabled(bool enabled)
{
    m_retry->setEnabled(enabled);
    if (m_again) m_again->setEnabled(enabled);
    updateMask();
}

void OcrDialog::updateMask()
{
    // Shown only between runs, while the original differs from the text the translation came from.
    if (!m_mask) return;
    const QString source = m_sourcePane->toPlainText();
    const bool stale = m_retry->isEnabled() && !m_sourcePane->isReadOnly()
                       && !source.trimmed().isEmpty() && source != m_translatedSource;
    if (stale && !m_mask->isVisible()) { m_mask->setGeometry(m_text->rect()); m_mask->raise(); }
    m_mask->setVisible(stale);
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
    // Each block keeps its actions along its own bottom edge, so stacking needs only a little more height.
    setMinimumSize(640, rows ? 440 : 360);
    const int span = rows ? m_split->height() : m_split->width();
    m_split->setSizes({span/2,span/2});
    syncLayoutButton();
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
    QTextCursor cursor = m_findTarget->document()->find(needle,0);
    while (!cursor.isNull() && found.size()<10000) {
        found.append(cursor);
        cursor = m_findTarget->document()->find(needle,cursor);
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
    m_findTarget->setExtraSelections(marks);
    m_findCount->setText(found.isEmpty() ? (m_findEdit->text().isEmpty() ? QString() : QStringLiteral("无结果"))
                                         : QStringLiteral("%1/%2").arg(m_match+1).arg(found.size()));
    if (reveal && m_match>=0) {
        QTextCursor caret = found[m_match];
        caret.clearSelection();
        m_findTarget->setTextCursor(caret);
        m_findTarget->ensureCursorVisible();
    }
}

void OcrDialog::showFind(bool replace, QPlainTextEdit *target) {
    if (!target) target = m_text;
    if (target != m_findTarget) {
        // Move the strip into the pane being searched; marks never linger in the other one.
        m_findTarget->setExtraSelections({});
        m_findTarget = target;
        if (m_resultLayout) (target==m_sourcePane ? m_sourceLayout : m_resultLayout)->insertWidget(0,m_find);
    }
    const QString selected = target->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator)) m_findEdit->setText(selected);
    m_find->show();
    m_replaceRow->setVisible(replace);
    m_replaceToggle->setIcon(cardIcon(replace ? Icon::CardDown : Icon::CardRight));
    m_findEdit->setFocus();
    m_findEdit->selectAll();
    highlightMatches(true);
}

void OcrDialog::hideFind() {
    m_find->hide();
    m_findTarget->setExtraSelections({});
    m_findTarget->setFocus();
}

void OcrDialog::findStep(int direction) {
    const int count = matches().size();
    if (!count) return;
    m_match = ((m_match<0 ? 0 : m_match+direction)%count+count)%count;
    highlightMatches(true);
}

void OcrDialog::replaceOne() {
    const QList<QTextCursor> found = matches();
    if (m_findTarget->isReadOnly() || m_match<0 || m_match>=found.size()) return;
    QTextCursor cursor = found[m_match];
    cursor.insertText(m_replaceEdit->text());   // m_match now points at the following hit.
    highlightMatches(true);
}

void OcrDialog::replaceAll() {
    const QList<QTextCursor> found = matches();
    if (m_findTarget->isReadOnly() || found.isEmpty()) return;
    QTextCursor edit(m_findTarget->document());
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
    if (object==m_text) {
        if (event->type()==QEvent::Resize && m_mask) m_mask->setGeometry(m_text->rect());
        return QDialog::eventFilter(object,event);
    }
    return QDialog::eventFilter(object,event);
}

void OcrDialog::mousePressEvent(QMouseEvent *event)  {
    if (event->button()==Qt::LeftButton) {
        const QPoint at = event->position().toPoint();
        if (const Qt::Edges edges = resizeEdges(at)) {
            // Resizing works maximized too: the card leaves the maximized state and keeps its current frame.
            if (m_maximized) { m_maximized = false; updateMaximizeButton(); }
            m_resizing = edges;
            m_pressGlobal = event->globalPosition().toPoint();
            m_pressGeometry = geometry();
            event->accept(); return;
        }
        if (dragsWindow(at)) {
            m_dragging = true;
            m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
            event->accept(); return;
        }
    }
    QDialog::mousePressEvent(event);
}

void OcrDialog::mouseMoveEvent(QMouseEvent *event)  {
    const QPoint global = event->globalPosition().toPoint();
    if (m_resizing) {
        // Opposite edges stay put; the moving edge stops where the card reaches its minimum size.
        const QSize least = minimumSize().expandedTo(minimumSizeHint());
        const QPoint delta = global - m_pressGlobal;
        QRect next = m_pressGeometry;
        if (m_resizing & Qt::LeftEdge) next.setLeft(qMin(m_pressGeometry.left()+delta.x(),m_pressGeometry.right()+1-least.width()));
        if (m_resizing & Qt::RightEdge) next.setRight(qMax(m_pressGeometry.right()+delta.x(),m_pressGeometry.left()+least.width()-1));
        if (m_resizing & Qt::TopEdge) next.setTop(qMin(m_pressGeometry.top()+delta.y(),m_pressGeometry.bottom()+1-least.height()));
        if (m_resizing & Qt::BottomEdge) next.setBottom(qMax(m_pressGeometry.bottom()+delta.y(),m_pressGeometry.top()+least.height()-1));
        setGeometry(next);
        event->accept(); return;
    }
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(global - m_dragOffset);
        event->accept(); return;
    }
    updateResizeCursor(resizeEdges(event->position().toPoint()));
    QDialog::mouseMoveEvent(event);
}

void OcrDialog::mouseReleaseEvent(QMouseEvent *event)  {
    if (event->button()!=Qt::LeftButton) { QDialog::mouseReleaseEvent(event); return; }
    const bool resized = bool(m_resizing);
    m_resizing = {};
    m_dragging = false;
    if (resized) updateResizeCursor(resizeEdges(mapFromGlobal(QCursor::pos())));
    event->accept();
}

void OcrDialog::mouseDoubleClickEvent(QMouseEvent *event)  {
    // Only the title bar maximizes; a double click anywhere else keeps its own meaning.
    const QPoint at = event->position().toPoint();
    if (event->button()==Qt::LeftButton && m_header->geometry().contains(at) && dragsWindow(at)) {
        m_resizing = {};
        m_dragging = false;
        toggleMaximize();
        event->accept(); return;
    }
    QDialog::mouseDoubleClickEvent(event);
}

void OcrDialog::leaveEvent(QEvent *event)  {
    if (!m_resizing) unsetCursor();
    QDialog::leaveEvent(event);
}

Qt::Edges OcrDialog::resizeEdges(const QPoint &point) const {
    // The outer half of the child-free margin reacts; corners combine two edges.
    const int band = qMax(1,ResizeBand/2);
    Qt::Edges edges;
    if (!rect().contains(point)) return edges;
    if (point.x() < band) edges |= Qt::LeftEdge;
    else if (point.x() > width()-1-band) edges |= Qt::RightEdge;
    if (point.y() < band) edges |= Qt::TopEdge;
    else if (point.y() > height()-1-band) edges |= Qt::BottomEdge;
    return edges;
}

void OcrDialog::updateResizeCursor(Qt::Edges edges) {
    const bool horizontal = edges & (Qt::LeftEdge|Qt::RightEdge), vertical = edges & (Qt::TopEdge|Qt::BottomEdge);
    if (horizontal && vertical)
        setCursor(bool(edges & Qt::LeftEdge) == bool(edges & Qt::TopEdge) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
    else if (horizontal) setCursor(Qt::SizeHorCursor);
    else if (vertical) setCursor(Qt::SizeVerCursor);
    else unsetCursor();
}

bool OcrDialog::dragsWindow(const QPoint &point) const {
    // Empty chrome, labels and block backgrounds move the card; controls keep their own drags.
    for (QWidget *widget = childAt(point); widget && widget != this; widget = widget->parentWidget())
        if (widget->property("NoWindowDrag").toBool() || qobject_cast<QAbstractButton *>(widget)
            || qobject_cast<QAbstractSpinBox *>(widget) || qobject_cast<QLineEdit *>(widget)
            || qobject_cast<QComboBox *>(widget) || qobject_cast<QAbstractSlider *>(widget)
            || qobject_cast<QAbstractScrollArea *>(widget) || qobject_cast<QSplitterHandle *>(widget))
            return false;
    return rect().contains(point);
}

void OcrDialog::toggleMaximize() {
    // Fills the work area of the card's screen by geometry and keeps the frame resizable and draggable.
    if (!m_maximized) {
        const QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
        if (!screen) screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        m_restoreGeometry = geometry();
        setGeometry(screen->availableGeometry());
        m_maximized = true;
    } else {
        if (m_restoreGeometry.isValid()) setGeometry(m_restoreGeometry);
        m_maximized = false;
    }
    updateMaximizeButton();
}

void OcrDialog::updateMaximizeButton() {
    m_maximize->setIcon(cardIcon(m_maximized ? Icon::CardRestore : Icon::CardMaximize));
    const QString tip = m_maximized ? QStringLiteral("还原") : QStringLiteral("最大化");
    m_maximize->setToolTip(tip); m_maximize->setAccessibleName(tip);
}
