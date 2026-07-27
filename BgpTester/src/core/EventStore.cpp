#include "core/EventStore.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>

namespace bgptester
{
namespace
{

std::string sqliteError(sqlite3* database)
{
    return database ? sqlite3_errmsg(database) : "SQLite handle is not available";
}

std::string safeRunName(std::string value)
{
    for (auto& character : value)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x80 && !std::isalnum(byte) && character != '-' && character != '_')
        {
            character = '_';
        }
    }
    while (!value.empty() && (value.front() == '.' || value.front() == ' '))
    {
        value.erase(value.begin());
    }
    return value.empty() ? "bgp-lab" : value;
}

std::string timestampForFileName()
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    auto fraction = static_cast<int>((milliseconds - seconds).count());
    auto wholeSeconds = seconds;
    if (fraction < 0)
    {
        fraction += 1000;
        wholeSeconds -= std::chrono::seconds(1);
    }
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point(wholeSeconds));
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream result;
    result << std::put_time(&local, "%Y%m%d_%H%M%S_") << std::setfill('0') << std::setw(3) << fraction;
    return result.str();
}

std::string formatIsoTimestamp(std::int64_t milliseconds)
{
    auto seconds = milliseconds / 1000;
    auto fraction = static_cast<int>(milliseconds % 1000);
    if (fraction < 0)
    {
        fraction += 1000;
        --seconds;
    }
    const auto time = static_cast<std::time_t>(seconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S.") << std::setfill('0') << std::setw(3) << fraction << 'Z';
    return result.str();
}

std::int64_t daysFromCivil(int year, unsigned month, unsigned day) noexcept
{
    year -= month <= 2;
    const auto era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - era * 400);
    const auto adjustedMonth = static_cast<int>(month) + (month > 2 ? -3 : 9);
    const auto dayOfYear = static_cast<unsigned>((153 * adjustedMonth + 2) / 5) + day - 1;
    const auto dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

std::optional<std::int64_t> parseIsoTimestamp(const std::string& value)
{
    if (value.size() < 19 || value[4] != '-' || value[7] != '-' || (value[10] != 'T' && value[10] != ' ') ||
        value[13] != ':' || value[16] != ':')
    {
        return std::nullopt;
    }
    const auto parseNumber = [&](std::size_t offset, std::size_t length) -> std::optional<int>
    {
        if (offset > value.size() || length > value.size() - offset)
        {
            return std::nullopt;
        }
        int number = 0;
        for (std::size_t index = 0; index < length; ++index)
        {
            const auto character = value[offset + index];
            if (character < '0' || character > '9')
            {
                return std::nullopt;
            }
            number = number * 10 + character - '0';
        }
        return number;
    };
    const auto year = parseNumber(0, 4);
    const auto month = parseNumber(5, 2);
    const auto day = parseNumber(8, 2);
    const auto hour = parseNumber(11, 2);
    const auto minute = parseNumber(14, 2);
    const auto second = parseNumber(17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *year < 1 || *month < 1 || *month > 12 ||
        *day < 1 || *hour > 23 || *minute > 59 || *second > 59)
    {
        return std::nullopt;
    }
    constexpr std::array daysByMonth{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    auto maximumDay = daysByMonth[static_cast<std::size_t>(*month - 1)];
    const auto leapYear = (*year % 4 == 0 && *year % 100 != 0) || *year % 400 == 0;
    if (*month == 2 && leapYear)
    {
        maximumDay = 29;
    }
    if (*day > maximumDay)
    {
        return std::nullopt;
    }
    std::size_t offset = 19;
    int fraction = 0;
    if (offset < value.size() && value[offset] == '.')
    {
        ++offset;
        int digits = 0;
        while (offset < value.size() && std::isdigit(static_cast<unsigned char>(value[offset])))
        {
            if (digits < 3)
            {
                fraction = fraction * 10 + value[offset] - '0';
            }
            ++digits;
            ++offset;
        }
        if (digits == 0)
        {
            return std::nullopt;
        }
        while (digits < 3)
        {
            fraction *= 10;
            ++digits;
        }
    }
    int zoneMinutes = 0;
    if (offset < value.size() && (value[offset] == 'Z' || value[offset] == 'z'))
    {
        ++offset;
    }
    else if (offset < value.size() && (value[offset] == '+' || value[offset] == '-'))
    {
        const auto sign = value[offset++] == '+' ? 1 : -1;
        const auto zoneHour = parseNumber(offset, 2);
        if (!zoneHour)
        {
            return std::nullopt;
        }
        offset += 2;
        if (offset < value.size() && value[offset] == ':')
        {
            ++offset;
        }
        const auto zoneMinute = parseNumber(offset, 2);
        if (!zoneHour || !zoneMinute || *zoneHour > 23 || *zoneMinute > 59)
        {
            return std::nullopt;
        }
        offset += 2;
        zoneMinutes = sign * (*zoneHour * 60 + *zoneMinute);
    }
    if (offset != value.size())
    {
        return std::nullopt;
    }
    const auto days = daysFromCivil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day));
    return ((days * 24 + *hour) * 60 + *minute - zoneMinutes) * 60'000 + *second * 1000 + fraction;
}

