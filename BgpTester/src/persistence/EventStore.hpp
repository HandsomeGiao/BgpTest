#pragma once

#include "engine/BgpTypes.hpp"
#include "model/Topology.hpp"

#include <QFile>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QVector>

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
    void clearRecent();

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
    const QVector<SimulationEvent>& recentEvents() const
    {
        return recentEvents_;
    }

    static QVector<SimulationEvent> readDatabase(const QString& path, int limit, QString* error = nullptr);
    static QJsonObject eventToJson(const SimulationEvent& event);

public slots:
    void appendEvent(bgptester::SimulationEvent event);

signals:
    void eventStored(bgptester::SimulationEvent event);
    void storeError(QString message);
    void pathsChanged(QString logFile, QString databaseFile);

private:
    bool initializeSchema(QString* error);
    bool insertEvent(const SimulationEvent& event, const QByteArray& rawJson, QString* error);
    static SimulationEvent eventFromQuery(const QSqlQuery& query);
    static QString uniqueConnectionName(const char* prefix);

    QString connectionName_;
    QSqlDatabase database_;
    QFile logFile_;
    QString runDirectory_;
    QString databasePath_;
    QVector<SimulationEvent> recentEvents_;
    quint64 nextId_ = 1;
    int pendingTransactionRows_ = 0;
    bool transactionOpen_ = false;
    static constexpr qsizetype recentCapacity_ = 20000;
};

} // namespace bgptester
