#include "history/history_store.h"
#include "platform/output.h"
#include "shared/settings.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>
#include <QSet>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimeZone>
#include <QUuid>
#include <QVariant>
#include <QDebug>

#include <atomic>
#include <cstdio>

namespace {

QThreadPool &historyPool()
{
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(1); return true; }();
    Q_UNUSED(configured);
    return pool;
}

std::atomic<int> &pendingQueue()
{
    static std::atomic<int> count{0};
    return count;
}

constexpr int kMaxPendingWrites = 8;

QString historyRoot()
{
    const QByteArray overridePath = qgetenv("LENSSNIP_HISTORY_ROOT");
    if (!overridePath.isEmpty()) return QString::fromLocal8Bit(overridePath);
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base + QStringLiteral("/history");
}

QString databasePath()
{
    return historyRoot() + QStringLiteral("/index.db");
}

bool ensureDirectories(QString *error)
{
    QDir root(historyRoot());
    if (!root.mkpath(QStringLiteral("images")) || !root.mkpath(QStringLiteral("thumbs"))) {
        if (error) *error = QObject::tr("无法创建历史目录：%1").arg(root.absolutePath());
        return false;
    }
    return true;
}

// Reject absolute paths and anything that escapes the history root.
QString pathForRelative(const QString &relative)
{
    if (relative.isEmpty() || QDir::isAbsolutePath(relative)) return {};
    QDir root(historyRoot());
    const QString rootPath = QDir::cleanPath(root.absolutePath());
    const QString path = QDir::cleanPath(root.absoluteFilePath(relative));
    if (path == rootPath || !path.startsWith(rootPath + QLatin1Char('/'))) return {};
    return path;
}

bool removeHistoryFile(const QString &relative)
{
    const QString path = pathForRelative(relative);
    if (path.isEmpty() || !QFileInfo::exists(path)) return true;
    if (QFile::remove(path)) return true;
    qWarning() << "history: cannot delete" << path;
    return false;
}

void sweepOrphans(QSqlDatabase &db)
{
    QSet<QString> referenced;
    {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT image_rel,thumb_rel FROM item"))) return;
        while (query.next()) {
            referenced.insert(query.value(0).toString());
            referenced.insert(query.value(1).toString());
        }
    }
    const QDir root(historyRoot());
    for (const QString &sub : {QStringLiteral("images"), QStringLiteral("thumbs")}) {
        const QDir dir(root.filePath(sub));
        for (const QFileInfo &file : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
            const QString relative = sub + QLatin1Char('/') + file.fileName();
            if (!referenced.contains(relative)) removeHistoryFile(relative);
        }
    }
}

class HistoryDb
{
public:
    ~HistoryDb() { close(); }

    bool open(QString *error)
    {
        if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
            if (error) *error = QObject::tr("缺少 SQLite 驱动");
            return false;
        }
        if (!ensureDirectories(error)) return false;
        m_name = QStringLiteral("lenssnip-history-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        m_db.setDatabaseName(databasePath());
        if (!m_db.open()) {
            if (error) *error = m_db.lastError().text();
            close();
            return false;
        }

        bool ok = true;
        {
            QSqlQuery query(m_db);
            query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            query.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
            query.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
            // shortcut: history schema is v1; add a user_version migration before changing columns.
            if (!query.exec(QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS item ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "kind TEXT NOT NULL,"
                    "created_ms INTEGER NOT NULL,"
                    "image_rel TEXT NOT NULL,"
                    "thumb_rel TEXT NOT NULL,"
                    "width INTEGER NOT NULL,"
                    "height INTEGER NOT NULL,"
                    "bytes INTEGER NOT NULL,"
                    "dpr REAL NOT NULL)"))) {
                if (error) *error = query.lastError().text();
                ok = false;
            }
            if (ok && !query.exec(QStringLiteral(
                    "CREATE INDEX IF NOT EXISTS idx_item_created ON item(created_ms DESC)"))) {
                if (error) *error = query.lastError().text();
                ok = false;
            }
        }
        if (!ok) close();
        return ok;
    }

    QSqlDatabase &db() { return m_db; }