Json unsigned64ToJson(std::uint64_t value)
{
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return static_cast<std::int64_t>(value);
    }
    return std::to_string(value);
}

std::optional<std::uint64_t> jsonUnsigned64(const Json& value)
{
    if (value.is_number_unsigned())
    {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer())
    {
        const auto number = value.get<std::int64_t>();
        return number >= 0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(number)) : std::nullopt;
    }
    if (value.is_string())
    {
        try
        {
            const auto& text = value.get_ref<const std::string&>();
            if (text.empty() || !std::all_of(text.begin(), text.end(), [](char character)
                                             { return character >= '0' && character <= '9'; }))
            {
                return std::nullopt;
            }
            std::size_t consumed = 0;
            const auto number = std::stoull(text, &consumed);
            return consumed == text.size() ? std::optional<std::uint64_t>(number) : std::nullopt;
        }
        catch (...)
        {
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> jsonUnsigned32(const Json& value)
{
    std::uint64_t number = 0;
    if (value.is_number_unsigned())
    {
        number = value.get<std::uint64_t>();
    }
    else if (value.is_number_integer())
    {
        const auto signedNumber = value.get<std::int64_t>();
        if (signedNumber < 0)
        {
            return std::nullopt;
        }
        number = static_cast<std::uint64_t>(signedNumber);
    }
    else
    {
        return std::nullopt;
    }
    if (number > std::numeric_limits<std::uint32_t>::max())
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(number);
}

std::string join(const std::vector<std::string>& values, char separator)
{
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index)
        {
            result.push_back(separator);
        }
        result += values[index];
    }
    return result;
}

std::string joinAsPath(const std::vector<std::uint32_t>& values)
{
    std::ostringstream result;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index)
        {
            result << ' ';
        }
        result << values[index];
    }
    return result.str();
}

bool cancelledQuery(const std::function<bool()>& cancelled, std::string* error)
{
    if (cancelled && cancelled())
    {
        if (error)
        {
            *error = "查询已取消";
        }
        return true;
    }
    return false;
}

class ReadDatabase final
{
public:
    ReadDatabase() = default;
    ~ReadDatabase()
    {
        if (handle)
        {
            sqlite3_close_v2(handle);
        }
    }

    ReadDatabase(const ReadDatabase&) = delete;
    ReadDatabase& operator=(const ReadDatabase&) = delete;

    sqlite3* handle = nullptr;
};

class Statement final
{
public:
    Statement() = default;
    ~Statement()
    {
        if (handle)
        {
            sqlite3_finalize(handle);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* handle = nullptr;
};

class ReadTransaction final
{
public:
    ReadTransaction() = default;
    ~ReadTransaction()
    {
        if (active_)
        {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    ReadTransaction(const ReadTransaction&) = delete;
    ReadTransaction& operator=(const ReadTransaction&) = delete;

    bool begin(sqlite3* database, std::string_view description, std::string* error)
    {
        char* rawError = nullptr;
        const auto result = sqlite3_exec(database, "BEGIN", nullptr, nullptr, &rawError);
        if (result == SQLITE_OK)
        {
            sqlite3_free(rawError);
            database_ = database;
            active_ = true;
            return true;
        }
        if (error)
        {
            const std::string detail = rawError ? rawError : sqliteError(database);
            *error = std::string(description) + "：" + detail;
        }
        sqlite3_free(rawError);
        return false;
    }

private:
    sqlite3* database_ = nullptr;
    bool active_ = false;
};

bool openReadDatabase(const std::string& path, ReadDatabase* database, std::string* error)
{
    if (error)
    {
        error->clear();
    }
    const auto result = sqlite3_open_v2(path.c_str(), &database->handle, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result == SQLITE_OK)
    {
        return true;
    }
    if (error)
    {
        *error = "无法打开历史日志：" + sqliteError(database->handle);
    }
    return false;
}

bool prepare(sqlite3* database, const std::string& sql, Statement* statement, std::string* error)
{
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement->handle, nullptr) == SQLITE_OK)
    {
        return true;
    }
    if (error)
    {
        *error = "准备 SQLite 查询失败：" + sqliteError(database);
    }
    return false;
}

bool bindText(sqlite3* database, sqlite3_stmt* statement, int index, std::string_view value,
              std::string_view description, std::string* error)
{
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        if (error)
        {
            *error = std::string(description) + "：文本参数过长";
        }
        return false;
    }
    if (sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK)
    {
        return true;
    }
    if (error)
    {
        *error = std::string(description) + "：" + sqliteError(database);
    }
    return false;
}

bool bindInteger(sqlite3* database, sqlite3_stmt* statement, int index, sqlite3_int64 value,
                 std::string_view description, std::string* error)
{
    if (sqlite3_bind_int64(statement, index, value) == SQLITE_OK)
    {
        return true;
    }
    if (error)
    {
        *error = std::string(description) + "：" + sqliteError(database);
    }
    return false;
}

bool bindOptionalUnsigned(sqlite3* database, sqlite3_stmt* statement, int index,
                          const std::optional<std::uint32_t>& value, std::string* error)
{
    const auto result = value ? sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(*value))
                              : sqlite3_bind_null(statement, index);
    if (result == SQLITE_OK)
    {
        return true;
    }
    if (error)
    {
        *error = "绑定 SQLite 可选整数失败：" + sqliteError(database);
    }
    return false;
}

