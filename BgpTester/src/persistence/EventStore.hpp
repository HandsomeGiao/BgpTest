#pragma once

#include "engine/BgpTypes.hpp"
#include "model/Topology.hpp"

#include <QFile>
#include <QMutex>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QThreadPool>
#include <QVector>
#include <QWaitCondition>

#include <deque>
#include <functional>

namespace bgptester
{

struct EventHistoryPage
{
    QVector<SimulationEvent> events;
    qint64 totalCount = 0;
    qint64 filteredCount = 0;
    qint64 messageTotalCount = 0;
    qint64 filteredMessageCount = 0;
    quint64 maxEventId = 0;
};

struct ConvergenceHistoryPage
{
    QVector<SimulationEvent> events;
    qint64 totalCount = 0;
    quint64 maxEventId = 0;
};

class EventStore final : public QObject
{
    Q_OBJECT

public:
    explicit EventStore(QObject* parent = nullptr);
    ~EventStore() override;

    bool beginRun(const SimulationSettings& settings, QString* error = nullptr);
    void endRun();
    void flush();

    bool isOpen() const
    {
        return database_.isOpen();
    }
    QString runDirectory() const
    {
        return runDirectory_;
    }
    QString logFilePath() const
    {
        return logFile_.fileName();
    }
    QString databasePath() const
    {
        return databasePath_;
    }
    quint64 runSerial() const
    {
        return runSerial_;
    }
    quint64 committedEventId() const
    {
        return committedEventId_;
    }
    QString lastError() const
    {
        return lastError_;
    }
    int encodingWorkerCount() const
    {
        return encodingPool_.maxThreadCount();
    }
    static EventHistoryPage queryDatabase(const QString& path, int limit, const QString& filter = {}, QString* error = nullptr,
                                          const std::function<bool(qsizetype, qsizetype)>& progress = {},
                                          const std::function<bool()>& cancelled = {});
    static EventHistoryPage countDatabase(const QString& path, const QString& filter, quint64 maxEventId,
                                          QString* error = nullptr, const std::function<bool()>& cancelled = {});
    static ConvergenceHistoryPage queryConvergenceDatabase(const QString& path, int limit, QString* error = nullptr,
                                                            const std::function<bool()>& cancelled = {});
    static QVector<SimulationEvent> readDatabase(const QString& path, int limit, QString* error = nullptr,
                                                 const std::function<bool(qsizetype, qsizetype)>& progress = {});
    static QJsonObject eventToJson(const SimulationEvent& event);

public slots:
    // Thread-safe. The simulation thread calls this directly; only the small
    // in-memory queue is touched there. SQLite and JSONL I/O stay on the
    // EventStore object's worker thread.
    void enqueueEvents(QVector<bgptester::SimulationEvent> events);
    void appendEvent(bgptester::SimulationEvent event);
    void requestCountSnapshot(quint64 requestId);

signals:
    void eventsStored(quint64 runSerial, QVector<bgptester::SimulationEvent> events);
    void countSnapshotReady(quint64 requestId, quint64 runSerial, QString databasePath, quint64 maxEventId, QString error);
    void storeError(QString message);
    void pathsChanged(QString logFile, QString databaseFile);

private slots:
    void drainQueue();

private:
    bool initializeSchema(QString* error);
    bool prepareInsert(QString* error);
    bool insertEvent(const SimulationEvent& event, const QByteArray& rawJson, QString* error);
    void persistBatch(QVector<SimulationEvent> events);
    void drainAllPending();
    bool commitTransaction(bool restart, QString* error = nullptr);
    static SimulationEvent eventFromQuery(const QSqlQuery& query);
    static QString uniqueConnectionName(const char* prefix);

    QString connectionName_;
    QSqlDatabase database_;
    QSqlQuery insertQuery_;
    QFile logFile_;
    QString runDirectory_;
    QString databasePath_;
    quint64 nextId_ = 1;
    quint64 runSerial_ = 0;
    quint64 lastInsertedEventId_ = 0;
    quint64 committedEventId_ = 0;
    QString lastError_;
    int pendingTransactionRows_ = 0;
    bool transactionOpen_ = false;
    QThreadPool encodingPool_;

    QMutex queueMutex_;
    QWaitCondition queueNotFull_;
    std::deque<SimulationEvent> pendingEvents_;
    bool acceptingEvents_ = false;
    bool drainScheduled_ = false;

    static constexpr qsizetype maxQueuedEvents_ = 65536;
    static constexpr qsizetype writeBatchSize_ = 4096;
    static constexpr int transactionBatchSize_ = 16384;
};

} // namespace bgptester