private:
    void close()
    {
        if (m_db.isValid()) m_db.close();
        m_db = QSqlDatabase();
        if (!m_name.isEmpty()) {
            QSqlDatabase::removeDatabase(m_name);
            m_name.clear();
        }
    }

    QSqlDatabase m_db;
    QString m_name;
};

void prune(QSqlDatabase &db)
{
    LensSnipSettings settings;
    const int maxItems = qMax(0, settings.value(QStringLiteral("History/maxItems"), 100).toInt());
    const qint64 maxBytes = qMax<qint64>(0, settings.value(QStringLiteral("History/maxBytes"),
                                                          Q_INT64_C(1073741824)).toLongLong());
    const int maxAgeDays = qMax(0, settings.value(QStringLiteral("History/maxAgeDays"), 30).toInt());
    const qint64 cutoff = maxAgeDays > 0
            ? QDateTime::currentMSecsSinceEpoch() - qint64(maxAgeDays) * 86400000
            : 0;

    struct Row { qint64 id; qint64 created; qint64 bytes; QString imageRel; QString thumbRel; };
    QVector<Row> rows;
    {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral(
                "SELECT id,created_ms,bytes,image_rel,thumb_rel FROM item ORDER BY created_ms DESC")))
            return;
        while (query.next()) {
            rows.append({query.value(0).toLongLong(), query.value(1).toLongLong(),
                         query.value(2).toLongLong(), query.value(3).toString(),
                         query.value(4).toString()});
        }
    }

    qint64 totalBytes = 0;
    int kept = 0;
    QVector<qint64> removeIds;
    QStringList removeFiles;
    for (const Row &row : rows) {
        const bool overAge = maxAgeDays > 0 && row.created < cutoff;
        const bool overCount = maxItems > 0 && kept >= maxItems;
        const bool overBytes = maxBytes > 0 && totalBytes + row.bytes > maxBytes;
        if (overAge || overCount || overBytes) {
            removeIds.append(row.id);
            removeFiles << row.imageRel << row.thumbRel;
        } else {
            totalBytes += row.bytes;
            ++kept;
        }
    }
    if (removeIds.isEmpty()) return;

    bool deleted = false;
    if (db.transaction()) {
        QSqlQuery remove(db);
        bool ok = remove.prepare(QStringLiteral("DELETE FROM item WHERE id=?"));
        for (int i = 0; ok && i < removeIds.size(); ++i) {
            remove.bindValue(0, removeIds.at(i));
            ok = remove.exec();
            if (!ok) qWarning() << "history prune:" << remove.lastError().text();
        }
        deleted = ok && db.commit();
        if (!deleted) db.rollback();
    }
    if (!deleted) return;
    for (const QString &relative : removeFiles) removeHistoryFile(relative);
    sweepOrphans(db);
}

} // namespace

bool HistoryStore::enabled()
{
    return LensSnipSettings()
            .value(QStringLiteral("History/enabled"), false).toBool();
}

void HistoryStore::setEnabled(bool enabled)
{
    LensSnipSettings settings;
    settings.setValue(QStringLiteral("History/enabled"), enabled);
    settings.sync();
}

