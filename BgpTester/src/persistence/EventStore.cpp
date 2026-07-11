#include "persistence/EventStore.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>

namespace bgptester {
namespace {

QString joinedAsPath(const QVector<quint32> &path) {
  QStringList values;
  values.reserve(path.size());
  for (const auto asn : path) {
    values.append(QString::number(asn));
  }
  return values.join(u' ');
}

QVector<quint32> splitAsPath(const QString &path) {
  QVector<quint32> result;
  for (const auto &part : path.split(u' ', Qt::SkipEmptyParts)) {
    bool ok = false;
    const auto asn = part.toUInt(&ok);
    if (ok) {
      result.append(asn);
    }
  }
  return result;
}

QString safeRunName(QString value) {
  for (auto &ch : value) {
    if (!ch.isLetterOrNumber() && ch != u'-' && ch != u'_') {
      ch = u'_';
    }
  }
  value = value.trimmed();
  return value.isEmpty() ? QStringLiteral("bgp-lab") : value;
}

} // namespace

EventStore::EventStore(QObject *parent) : QObject(parent) {
  connectionName_ = uniqueConnectionName("bgptester-live");
}

EventStore::~EventStore() { endRun(); }

bool EventStore::beginRun(const SimulationSettings &settings, QString *error) {
  endRun();
  clearRecent();
  nextId_ = 1;

  QDir baseDirectory(settings.logDirectory);
  if (QDir::isRelativePath(settings.logDirectory)) {
    baseDirectory = QDir(QDir::current().absoluteFilePath(
        settings.logDirectory));
  }
  if (!baseDirectory.exists() && !baseDirectory.mkpath(QStringLiteral("."))) {
    if (error) {
      *error = QStringLiteral("无法创建日志目录：%1")
                   .arg(baseDirectory.absolutePath());
    }
    return false;
  }

  const auto timestamp =
      QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
  auto leaf = QStringLiteral("%1_%2").arg(safeRunName(settings.name), timestamp);
  int suffix = 1;
  while (baseDirectory.exists(leaf)) {
    leaf = QStringLiteral("%1_%2_%3")
               .arg(safeRunName(settings.name), timestamp)
               .arg(suffix++);
  }
  if (!baseDirectory.mkpath(leaf)) {
    if (error) {
      *error = QStringLiteral("无法创建本次运行目录：%1")
                   .arg(baseDirectory.filePath(leaf));
    }
    return false;
  }
  runDirectory_ = QDir(baseDirectory.filePath(leaf)).absolutePath();
  databasePath_ = QDir(runDirectory_).filePath(
      QStringLiteral("bmp_collector.sqlite"));
  logFile_.setFileName(
      QDir(runDirectory_).filePath(QStringLiteral("bmp_collector.log")));
  if (!logFile_.open(QIODevice::WriteOnly | QIODevice::Text)) {
    if (error) {
      *error = QStringLiteral("无法创建日志文件：%1")
                   .arg(logFile_.errorString());
    }
    return false;
  }

  database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                         connectionName_);
  database_.setDatabaseName(databasePath_);
  if (!database_.open()) {
    const auto message = QStringLiteral("无法创建 SQLite 日志：%1")
                             .arg(database_.lastError().text());
    if (error) {
      *error = message;
    }
    logFile_.close();
    database_ = {};
    QSqlDatabase::removeDatabase(connectionName_);
    return false;
  }
  if (!initializeSchema(error)) {
    endRun();
    return false;
  }
  transactionOpen_ = database_.transaction();
  pendingTransactionRows_ = 0;
  emit pathsChanged(logFile_.fileName(), databasePath_);
  return true;
}

void EventStore::endRun() {
  flush();
  if (database_.isValid()) {
    database_.close();
    database_ = {};
    QSqlDatabase::removeDatabase(connectionName_);
  }
  if (logFile_.isOpen()) {
    logFile_.close();
  }
  transactionOpen_ = false;
  pendingTransactionRows_ = 0;
}

void EventStore::flush() {
  if (database_.isOpen() && transactionOpen_) {
    if (!database_.commit()) {
      emit storeError(QStringLiteral("提交 SQLite 日志失败：%1")
                          .arg(database_.lastError().text()));
    }
    transactionOpen_ = database_.transaction();
    pendingTransactionRows_ = 0;
  }
  if (logFile_.isOpen()) {
    logFile_.flush();
  }
}

void EventStore::clearRecent() { recentEvents_.clear(); }