const std::array<std::string_view, 17>& eventFilterExpressions()
{
    static constexpr std::array<std::string_view, 17> expressions{
        "CAST(id AS TEXT)",
        "substr(timestamp, 12)",
        "COALESCE(event, '')",
        "COALESCE(router, '')",
        "COALESCE(from_peer, '')",
        "COALESCE(to_peer, '')",
        "COALESCE(CAST(from_as AS TEXT), '')",
        "COALESCE(CAST(to_as AS TEXT), '')",
        "COALESCE(msg_type, '')",
        "COALESCE(action, '')",
        "CASE WHEN sequence IS NULL OR sequence = 0 THEN '' ELSE CAST(sequence AS TEXT) END",
        "replace(COALESCE(prefixes, ''), ',', ', ')",
        "replace(COALESCE(withdrawn, ''), ',', ', ')",
        "COALESCE(next_hop, '')",
        "COALESCE(as_path, '')",
        "COALESCE(CAST(local_pref AS TEXT), '')",
        "COALESCE(CAST(med AS TEXT), '')",
    };
    return expressions;
}

std::string eventFilterPredicate()
{
    std::string result = "(";
    for (std::size_t index = 0; index < eventFilterExpressions().size(); ++index)
    {
        if (index != 0)
        {
            result += " OR ";
        }
        result += "instr(lower(";
        result += eventFilterExpressions()[index];
        result += "), lower(?)) > 0";
    }
    result += ')';
    return result;
}

std::string eventWhereClause(const std::string& filter, const std::optional<std::uint64_t>& maxEventId = {},
                             std::string_view exactEvent = {})
{
    std::vector<std::string> predicates;
    if (maxEventId)
    {
        predicates.emplace_back("id <= ?");
    }
    if (!exactEvent.empty())
    {
        predicates.emplace_back("event = ?");
    }
    if (!filter.empty())
    {
        predicates.push_back(eventFilterPredicate());
    }
    if (predicates.empty())
    {
        return {};
    }
    std::string result = " WHERE ";
    for (std::size_t index = 0; index < predicates.size(); ++index)
    {
        if (index != 0)
        {
            result += " AND ";
        }
        result += predicates[index];
    }
    return result;
}

bool bindEventConditions(sqlite3* database, sqlite3_stmt* statement, const std::string& filter,
                         const std::optional<std::uint64_t>& maxEventId, std::string_view exactEvent,
                         int* nextBinding, std::string* error)
{
    auto binding = *nextBinding;
    if (maxEventId)
    {
        constexpr auto maximumSqliteId = static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max());
        const auto bounded = std::min(*maxEventId, maximumSqliteId);
        if (!bindInteger(database, statement, binding++, static_cast<sqlite3_int64>(bounded),
                         "绑定 SQLite 最大事件 ID 失败", error))
        {
            return false;
        }
    }
    if (!exactEvent.empty() &&
        !bindText(database, statement, binding++, exactEvent, "绑定 SQLite 事件类型失败", error))
    {
        return false;
    }
    if (!filter.empty())
    {
        for (std::size_t index = 0; index < eventFilterExpressions().size(); ++index)
        {
            if (!bindText(database, statement, binding++, filter, "绑定 SQLite 过滤条件失败", error))
            {
                return false;
            }
        }
    }
    *nextBinding = binding;
    return true;
}

bool querySingleCount(sqlite3* database, const std::string& filter,
                      const std::optional<std::uint64_t>& maxEventId, std::string_view exactEvent,
                      std::int64_t* count, std::string_view description, std::string* error,
                      const std::function<bool()>& cancelled)
{
    if (cancelledQuery(cancelled, error))
    {
        return false;
    }
    Statement statement;
    const auto sql = std::string("SELECT COUNT(*) FROM bmp_events") + eventWhereClause(filter, maxEventId, exactEvent);
    if (!prepare(database, sql, &statement, error))
    {
        return false;
    }
    int binding = 1;
    if (!bindEventConditions(database, statement.handle, filter, maxEventId, exactEvent, &binding, error))
    {
        return false;
    }
    if (sqlite3_step(statement.handle) != SQLITE_ROW)
    {
        if (error)
        {
            *error = std::string(description) + "失败：" + sqliteError(database);
        }
        return false;
    }
    if (cancelledQuery(cancelled, error))
    {
        return false;
    }
    *count = sqlite3_column_int64(statement.handle, 0);
    return true;
}

