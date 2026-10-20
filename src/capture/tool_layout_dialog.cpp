#include "capture/tool_layout_dialog.h"
#include "shared/widgets.h"
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <functional>

namespace {
const char kToolMime[] = "application/x-lenssnip-tool";

QString normalizedSide(const QString &side)
{
    return (side == QStringLiteral("top") || side == QStringLiteral("right")
            || side == QStringLiteral("left") || side == QStringLiteral("hidden"))
        ? side : QStringLiteral("bottom");
}

void setDropHighlight(QWidget *widget, bool on)
{
    if (!widget || widget->property("drop").toBool() == on) return;
    widget->setProperty("drop", on);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

class LayoutButton : public QToolButton
{
public:
    explicit LayoutButton(const QString &id, QWidget *parent = nullptr)
        : QToolButton(parent), m_id(id) {}
    QString id() const { return m_id; }
    std::function<void(const QString &, LayoutButton *)> dragStarted;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_press = event->position().toPoint();
            m_armed = true;
        }
        QToolButton::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_armed && (event->buttons() & Qt::LeftButton)
            && (event->position().toPoint() - m_press).manhattanLength()
                >= QApplication::startDragDistance()) {
            m_armed = false;
            setDown(false);
            if (dragStarted) dragStarted(m_id, this);
            return;
        }
        QToolButton::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        m_armed = false;
        QToolButton::mouseReleaseEvent(event);
    }

private:
    QString m_id;
    QPoint m_press;
    bool m_armed = false;
};

class TitleBar : public QWidget
{
public:
    TitleBar(QDialog *dialog, const QString &title, QWidget *parent)
        : QWidget(parent), m_dialog(dialog)
    {
        setFixedHeight(28);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto *label = new QLabel(title, this);
        label->setObjectName(QStringLiteral("toolLayoutTitle"));
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(label);
        row->addStretch();
        auto *close = new QToolButton(this);
        close->setObjectName(QStringLiteral("toolLayoutClose"));
        close->setIcon(barIcon(Icon::Cancel, 14));
        close->setIconSize(QSize(14, 14));
        close->setFixedSize(28, 28);
        close->setCursor(Qt::PointingHandCursor);
        close->setFocusPolicy(Qt::NoFocus);
        close->setAccessibleName(tr("关闭"));
        connect(close, &QToolButton::clicked, dialog, &QDialog::reject);
        row->addWidget(close);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            m_drag = event->globalPosition().toPoint() - m_dialog->pos();
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if ((event->buttons() & Qt::LeftButton) && !m_drag.isNull())
            m_dialog->move(event->globalPosition().toPoint() - m_drag);
    }
    void mouseReleaseEvent(QMouseEvent *) override { m_drag = QPoint(); }

private:
    QDialog *m_dialog;
    QPoint m_drag;
};
}

