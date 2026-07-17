#include "persistence/EventStore.hpp"
#include "persistence/SimulationEventCodec.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <optional>

namespace bgptester
{
namespace
{

// raw_json is the canonical SimulationEvent representation. These columns
// are indexed/query projections and a compatibility fallback for legacy or
// incomplete raw_json values. Keep the enum and SELECT/INSERT lists aligned.
enum class EventSqlColumn : int
{
    Id,
    Timestamp,
    Event,
    Router,
    FromPeer,
    ToPeer,
    FromAs,
    ToAs,
    MessageType,
    Action,
    Sequence,
    Prefixes,
    Withdrawn,
    NextHop,
    AsPath,
    LocalPref,
    Med,
    RawJson,
};

constexpr int columnIndex(EventSqlColumn column)
{
    return static_cast<int>(column);
}

const QString& eventSelectClause()
{
    static const auto select = QStringLiteral("SELECT id,timestamp,event,router,from_peer,to_peer,from_as,to_as,"
                                              "msg_type,action,sequence,prefixes,withdrawn,next_hop,as_path,"
                                              "local_pref,med,raw_json FROM bmp_events");
    return select;
}

QVariant eventColumnValue(const QSqlQuery& query, EventSqlColumn column)
{
    return query.value(columnIndex(column));
}

QString joinedAsPath(const QVector<quint32>& path)
{
    QStringList values;
    values.reserve(path.size());
    for (const auto asn : path)
    {
        values.append(QString::number(asn));
    }
    return values.join(u' ');
}

QVector<quint32> splitAsPath(const QString& path)
{
    QVector<quint32> result;
    for (const auto& part : path.split(u' ', Qt::SkipEmptyParts))
    {
        bool ok = false;
        const auto asn = part.toUInt(&ok);
        if (ok)
        {
            result.append(asn);
        }
    }
    return result;
}

QString safeRunName(QString value)
{
    for (auto& ch : value)
    {
        if (!ch.isLetterOrNumber() && ch != u'-' && ch != u'_')
        {
            ch = u'_';
        }
    }
    value = value.trimmed();
    return value.isEmpty() ? QStringLiteral("bgp-lab") : value;
}

const QStringList& eventFilterExpressions()
{
    static const QStringList expressions{
        QStringLiteral("CAST(id AS TEXT)"),
        QStringLiteral("substr(timestamp, 12)"),
        QStringLiteral("COALESCE(event, '')"),
        QStringLiteral("COALESCE(router, '')"),
        QStringLiteral("COALESCE(from_peer, '')"),
        QStringLiteral("COALESCE(to_peer, '')"),
        QStringLiteral("COALESCE(CAST(from_as AS TEXT), '')"),
        QStringLiteral("COALESCE(CAST(to_as AS TEXT), '')"),
        QStringLiteral("COALESCE(msg_type, '')"),
        QStringLiteral("COALESCE(action, '')"),
        QStringLiteral("CASE WHEN sequence IS NULL OR sequence = 0 THEN '' ELSE CAST(sequence AS TEXT) END"),
        QStringLiteral("replace(COALESCE(prefixes, ''), ',', ', ')"),
        QStringLiteral("replace(COALESCE(withdrawn, ''), ',', ', ')"),
        QStringLiteral("COALESCE(next_hop, '')"),
        QStringLiteral("COALESCE(as_path, '')"),
        QStringLiteral("COALESCE(CAST(local_pref AS TEXT), '')"),
        QStringLiteral("COALESCE(CAST(med AS TEXT), '')"),
    };
    return expressions;
}

QString eventFilterPredicate()
{
    QStringList predicates;
    predicates.reserve(eventFilterExpressions().size());
    for (const auto& expression : eventFilterExpressions())
    {
        predicates.append(QStringLiteral("instr(lower(%1), lower(?)) > 0").arg(expression));
    }
    return QStringLiteral("(%1)").arg(predicates.join(QStringLiteral(" OR ")));
}

QString eventWhereClause(const QString& filter, const std::optional<quint64>& maxEventId = {}, const QString& exactEvent = {})
{
    QStringList predicates;
    if (maxEventId)
    {
        predicates.append(QStringLiteral("id <= ?"));
    }
    if (!exactEvent.isEmpty())
    {
        predicates.append(QStringLiteral("event = ?"));
    }
    if (!filter.isEmpty())
    {
        predicates.append(eventFilterPredicate());
    }
    return predicates.isEmpty() ? QString{} : QStringLiteral(" WHERE %1").arg(predicates.join(QStringLiteral(" AND ")));
}

void bindEventConditions(QSqlQuery& query, const QString& filter, const std::optional<quint64>& maxEventId = {},
                         const QString& exactEvent = {})
{
    if (maxEventId)
    {
        query.addBindValue(QVariant::fromValue(*maxEventId));
    }
    if (!exactEvent.isEmpty())
    {
        query.addBindValue(exactEvent);
    }
    if (!filter.isEmpty())
    {
        for (qsizetype index = 0; index < eventFilterExpressions().size(); ++index)
        {
            query.addBindValue(filter);
        }
    }
}

bool queryCancelled(const std::function<bool()>& cancelled, QString* error)
{
    if (!cancelled || !cancelled())
    {
        return false;
    }
    if (error)
    {
        *error = QStringLiteral("查询已取消");
    }
    return true;
}

bool querySingleCount(QSqlDatabase& database, const QString& filter, const std::optional<quint64>& maxEventId,
                      const QString& exactEvent, qint64* count, const QString& description, QString* error,
                      const std::function<bool()>& cancelled)
{
    if (queryCancelled(cancelled, error))
    {
        return false;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM bmp_events") + eventWhereClause(filter, maxEventId, exactEvent));
    bindEventConditions(query, filter, maxEventId, exactEvent);
    if (!query.exec() || !query.next())
    {
        if (error)
        {
            *error = QStringLiteral("%1失败：%2").arg(description, query.lastError().text());
        }
        return false;
    }
    if (queryCancelled(cancelled, error))
    {
        return false;
    }
    *count = query.value(0).toLongLong();
    return true;
}

bool queryEventCounts(QSqlDatabase& database, const QString& filter, const std::optional<quint64>& maxEventId,
                      EventHistoryPage* result, QString* error, const std::function<bool()>& cancelled)
{
    if (!querySingleCount(database, {}, maxEventId, {}, &result->totalCount, QStringLiteral("统计事件总数"), error, cancelled) ||
        !querySingleCount(database, {}, maxEventId, QStringLiteral("message_received"), &result->messageTotalCount,
                          QStringLiteral("统计报文总数"), error, cancelled))
    {
        return false;
    }
    if (filter.isEmpty())
    {
        result->filteredCount = result->totalCount;
        result->filteredMessageCount = result->messageTotalCount;
        return true;
    }
    return querySingleCount(database, filter, maxEventId, {}, &result->filteredCount, QStringLiteral("统计过滤后事件数"), error,
                            cancelled) &&
           querySingleCount(database, filter, maxEventId, QStringLiteral("message_received"), &result->filteredMessageCount,
                            QStringLiteral("统计过滤后报文数"), error, cancelled);
}

} // namespace

EventStore::EventStore(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<QVector<SimulationEvent>>();
    connectionName_ = uniqueConnectionName("bgptester-live");
}

EventStore::~EventStore()
{
    endRun();
}

bool EventStore::beginRun(const SimulationSettings& settings, QString* error)
{
    endRun();
    nextId_ = 1;
    lastInsertedEventId_ = 0;
    committedEventId_ = 0;
    ++runSerial_;

    QDir baseDirectory(settings.logDirectory);
    if (QDir::isRelativePath(settings.logDirectory))
    {
        baseDirectory = QDir(QDir::current().absoluteFilePath(settings.logDirectory));
    }
    if (!baseDirectory.exists() && !baseDirectory.mkpath(QStringLiteral(".")))
    {
        if (error)
        {
            *error = QStringLiteral("无法创建日志目录：%1").arg(baseDirectory.absolutePath());
        }
        return false;
    }

    const auto timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    auto leaf = QStringLiteral("%1_%2").arg(safeRunName(settings.name), timestamp);
    int suffix = 1;
    while (baseDirectory.exists(leaf))
    {
        leaf = QStringLiteral("%1_%2_%3").arg(safeRunName(settings.name), timestamp).arg(suffix++);
    }
    if (!baseDirectory.mkpath(leaf))
    {
        if (error)
        {
            *error = QStringLiteral("无法创建本次运行目录：%1").arg(baseDirectory.filePath(leaf));
        }
        return false;
    }
    runDirectory_ = QDir(baseDirectory.filePath(leaf)).absolutePath();
    databasePath_ = QDir(runDirectory_).filePath(QStringLiteral("bmp_collector.sqlite"));
    logFile_.setFileName(QDir(runDirectory_).filePath(QStringLiteral("bmp_collector.log")));
    if (!logFile_.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        if (error)
        {
            *error = QStringLiteral("无法创建日志文件：%1").arg(logFile_.errorString());
        }
        return false;
    }

    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    database_.setDatabaseName(databasePath_);
    if (!database_.open())
    {
        const auto message = QStringLiteral("无法创建 SQLite 日志：%1").arg(database_.lastError().text());
        if (error)
        {
            *error = message;
        }
        logFile_.close();
        database_ = {};
        QSqlDatabase::removeDatabase(connectionName_);
        return false;
    }
    if (!initializeSchema(error))
    {
        endRun();
        return false;
    }
    if (!prepareInsert(error))
    {
        endRun();
        return false;
    }
    transactionOpen_ = database_.transaction();
    pendingTransactionRows_ = 0;
    {
        QMutexLocker locker(&queueMutex_);
        acceptingEvents_ = true;
    }
    emit pathsChanged(logFile_.fileName(), databasePath_);
    return true;
}

void EventStore::endRun()
{
    {
        QMutexLocker locker(&queueMutex_);
        acceptingEvents_ = false;
        queueNotFull_.wakeAll();
    }
    drainAllPending();
    commitTransaction(false);
    if (logFile_.isOpen())
    {
        logFile_.flush();
    }
    insertQuery_ = QSqlQuery();
    if (database_.isValid())
    {
        database_.close();
        database_ = {};
        QSqlDatabase::removeDatabase(connectionName_);
    }
    if (logFile_.isOpen())
    {
        logFile_.close();
    }
    transactionOpen_ = false;
    pendingTransactionRows_ = 0;
}

void EventStore::flush()
{
    drainAllPending();
    commitTransaction(true);
    if (logFile_.isOpen())
    {
        logFile_.flush();
    }
}

void EventStore::enqueueEvents(QVector<SimulationEvent> events)
{
    qsizetype offset = 0;
    while (offset < events.size())
    {
        bool scheduleDrain = false;
        {
            QMutexLocker locker(&queueMutex_);
            while (acceptingEvents_ && static_cast<qsizetype>(pendingEvents_.size()) >= maxQueuedEvents_)
            {
                queueNotFull_.wait(&queueMutex_);
            }
            if (!acceptingEvents_)
            {
                return;
            }

            const auto available = maxQueuedEvents_ - static_cast<qsizetype>(pendingEvents_.size());
            const auto count = std::min(available, events.size() - offset);
            for (qsizetype index = 0; index < count; ++index)
            {
                pendingEvents_.push_back(std::move(events[offset + index]));
            }
            offset += count;
            if (!drainScheduled_)
            {
                drainScheduled_ = true;
                scheduleDrain = true;
            }
        }
        if (scheduleDrain)
        {
            QMetaObject::invokeMethod(this, &EventStore::drainQueue, Qt::QueuedConnection);
        }
    }
}

void EventStore::appendEvent(SimulationEvent event)
{
    QVector<SimulationEvent> events;
    events.append(std::move(event));
    enqueueEvents(std::move(events));
}

void EventStore::requestCountSnapshot(quint64 requestId)
{
    if (!database_.isOpen())
    {
        emit countSnapshotReady(requestId, runSerial_, QString{}, 0, QStringLiteral("当前没有打开的 BMP 日志数据库"));
        return;
    }

    QString error;
    if (!commitTransaction(true, &error))
    {
        emit countSnapshotReady(requestId, runSerial_, databasePath_, committedEventId_, error);
        return;
    }
    // Events still waiting in pendingEvents_ are deliberately not drained.
    // Their later eventsStored signals are ordered after this watermark and
    // are merged into the read-only count result by MainWindow.
    emit countSnapshotReady(requestId, runSerial_, databasePath_, committedEventId_, QString{});
}

void EventStore::drainQueue()
{
    QVector<SimulationEvent> batch;
    batch.reserve(writeBatchSize_);
    {
        QMutexLocker locker(&queueMutex_);
        const auto count = std::min(writeBatchSize_, static_cast<qsizetype>(pendingEvents_.size()));
        for (qsizetype index = 0; index < count; ++index)
        {
            batch.append(std::move(pendingEvents_.front()));
            pendingEvents_.pop_front();
        }
        queueNotFull_.wakeAll();
    }
    if (!batch.isEmpty())
    {
        persistBatch(std::move(batch));
    }

    bool scheduleAgain = false;
    {
        QMutexLocker locker(&queueMutex_);
        if (pendingEvents_.empty())
        {
            drainScheduled_ = false;
        }
        else
        {
            scheduleAgain = true;
        }
    }
    if (scheduleAgain)
    {
        QMetaObject::invokeMethod(this, &EventStore::drainQueue, Qt::QueuedConnection);
    }
}

void EventStore::persistBatch(QVector<SimulationEvent> events)
{
    QByteArray logBuffer;
    logBuffer.reserve(events.size() * 256);
    for (auto& event : events)
    {
        if (!event.timestamp.isValid())
        {
            event.timestamp = QDateTime::currentDateTime();
        }
        if (event.id == 0)
        {
            event.id = nextId_++;
        }
        else
        {
            nextId_ = std::max(nextId_, event.id + 1);
        }

        const auto rawJson = QJsonDocument(eventToJson(event)).toJson(QJsonDocument::Compact);
        logBuffer.append(rawJson);
        logBuffer.append('\n');
        if (database_.isOpen())
        {
            QString error;
            if (!insertEvent(event, rawJson, &error))
            {
                emit storeError(error);
            }
            else
            {
                lastInsertedEventId_ = std::max(lastInsertedEventId_, event.id);
                ++pendingTransactionRows_;
                if (pendingTransactionRows_ >= transactionBatchSize_)
                {
                    commitTransaction(true);
                }
            }
        }
    }
    if (logFile_.isOpen() && !logBuffer.isEmpty())
    {
        logFile_.write(logBuffer);
    }
    emit eventsStored(runSerial_, std::move(events));
}

void EventStore::drainAllPending()
{
    while (true)
    {
        QVector<SimulationEvent> batch;
        batch.reserve(writeBatchSize_);
        {
            QMutexLocker locker(&queueMutex_);
            const auto count = std::min(writeBatchSize_, static_cast<qsizetype>(pendingEvents_.size()));
            for (qsizetype index = 0; index < count; ++index)
            {
                batch.append(std::move(pendingEvents_.front()));
                pendingEvents_.pop_front();
            }
            if (pendingEvents_.empty())
            {
                drainScheduled_ = false;
            }
            queueNotFull_.wakeAll();
        }
        if (batch.isEmpty())
        {
            return;
        }
        persistBatch(std::move(batch));
    }
}

bool EventStore::commitTransaction(bool restart, QString* error)
{
    if (!database_.isOpen())
    {
        return false;
    }
    if (!transactionOpen_)
    {
        // Successful inserts performed without an explicit transaction are
        // already visible to read-only connections.
        committedEventId_ = std::max(committedEventId_, lastInsertedEventId_);
        if (!restart)
        {
            return true;
        }
        transactionOpen_ = database_.transaction();
        if (!transactionOpen_ && error)
        {
            *error = QStringLiteral("启动 SQLite 日志事务失败：%1").arg(database_.lastError().text());
        }
        return transactionOpen_;
    }
    bool success = true;
    if (!database_.commit())
    {
        success = false;
        const auto message = QStringLiteral("提交 SQLite 日志失败：%1").arg(database_.lastError().text());
        emit storeError(message);
        if (error)
        {
            *error = message;
        }
        database_.rollback();
        lastInsertedEventId_ = committedEventId_;
        transactionOpen_ = false;
    }
    else
    {
        committedEventId_ = std::max(committedEventId_, lastInsertedEventId_);
        transactionOpen_ = false;
    }
    if (restart)
    {
        transactionOpen_ = database_.transaction();
        if (!transactionOpen_)
        {
            success = false;
            const auto message = QStringLiteral("重新启动 SQLite 日志事务失败：%1").arg(database_.lastError().text());
            emit storeError(message);
            if (error && error->isEmpty())
            {
                *error = message;
            }
        }
    }
    pendingTransactionRows_ = 0;
    return success;
}

bool EventStore::initializeSchema(QString* error)
{
    QSqlQuery query(database_);
    const QStringList statements{
        QStringLiteral("PRAGMA journal_mode=WAL"),
        QStringLiteral("PRAGMA synchronous=NORMAL"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS bmp_events ("
                       "id INTEGER PRIMARY KEY, timestamp TEXT NOT NULL, event TEXT, "
                       "router TEXT, from_peer TEXT, to_peer TEXT, from_as INTEGER, "
                       "to_as INTEGER, msg_type TEXT, action TEXT, sequence INTEGER, "
                       "prefixes TEXT, withdrawn TEXT, next_hop TEXT, as_path TEXT, "
                       "local_pref INTEGER, med INTEGER, raw_json TEXT NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_bmp_router ON bmp_events(router)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_bmp_from ON bmp_events(from_peer)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_bmp_to ON bmp_events(to_peer)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_bmp_action ON bmp_events(action)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_bmp_event ON bmp_events(event)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_bmp_sequence ON bmp_events(sequence)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_bmp_from_as ON bmp_events(from_as)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_bmp_to_as ON bmp_events(to_as)"),
    };
    for (const auto& statement : statements)
    {
        if (!query.exec(statement))
        {
            if (error)
            {
                *error = QStringLiteral("初始化 SQLite schema 失败：%1").arg(query.lastError().text());
            }
            return false;
        }
    }
    return true;
}

bool EventStore::prepareInsert(QString* error)
{
    insertQuery_ = QSqlQuery(database_);
    if (insertQuery_.prepare(QStringLiteral("INSERT INTO bmp_events "
                                            "(id,timestamp,event,router,from_peer,to_peer,from_as,to_as,msg_type,"
                                            "action,sequence,prefixes,withdrawn,next_hop,as_path,local_pref,med,"
                                            "raw_json) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")))
    {
        return true;
    }
    if (error)
    {
        *error = QStringLiteral("准备 SQLite 写入语句失败：%1").arg(insertQuery_.lastError().text());
    }
    return false;
}

bool EventStore::insertEvent(const SimulationEvent& event, const QByteArray& rawJson, QString* error)
{
    insertQuery_.bindValue(columnIndex(EventSqlColumn::Id), static_cast<qulonglong>(event.id));
    insertQuery_.bindValue(columnIndex(EventSqlColumn::Timestamp),
                           event.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")));
    insertQuery_.bindValue(columnIndex(EventSqlColumn::Event), event.event);
    insertQuery_.bindValue(columnIndex(EventSqlColumn::Router), event.router);
    insertQuery_.bindValue(columnIndex(EventSqlColumn::FromPeer), event.from);
    insertQuery_.bindValue(columnIndex(EventSqlColumn::ToPeer), event.to);
    insertQuery_.bindValue(columnIndex(EventSqlColumn::FromAs), event.fromAs ? QVariant::fromValue(*event.fromAs) : QVariant{});
    insertQuery_.bindValue(columnIndex(EventSqlColumn::ToAs), event.toAs ? QVariant::fromValue(*event.toAs) : QVariant{});
    insertQuery_.bindValue(columnIndex(EventSqlColumn::MessageType), event.messageType);
    insertQuery_.bindValue(columnIndex(EventSqlColumn::Action), event.action);
    insertQuery_.bindValue(columnIndex(EventSqlColumn::Sequence), static_cast<qulonglong>(event.sequence));
    insertQuery_.bindValue(columnIndex(EventSqlColumn::Prefixes), event.prefixes.join(u','));
    insertQuery_.bindValue(columnIndex(EventSqlColumn::Withdrawn), event.withdrawn.join(u','));
    insertQuery_.bindValue(columnIndex(EventSqlColumn::NextHop), event.nextHop);
    insertQuery_.bindValue(columnIndex(EventSqlColumn::AsPath), joinedAsPath(event.asPath));
    insertQuery_.bindValue(columnIndex(EventSqlColumn::LocalPref),
                           event.localPref ? QVariant::fromValue(*event.localPref) : QVariant{});
    insertQuery_.bindValue(columnIndex(EventSqlColumn::Med), event.med ? QVariant::fromValue(*event.med) : QVariant{});
    insertQuery_.bindValue(columnIndex(EventSqlColumn::RawJson), QString::fromUtf8(rawJson));
    if (!insertQuery_.exec())
    {
        if (error)
        {
            *error = QStringLiteral("写入 SQLite 日志失败：%1").arg(insertQuery_.lastError().text());
        }
        return false;
    }
    insertQuery_.finish();
    return true;
}

QJsonObject EventStore::eventToJson(const SimulationEvent& event)
{
    return SimulationEventCodec::toJson(event);
}

EventHistoryPage EventStore::queryDatabase(const QString& path, int limit, const QString& filter, QString* error,
                                           const std::function<bool(qsizetype, qsizetype)>& progress,
                                           const std::function<bool()>& cancelled)
{
    EventHistoryPage result;
    if (error)
    {
        error->clear();
    }
    const auto connection = uniqueConnectionName("bgptester-history");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        if (!database.open())
        {
            if (error)
            {
                *error = QStringLiteral("无法打开历史日志：%1").arg(database.lastError().text());
            }
        }
        else
        {
            const auto transactionStarted = database.transaction();
            if (!transactionStarted)
            {
                if (error)
                {
                    *error = QStringLiteral("无法创建历史日志只读快照：%1").arg(database.lastError().text());
                }
            }
            else if (queryEventCounts(database, filter, {}, &result, error, cancelled))
            {
                const auto rowsToLoad = limit > 0 ? std::min(result.filteredCount, static_cast<qint64>(limit)) : result.filteredCount;
                const auto continueLoading = !queryCancelled(cancelled, error) &&
                                             (!progress || progress(0, static_cast<qsizetype>(rowsToLoad)));
                if (continueLoading)
                {
                    QSqlQuery query(database);
                    const auto& select = eventSelectClause();
                    const auto where = eventWhereClause(filter);
                    if (limit > 0)
                    {
                        query.prepare(select + where + QStringLiteral(" ORDER BY id DESC LIMIT ?"));
                    }
                    else
                    {
                        query.prepare(select + where + QStringLiteral(" ORDER BY id ASC"));
                    }
                    if (!filter.isEmpty())
                    {
                        bindEventConditions(query, filter);
                    }
                    if (limit > 0)
                    {
                        query.addBindValue(limit);
                    }
                    if (!query.exec())
                    {
                        if (error)
                        {
                            *error = QStringLiteral("查询历史日志失败：%1").arg(query.lastError().text());
                        }
                    }
                    else
                    {
                        result.events.reserve(static_cast<qsizetype>(rowsToLoad));
                        qsizetype loadedRows = 0;
                        while (query.next())
                        {
                            if (queryCancelled(cancelled, error))
                            {
                                result.events.clear();
                                break;
                            }
                            result.events.append(eventFromQuery(query));
                            ++loadedRows;
                            if (progress && loadedRows % 512 == 0 &&
                                !progress(loadedRows, static_cast<qsizetype>(rowsToLoad)))
                            {
                                result.events.clear();
                                break;
                            }
                        }
                        if (limit > 0)
                        {
                            std::reverse(result.events.begin(), result.events.end());
                        }
                        if (progress)
                        {
                            progress(loadedRows, static_cast<qsizetype>(rowsToLoad));
                        }
                    }
                }
            }
            if (transactionStarted)
            {
                database.rollback();
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

EventHistoryPage EventStore::countDatabase(const QString& path, const QString& filter, quint64 maxEventId, QString* error,
                                           const std::function<bool()>& cancelled)
{
    EventHistoryPage result;
    if (error)
    {
        error->clear();
    }
    const auto connection = uniqueConnectionName("bgptester-count");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        if (!database.open())
        {
            if (error)
            {
                *error = QStringLiteral("无法打开实时日志计数快照：%1").arg(database.lastError().text());
            }
        }
        else
        {
            const auto transactionStarted = database.transaction();
            if (!transactionStarted)
            {
                if (error)
                {
                    *error = QStringLiteral("无法创建实时日志只读快照：%1").arg(database.lastError().text());
                }
            }
            else
            {
                queryEventCounts(database, filter, maxEventId, &result, error, cancelled);
                database.rollback();
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

ConvergenceHistoryPage EventStore::queryConvergenceDatabase(const QString& path, int limit, QString* error,
                                                             const std::function<bool()>& cancelled)
{
    ConvergenceHistoryPage result;
    if (error)
    {
        error->clear();
    }
    const auto connection = uniqueConnectionName("bgptester-convergence-history");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        if (!database.open())
        {
            if (error)
            {
                *error = QStringLiteral("无法打开收敛历史：%1").arg(database.lastError().text());
            }
        }
        else
        {
            const auto transactionStarted = database.transaction();
            if (!transactionStarted)
            {
                if (error)
                {
                    *error = QStringLiteral("无法创建收敛历史只读快照：%1").arg(database.lastError().text());
                }
            }
            else if (querySingleCount(database, {}, {}, QStringLiteral("converged"), &result.totalCount,
                                      QStringLiteral("统计收敛记录总数"), error, cancelled) &&
                     !queryCancelled(cancelled, error))
            {
                QSqlQuery query(database);
                const auto& select = eventSelectClause();
                query.prepare(select + eventWhereClause({}, {}, QStringLiteral("converged")) +
                              QStringLiteral(" ORDER BY id DESC LIMIT ?"));
                bindEventConditions(query, {}, {}, QStringLiteral("converged"));
                query.addBindValue(std::max(0, limit));
                if (!query.exec())
                {
                    if (error)
                    {
                        *error = QStringLiteral("查询收敛历史失败：%1").arg(query.lastError().text());
                    }
                }
                else
                {
                    result.events.reserve(static_cast<qsizetype>(std::min(result.totalCount, static_cast<qint64>(std::max(0, limit)))));
                    while (query.next())
                    {
                        if (queryCancelled(cancelled, error))
                        {
                            result.events.clear();
                            break;
                        }
                        result.events.append(eventFromQuery(query));
                    }
                    std::reverse(result.events.begin(), result.events.end());
                }
            }
            if (transactionStarted)
            {
                database.rollback();
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

QVector<SimulationEvent> EventStore::readDatabase(const QString& path, int limit, QString* error,
                                                  const std::function<bool(qsizetype, qsizetype)>& progress)
{
    return queryDatabase(path, limit, {}, error, progress).events;
}

SimulationEvent EventStore::eventFromQuery(const QSqlQuery& query)
{
    const auto rawJson = eventColumnValue(query, EventSqlColumn::RawJson).toByteArray();
    if (auto decoded = SimulationEventCodec::fromJson(rawJson))
    {
        return std::move(*decoded);
    }

    // Compatibility path for legacy, incomplete, or malformed raw_json.
    // Indexed SQL projections recover the established fields; details are
    // still restored opportunistically from any usable JSON object.
    SimulationEvent event;
    event.id = eventColumnValue(query, EventSqlColumn::Id).toULongLong();
    event.timestamp = QDateTime::fromString(eventColumnValue(query, EventSqlColumn::Timestamp).toString(),
                                            QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    event.event = eventColumnValue(query, EventSqlColumn::Event).toString();
    event.router = eventColumnValue(query, EventSqlColumn::Router).toString();
    event.from = eventColumnValue(query, EventSqlColumn::FromPeer).toString();
    event.to = eventColumnValue(query, EventSqlColumn::ToPeer).toString();
    const auto fromAs = eventColumnValue(query, EventSqlColumn::FromAs);
    if (!fromAs.isNull())
    {
        event.fromAs = fromAs.toUInt();
    }
    const auto toAs = eventColumnValue(query, EventSqlColumn::ToAs);
    if (!toAs.isNull())
    {
        event.toAs = toAs.toUInt();
    }
    event.messageType = eventColumnValue(query, EventSqlColumn::MessageType).toString();
    event.action = eventColumnValue(query, EventSqlColumn::Action).toString();
    event.sequence = eventColumnValue(query, EventSqlColumn::Sequence).toULongLong();
    event.prefixes = eventColumnValue(query, EventSqlColumn::Prefixes).toString().split(u',', Qt::SkipEmptyParts);
    event.withdrawn = eventColumnValue(query, EventSqlColumn::Withdrawn).toString().split(u',', Qt::SkipEmptyParts);
    event.nextHop = eventColumnValue(query, EventSqlColumn::NextHop).toString();
    event.asPath = splitAsPath(eventColumnValue(query, EventSqlColumn::AsPath).toString());
    const auto localPref = eventColumnValue(query, EventSqlColumn::LocalPref);
    if (!localPref.isNull())
    {
        event.localPref = localPref.toUInt();
    }
    const auto med = eventColumnValue(query, EventSqlColumn::Med);
    if (!med.isNull())
    {
        event.med = med.toUInt();
    }
    const auto rawDocument = QJsonDocument::fromJson(rawJson);
    if (rawDocument.isObject())
    {
        const auto details = rawDocument.object().value(QStringLiteral("details")).toObject();
        for (auto it = details.constBegin(); it != details.constEnd(); ++it)
        {
            event.details.insert(it.key(), it.value().toVariant().toString());
        }
    }
    return event;
}

QString EventStore::uniqueConnectionName(const char* prefix)
{
    static std::atomic<quint64> sequence{0};
    return QStringLiteral("%1-%2-%3")
        .arg(QString::fromLatin1(prefix))
        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()))
        .arg(++sequence);
}

} // namespace bgptester