bool queryMaximumEventId(sqlite3* database, std::uint64_t* maximumId, std::string* error,
                         const std::function<bool()>& cancelled)
{
    if (cancelledQuery(cancelled, error))
    {
        return false;
    }
    Statement statement;
    if (!prepare(database, "SELECT COALESCE(MAX(id),0) FROM bmp_events", &statement, error))
    {
        return false;
    }
    if (sqlite3_step(statement.handle) != SQLITE_ROW)
    {
        if (error)
        {
            *error = "读取 SQLite 最大事件 ID 失败：" + sqliteError(database);
        }
        return false;
    }
    const auto value = sqlite3_column_int64(statement.handle, 0);
    if (value < 0)
    {
        if (error)
        {
            *error = "SQLite 最大事件 ID 为负数";
        }
        return false;
    }
    *maximumId = static_cast<std::uint64_t>(value);
    return !cancelledQuery(cancelled, error);
}

bool queryEventCounts(sqlite3* database, const std::string& filter,
                      const std::optional<std::uint64_t>& maxEventId, EventHistoryPage* result,
                      std::string* error, const std::function<bool()>& cancelled)
{
    if (!querySingleCount(database, {}, maxEventId, {}, &result->totalCount, "统计事件总数", error, cancelled) ||
        !querySingleCount(database, {}, maxEventId, "message_received", &result->messageTotalCount,
                          "统计报文总数", error, cancelled))
    {
        return false;
    }
    if (filter.empty())
    {
        result->filteredCount = result->totalCount;
        result->filteredMessageCount = result->messageTotalCount;
        return true;
    }
    return querySingleCount(database, filter, maxEventId, {}, &result->filteredCount, "统计过滤后事件数", error,
                            cancelled) &&
           querySingleCount(database, filter, maxEventId, "message_received", &result->filteredMessageCount,
                            "统计过滤后报文数", error, cancelled);
}

std::size_t countAsSize(std::int64_t count)
{
    if (count <= 0)
    {
        return 0;
    }
    const auto unsignedCount = static_cast<std::uint64_t>(count);
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    return static_cast<std::size_t>(std::min(unsignedCount, maximum));
}

bool appendEventFromColumn(sqlite3_stmt* statement, std::vector<SimulationEvent>* events,
                           std::string_view description, std::string* error)
{
    const auto* raw = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    const auto bytes = sqlite3_column_bytes(statement, 0);
    if (!raw || bytes < 0)
    {
        if (error)
        {
            *error = std::string(description) + "：raw_json 为空";
        }
        return false;
    }
    try
    {
        std::string decodeError;
        auto event = EventStore::eventFromJson(Json::parse(std::string(raw, static_cast<std::size_t>(bytes))), &decodeError);
        if (!event)
        {
            if (error)
            {
                *error = std::string(description) + "：" + decodeError;
            }
            return false;
        }
        events->push_back(std::move(*event));
        return true;
    }
    catch (const std::exception& exception)
    {
        if (error)
        {
            *error = std::string(description) + "：" + exception.what();
        }
        return false;
    }
}

} // namespace

EventStore::~EventStore()
{
    endRun();
}

