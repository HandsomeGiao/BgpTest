#include "toposim/BmpLogManager.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>

#include <sqlite3.h>

namespace toposim {
namespace {

sqlite3 *asDb(void *db) { return static_cast<sqlite3 *>(db); }

std::string timestampNow() {
  const auto now = std::chrono::system_clock::now();
  const auto china_time = now + std::chrono::hours{8};
  const auto time = std::chrono::system_clock::to_time_t(china_time);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) %
                      1000;

  std::tm tm{};
  gmtime_s(&tm, &time);

  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3)
      << std::setfill('0') << millis.count();
  return oss.str();
}

std::string jsonString(std::string_view value) {
  std::ostringstream oss;
  oss << '"';
  for (const unsigned char ch : value) {
    switch (ch) {
    case '"':
      oss << "\\\"";
      break;
    case '\\':
      oss << "\\\\";
      break;
    case '\b':
      oss << "\\b";
      break;
    case '\f':
      oss << "\\f";
      break;
    case '\n':
      oss << "\\n";
      break;
    case '\r':
      oss << "\\r";
      break;
    case '\t':
      oss << "\\t";
      break;
    default:
      if (ch < 0x20) {
        oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<int>(ch) << std::dec;
      } else {
        oss << static_cast<char>(ch);
      }
      break;
    }
  }
  oss << '"';
  return oss.str();
}

template <typename T>
std::string joinVector(const std::vector<T> &values, std::string_view sep) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      oss << sep;
    }
    oss << values[i];
  }
  return oss.str();
}

std::string stringArrayJson(const std::vector<std::string> &values) {
  std::ostringstream oss;
  oss << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      oss << ',';
    }
    oss << jsonString(values[i]);
  }
  oss << ']';
  return oss.str();
}

std::string uintArrayJson(const std::vector<std::uint32_t> &values) {
  std::ostringstream oss;
  oss << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      oss << ',';
    }
    oss << values[i];
  }
  oss << ']';
  return oss.str();
}

std::string stringMapJson(const std::map<std::string, std::string> &values) {
  std::ostringstream oss;
  oss << '{';
  bool first = true;
  for (const auto &[key, value] : values) {
    if (!first) {
      oss << ',';
    }
    first = false;
    oss << jsonString(key) << ':' << jsonString(value);
  }
  oss << '}';
  return oss.str();
}

std::string pathAttributesJson(const PathAttributes &attrs) {
  std::ostringstream oss;
  oss << '{' << jsonString("origin") << ':' << jsonString(attrs.origin) << ','
      << jsonString("as_path") << ':' << uintArrayJson(attrs.as_path) << ','
      << jsonString("next_hop") << ':' << jsonString(attrs.next_hop) << ','
      << jsonString("local_pref") << ':' << attrs.local_pref << ','
      << jsonString("med") << ':' << attrs.med << ','
      << jsonString("communities") << ':' << stringMapJson(attrs.communities);
  if (attrs.originator_id) {
    oss << ',' << jsonString("originator_id") << ':'
        << jsonString(*attrs.originator_id);
  }
  if (attrs.cluster_list) {
    oss << ',' << jsonString("cluster_list") << ':'
        << jsonString(*attrs.cluster_list);
  }
  oss << '}';
  return oss.str();
}