void HistoryStore::record(const QImage &image, const QString &kind)
{
    if (image.isNull() || !enabled() || !QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        return;
    if (pendingQueue().load() >= kMaxPendingWrites) {
        qWarning() << "history: queue full, dropping screenshot";
        return;
    }
    const QString normalizedKind = kind.isEmpty() ? QStringLiteral("capture") : kind;
    pendingQueue().fetch_add(1);
    historyPool().start([image, normalizedKind] {
        struct Done {
            ~Done() { pendingQueue().fetch_sub(1); }
        } done;
        if (!HistoryStore::enabled()) return;
        HistoryStore::writeItem(image, normalizedKind);
    });
}

bool HistoryStore::writeItem(const QImage &source, const QString &kind)
{
    QString error;
    if (!ensureDirectories(&error)) {
        qWarning() << "history:" << error;
        return false;
    }

    const QString stem = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))
            + QLatin1Char('-') + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    const QString imageRel = QStringLiteral("images/") + stem + QStringLiteral(".png");
    const QString thumbRel = QStringLiteral("thumbs/") + stem + QStringLiteral(".png");
    const QString imagePath = pathForRelative(imageRel);
    const QString thumbPath = pathForRelative(thumbRel);
    if (imagePath.isEmpty() || thumbPath.isEmpty()) return false;

    const qreal dpr = source.devicePixelRatio() > 0 ? source.devicePixelRatio() : 1.0;
    if (!saveImageAtomic(source, imagePath)) {
        qWarning() << "history: cannot write" << imagePath;
        return false;
    }

    QImage thumbnail = source;
    if (thumbnail.width() > 240 || thumbnail.height() > 160)
        thumbnail = thumbnail.scaled(240, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    thumbnail.setDevicePixelRatio(1.0);
    if (!saveImageAtomic(thumbnail, thumbPath)) {
        QFile::remove(imagePath);
        qWarning() << "history: cannot write" << thumbPath;
        return false;
    }

    HistoryDb db;
    if (!db.open(&error)) {
        QFile::remove(imagePath);
        QFile::remove(thumbPath);
        qWarning() << "history:" << error;
        return false;
    }

    bool inserted = false;
    {
        QSqlQuery query(db.db());
        query.prepare(QStringLiteral(
                "INSERT INTO item(kind,created_ms,image_rel,thumb_rel,width,height,bytes,dpr) "
                "VALUES(?,?,?,?,?,?,?,?)"));
        query.addBindValue(kind);
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        query.addBindValue(imageRel);
        query.addBindValue(thumbRel);
        query.addBindValue(source.width());
        query.addBindValue(source.height());
        query.addBindValue(QFileInfo(imagePath).size());
        query.addBindValue(dpr);
        inserted = query.exec();
        if (!inserted) qWarning() << "history:" << query.lastError().text();
    }
    if (inserted) prune(db.db());

    if (!inserted) {
        QFile::remove(imagePath);
        QFile::remove(thumbPath);
    }
    return inserted;
}

QVector<HistoryEntry> HistoryStore::entries(int limit, QString *error)
{
    QVector<HistoryEntry> result;
    HistoryDb db;
    QString openError;
    if (!db.open(&openError)) {
        if (error) *error = openError;
        if (!openError.isEmpty()) qWarning() << "history:" << openError;
        return result;
    }

    QString sql = QStringLiteral(
            "SELECT id,kind,created_ms,image_rel,thumb_rel,width,height,bytes,dpr "
            "FROM item ORDER BY created_ms DESC");
    if (limit > 0) sql += QStringLiteral(" LIMIT %1").arg(limit);
    QSqlQuery query(db.db());
    if (!query.exec(sql)) {
        if (error) *error = query.lastError().text();
        qWarning() << "history:" << query.lastError().text();
        return result;
    }
    while (query.next()) {
        HistoryEntry entry;
        entry.id = query.value(0).toLongLong();
        entry.kind = query.value(1).toString();
        entry.created = QDateTime::fromMSecsSinceEpoch(query.value(2).toLongLong(), QTimeZone::UTC);
        entry.imageRel = query.value(3).toString();
        entry.thumbRel = query.value(4).toString();
        entry.width = query.value(5).toInt();
        entry.height = query.value(6).toInt();
        entry.bytes = query.value(7).toLongLong();
        entry.dpr = query.value(8).toDouble();
        result.append(entry);
    }
    return result;
}

QImage HistoryStore::image(const HistoryEntry &entry, QString *error)
{
    const QString path = absoluteImagePath(entry);
    if (path.isEmpty()) {
        if (error) *error = QObject::tr("历史图片路径无效");
        return {};
    }
    QImage image(path);
    if (image.isNull()) {
        if (error) *error = QObject::tr("无法读取历史图片");
        return {};
    }
    image.setDevicePixelRatio(entry.dpr > 0 ? entry.dpr : 1.0);
    return image;
}