bool EventStore::beginRun(const SimulationSettings& settings, std::string* error)
{
    endRun();
    std::scoped_lock lock(mutex_);
    if (error)
    {
        error->clear();
    }
    lastError_.clear();
    nextId_ = 1;
    lastInsertedEventId_ = 0;
    committedEventId_ = 0;
    pendingTransactionRows_ = 0;
    ++runSerial_;
    const auto hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    encodingWorkerCount_ = settings.workerThreads > 0 ? std::clamp(settings.workerThreads, 1, 256)
                                                       : static_cast<int>(std::min(256u, hardwareThreads));

    std::error_code filesystemError;
    auto base = std::filesystem::absolute(settings.logDirectory, filesystemError);
    if (filesystemError)
    {
        setLastError("无法解析日志目录：" + filesystemError.message(), error);
        return false;
    }
    std::filesystem::create_directories(base, filesystemError);
    if (filesystemError)
    {
        setLastError("无法创建日志目录：" + base.string() + "：" + filesystemError.message(), error);
        return false;
    }
    const auto stem = safeRunName(settings.name) + '_' + timestampForFileName();
    auto directory = base / stem;
    int suffix = 1;
    while (std::filesystem::exists(directory, filesystemError))
    {
        if (filesystemError)
        {
            setLastError("无法检查本次运行目录：" + directory.string() + "：" + filesystemError.message(), error);
            return false;
        }
        directory = base / (stem + '_' + std::to_string(suffix++));
    }
    if (filesystemError)
    {
        setLastError("无法检查本次运行目录：" + directory.string() + "：" + filesystemError.message(), error);
        return false;
    }
    std::filesystem::create_directories(directory, filesystemError);
    if (filesystemError)
    {
        setLastError("无法创建本次运行目录：" + directory.string() + "：" + filesystemError.message(), error);
        return false;
    }
    runDirectory_ = directory.string();
    logFilePath_ = (directory / "bmp_collector.log").string();
    databasePath_ = (directory / "bmp_collector.sqlite").string();
    logFile_.open(logFilePath_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!logFile_)
    {
        setLastError("无法创建日志文件：" + logFilePath_, error);
        return false;
    }
    if (sqlite3_open_v2(databasePath_.c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
    {
        setLastError("无法创建 SQLite 日志：" + sqliteError(database_), error);
        if (database_)
        {
            sqlite3_close_v2(database_);
            database_ = nullptr;
        }
        logFile_.close();
        return false;
    }
    if (!initializeSchema(error) || !prepareInsert(error) || !beginTransaction(error))
    {
        if (insertStatement_)
        {
            sqlite3_finalize(insertStatement_);
            insertStatement_ = nullptr;
        }
        sqlite3_close_v2(database_);
        database_ = nullptr;
        logFile_.close();
        return false;
    }
    return true;
}

void EventStore::endRun()
{
    std::scoped_lock lock(mutex_);
    if (database_)
    {
        std::string ignored;
        commitTransaction(false, &ignored);
    }
    if (logFile_.is_open())
    {
        logFile_.flush();
        if (!logFile_)
        {
            setLastError("刷新 BMP JSONL 失败：" + logFilePath_);
        }
        logFile_.close();
    }
    if (insertStatement_)
    {
        sqlite3_finalize(insertStatement_);
        insertStatement_ = nullptr;
    }
    if (database_)
    {
        const auto closeResult = sqlite3_close_v2(database_);
        if (closeResult != SQLITE_OK && lastError_.empty())
        {
            setLastError("关闭 SQLite 日志失败：" + std::string(sqlite3_errstr(closeResult)));
        }
        database_ = nullptr;
    }
    transactionOpen_ = false;
    pendingTransactionRows_ = 0;
}

bool EventStore::flush(std::string* error)
{
    std::scoped_lock lock(mutex_);
    if (error)
    {
        error->clear();
    }
    if (!database_)
    {
        if (!logFile_.is_open())
        {
            return true;
        }
        setLastError("SQLite 日志数据库未打开，无法提交", error);
        return false;
    }
    const auto committed = commitTransaction(true, error);
    logFile_.flush();
    if (!logFile_)
    {
        setLastError("刷新 BMP JSONL 失败：" + logFilePath_, error);
        return false;
    }
    return committed;
}

void EventStore::enqueueEvents(std::vector<SimulationEvent> events)
{
    std::scoped_lock lock(mutex_);
    if (!database_ || !logFile_.is_open())
    {
        return;
    }
    for (auto& event : events)
    {
        if (event.timestamp == 0)
        {
            event.timestamp = SimulationEpochMilliseconds;
        }
        if (event.id == 0)
        {
            event.id = nextId_++;
        }
        else
        {
            nextId_ = std::max(nextId_, event.id + 1);
        }
        const auto rawJson = eventToJson(event).dump();
        std::string error;
        if (!insertEvent(event, rawJson, &error))
        {
            setLastError(std::move(error));
            continue;
        }
        logFile_ << rawJson << '\n';
        if (!logFile_)
        {
            setLastError("写入 BMP JSONL 失败：" + logFilePath_);
        }
        lastInsertedEventId_ = std::max(lastInsertedEventId_, event.id);
        ++pendingTransactionRows_;
        if (pendingTransactionRows_ >= transactionBatchSize_)
        {
            commitTransaction(true, nullptr);
        }
    }
}

void EventStore::appendEvent(SimulationEvent event)
{
    std::vector<SimulationEvent> events;
    events.push_back(std::move(event));
    enqueueEvents(std::move(events));
}

bool EventStore::isOpen() const noexcept
{
    std::scoped_lock lock(mutex_);
    return database_ != nullptr;
}

bool EventStore::initializeSchema(std::string* error)
{
    static constexpr std::array statements{
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",
        "PRAGMA temp_store=MEMORY",
        "PRAGMA cache_size=-65536",
        "PRAGMA wal_autocheckpoint=16384",
        "CREATE TABLE IF NOT EXISTS bmp_events (id INTEGER PRIMARY KEY, timestamp TEXT NOT NULL, event TEXT, "
        "router TEXT, from_peer TEXT, to_peer TEXT, from_as INTEGER, to_as INTEGER, msg_type TEXT, action TEXT, "
        "sequence INTEGER, prefixes TEXT, withdrawn TEXT, next_hop TEXT, as_path TEXT, local_pref INTEGER, med INTEGER, "
        "raw_json TEXT NOT NULL)",
        "CREATE INDEX IF NOT EXISTS idx_bmp_event ON bmp_events(event)",
    };
    for (const auto* statement : statements)
    {
        if (!execute(statement, error))
        {
            return false;
        }
    }
    return true;
}

bool EventStore::prepareInsert(std::string* error)
{
    static constexpr auto sql =
        "INSERT INTO bmp_events (id,timestamp,event,router,from_peer,to_peer,from_as,to_as,msg_type,action,sequence,"
        "prefixes,withdrawn,next_hop,as_path,local_pref,med,raw_json) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(database_, sql, -1, &insertStatement_, nullptr) == SQLITE_OK)
    {
        return true;
    }
    setLastError("准备 SQLite 写入语句失败：" + sqliteError(database_), error);
    return false;
}

bool EventStore::insertEvent(const SimulationEvent& event, const std::string& rawJson, std::string* error)
{
    // sqlite3_reset() returns the previous sqlite3_step() status even when the
    // statement was reset successfully, so that return value is not a reset
    // failure signal for a reusable INSERT statement.
    sqlite3_reset(insertStatement_);
    if (sqlite3_clear_bindings(insertStatement_) != SQLITE_OK)
    {
        if (error)
        {
            *error = "清除 SQLite 写入参数失败：" + sqliteError(database_);
        }
        return false;
    }
    constexpr auto maximumSqliteInteger = static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max());
    if (event.id > maximumSqliteInteger || event.sequence > maximumSqliteInteger)
    {
        if (error)
        {
            *error = "事件 ID 或序列号超过 SQLite INTEGER 范围";
        }
        return false;
    }
    const auto timestamp = formatIsoTimestamp(event.timestamp);
    const auto prefixes = join(event.prefixes, ',');
    const auto withdrawn = join(event.withdrawn, ',');
    const auto asPath = joinAsPath(event.asPath);
    if (!bindInteger(database_, insertStatement_, 1, static_cast<sqlite3_int64>(event.id), "绑定事件 ID 失败", error) ||
        !bindText(database_, insertStatement_, 2, timestamp, "绑定事件时间失败", error) ||
        !bindText(database_, insertStatement_, 3, event.event, "绑定事件类型失败", error) ||
        !bindText(database_, insertStatement_, 4, event.router, "绑定路由器失败", error) ||
        !bindText(database_, insertStatement_, 5, event.from, "绑定来源邻居失败", error) ||
        !bindText(database_, insertStatement_, 6, event.to, "绑定目标邻居失败", error) ||
        !bindOptionalUnsigned(database_, insertStatement_, 7, event.fromAs, error) ||
        !bindOptionalUnsigned(database_, insertStatement_, 8, event.toAs, error) ||
        !bindText(database_, insertStatement_, 9, event.messageType, "绑定报文类型失败", error) ||
        !bindText(database_, insertStatement_, 10, event.action, "绑定动作失败", error) ||
        !bindInteger(database_, insertStatement_, 11, static_cast<sqlite3_int64>(event.sequence), "绑定序列号失败", error) ||
        !bindText(database_, insertStatement_, 12, prefixes, "绑定前缀失败", error) ||
        !bindText(database_, insertStatement_, 13, withdrawn, "绑定撤销前缀失败", error) ||
        !bindText(database_, insertStatement_, 14, event.nextHop, "绑定下一跳失败", error) ||
        !bindText(database_, insertStatement_, 15, asPath, "绑定 AS 路径失败", error) ||
        !bindOptionalUnsigned(database_, insertStatement_, 16, event.localPref, error) ||
        !bindOptionalUnsigned(database_, insertStatement_, 17, event.med, error) ||
        !bindText(database_, insertStatement_, 18, rawJson, "绑定原始事件 JSON 失败", error))
    {
        return false;
    }
    if (sqlite3_step(insertStatement_) == SQLITE_DONE)
    {
        return true;
    }
    if (error)
    {
        *error = "写入 SQLite 日志失败：" + sqliteError(database_);
    }
    return false;
}

bool EventStore::execute(const char* sql, std::string* error)
{
    char* rawError = nullptr;
    if (sqlite3_exec(database_, sql, nullptr, nullptr, &rawError) == SQLITE_OK)
    {
        return true;
    }
    std::string message = rawError ? rawError : sqliteError(database_);
    sqlite3_free(rawError);
    setLastError("执行 SQLite 语句失败：" + message, error);
    return false;
}

bool EventStore::beginTransaction(std::string* error)
{
    if (transactionOpen_)
    {
        return true;
    }
    if (!execute("BEGIN IMMEDIATE", error))
    {
        return false;
    }
    transactionOpen_ = true;
    pendingTransactionRows_ = 0;
    return true;
}

bool EventStore::commitTransaction(bool restart, std::string* error)
{
    if (!database_)
    {
        return false;
    }
    if (transactionOpen_)
    {
        if (!execute("COMMIT", error))
        {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
            transactionOpen_ = false;
            lastInsertedEventId_ = committedEventId_;
            return false;
        }
        transactionOpen_ = false;
        committedEventId_ = std::max(committedEventId_, lastInsertedEventId_);
    }
    else
    {
        // If restarting a previous transaction failed, SQLite writes made
        // afterwards were autocommitted and must advance the visible watermark.
        committedEventId_ = std::max(committedEventId_, lastInsertedEventId_);
    }
    pendingTransactionRows_ = 0;
    return !restart || beginTransaction(error);
}

void EventStore::setLastError(std::string message, std::string* error)
{
    lastError_ = std::move(message);
    if (error)
    {
        *error = lastError_;
    }
}

Json EventStore::eventToJson(const SimulationEvent& event)
{
    Json object{{"id", unsigned64ToJson(event.id)},
                {"timestamp", formatIsoTimestamp(event.timestamp == 0 ? SimulationEpochMilliseconds : event.timestamp)},
                {"event", event.event},
                {"router", event.router},
                {"from", event.from},
                {"to", event.to},
                {"msg_type", event.messageType},
                {"action", event.action},
                {"sequence", unsigned64ToJson(event.sequence)},
                {"prefixes", event.prefixes},
                {"withdrawn", event.withdrawn},
                {"next_hop", event.nextHop},
                {"as_path", event.asPath},
                {"details", event.details}};
    if (event.fromAs)
    {
        object["from_as"] = *event.fromAs;
    }
    if (event.toAs)
    {
        object["to_as"] = *event.toAs;
    }
    if (event.localPref)
    {
        object["local_pref"] = *event.localPref;
    }
    if (event.med)
    {
        object["med"] = *event.med;
    }
    return object;
}

std::optional<SimulationEvent> EventStore::eventFromJson(const Json& object, std::string* error)
{
    if (error)
    {
        error->clear();
    }
    if (!object.is_object())
    {
        if (error)
        {
            *error = "SimulationEvent JSON 根节点必须是对象";
        }
        return std::nullopt;
    }
    try
    {
        SimulationEvent event;
        const auto id = jsonUnsigned64(object.at("id"));
        const auto sequence = jsonUnsigned64(object.at("sequence"));
        const auto timestampText = object.at("timestamp").get<std::string>();
        const auto timestamp = parseIsoTimestamp(timestampText);
        if (!id || !sequence || !timestamp)
        {
            throw std::runtime_error("id、sequence 或 timestamp 无效");
        }
        event.id = *id;
        event.sequence = *sequence;
        event.timestamp = *timestamp;
        event.event = object.at("event").get<std::string>();
        event.router = object.at("router").get<std::string>();
        event.from = object.at("from").get<std::string>();
        event.to = object.at("to").get<std::string>();
        event.messageType = object.at("msg_type").get<std::string>();
        event.action = object.at("action").get<std::string>();
        event.prefixes = object.at("prefixes").get<std::vector<std::string>>();
        event.withdrawn = object.at("withdrawn").get<std::vector<std::string>>();
        event.nextHop = object.at("next_hop").get<std::string>();
        const auto& jsonPath = object.at("as_path");
        if (!jsonPath.is_array())
        {
            throw std::runtime_error("as_path 必须是 32 位非负整数数组");
        }
        event.asPath.reserve(jsonPath.size());
        for (const auto& entry : jsonPath)
        {
            const auto asn = jsonUnsigned32(entry);
            if (!asn)
            {
                throw std::runtime_error("as_path 必须是 32 位非负整数数组");
            }
            event.asPath.push_back(*asn);
        }
        const auto& details = object.at("details");
        if (!details.is_object())
        {
            throw std::runtime_error("details 必须是对象");
        }
        for (auto iterator = details.begin(); iterator != details.end(); ++iterator)
        {
            if (!iterator.value().is_string())
            {
                throw std::runtime_error("details 中的值必须是字符串");
            }
            event.details.emplace(iterator.key(), iterator.value().get<std::string>());
        }
        const auto readOptionalUnsigned32 = [&](std::string_view key, std::optional<std::uint32_t>* value)
        {
            const auto iterator = object.find(std::string(key));
            if (iterator == object.end() || iterator->is_null())
            {
                value->reset();
                return;
            }
            const auto decoded = jsonUnsigned32(*iterator);
            if (!decoded)
            {
                throw std::runtime_error(std::string(key) + " 必须是 32 位非负整数或 null");
            }
            *value = *decoded;
        };
        readOptionalUnsigned32("from_as", &event.fromAs);
        readOptionalUnsigned32("to_as", &event.toAs);
        readOptionalUnsigned32("local_pref", &event.localPref);
        readOptionalUnsigned32("med", &event.med);
        return event;
    }
    catch (const std::exception& exception)
    {
        if (error)
        {
            *error = std::string("SimulationEvent JSON 无效：") + exception.what();
        }
        return std::nullopt;
    }
}

EventHistoryPage EventStore::queryDatabase(const std::string& path, int limit, const std::string& filter,
                                           std::string* error,
                                           const std::function<bool(std::size_t, std::size_t)>& progress,
                                           const std::function<bool()>& cancelled)
{
    EventHistoryPage result;
    std::string localError;
    auto* queryError = error ? error : &localError;
    queryError->clear();
    ReadDatabase database;
    if (!openReadDatabase(path, &database, queryError) || cancelledQuery(cancelled, queryError))
    {
        return result;
    }
    ReadTransaction snapshot;
    if (!snapshot.begin(database.handle, "无法创建历史日志只读快照", queryError) ||
        !queryMaximumEventId(database.handle, &result.maxEventId, queryError, cancelled) ||
        !queryEventCounts(database.handle, filter, {}, &result, queryError, cancelled))
    {
        return result;
    }
    const auto rowsToLoad = limit > 0 ? std::min<std::int64_t>(result.filteredCount, limit) : result.filteredCount;
    const auto progressTotal = countAsSize(rowsToLoad);
    if (progress && !progress(0, progressTotal))
    {
        return result;
    }
    std::string sql = "SELECT raw_json FROM bmp_events" + eventWhereClause(filter);
    sql += limit > 0 ? " ORDER BY id DESC LIMIT ?" : " ORDER BY id ASC";
    Statement statement;
    if (!prepare(database.handle, sql, &statement, queryError))
    {
        return result;
    }
    int binding = 1;
    if (!bindEventConditions(database.handle, statement.handle, filter, {}, {}, &binding, queryError))
    {
        return result;
    }
    if (limit > 0 &&
        !bindInteger(database.handle, statement.handle, binding, static_cast<sqlite3_int64>(limit),
                     "绑定历史日志条数限制失败", queryError))
    {
        return result;
    }
    result.events.reserve(progressTotal);
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement.handle)) == SQLITE_ROW)
    {
        if (cancelledQuery(cancelled, queryError))
        {
            result.events.clear();
            return result;
        }
        if (!appendEventFromColumn(statement.handle, &result.events, "历史事件 JSON 解析失败", queryError))
        {
            result.events.clear();
            return result;
        }
        if (progress && result.events.size() % 512 == 0 &&
            !progress(result.events.size(), progressTotal))
        {
            result.events.clear();
            return result;
        }
    }
    if (stepResult != SQLITE_DONE)
    {
        *queryError = "查询历史日志失败：" + sqliteError(database.handle);
        result.events.clear();
        return result;
    }
    if (limit > 0)
    {
        std::reverse(result.events.begin(), result.events.end());
    }
    if (progress)
    {
        progress(result.events.size(), progressTotal);
    }
    return result;
}