std::string messageJson(const BgpMessage &message) {
  std::ostringstream oss;
  oss << '{' << jsonString("type") << ':' << jsonString(toString(message.type))
      << ',' << jsonString("from") << ':' << jsonString(message.from) << ','
      << jsonString("to") << ':' << jsonString(message.to) << ','
      << jsonString("sequence") << ':' << message.sequence;
  if (message.open) {
    const auto &open = *message.open;
    oss << ',' << jsonString("open") << ":{"
        << jsonString("version") << ':' << open.version << ','
        << jsonString("asn") << ':' << open.asn << ','
        << jsonString("hold_time_seconds") << ':'
        << open.hold_time_seconds << ',' << jsonString("router_id") << ':'
        << jsonString(open.router_id) << '}';
  }
  if (message.update) {
    const auto &update = *message.update;
    oss << ',' << jsonString("update") << ":{"
        << jsonString("withdrawn_routes") << ':'
        << stringArrayJson(update.withdrawn_routes) << ','
        << jsonString("nlri") << ':' << stringArrayJson(update.nlri) << ','
        << jsonString("path_attributes") << ':'
        << pathAttributesJson(update.path_attributes) << '}';
  }
  if (message.notification) {
    const auto &notification = *message.notification;
    oss << ',' << jsonString("notification") << ":{"
        << jsonString("error_code") << ':' << notification.error_code << ','
        << jsonString("error_subcode") << ':' << notification.error_subcode
        << ',' << jsonString("data") << ':'
        << jsonString(notification.data) << '}';
  }
  oss << '}';
  return oss.str();
}

std::string eventValueJson(const BmpEventValue &value) {
  return std::visit(
      [](const auto &item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return jsonString(item);
        } else if constexpr (std::is_same_v<T, bool>) {
          return item ? "true" : "false";
        } else {
          return std::to_string(item);
        }
      },
      value);
}

std::string eventDetailJson(const BmpEventDetail &detail) {
  std::ostringstream oss;
  oss << '{';
  bool first = true;
  for (const auto &[key, value] : detail) {
    if (!first) {
      oss << ',';
    }
    first = false;
    oss << jsonString(key) << ':' << eventValueJson(value);
  }
  oss << '}';
  return oss.str();
}

void sqliteCheck(int rc, sqlite3 *db, const char *context) {
  if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
    const char *message = db ? sqlite3_errmsg(db) : "unknown sqlite error";
    throw std::runtime_error(std::string(context) + ": " + message);
  }
}

void execSql(sqlite3 *db, const char *sql) {
  char *error = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
  if (rc != SQLITE_OK) {
    std::string message = error ? error : "unknown sqlite error";
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

void bindText(sqlite3_stmt *stmt, int index, const std::string &value) {
  sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
}

std::string columnText(sqlite3_stmt *stmt, int column) {
  const auto *text = sqlite3_column_text(stmt, column);
  return text ? reinterpret_cast<const char *>(text) : std::string{};
}

std::string updateAction(const BgpUpdatePayload &update) {
  if (!update.withdrawn_routes.empty() && update.nlri.empty()) {
    return "WITHDRAW";
  }
  if (!update.withdrawn_routes.empty() && !update.nlri.empty()) {
    return "UPDATE+WITHDRAW";
  }
  return "UPDATE";
}

class SqliteStatement {
public:
  SqliteStatement(sqlite3 *db, const char *sql, const char *context) : db_(db) {
    sqliteCheck(sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr), db_, context);
  }

  SqliteStatement(const SqliteStatement &) = delete;
  SqliteStatement &operator=(const SqliteStatement &) = delete;

  ~SqliteStatement() {
    if (stmt_) {
      sqlite3_finalize(stmt_);
    }
  }

  sqlite3_stmt *get() const { return stmt_; }

private:
  sqlite3 *db_{nullptr};
  sqlite3_stmt *stmt_{nullptr};
};

class SqliteTransaction {
public:
  explicit SqliteTransaction(sqlite3 *db) : db_(db) {
    execSql(db_, "BEGIN IMMEDIATE TRANSACTION;");
  }

  SqliteTransaction(const SqliteTransaction &) = delete;
  SqliteTransaction &operator=(const SqliteTransaction &) = delete;

  ~SqliteTransaction() {
    if (active_) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
  }

  void commit() {
    execSql(db_, "COMMIT;");
    active_ = false;
  }

private:
  sqlite3 *db_{nullptr};
  bool active_{true};
};

} // namespace