bool HistoryStore::remove(qint64 id)
{
    waitForPending();
    HistoryDb db;
    QString error;
    if (!db.open(&error)) return false;

    QStringList files;
    {
        QSqlQuery query(db.db());
        query.prepare(QStringLiteral("SELECT image_rel,thumb_rel FROM item WHERE id=?"));
        query.addBindValue(id);
        if (!query.exec()) {
            qWarning() << "history remove:" << query.lastError().text();
            return false;
        }
        if (query.next()) files << query.value(0).toString() << query.value(1).toString();
    }
    bool removed = false;
    {
        QSqlQuery query(db.db());
        query.prepare(QStringLiteral("DELETE FROM item WHERE id=?"));
        query.addBindValue(id);
        removed = query.exec() && query.numRowsAffected() > 0;
    }
    if (removed) {
        for (const QString &relative : files) removeHistoryFile(relative);
        sweepOrphans(db.db());
    }
    return removed;
}

bool HistoryStore::clear()
{
    waitForPending();
    HistoryDb db;
    QString error;
    if (!db.open(&error)) return false;

    QStringList files;
    {
        QSqlQuery query(db.db());
        if (!query.exec(QStringLiteral("SELECT image_rel,thumb_rel FROM item"))) {
            qWarning() << "history clear:" << query.lastError().text();
            return false;
        }
        while (query.next()) files << query.value(0).toString() << query.value(1).toString();
    }
    bool cleared = false;
    {
        QSqlQuery query(db.db());
        cleared = query.exec(QStringLiteral("DELETE FROM item"));
    }
    if (cleared) {
        for (const QString &relative : files) removeHistoryFile(relative);
        sweepOrphans(db.db());
    }
    return cleared;
}

QString HistoryStore::rootPath()
{
    return historyRoot();
}

QString HistoryStore::absoluteImagePath(const HistoryEntry &entry)
{
    return pathForRelative(entry.imageRel);
}

QString HistoryStore::absoluteThumbPath(const HistoryEntry &entry)
{
    return pathForRelative(entry.thumbRel);
}

int HistoryStore::pendingWrites()
{
    return pendingQueue().load();
}

void HistoryStore::waitForPending()
{
    historyPool().waitForDone();
}