ToolLayoutDialog::ToolLayoutDialog(const QVector<ToolLayoutItem> &items,
                                   const QVector<ToolLayoutItem> &defaults,
                                   QWidget *parent)
    : QDialog(parent), m_items(items), m_defaults(defaults)
{
    setWindowTitle(tr("调整工具栏"));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_TranslucentBackground, true);
    resize(920, 740);
    setStyleSheet(QStringLiteral(
        "QDialog{background:transparent;}"
        "QWidget#toolLayoutContent,QScrollArea,QScrollArea > QWidget > QWidget{background:transparent;border:none;}"
        "QWidget#toolLayoutCard{background:#121213;border:1px solid #2b2d33;border-radius:12px;}"
        "QLabel{color:#f4f5f8;background:transparent;font-size:13px;}"
        "QLabel#toolLayoutTitle{font-size:15px;font-weight:600;}"
        "QLabel#toolLayoutHint{color:#9c9da1;font-size:12px;}"
        "QLabel#toolLayoutPreview{background:#0b0c0f;border:1px solid #2b2d33;border-radius:8px;"
        "color:#73757f;font-size:13px;}"
        "QWidget#toolLayoutZone{border:1px dashed #2b2d33;border-radius:8px;background:transparent;}"
        "QWidget#toolLayoutZone[drop=\"true\"]{border-color:#5e6ad2;background:#171a2a;}"
        "QLabel#toolLayoutZoneLabel{color:#73757f;font-size:12px;}"
        "QWidget#toolLayoutGroup{background:transparent;border:none;}"
        "QToolButton#toolLayoutTool{color:#9c9da1;background:#1b1b1c;border:1px solid #2b2d33;"
        "border-radius:7px;padding:0px;}"
        "QToolButton#toolLayoutTool:hover{background:#222326;border-color:#3a3c44;color:#f4f5f8;}"
        "QToolButton#toolLayoutTool:focus{background:#222326;border:1px solid #5e6ad2;}"
        "QToolButton#toolLayoutHiddenChip{color:#9c9da1;background:#1b1b1c;border:1px solid #2b2d33;"
        "border-radius:7px;padding:0px 10px;font-size:12px;}"
        "QToolButton#toolLayoutHiddenChip:hover{background:#222326;color:#f4f5f8;}"
        "QWidget#toolLayoutHiddenTray{border:1px dashed #2b2d33;border-radius:8px;background:transparent;}"
        "QWidget#toolLayoutHiddenTray[drop=\"true\"]{border-color:#5e6ad2;background:#171a2a;}"
        "QLabel#toolLayoutHiddenHint{color:#73757f;font-size:12px;}"
        "QLabel#toolLayoutHiddenLabel{color:#9c9da1;font-size:12px;}"
        "QToolButton#toolLayoutClose{background:transparent;border:none;border-radius:6px;}"
        "QToolButton#toolLayoutClose:hover{background:#222326;}"
        "QPushButton{color:#9c9da1;background:transparent;border:none;border-radius:8px;"
        "padding:0px 12px;font-size:13px;}"
        "QPushButton:hover{color:#f4f5f8;background:#222326;}"
        "QPushButton:pressed{background:#2b2d33;}"
        "QPushButton#toolLayoutSave{color:#ffffff;background:#5e6ad2;}"
        "QPushButton#toolLayoutSave:hover{background:#6d78de;}"
        "QScrollBar:vertical{background:transparent;width:8px;margin:0px;}"
        "QScrollBar::handle:vertical{background:#35373e;border-radius:4px;min-height:28px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("toolLayoutScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_content = new QWidget(m_scroll);
    m_content->setObjectName(QStringLiteral("toolLayoutContent"));
    m_content->setMinimumWidth(900);
    auto *contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(10, 10, 10, 10);
    contentLayout->setSpacing(10);

    auto *card = new QWidget(m_content);
    card->setObjectName(QStringLiteral("toolLayoutCard"));
    card->setFixedHeight(700);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(18, 14, 18, 18);
    cardLayout->setSpacing(10);

    auto *title = new TitleBar(this, tr("调整工具栏"), card);
    cardLayout->addWidget(title);
    auto *hint = new QLabel(tr("拖动按钮到四边；隐藏工具进入下拉；下拉入口不能隐藏。"), card);
    hint->setObjectName(QStringLiteral("toolLayoutHint"));
    hint->setWordWrap(true);
    cardLayout->addWidget(hint);

    m_stage = new QWidget(card);
    m_stage->setObjectName(QStringLiteral("toolLayoutStage"));
    m_stage->setFixedHeight(480);
    auto *grid = new QGridLayout(m_stage);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    grid->setColumnMinimumWidth(0, 80);
    grid->setColumnStretch(1, 1);
    grid->setColumnMinimumWidth(2, 80);
    grid->setRowMinimumHeight(0, 60);
    grid->setRowStretch(1, 1);
    grid->setRowMinimumHeight(2, 64);

    QWidget *top = buildZone(QStringLiteral("top"), tr("上方"));
    QWidget *left = buildZone(QStringLiteral("left"), tr("左侧"));
    QWidget *right = buildZone(QStringLiteral("right"), tr("右侧"));
    QWidget *bottom = buildZone(QStringLiteral("bottom"), tr("下方"));
    right->setToolTip(tr("把按钮拖到这里，右侧变竖栏"));

    m_preview = new QLabel(tr("截图区域（默认黑色）"), m_stage);
    m_preview->setObjectName(QStringLiteral("toolLayoutPreview"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFixedSize(620, 310);

    grid->addWidget(top, 0, 1);
    grid->addWidget(left, 1, 0);
    grid->addWidget(m_preview, 1, 1, Qt::AlignCenter);
    grid->addWidget(right, 1, 2);
    grid->addWidget(bottom, 2, 1);
    cardLayout->addWidget(m_stage);

    m_hiddenTray = new QWidget(card);
    m_hiddenTray->setObjectName(QStringLiteral("toolLayoutHiddenTray"));
    m_hiddenTray->setAcceptDrops(true);
    m_hiddenTray->installEventFilter(this);
    m_hiddenTray->setMinimumHeight(48);
    auto *hiddenLayout = new QHBoxLayout(m_hiddenTray);
    hiddenLayout->setContentsMargins(10, 6, 10, 6);
    hiddenLayout->setSpacing(8);
    auto *hiddenLabel = new QLabel(tr("隐藏栏"), m_hiddenTray);
    hiddenLabel->setObjectName(QStringLiteral("toolLayoutHiddenLabel"));
    hiddenLabel->setProperty("lenssnipSide", QStringLiteral("hidden"));
    hiddenLabel->setAcceptDrops(true);
    hiddenLabel->installEventFilter(this);
    m_hiddenPlaceholder = new QLabel(
        tr("隐藏工具拖到这里 · 实际截图下拉末尾为“调整”"), m_hiddenTray);
    m_hiddenPlaceholder->setObjectName(QStringLiteral("toolLayoutHiddenHint"));
    m_hiddenPlaceholder->setProperty("lenssnipSide", QStringLiteral("hidden"));
    m_hiddenPlaceholder->setAcceptDrops(true);
    m_hiddenPlaceholder->installEventFilter(this);
    m_hiddenGroup = new QWidget(m_hiddenTray);
    m_hiddenGroup->setObjectName(QStringLiteral("toolLayoutHiddenGroup"));
    m_hiddenGroup->setProperty("lenssnipSide", QStringLiteral("hidden"));
    m_hiddenGroup->setAcceptDrops(true);
    m_hiddenGroup->installEventFilter(this);
    auto *hiddenGroupLayout = new QHBoxLayout(m_hiddenGroup);
    hiddenGroupLayout->setContentsMargins(0, 0, 0, 0);
    hiddenGroupLayout->setSpacing(6);
    hiddenLayout->addWidget(hiddenLabel);
    hiddenLayout->addWidget(m_hiddenPlaceholder);
    hiddenLayout->addWidget(m_hiddenGroup);
    hiddenLayout->addStretch();
    cardLayout->addWidget(m_hiddenTray);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto *restore = new QPushButton(tr("恢复默认"), card);
    auto *cancel = new QPushButton(tr("取消"), card);
    auto *save = new QPushButton(tr("保存"), card);
    save->setObjectName(QStringLiteral("toolLayoutSave"));
    save->setDefault(true);
    for (QPushButton *button : {restore, cancel, save}) button->setFixedHeight(32);
    actions->addWidget(restore);
    actions->addStretch();
    actions->addWidget(cancel);
    actions->addWidget(save);
    cardLayout->addStretch(1);
    cardLayout->addLayout(actions);

    contentLayout->addWidget(card);
    contentLayout->addStretch(1);

    m_scroll->setWidget(m_content);
    root->addWidget(m_scroll);

    connect(restore, &QPushButton::clicked, this, &ToolLayoutDialog::restoreDefaults);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &QDialog::accept);
    rebuild();
    save->setFocus();
}

void ToolLayoutDialog::fitToScreen(const QRect &available)
{
    const int width = qMax(720, qMin(920, available.width() - 24));
    const int height = qMax(360, qMin(740, available.height() - 24));
    resize(width, height);
    move(available.center() - rect().center());
}

QWidget *ToolLayoutDialog::buildZone(const QString &side, const QString &labelText)
{
    auto *zone = new QWidget(m_stage);
    zone->setObjectName(QStringLiteral("toolLayoutZone"));
    zone->setProperty("lenssnipSide", side);
    zone->setAcceptDrops(true);
    zone->installEventFilter(this);
    auto *layout = new QVBoxLayout(zone);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);
    auto *label = new QLabel(labelText, zone);
    label->setObjectName(QStringLiteral("toolLayoutZoneLabel"));
    label->setAlignment(Qt::AlignCenter);
    label->setProperty("lenssnipSide", side);
    label->setAcceptDrops(true);
    label->installEventFilter(this);
    auto *group = new QWidget(zone);
    group->setObjectName(QStringLiteral("toolLayoutGroup"));
    group->setProperty("lenssnipSide", side);
    group->setAcceptDrops(true);
    group->installEventFilter(this);
    if (side == QStringLiteral("left") || side == QStringLiteral("right")) {
        auto *box = new QVBoxLayout(group);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(4);
    } else {
        auto *box = new QHBoxLayout(group);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(4);
    }
    group->hide();
    if (side == QStringLiteral("left") || side == QStringLiteral("right")) {
        layout->addWidget(label, 0, Qt::AlignHCenter | Qt::AlignTop);
        layout->addWidget(group, 0, Qt::AlignHCenter | Qt::AlignTop);
        layout->addStretch(1);
    } else {
        layout->addStretch(1);
        layout->addWidget(label, 0, Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(group, 0, Qt::AlignRight | Qt::AlignVCenter);
        layout->addStretch(1);
    }
    m_zones.insert(side, zone);
    m_groups.insert(side, group);
    m_zoneLabels.insert(side, label);
    return zone;
}

void ToolLayoutDialog::clearLayout(QLayout *layout)
{
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) delete widget;
        delete item;
    }
}

