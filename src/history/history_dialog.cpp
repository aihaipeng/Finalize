#include "history/history_dialog.h"
#include "platform/output.h"
#include "shared/settings.h"
#include "shared/widgets.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <cstdio>

HistoryDialog::HistoryDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("截图历史"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(920, 620);
    setMinimumSize(640, 420);
    setStyleSheet(QStringLiteral(
        "QDialog { background:#18181b; }"
        "QListWidget { background:#101013; border:1px solid #27272a; border-radius:8px;"
        " padding:6px; color:#fcfcfc; }"
        "QListWidget::item { border-radius:6px; padding:4px; }"
        "QListWidget::item:selected { background:#264f78; }"
        "QCheckBox, QLabel { color:#fcfcfc; }"
        "QPushButton { background:#27272a; color:#fcfcfc; border:1px solid #3a3a40;"
        " border-radius:6px; padding:6px 12px; }"
        "QPushButton:hover { background:#3a3a40; }"
        "QPushButton:disabled { color:#71717a; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);

    auto *top = new QHBoxLayout;
    auto *title = new QLabel(tr("截图历史"), this);
    title->setStyleSheet(QStringLiteral("font-size:16px; font-weight:600;"));
    top->addWidget(title);
    top->addSpacing(16);
    m_enabled = new QCheckBox(tr("记录截图历史（仅保存在本机）"), this);
    m_enabled->setToolTip(tr("默认关闭。开启后，未保存的截图也会复制到本机历史；可随时关闭或清空。"));
    m_enabled->setChecked(HistoryStore::enabled());
    top->addWidget(m_enabled);
    top->addStretch();
    layout->addLayout(top);

    m_list = new QListWidget(this);
    m_list->setViewMode(QListView::IconMode);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setIconSize(QSize(176, 110));
    m_list->setGridSize(QSize(200, 150));
    m_list->setSpacing(6);
    m_list->setUniformItemSizes(true);
    layout->addWidget(m_list, 1);

    auto *bottom = new QHBoxLayout;
    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color:#8b8b94;"));
    bottom->addWidget(m_status);
    bottom->addStretch();

    m_pin = new QPushButton(makeIcon(Icon::Pin, 16, true), tr("贴图"), this);
    m_copy = new QPushButton(makeIcon(Icon::Copy, 16, true), tr("复制"), this);
    m_save = new QPushButton(makeIcon(Icon::Save, 16, true), tr("保存"), this);
    m_delete = new QPushButton(makeIcon(Icon::Cancel, 16, true), tr("删除"), this);
    m_clear = new QPushButton(tr("清空"), this);
    auto *close = new QPushButton(tr("关闭"), this);
    for (QPushButton *button : {m_pin, m_copy, m_save, m_delete, m_clear, close})
        bottom->addWidget(button);
    layout->addLayout(bottom);

    connect(m_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
        HistoryStore::setEnabled(enabled);
        emit enabledChanged(enabled);
    });
    connect(m_list, &QListWidget::itemSelectionChanged, this, &HistoryDialog::updateActions);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { pinSelected(); });
    connect(m_pin, &QPushButton::clicked, this, &HistoryDialog::pinSelected);
    connect(m_copy, &QPushButton::clicked, this, &HistoryDialog::copySelected);
    connect(m_save, &QPushButton::clicked, this, &HistoryDialog::saveSelected);
    connect(m_delete, &QPushButton::clicked, this, &HistoryDialog::removeSelected);
    connect(m_clear, &QPushButton::clicked, this, &HistoryDialog::clearAll);
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    refresh();
}

void HistoryDialog::setRecordingEnabled(bool enabled)
{
    QSignalBlocker blocker(m_enabled);
    m_enabled->setChecked(enabled);
}

void HistoryDialog::refresh()
{
    HistoryStore::waitForPending();
    m_entries.clear();
    m_list->clear();

    const QVector<HistoryEntry> entries = HistoryStore::entries();
    qint64 bytes = 0;
    for (const HistoryEntry &entry : entries) {
        m_entries.insert(entry.id, entry);
        bytes += entry.bytes;

        auto *item = new QListWidgetItem;
        item->setData(Qt::UserRole, entry.id);
        item->setText(entry.created.toLocalTime().toString(QStringLiteral("MM-dd HH:mm"))
                      + QLatin1Char('\n')
                      + QStringLiteral("%1 × %2").arg(entry.width).arg(entry.height));
        item->setTextAlignment(Qt::AlignHCenter);
        item->setSizeHint(QSize(196, 146));
        item->setToolTip(entry.created.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                         + QLatin1Char('\n')
                         + tr("%1 × %2 · %3").arg(entry.width).arg(entry.height)
                                   .arg(QLocale().formattedDataSize(entry.bytes))
                         + QLatin1Char('\n') + tr("双击贴图"));

        const QString thumbPath = HistoryStore::absoluteThumbPath(entry);
        QPixmap thumb(thumbPath);
        item->setIcon(thumb.isNull() ? makeIcon(Icon::Pin, 48, true) : QIcon(thumb));
        m_list->addItem(item);
    }

    m_status->setText(entries.isEmpty()
            ? tr("暂无历史截图")
            : tr("%1 项 · %2").arg(entries.size()).arg(QLocale().formattedDataSize(bytes)));
    updateActions();
}

