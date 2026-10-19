#pragma once

#include "history/history_store.h"
#include <QDialog>
#include <QHash>
#include <QImage>

class QCheckBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

class HistoryDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HistoryDialog(QWidget *parent = nullptr);

    void setRecordingEnabled(bool enabled);
    static int testUi(const QString &previewPath = {});

signals:
    void pinRequested(const QImage &image);
    void enabledChanged(bool enabled);

private:
    void refresh();
    void updateActions();
    void pinSelected();
    void copySelected();
    void saveSelected();
    void removeSelected();
    void clearAll();
    HistoryEntry selectedEntry() const;

    QCheckBox *m_enabled = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_status = nullptr, *m_empty = nullptr;
    QPushButton *m_pin = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_clear = nullptr;
    QHash<qint64, HistoryEntry> m_entries;
};