void ToolLayoutDialog::rebuild()
{
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it)
        clearLayout(it.value()->layout());
    clearLayout(m_hiddenGroup->layout());

    for (const ToolLayoutItem &entry : m_items) {
        const QString side = normalizedSide(entry.side);
        const bool hidden = side == QStringLiteral("hidden");
        if (hidden && entry.special) continue;
        QWidget *parent = hidden ? m_hiddenGroup : m_groups.value(side);
        if (!parent) continue;
        auto *button = new LayoutButton(entry.id, parent);
        button->setProperty("lenssnipLayoutId", entry.id);
        button->setProperty("lenssnipSide", side);
        button->setAcceptDrops(true);
        button->installEventFilter(this);
        button->setIcon(barIcon(entry.icon, hidden ? 16 : 18));
        button->setIconSize(QSize(hidden ? 16 : 18, hidden ? 16 : 18));
        button->setToolTip(entry.name);
        button->setAccessibleName(entry.name);
        button->setCursor(Qt::OpenHandCursor);
        button->setFocusPolicy(Qt::StrongFocus);
        if (hidden) {
            button->setObjectName(QStringLiteral("toolLayoutHiddenChip"));
            button->setText(entry.name);
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            button->setFixedHeight(34);
            button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            static_cast<QHBoxLayout *>(parent->layout())->addWidget(button);
        } else {
            button->setObjectName(QStringLiteral("toolLayoutTool"));
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setFixedSize(34, 34);
            auto *layout = qobject_cast<QBoxLayout *>(parent->layout());
            if (layout) layout->addWidget(button);
        }
        button->dragStarted = [this](const QString &id, LayoutButton *source) {
            if (!m_dragId.isEmpty()) return;
            m_dragId = id;
            auto *drag = new QDrag(this);
            auto *mime = new QMimeData;
            mime->setData(kToolMime, id.toUtf8());
            drag->setMimeData(mime);
            drag->setPixmap(source->grab());
            drag->setHotSpot(source->rect().center());
            drag->exec(Qt::MoveAction);
            drag->deleteLater();
            for (QWidget *zone : m_zones) setDropHighlight(zone, false);
            setDropHighlight(m_hiddenTray, false);
            const QString dropId = m_dropId;
            const QString dropSide = m_dropSide;
            const int drop = m_dropIndex;
            m_dropId.clear();
            m_dragId.clear();
            // Rebuilding here would delete the source button while its mouseMoveEvent
            // is still on the stack. Defer until the drag event has fully unwound.
            if (!dropId.isEmpty()) {
                QTimer::singleShot(0, this, [this, dropId, dropSide, drop] {
                    moveItem(dropId, dropSide, drop);
                });
            }
        };
    }

    for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
        QWidget *group = it.value();
        const bool hasItems = group->layout() && group->layout()->count() > 0;
        group->setVisible(hasItems);
        if (QLabel *label = m_zoneLabels.value(it.key())) label->setVisible(!hasItems);
        group->adjustSize();
    }
    const bool hasHidden = m_hiddenGroup->layout() && m_hiddenGroup->layout()->count() > 0;
    m_hiddenGroup->setVisible(hasHidden);
    m_hiddenPlaceholder->setVisible(!hasHidden);
    m_hiddenGroup->adjustSize();
}