EventHistoryPage EventStore::countDatabase(const std::string& path, const std::string& filter,
                                           std::uint64_t maxEventId, std::string* error,
                                           const std::function<bool()>& cancelled)
{
    EventHistoryPage result;
    result.maxEventId = maxEventId;
    std::string localError;
    auto* queryError = error ? error : &localError;
    queryError->clear();
    ReadDatabase database;
    if (!openReadDatabase(path, &database, queryError) || cancelledQuery(cancelled, queryError))
    {
        return result;
    }
    ReadTransaction snapshot;
    if (!snapshot.begin(database.handle, "无法创建实时日志只读快照", queryError))
    {
        return result;
    }
    queryEventCounts(database.handle, filter, std::optional<std::uint64_t>{maxEventId}, &result, queryError, cancelled);
    return result;
}

ConvergenceHistoryPage EventStore::queryConvergenceDatabase(const std::string& path, int limit, std::string* error,
                                                             const std::function<bool()>& cancelled)
{
    ConvergenceHistoryPage result;
    std::string localError;
    auto* queryError = error ? error : &localError;
    queryError->clear();
    ReadDatabase database;
    if (!openReadDatabase(path, &database, queryError) || cancelledQuery(cancelled, queryError))
    {
        return result;
    }
    ReadTransaction snapshot;
    if (!snapshot.begin(database.handle, "无法创建收敛历史只读快照", queryError) ||
        !queryMaximumEventId(database.handle, &result.maxEventId, queryError, cancelled) ||
        !querySingleCount(database.handle, {}, {}, "converged", &result.totalCount, "统计收敛记录总数", queryError,
                          cancelled) ||
        cancelledQuery(cancelled, queryError))
    {
        return result;
    }
    Statement statement;
    const auto sql = std::string("SELECT raw_json FROM bmp_events") + eventWhereClause({}, {}, "converged") +
                     " ORDER BY id DESC LIMIT ?";
    if (!prepare(database.handle, sql, &statement, queryError))
    {
        return result;
    }
    int binding = 1;
    if (!bindEventConditions(database.handle, statement.handle, {}, {}, "converged", &binding, queryError) ||
        !bindInteger(database.handle, statement.handle, binding, static_cast<sqlite3_int64>(std::max(0, limit)),
                     "绑定收敛历史条数限制失败", queryError))
    {
        return result;
    }
    result.events.reserve(countAsSize(std::min<std::int64_t>(result.totalCount, std::max(0, limit))));
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement.handle)) == SQLITE_ROW)
    {
        if (cancelledQuery(cancelled, queryError))
        {
            result.events.clear();
            return result;
        }
        if (!appendEventFromColumn(statement.handle, &result.events, "收敛事件 JSON 解析失败", queryError))
        {
            result.events.clear();
            return result;
        }
    }
    if (stepResult != SQLITE_DONE)
    {
        *queryError = "查询收敛历史失败：" + sqliteError(database.handle);
        result.events.clear();
        return result;
    }
    std::reverse(result.events.begin(), result.events.end());
    return result;
}

std::vector<SimulationEvent> EventStore::readDatabase(
    const std::string& path, int limit, std::string* error,
    const std::function<bool(std::size_t, std::size_t)>& progress)
{
    return queryDatabase(path, limit, {}, error, progress).events;
}

} // namespace bgptester