BmpLogManager &BmpLogManager::instance() {
  static BmpLogManager manager;
  return manager;
}

BmpLogManager::~BmpLogManager() { shutdown(); }

void BmpLogManager::initialize(std::filesystem::path log_file,
                               std::filesystem::path database_file,
                               std::size_t live_capacity) {
  shutdown();

  {
    std::lock_guard lock(mutex_);
    log_file_ = std::move(log_file);
    database_file_ = std::move(database_file);
    live_capacity_ = std::max<std::size_t>(1, live_capacity);
    stopping_ = false;
    initialized_ = true;
    next_id_ = 1;
    total_events_ = 0;
    queue_.clear();
    live_records_.clear();
  }

  std::filesystem::create_directories(log_file_.parent_path());
  std::filesystem::create_directories(database_file_.parent_path());
  std::error_code ec;
  std::filesystem::remove(database_file_, ec);
  std::filesystem::remove(database_file_.string() + "-wal", ec);
  std::filesystem::remove(database_file_.string() + "-shm", ec);

  {
    std::lock_guard io_lock(io_mutex_);
    out_.open(log_file_, std::ios::out | std::ios::trunc);
    if (!out_) {
      throw std::runtime_error("Unable to open BMP collector log: " +
                               log_file_.string());
    }
    openDatabase();
    createSchema();
  }

  writer_thread_ = std::thread([this] { writerLoop(); });
}

void BmpLogManager::shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (!initialized_) {
      return;
    }
    stopping_ = true;
  }
  cv_.notify_all();
  if (writer_thread_.joinable()) {
    writer_thread_.join();
  }

  {
    std::lock_guard io_lock(io_mutex_);
    if (out_.is_open()) {
      out_.flush();
      out_.close();
    }
    closeDatabase();
  }

  {
    std::lock_guard lock(mutex_);
    initialized_ = false;
    stopping_ = false;
    queue_.clear();
  }
}