void EventStore::appendEvent(SimulationEvent event) {
  if (!event.timestamp.isValid()) {
    event.timestamp = QDateTime::currentDateTime();
  }
  if (event.id == 0) {
    event.id = nextId_++;
  } else {
    nextId_ = std::max(nextId_, event.id + 1);
  }

  const auto rawJson = QJsonDocument(eventToJson(event)).toJson(
      QJsonDocument::Compact);
  if (logFile_.isOpen()) {
    logFile_.write(rawJson);
    logFile_.write("\n");
  }
  if (database_.isOpen()) {
    QString error;
    if (!insertEvent(event, rawJson, &error)) {
      emit storeError(error);
    } else {
      ++pendingTransactionRows_;
      if (pendingTransactionRows_ >= 256) {
        flush();
      }
    }
  }

  recentEvents_.append(event);
  if (recentEvents_.size() > recentCapacity_) {
    recentEvents_.remove(0, recentEvents_.size() - recentCapacity_);
  }
  emit eventStored(event);
}

bool EventStore::initializeSchema(QString *error) {
  QSqlQuery query(database_);
  const QStringList statements{
      QStringLiteral(
          "CREATE TABLE IF NOT EXISTS bmp_events ("
          "id INTEGER PRIMARY KEY, timestamp TEXT NOT NULL, event TEXT, "
          "router TEXT, from_peer TEXT, to_peer TEXT, from_as INTEGER, "
          "to_as INTEGER, msg_type TEXT, action TEXT, sequence INTEGER, "
          "prefixes TEXT, withdrawn TEXT, next_hop TEXT, as_path TEXT, "
          "local_pref INTEGER, med INTEGER, raw_json TEXT NOT NULL)"),
      QStringLiteral(
          "CREATE INDEX IF NOT EXISTS idx_bmp_router ON bmp_events(router)"),
      QStringLiteral(
          "CREATE INDEX IF NOT EXISTS idx_bmp_from ON bmp_events(from_peer)"),
      QStringLiteral(
          "CREATE INDEX IF NOT EXISTS idx_bmp_to ON bmp_events(to_peer)"),
      QStringLiteral(
          "CREATE INDEX IF NOT EXISTS idx_bmp_action ON bmp_events(action)"),
      QStringLiteral(
          "CREATE INDEX IF NOT EXISTS idx_bmp_sequence ON bmp_events(sequence)"),
      QStringLiteral(
          "CREATE INDEX IF NOT EXISTS idx_bmp_from_as ON bmp_events(from_as)"),
      QStringLiteral(
          "CREATE INDEX IF NOT EXISTS idx_bmp_to_as ON bmp_events(to_as)"),
  };
  for (const auto &statement : statements) {
    if (!query.exec(statement)) {
      if (error) {
        *error = QStringLiteral("初始化 SQLite schema 失败：%1")
                     .arg(query.lastError().text());
      }
      return false;
    }
  }
  return true;
}