QVector<ToolLayoutItem> ToolLayoutDialog::items() const
{
    return m_items;
}

int ToolLayoutDialog::dropIndex(QWidget *zone, const QString &side, const QPoint &position) const
{
    QWidget *group = side == QStringLiteral("hidden") ? m_hiddenGroup : m_groups.value(side);
    if (!group) return 0;
    const bool vertical = side == QStringLiteral("left") || side == QStringLiteral("right");
    int index = 0;
    const auto buttons = group->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly);
    for (QToolButton *button : buttons) {
        if (!button->isVisible() || button->property("lenssnipLayoutId").toString() == m_dragId)
            continue;
        const QPoint center = button->mapTo(zone, button->rect().center());
        if (vertical ? position.y() > center.y() : position.x() > center.x()) ++index;
        else break;
    }
    return index;
}

void ToolLayoutDialog::moveItem(const QString &id, const QString &side, int index)
{
    int source = -1;
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).id == id) { source = i; break; }
    if (source < 0) return;
    const QString targetSide = normalizedSide(side);
    if (targetSide == QStringLiteral("hidden") && m_items.at(source).special) {
        QToolTip::showText(QCursor::pos(), tr("下拉按钮不能隐藏"), this);
        return;
    }
    ToolLayoutItem item = m_items.takeAt(source);
    item.side = targetSide;
    QStringList sameSide;
    for (const ToolLayoutItem &entry : m_items)
        if (normalizedSide(entry.side) == targetSide) sameSide << entry.id;
    index = qBound(0, index, sameSide.size());
    int target = m_items.size();
    if (index < sameSide.size()) {
        const QString before = sameSide.at(index);
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items.at(i).id == before) { target = i; break; }
    } else if (!sameSide.isEmpty()) {
        const QString after = sameSide.last();
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items.at(i).id == after) { target = i + 1; break; }
    }
    m_items.insert(target, item);
    rebuild();
    emitChanged();
}