void BmpLogManager::flush() {
  while (true) {
    {
      std::lock_guard lock(mutex_);
      if (queue_.empty()) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::lock_guard io_lock(io_mutex_);
  if (out_.is_open()) {
    out_.flush();
  }
}

void BmpLogManager::recordReceive(const std::string &router_id,
                                  const BgpMessage &message,
                                  std::optional<std::uint32_t> from_as,
                                  std::optional<std::uint32_t> to_as) {
  BmpLogRecord record;
  record.id = next_id_.fetch_add(1);
  record.timestamp = timestampNow();
  record.event = "message_received";
  record.router = router_id;
  record.from = message.from;
  record.to = message.to;
  record.from_as = from_as;
  record.to_as = to_as;
  record.msg_type = toString(message.type);
  record.action = record.msg_type;
  record.sequence = message.sequence;
  if (message.update) {
    const auto &update = *message.update;
    record.action = updateAction(update);
    record.prefixes = joinVector(update.nlri, ",");
    record.withdrawn = joinVector(update.withdrawn_routes, ",");
    record.next_hop = update.path_attributes.next_hop;
    record.as_path = joinVector(update.path_attributes.as_path, " ");
    record.local_pref = update.path_attributes.local_pref;
    record.med = update.path_attributes.med;
  }

  std::ostringstream line;
  line << '{' << jsonString("timestamp") << ':' << jsonString(record.timestamp)
       << ',' << jsonString("event") << ':' << jsonString(record.event) << ','
       << jsonString("router") << ':' << jsonString(record.router) << ','
       << jsonString("from") << ':' << jsonString(record.from) << ','
       << jsonString("to") << ':' << jsonString(record.to) << ',';
  if (record.from_as) {
    line << jsonString("from_as") << ':' << *record.from_as << ',';
  }
  if (record.to_as) {
    line << jsonString("to_as") << ':' << *record.to_as << ',';
  }
  line
       << jsonString("msg_type") << ':' << jsonString(record.msg_type) << ','
       << jsonString("action") << ':' << jsonString(record.action) << ','
       << jsonString("sequence") << ':' << record.sequence << ','
       << jsonString("message") << ':' << messageJson(message) << '}';
  record.raw_json = line.str();
  enqueue(std::move(record));
}

void BmpLogManager::recordTopologyEvent(const std::string &event_name,
                                        const BmpEventDetail &detail) {
  BmpLogRecord record;
  record.id = next_id_.fetch_add(1);
  record.timestamp = timestampNow();
  record.event = event_name;
  record.action = event_name;

  std::ostringstream line;
  line << '{' << jsonString("timestamp") << ':' << jsonString(record.timestamp)
       << ',' << jsonString("event") << ':' << jsonString(event_name) << ','
       << jsonString("detail") << ':' << eventDetailJson(detail) << '}';
  record.raw_json = line.str();
  enqueue(std::move(record));
}

std::vector<BmpLogRecord> BmpLogManager::liveSnapshot() const {
  std::lock_guard lock(mutex_);
  return {live_records_.begin(), live_records_.end()};
}

std::vector<BmpLogRecord>
BmpLogManager::queryHistory(const BmpLogQuery &query) const {
  std::lock_guard io_lock(io_mutex_);
  std::vector<BmpLogRecord> result;
  if (!db_) {
    return result;
  }

  std::string sql =
      "SELECT e.id,e.timestamp,e.event,e.router,e.from_peer,e.to_peer,"
      "e.from_as,e.to_as,e.msg_type,e.action,e.sequence,e.prefixes,"
      "e.withdrawn,e.next_hop,e.as_path,e.local_pref,e.med,e.raw_json "
      "FROM bmp_events e ";
  sql += "WHERE 1=1 ";

  struct QueryParam {
    enum class Type { Text, Integer };
    Type type{Type::Text};
    std::string text;
    std::uint64_t integer{0};
  };
  std::vector<QueryParam> params;
  auto append_text_in = [&](const std::string &column,
                            const std::vector<std::string> &values) {
    if (values.empty()) {
      return;
    }
    sql += "AND " + column + " IN (";
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i != 0) {
        sql += ',';
      }
      sql += '?';
      params.push_back({QueryParam::Type::Text, values[i], 0});
    }
    sql += ") ";
  };
  auto append_uint_in = [&](const std::string &column,
                            const std::vector<std::uint32_t> &values) {
    if (values.empty()) {
      return;
    }
    sql += "AND " + column + " IN (";
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i != 0) {
        sql += ',';
      }
      sql += '?';
      params.push_back({QueryParam::Type::Integer, {},
                        static_cast<std::uint64_t>(values[i])});
    }
    sql += ") ";
  };
  if (!query.routers.empty()) {
    sql += "AND (";
    bool first = true;
    for (const auto &router : query.routers) {
      if (!first) {
        sql += " OR ";
      }
      first = false;
      sql += "e.router=? OR e.from_peer=? OR e.to_peer=?";
      params.push_back({QueryParam::Type::Text, router, 0});
      params.push_back({QueryParam::Type::Text, router, 0});
      params.push_back({QueryParam::Type::Text, router, 0});
    }
    sql += ") ";
  }
  append_text_in("e.from_peer", query.from_routers);
  append_text_in("e.to_peer", query.to_routers);
  append_uint_in("e.from_as", query.from_asns);
  append_uint_in("e.to_as", query.to_asns);
  append_text_in("e.action", query.actions);
  sql += "ORDER BY e.id DESC LIMIT ?;";

  SqliteStatement stmt(asDb(db_), sql.c_str(), "prepare BMP query");
  int index = 1;
  for (const auto &param : params) {
    if (param.type == QueryParam::Type::Text) {
      bindText(stmt.get(), index++, param.text);
    } else {
      sqlite3_bind_int64(stmt.get(), index++,
                         static_cast<sqlite3_int64>(param.integer));
    }
  }
  sqlite3_bind_int64(stmt.get(), index++,
                     static_cast<sqlite3_int64>(std::max<std::size_t>(
                         1, std::min<std::size_t>(query.limit, 10000))));

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    BmpLogRecord record;
    record.id =
        static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0));
    record.timestamp = columnText(stmt.get(), 1);
    record.event = columnText(stmt.get(), 2);
    record.router = columnText(stmt.get(), 3);
    record.from = columnText(stmt.get(), 4);
    record.to = columnText(stmt.get(), 5);
    if (sqlite3_column_type(stmt.get(), 6) != SQLITE_NULL) {
      record.from_as =
          static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 6));
    }
    if (sqlite3_column_type(stmt.get(), 7) != SQLITE_NULL) {
      record.to_as =
          static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 7));
    }
    record.msg_type = columnText(stmt.get(), 8);
    record.action = columnText(stmt.get(), 9);
    record.sequence =
        static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 10));
    record.prefixes = columnText(stmt.get(), 11);
    record.withdrawn = columnText(stmt.get(), 12);
    record.next_hop = columnText(stmt.get(), 13);
    record.as_path = columnText(stmt.get(), 14);
    if (sqlite3_column_type(stmt.get(), 15) != SQLITE_NULL) {
      record.local_pref =
          static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 15));
    }
    if (sqlite3_column_type(stmt.get(), 16) != SQLITE_NULL) {
      record.med =
          static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 16));
    }
    record.raw_json = columnText(stmt.get(), 17);
    result.push_back(std::move(record));
  }
  return result;
}