void HistoryDialog::updateActions()
{
    const bool selected = !m_list->selectedItems().isEmpty();
    m_pin->setEnabled(selected);
    m_copy->setEnabled(selected);
    m_save->setEnabled(selected);
    m_delete->setEnabled(selected);
    m_clear->setEnabled(!m_entries.isEmpty());
}

HistoryEntry HistoryDialog::selectedEntry() const
{
    const QList<QListWidgetItem *> items = m_list->selectedItems();
    if (items.isEmpty()) return {};
    const qint64 id = items.first()->data(Qt::UserRole).toLongLong();
    return m_entries.value(id);
}

void HistoryDialog::pinSelected()
{
    const HistoryEntry entry = selectedEntry();
    if (entry.id < 0) return;
    QString error;
    const QImage image = HistoryStore::image(entry, &error);
    if (image.isNull()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    emit pinRequested(image);
}

void HistoryDialog::copySelected()
{
    const HistoryEntry entry = selectedEntry();
    if (entry.id < 0) return;
    QString error;
    const QImage image = HistoryStore::image(entry, &error);
    if (image.isNull()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    if (!copyImageToClipboard(image)) {
        QMessageBox::warning(this, windowTitle(), tr("复制失败，剪贴板可能被占用，请重试"));
        return;
    }
    m_status->setText(tr("已复制到剪贴板"));
}

void HistoryDialog::saveSelected()
{
    const HistoryEntry entry = selectedEntry();
    if (entry.id < 0) return;
    QString error;
    const QImage image = HistoryStore::image(entry, &error);
    if (image.isNull()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString suggested = dir + QStringLiteral("/nami-history-")
            + entry.created.toLocalTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) + QStringLiteral(".png");
    const QString path = QFileDialog::getSaveFileName(
            this, tr("保存图片"), suggested,
            QStringLiteral("PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)"));
    if (path.isEmpty()) return;
    if (!saveImageAtomic(image, path)) {
        QMessageBox::warning(this, windowTitle(), tr("保存失败，请检查路径或磁盘空间后重试"));
        return;
    }
    m_status->setText(tr("已保存到 %1").arg(QFileInfo(path).fileName()));
}

void HistoryDialog::removeSelected()
{
    QVector<qint64> ids;
    for (QListWidgetItem *item : m_list->selectedItems())
        ids.append(item->data(Qt::UserRole).toLongLong());
    if (ids.isEmpty()) return;
    if (QMessageBox::question(this, tr("删除历史"),
                              tr("确定删除选中的 %1 项历史记录？应用副本会被删除，用户保存的图片不受影响。")
                                      .arg(ids.size()))
            != QMessageBox::Yes)
        return;
    for (const qint64 id : ids) HistoryStore::remove(id);
    refresh();
}

void HistoryDialog::clearAll()
{
    if (m_entries.isEmpty()) return;
    if (QMessageBox::question(this, tr("清空历史"),
                              tr("确定清空全部截图历史？应用副本会被删除，用户保存的图片不受影响。"))
            != QMessageBox::Yes)
        return;
    HistoryStore::clear();
    refresh();
}

int HistoryDialog::testUi()
{
    bool pass = true;
    const auto check = [&pass](bool condition, const char *message) {
        std::printf("%s history UI: %s\n", condition ? "PASS" : "FAIL", message);
        if (!condition) pass = false;
    };

    QTemporaryDir temp;
    check(temp.isValid(), "temporary history root");
    if (!temp.isValid()) return 1;
    qputenv("NAMI_HISTORY_ROOT", temp.path().toLocal8Bit());

    NamiSettings settings;
    const QVariant oldEnabled = settings.value(QStringLiteral("History/enabled"));
    const QVariant oldItems = settings.value(QStringLiteral("History/maxItems"));
    settings.setValue(QStringLiteral("History/enabled"), true);
    settings.setValue(QStringLiteral("History/maxItems"), 100);
    settings.sync();

    QImage image(160, 100, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#4c9bff")));
    HistoryStore::record(image);
    HistoryStore::waitForPending();

    {
        HistoryDialog dialog;
        auto *list = dialog.findChild<QListWidget *>();
        auto *enabled = dialog.findChild<QCheckBox *>();
        check(list != nullptr && list->count() == 1, "dialog lists the recorded item");
        check(enabled != nullptr && enabled->isChecked(), "dialog reflects the enabled switch");
        check(dialog.windowTitle() == QCoreApplication::translate("HistoryDialog", "截图历史"),
              "dialog title is translated");
    }

    HistoryStore::clear();
    if (oldEnabled.isValid()) settings.setValue(QStringLiteral("History/enabled"), oldEnabled);
    else settings.remove(QStringLiteral("History/enabled"));
    if (oldItems.isValid()) settings.setValue(QStringLiteral("History/maxItems"), oldItems);
    else settings.remove(QStringLiteral("History/maxItems"));
    settings.sync();
    qunsetenv("NAMI_HISTORY_ROOT");
    return pass ? 0 : 1;
}