void ToolLayoutDialog::restoreDefaults()
{
    m_items = m_defaults;
    for (ToolLayoutItem &entry : m_items) entry.side = QStringLiteral("bottom");
    rebuild();
    emitChanged();
}

void ToolLayoutDialog::emitChanged()
{
    emit layoutChanged(m_items);
}

bool ToolLayoutDialog::eventFilter(QObject *obj, QEvent *event)
{
    auto *widget = qobject_cast<QWidget *>(obj);
    if (!widget) return QDialog::eventFilter(obj, event);
    QString side = widget->property("lenssnipSide").toString();
    if (side.isEmpty() && widget == m_hiddenTray) side = QStringLiteral("hidden");
    for (QWidget *parent = widget->parentWidget();
         side.isEmpty() && parent; parent = parent->parentWidget())
        side = parent->property("lenssnipSide").toString();
    if (side.isEmpty()) return QDialog::eventFilter(obj, event);
    const bool hidden = side == QStringLiteral("hidden");
    QWidget *highlight = hidden ? m_hiddenTray : m_zones.value(side);
    if (!highlight) return QDialog::eventFilter(obj, event);
    switch (event->type()) {
    case QEvent::DragEnter: {
        auto *drag = static_cast<QDragEnterEvent *>(event);
        if (m_dragId.isEmpty() || !drag->mimeData()->hasFormat(kToolMime)) break;
        drag->acceptProposedAction();
        setDropHighlight(highlight, true);
        return true;
    }
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        if (m_dragId.isEmpty()) break;
        drag->acceptProposedAction();
        return true;
    }
    case QEvent::DragLeave:
        setDropHighlight(highlight, false);
        return true;
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        if (m_dragId.isEmpty()) break;
        drop->acceptProposedAction();
        if (hidden && m_dragId == QStringLiteral("more")) {
            QToolTip::showText(QCursor::pos(), tr("下拉按钮不能隐藏"), this);
            setDropHighlight(highlight, false);
            return true;
        }
        m_dropId = m_dragId;
        m_dropSide = side;
        const QPoint global = widget->mapToGlobal(drop->position().toPoint());
        m_dropIndex = dropIndex(highlight, side, highlight->mapFromGlobal(global));
        setDropHighlight(highlight, false);
        return true;
    }
    default:
        break;
    }
    return QDialog::eventFilter(obj, event);
}

void ToolLayoutDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
}