const std::filesystem::path &BmpLogManager::logFile() const { return log_file_; }

const std::filesystem::path &BmpLogManager::databaseFile() const {
  return database_file_;
}

bool BmpLogManager::initialized() const {
  std::lock_guard lock(mutex_);
  return initialized_;
}

std::uint64_t BmpLogManager::totalEvents() const { return total_events_.load(); }

void BmpLogManager::enqueue(BmpLogRecord record) {
  {
    std::lock_guard lock(mutex_);
    if (!initialized_ || stopping_) {
      return;
    }
    live_records_.push_back(record);
    while (live_records_.size() > live_capacity_) {
      live_records_.pop_front();
    }
    queue_.push_back(std::move(record));
    total_events_.fetch_add(1);
  }
  cv_.notify_one();
}

void BmpLogManager::writerLoop() {
  std::vector<BmpLogRecord> batch;
  while (true) {
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty() && stopping_) {
        break;
      }
      while (!queue_.empty() && batch.size() < 256) {
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
      }
    }

    {
      std::lock_guard io_lock(io_mutex_);
      std::optional<SqliteTransaction> transaction;
      if (db_) {
        transaction.emplace(asDb(db_));
      }
      for (const auto &record : batch) {
        writeRecord(record);
      }
      if (transaction) {
        transaction->commit();
      }
      if (out_.is_open()) {
        out_.flush();
      }
    }
    batch.clear();
  }
}

void BmpLogManager::openDatabase() {
  sqlite3 *db = nullptr;
  sqliteCheck(sqlite3_open(database_file_.string().c_str(), &db), db,
              "open BMP sqlite database");
  db_ = db;
  execSql(asDb(db_), "PRAGMA journal_mode=WAL;");
  execSql(asDb(db_), "PRAGMA synchronous=NORMAL;");
  execSql(asDb(db_), "PRAGMA foreign_keys=ON;");
}

