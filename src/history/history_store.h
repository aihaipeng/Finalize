#pragma once

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QVector>

struct HistoryEntry
{
    qint64 id = -1;
    QString kind;
    QDateTime created;
    QString imageRel;
    QString thumbRel;
    int width = 0;
    int height = 0;
    qint64 bytes = 0;
    qreal dpr = 1.0;
};

// File-backed screenshot history. SQLite only indexes metadata; images stay as files.
class HistoryStore
{
public:
    static bool enabled();
    static void setEnabled(bool enabled);

    // Queues a background write. No-op while history is disabled.
    static void record(const QImage &image, const QString &kind = QStringLiteral("capture"));
    static QVector<HistoryEntry> entries(int limit = 0);
    static QImage image(const HistoryEntry &entry, QString *error = nullptr);
    static bool remove(qint64 id);
    static bool clear();

    static QString rootPath();
    static QString absoluteImagePath(const HistoryEntry &entry);
    static QString absoluteThumbPath(const HistoryEntry &entry);
    static void waitForPending();

    static int testHistory();

private:
    static bool writeItem(const QImage &image, const QString &kind);
};
