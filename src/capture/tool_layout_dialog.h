#pragma once

#include "shared/widgets.h"
#include <QDialog>
#include <QVector>
#include <QHash>
#include <QString>
#include <QPoint>

class QLabel;
class QLayout;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class QWidget;

struct ToolLayoutItem {
    QString id;
    QString name;
    Icon icon = Icon::Rect;
    QString side = QStringLiteral("bottom");   // top / right / bottom / left / hidden.
    bool special = false;                      // The draggable dropdown entry cannot be hidden.
};

class ToolLayoutDialog : public QDialog
{
    Q_OBJECT
public:
    ToolLayoutDialog(const QVector<ToolLayoutItem> &items,
                     const QVector<ToolLayoutItem> &defaults,
                     QWidget *parent = nullptr);
    QVector<ToolLayoutItem> items() const;
    void fitToScreen(const QRect &available);

signals:
    void layoutChanged(const QVector<ToolLayoutItem> &items);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    friend class CaptureOverlay;
    QWidget *buildZone(const QString &side, const QString &label);
    void rebuild();
    void clearLayout(QLayout *layout);
    void moveItem(const QString &id, const QString &side, int index);
    void restoreDefaults();
    void emitChanged();
    int dropIndex(QWidget *zone, const QString &side, const QPoint &position) const;
    void showItemMenu(const QString &id);
    void focusItem(const QString &id);
    void layoutHiddenItems();
    void showDropIndicator(const QString &side, int index);

    QVector<ToolLayoutItem> m_items;
    QVector<ToolLayoutItem> m_defaults;
    QHash<QString, QWidget *> m_zones;
    QHash<QString, QWidget *> m_groups;
    QHash<QString, QLabel *> m_zoneLabels;
    QWidget *m_hiddenTray = nullptr;
    QWidget *m_hiddenGroup = nullptr;
    QLabel *m_hiddenPlaceholder = nullptr;
    QWidget *m_stage = nullptr;
    QLabel *m_preview = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QWidget *m_dropIndicator = nullptr;
    QString m_dragId;
    QString m_dropId;
    QString m_dropSide;
    int m_dropIndex = -1;
};