bool EventStore::insertEvent(const SimulationEvent &event,
                             const QByteArray &rawJson, QString *error) {
  QSqlQuery query(database_);
  query.prepare(QStringLiteral(
      "INSERT INTO bmp_events "
      "(id,timestamp,event,router,from_peer,to_peer,from_as,to_as,msg_type,"
      "action,sequence,prefixes,withdrawn,next_hop,as_path,local_pref,med,"
      "raw_json) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
  query.addBindValue(static_cast<qulonglong>(event.id));
  query.addBindValue(event.timestamp.toString(
      QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")));
  query.addBindValue(event.event);
  query.addBindValue(event.router);
  query.addBindValue(event.from);
  query.addBindValue(event.to);
  query.addBindValue(event.fromAs ? QVariant::fromValue(*event.fromAs)
                                  : QVariant{});
  query.addBindValue(event.toAs ? QVariant::fromValue(*event.toAs)
                                : QVariant{});
  query.addBindValue(event.messageType);
  query.addBindValue(event.action);
  query.addBindValue(static_cast<qulonglong>(event.sequence));
  query.addBindValue(event.prefixes.join(u','));
  query.addBindValue(event.withdrawn.join(u','));
  query.addBindValue(event.nextHop);
  query.addBindValue(joinedAsPath(event.asPath));
  query.addBindValue(event.localPref
                         ? QVariant::fromValue(*event.localPref)
                         : QVariant{});
  query.addBindValue(event.med ? QVariant::fromValue(*event.med)
                               : QVariant{});
  query.addBindValue(QString::fromUtf8(rawJson));
  if (!query.exec()) {
    if (error) {
      *error = QStringLiteral("写入 SQLite 日志失败：%1")
                   .arg(query.lastError().text());
    }
    return false;
  }
  return true;
}

QJsonObject EventStore::eventToJson(const SimulationEvent &event) {
  QJsonArray path;
  for (const auto asn : event.asPath) {
    path.append(static_cast<qint64>(asn));
  }
  QJsonArray prefixes;
  for (const auto &prefix : event.prefixes) {
    prefixes.append(prefix);
  }
  QJsonArray withdrawn;
  for (const auto &prefix : event.withdrawn) {
    withdrawn.append(prefix);
  }
  QJsonObject details;
  for (auto it = event.details.cbegin(); it != event.details.cend(); ++it) {
    details.insert(it.key(), it.value());
  }
  QJsonObject object{
      {QStringLiteral("id"), static_cast<qint64>(event.id)},
      {QStringLiteral("timestamp"),
       event.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))},
      {QStringLiteral("event"), event.event},
      {QStringLiteral("router"), event.router},
      {QStringLiteral("from"), event.from},
      {QStringLiteral("to"), event.to},
      {QStringLiteral("msg_type"), event.messageType},
      {QStringLiteral("action"), event.action},
      {QStringLiteral("sequence"), static_cast<qint64>(event.sequence)},
      {QStringLiteral("prefixes"), prefixes},
      {QStringLiteral("withdrawn"), withdrawn},
      {QStringLiteral("next_hop"), event.nextHop},
      {QStringLiteral("as_path"), path},
      {QStringLiteral("details"), details},
  };
  if (event.fromAs) {
    object.insert(QStringLiteral("from_as"), static_cast<qint64>(*event.fromAs));
  }
  if (event.toAs) {
    object.insert(QStringLiteral("to_as"), static_cast<qint64>(*event.toAs));
  }
  if (event.localPref) {
    object.insert(QStringLiteral("local_pref"),
                  static_cast<qint64>(*event.localPref));
  }
  if (event.med) {
    object.insert(QStringLiteral("med"), static_cast<qint64>(*event.med));
  }
  return object;
}

QVector<SimulationEvent> EventStore::readDatabase(const QString &path,
                                                   int limit,
                                                   QString *error) {
  QVector<SimulationEvent> result;
  const auto connection = uniqueConnectionName("bgptester-history");
  {
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                              connection);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    database.setDatabaseName(path);
    if (!database.open()) {
      if (error) {
        *error = QStringLiteral("无法打开历史日志：%1")
                     .arg(database.lastError().text());
      }
    } else {
      QSqlQuery query(database);
      query.prepare(QStringLiteral(
          "SELECT id,timestamp,event,router,from_peer,to_peer,from_as,to_as,"
          "msg_type,action,sequence,prefixes,withdrawn,next_hop,as_path,"
          "local_pref,med,raw_json FROM bmp_events ORDER BY id DESC LIMIT ?"));
      query.addBindValue(std::clamp(limit, 1, 100000));
      if (!query.exec()) {
        if (error) {
          *error = QStringLiteral("查询历史日志失败：%1")
                       .arg(query.lastError().text());
        }
      } else {
        while (query.next()) {
          result.prepend(eventFromQuery(query));
        }
      }
      database.close();
    }
  }
  QSqlDatabase::removeDatabase(connection);
  return result;
}

SimulationEvent EventStore::eventFromQuery(const QSqlQuery &query) {
  SimulationEvent event;
  event.id = query.value(0).toULongLong();
  event.timestamp = QDateTime::fromString(
      query.value(1).toString(), QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
  event.event = query.value(2).toString();
  event.router = query.value(3).toString();
  event.from = query.value(4).toString();
  event.to = query.value(5).toString();
  if (!query.value(6).isNull()) {
    event.fromAs = query.value(6).toUInt();
  }
  if (!query.value(7).isNull()) {
    event.toAs = query.value(7).toUInt();
  }
  event.messageType = query.value(8).toString();
  event.action = query.value(9).toString();
  event.sequence = query.value(10).toULongLong();
  event.prefixes = query.value(11).toString().split(u',', Qt::SkipEmptyParts);
  event.withdrawn = query.value(12).toString().split(u',', Qt::SkipEmptyParts);
  event.nextHop = query.value(13).toString();
  event.asPath = splitAsPath(query.value(14).toString());
  if (!query.value(15).isNull()) {
    event.localPref = query.value(15).toUInt();
  }
  if (!query.value(16).isNull()) {
    event.med = query.value(16).toUInt();
  }
  return event;
}

QString EventStore::uniqueConnectionName(const char *prefix) {
  static std::atomic<quint64> sequence{0};
  return QStringLiteral("%1-%2-%3")
      .arg(QString::fromLatin1(prefix))
      .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()))
      .arg(++sequence);
}

} // namespace bgptester