int HistoryStore::testHistory()
{
    bool pass = true;
    const auto check = [&pass](bool condition, const char *message) {
        std::printf("%s history: %s\n", condition ? "PASS" : "FAIL", message);
        if (!condition) pass = false;
    };

    check(QSettings::defaultFormat() == QSettings::IniFormat, "settings use INI");
    const QString settingsFile = LensSnipSettings().fileName();
    check(settingsFile.endsWith(QStringLiteral("LensSnip.ini"), Qt::CaseInsensitive), "settings file is LensSnip.ini");
    check(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")), "SQLite driver available");
    check(QDir::fromNativeSeparators(rootPath()).contains(QStringLiteral("/LensSnip/"), Qt::CaseInsensitive),
          "default history root is under AppLocalData/LensSnip");

    QTemporaryDir temp;
    check(temp.isValid(), "temporary history root");
    if (!temp.isValid()) return 1;
    qputenv("LENSSNIP_HISTORY_ROOT", temp.path().toLocal8Bit());

    LensSnipSettings settings;
    const QVariant oldEnabled = settings.value(QStringLiteral("History/enabled"));
    const QVariant oldItems = settings.value(QStringLiteral("History/maxItems"));
    const QVariant oldBytes = settings.value(QStringLiteral("History/maxBytes"));
    const QVariant oldAge = settings.value(QStringLiteral("History/maxAgeDays"));
    if (!oldEnabled.isValid()) check(!enabled(), "history is off by default");
    settings.setValue(QStringLiteral("History/maxItems"), 100);
    settings.setValue(QStringLiteral("History/maxBytes"), Q_INT64_C(1073741824));
    settings.setValue(QStringLiteral("History/maxAgeDays"), 30);
    settings.setValue(QStringLiteral("History/enabled"), true);
    settings.sync();

    QImage image(320, 200, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#4c9bff")));
    image.setDevicePixelRatio(1.5);
    record(image);
    waitForPending();

    const QVector<HistoryEntry> first = entries();
    check(first.size() == 1, "one item recorded");
    if (first.size() == 1) {
        const HistoryEntry &entry = first.first();
        check(entry.width == 320 && entry.height == 200, "pixel dimensions");
        check(qAbs(entry.dpr - 1.5) < 0.001, "device pixel ratio");
        check(!absoluteImagePath(entry).isEmpty() && QFileInfo::exists(absoluteImagePath(entry)),
              "full image file");
        check(!absoluteThumbPath(entry).isEmpty() && QFileInfo::exists(absoluteThumbPath(entry)),
              "thumbnail file");
        const QImage loaded = HistoryStore::image(entry);
        check(!loaded.isNull() && loaded.size() == QSize(320, 200), "image round-trip");
    }

    settings.setValue(QStringLiteral("History/enabled"), false);
    settings.sync();
    record(image);
    waitForPending();
    check(entries().size() == 1, "disabled history writes nothing");

    settings.setValue(QStringLiteral("History/enabled"), true);
    settings.setValue(QStringLiteral("History/maxItems"), 1);
    settings.sync();
    record(image);
    record(image);
    waitForPending();
    check(entries().size() == 1, "item retention limit");

    settings.setValue(QStringLiteral("History/maxItems"), 100);
    settings.sync();
    record(image);
    record(image);
    waitForPending();
    const QVector<HistoryEntry> beforePrune = entries();
    QStringList prunedPaths;
    for (const HistoryEntry &entry : beforePrune) {
        prunedPaths << absoluteImagePath(entry) << absoluteThumbPath(entry);
    }
    check(beforePrune.size() == 3, "multiple rows available before prune");
    settings.setValue(QStringLiteral("History/maxItems"), 1);
    settings.sync();
    record(image);
    waitForPending();
    bool prunedFilesGone = true;
    for (const QString &path : prunedPaths)
        prunedFilesGone = prunedFilesGone && !path.isEmpty() && !QFileInfo::exists(path);
    check(entries().size() == 1 && prunedFilesGone, "prune removes every old row and file");
    check(clear(), "clear history");
    check(entries().isEmpty(), "history empty after clear");
    check(pendingWrites() == 0, "pending writes drain after the queue");
    QDir historyDir(rootPath());
    check(historyDir.mkpath(QStringLiteral("images")), "history image directory exists");
    const QString orphan = historyDir.filePath(QStringLiteral("images/orphan.png"));
    QFile orphanFile(orphan);
    if (!orphanFile.open(QIODevice::WriteOnly)) {
        check(false, "create an orphan history file");
    } else {
        orphanFile.write("orphan");
        orphanFile.close();
    }
    check(clear() && !QFileInfo::exists(orphan), "clear removes unreferenced history files");

    if (oldEnabled.isValid()) settings.setValue(QStringLiteral("History/enabled"), oldEnabled);
    else settings.remove(QStringLiteral("History/enabled"));
    if (oldItems.isValid()) settings.setValue(QStringLiteral("History/maxItems"), oldItems);
    else settings.remove(QStringLiteral("History/maxItems"));
    if (oldBytes.isValid()) settings.setValue(QStringLiteral("History/maxBytes"), oldBytes);
    else settings.remove(QStringLiteral("History/maxBytes"));
    if (oldAge.isValid()) settings.setValue(QStringLiteral("History/maxAgeDays"), oldAge);
    else settings.remove(QStringLiteral("History/maxAgeDays"));
    settings.sync();
    qunsetenv("LENSSNIP_HISTORY_ROOT");
    return pass ? 0 : 1;
}
