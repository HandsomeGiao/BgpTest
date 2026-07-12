#pragma once

#include "engine/BgpTypes.hpp"
#include "model/Topology.hpp"

#include <QFile>
#include <QMutex>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QVector>
#include <QWaitCondition>

#include <deque>
#include <functional>

namespace bgptester
{

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
    static QVector<SimulationEvent> readDatabase(const QString& path, int limit, QString* error = nullptr,
                                                 const std::function<bool(qsizetype, qsizetype)>& progress = {});
    static QJsonObject eventToJson(const SimulationEvent& event);

public slots:
    // Thread-safe. The simulation thread calls this directly; only the small
    // in-memory queue is touched there. SQLite and JSONL I/O stay on the
    // EventStore object's worker thread.
    void enqueueEvents(QVector<bgptester::SimulationEvent> events);
    void appendEvent(bgptester::SimulationEvent event);

signals:
    void eventsStored(quint64 runSerial, QVector<bgptester::SimulationEvent> events);
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
    void commitTransaction(bool restart);
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
    int pendingTransactionRows_ = 0;
    bool transactionOpen_ = false;

    QMutex queueMutex_;
    QWaitCondition queueNotFull_;
    std::deque<SimulationEvent> pendingEvents_;
    bool acceptingEvents_ = false;
    bool drainScheduled_ = false;

    static constexpr qsizetype maxQueuedEvents_ = 8192;
    static constexpr qsizetype writeBatchSize_ = 512;
    static constexpr int transactionBatchSize_ = 4096;
};

} // namespace bgptester