void BmpLogManager::createSchema() {
  execSql(asDb(db_),
          "CREATE TABLE bmp_events ("
          "id INTEGER PRIMARY KEY,"
          "timestamp TEXT NOT NULL,"
          "event TEXT NOT NULL,"
          "router TEXT,"
          "from_peer TEXT,"
          "to_peer TEXT,"
          "from_as INTEGER,"
          "to_as INTEGER,"
          "msg_type TEXT,"
          "action TEXT NOT NULL,"
          "sequence INTEGER,"
          "prefixes TEXT,"
          "withdrawn TEXT,"
          "next_hop TEXT,"
          "as_path TEXT,"
          "local_pref INTEGER,"
          "med INTEGER,"
          "raw_json TEXT NOT NULL);"
          "CREATE INDEX idx_bmp_events_router ON "
          "bmp_events(router);"
          "CREATE INDEX idx_bmp_events_from_peer ON "
          "bmp_events(from_peer);"
          "CREATE INDEX idx_bmp_events_to_peer ON "
          "bmp_events(to_peer);"
          "CREATE INDEX idx_bmp_events_msg_type ON "
          "bmp_events(msg_type);"
          "CREATE INDEX idx_bmp_events_action ON "
          "bmp_events(action);"
          "CREATE INDEX idx_bmp_events_sequence ON "
          "bmp_events(sequence);"
          "CREATE INDEX idx_bmp_events_from_as ON "
          "bmp_events(from_as);"
          "CREATE INDEX idx_bmp_events_to_as ON "
          "bmp_events(to_as);");
}

void BmpLogManager::writeRecord(const BmpLogRecord &record) {
  if (out_.is_open()) {
    out_ << record.raw_json << '\n';
  }
  if (db_) {
    insertRecord(record);
  }
}

void BmpLogManager::insertRecord(const BmpLogRecord &record) {
  constexpr const char *insert_event =
      "INSERT INTO bmp_events "
      "(id,timestamp,event,router,from_peer,to_peer,from_as,to_as,msg_type,action,"
      "sequence,prefixes,withdrawn,next_hop,as_path,local_pref,med,raw_json) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
  SqliteStatement event_stmt(asDb(db_), insert_event,
                             "prepare insert BMP event");
  sqlite3_bind_int64(event_stmt.get(), 1,
                     static_cast<sqlite3_int64>(record.id));
  bindText(event_stmt.get(), 2, record.timestamp);
  bindText(event_stmt.get(), 3, record.event);
  bindText(event_stmt.get(), 4, record.router);
  bindText(event_stmt.get(), 5, record.from);
  bindText(event_stmt.get(), 6, record.to);
  if (record.from_as) {
    sqlite3_bind_int64(event_stmt.get(), 7, *record.from_as);
  } else {
    sqlite3_bind_null(event_stmt.get(), 7);
  }
  if (record.to_as) {
    sqlite3_bind_int64(event_stmt.get(), 8, *record.to_as);
  } else {
    sqlite3_bind_null(event_stmt.get(), 8);
  }
  bindText(event_stmt.get(), 9, record.msg_type);
  bindText(event_stmt.get(), 10, record.action);
  sqlite3_bind_int64(event_stmt.get(), 11,
                     static_cast<sqlite3_int64>(record.sequence));
  bindText(event_stmt.get(), 12, record.prefixes);
  bindText(event_stmt.get(), 13, record.withdrawn);
  bindText(event_stmt.get(), 14, record.next_hop);
  bindText(event_stmt.get(), 15, record.as_path);
  if (record.local_pref) {
    sqlite3_bind_int64(event_stmt.get(), 16, *record.local_pref);
  } else {
    sqlite3_bind_null(event_stmt.get(), 16);
  }
  if (record.med) {
    sqlite3_bind_int64(event_stmt.get(), 17, *record.med);
  } else {
    sqlite3_bind_null(event_stmt.get(), 17);
  }
  bindText(event_stmt.get(), 18, record.raw_json);
  sqliteCheck(sqlite3_step(event_stmt.get()), asDb(db_), "insert BMP event");
}

void BmpLogManager::closeDatabase() {
  if (db_) {
    sqlite3_close(asDb(db_));
    db_ = nullptr;
  }
}

} // namespace toposim
