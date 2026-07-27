#include "core/HeadlessSession.hpp"

#include "core/EventStore.hpp"
#include "core/RouterPolicy.hpp"
#include "core/SimulationEngine.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bgptester
{
namespace
{

constexpr std::int64_t MaximumIntervalMs = 24LL * 60 * 60 * 1000;
constexpr std::int64_t MaximumQuietMs = 600000;

HeadlessCommandResult success(Json data = Json::object())
{
    return {.ok = true, .data = std::move(data)};
}

HeadlessCommandResult failure(std::string error, Json data = Json::object())
{
    return {.ok = false, .data = std::move(data), .error = std::move(error)};
}

std::string trim(std::string value)
{
    const auto space = [](unsigned char character) { return std::isspace(character) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), space);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), space).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string join(const std::vector<std::string>& values, std::string_view separator)
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
        {
            stream << separator;
        }
        stream << values[index];
    }
    return stream.str();
}

const Json* field(const Json& object, std::string_view key)
{
    if (!object.is_object())
    {
        return nullptr;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &*found;
}

bool readRequiredString(const Json& object, std::string_view key, std::string* value, std::string* error)
{
    const auto* entry = field(object, key);
    if (!entry || !entry->is_string())
    {
        *error = "字段 " + std::string(key) + " 必须是非空字符串";
        return false;
    }
    *value = trim(entry->get<std::string>());
    if (value->empty())
    {
        *error = "字段 " + std::string(key) + " 必须是非空字符串";
        return false;
    }
    return true;
}

bool readOptionalString(const Json& object, std::string_view key, std::string* value, std::string* error,
                        bool trimValue = true)
{
    const auto* entry = field(object, key);
    if (!entry)
    {
        return true;
    }
    if (!entry->is_string())
    {
        *error = "字段 " + std::string(key) + " 必须是字符串";
        return false;
    }
    *value = entry->get<std::string>();
    if (trimValue)
    {
        *value = trim(std::move(*value));
    }
    return true;
}

bool readOptionalBool(const Json& object, std::string_view key, bool* value, std::string* error)
{
    const auto* entry = field(object, key);
    if (!entry)
    {
        return true;
    }
    if (!entry->is_boolean())
    {
        *error = "字段 " + std::string(key) + " 必须是布尔值";
        return false;
    }
    *value = entry->get<bool>();
    return true;
}

bool readInteger(const Json& object, std::string_view key, std::int64_t minimum, std::int64_t maximum,
                 std::int64_t* value, std::string* error, bool required = false)
{
    const auto* entry = field(object, key);
    if (!entry)
    {
        if (required)
        {
            *error = "缺少必需字段 " + std::string(key);
            return false;
        }
        return true;
    }
    long double number = 0;
    try
    {
        if (entry->is_number_unsigned())
        {
            number = static_cast<long double>(entry->get<std::uint64_t>());
        }
        else if (entry->is_number_integer())
        {
            number = static_cast<long double>(entry->get<std::int64_t>());
        }
        else if (entry->is_number_float())
        {
            number = static_cast<long double>(entry->get<double>());
        }
        else
        {
            throw std::runtime_error("not numeric");
        }
    }
    catch (const std::exception&)
    {
        *error = "字段 " + std::string(key) + " 必须是整数";
        return false;
    }
    if (!std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum)
    {
        *error = "字段 " + std::string(key) + " 必须是 " + std::to_string(minimum) + " 到 " +
                 std::to_string(maximum) + " 之间的整数";
        return false;
    }
    *value = static_cast<std::int64_t>(number);
    return true;
}

bool readCoordinate(const Json& object, std::string_view key, double* value, std::string* error, bool required = false)
{
    const auto* entry = field(object, key);
    if (!entry)
    {
        if (required)
        {
            *error = "缺少必需字段 " + std::string(key);
            return false;
        }
        return true;
    }
    if (!entry->is_number())
    {
        *error = "字段 " + std::string(key) + " 必须是有限数值";
        return false;
    }
    const auto number = entry->get<double>();
    if (!std::isfinite(number))
    {
        *error = "字段 " + std::string(key) + " 必须是有限数值";
        return false;
    }
    *value = number;
    return true;
}

bool readStringList(const Json& object, std::string_view key, std::vector<std::string>* values, std::string* error)
{
    const auto* entry = field(object, key);
    if (!entry)
    {
        return true;
    }
    if (!entry->is_array())
    {
        *error = "字段 " + std::string(key) + " 必须是字符串数组";
        return false;
    }
    std::vector<std::string> result;
    std::set<std::string, std::less<>> seen;
    for (const auto& item : *entry)
    {
        if (!item.is_string())
        {
            *error = "字段 " + std::string(key) + " 必须是字符串数组";
            return false;
        }
        auto value = trim(item.get<std::string>());
        if (!value.empty() && seen.insert(value).second)
        {
            result.push_back(std::move(value));
        }
    }
    *values = std::move(result);
    return true;
}

std::string absolutePath(const std::filesystem::path& path)
{
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute.lexically_normal()).string();
}

std::string routeSourceName(RouteSource source)
{
    switch (source)
    {
        case RouteSource::Local:
            return "local";
        case RouteSource::Customer:
            return "customer";
        case RouteSource::Peer:
            return "peer";
        case RouteSource::Provider:
            return "provider";
        case RouteSource::Unspecified:
            return "unspecified";
    }
    return "unspecified";
}

Json tfpVectorToJson(const TfpVersionVector& vector)
{
    auto result = Json::array();
    for (const auto& [entity, version] : vector)
    {
        result.push_back(Json{{"asn", entity.asn}, {"entity_id", entity.entityId}, {"version", version}});
    }
    return result;
}

Json linkToJson(const LinkConfig& link)
{
    return Json{{"a", link.a},
                {"b", link.b},
                {"enabled", link.enabled},
                {"delay_ms", link.delayMs},
                {"rr_client_from_a", link.rrClientFromA},
                {"rr_client_from_b", link.rrClientFromB},
                {"mrai_ms_from_a", link.mraiMsFromA},
                {"mrai_ms_from_b", link.mraiMsFromB},
                {"relationship", toString(link.businessRelationship)}};
}

Json routerToJson(const RouterConfig& router)
{
    return Json{{"id", router.id},
                {"router_id", router.routerId},
                {"asn", router.asn},
                {"cluster_id", router.clusterId},
                {"originated_prefixes", router.originatedPrefixes},
                {"position", Json{{"x", router.position.x}, {"y", router.position.y}}},
                {"plugin", Json{{"id", router.pluginId}, {"settings", router.pluginSettings}}}};
}

constexpr std::array<std::uint32_t, 64> Sha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

std::uint32_t rotateRight(std::uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32U - bits));
}

std::string sha256(std::string_view input)
{
    std::vector<std::uint8_t> bytes(input.begin(), input.end());
    const auto bitLength = static_cast<std::uint64_t>(bytes.size()) * 8U;
    bytes.push_back(0x80U);
    while (bytes.size() % 64U != 56U)
    {
        bytes.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        bytes.push_back(static_cast<std::uint8_t>(bitLength >> shift));
    }

    std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64U)
    {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index)
        {
            const auto base = offset + index * 4U;
            words[index] = (static_cast<std::uint32_t>(bytes[base]) << 24U) |
                           (static_cast<std::uint32_t>(bytes[base + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(bytes[base + 2U]) << 8U) |
                           static_cast<std::uint32_t>(bytes[base + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index)
        {
            const auto s0 = rotateRight(words[index - 15U], 7U) ^ rotateRight(words[index - 15U], 18U) ^
                            (words[index - 15U] >> 3U);
            const auto s1 = rotateRight(words[index - 2U], 17U) ^ rotateRight(words[index - 2U], 19U) ^
                            (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }

        auto a = hash[0];
        auto b = hash[1];
        auto c = hash[2];
        auto d = hash[3];
        auto e = hash[4];
        auto f = hash[5];
        auto g = hash[6];
        auto h = hash[7];
        for (std::size_t index = 0; index < words.size(); ++index)
        {
            const auto upper = rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
            const auto choice = (e & f) ^ (~e & g);
            const auto temporary1 = h + upper + choice + Sha256Constants[index] + words[index];
            const auto lowerSigma = rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = lowerSigma + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : hash)
    {
        output << std::setw(8) << value;
    }
    return output.str();
}

std::string topologySha256(const Topology& topology)
{
    auto stable = topology;
    std::sort(stable.links.begin(), stable.links.end(), [](const LinkConfig& lhs, const LinkConfig& rhs)
              { return Topology::edgeKey(lhs.a, lhs.b) < Topology::edgeKey(rhs.a, rhs.b); });
    return sha256(stable.toJson().dump());
}

bool applyRouterFields(const Json& object, RouterConfig* router, std::string* error, bool adding)
{
    if (adding && object.contains("id") && !readRequiredString(object, "id", &router->id, error))
    {
        return false;
    }
    if (object.contains("router_id") && !readRequiredString(object, "router_id", &router->routerId, error))
    {
        return false;
    }
    std::int64_t asn = router->asn;
    if (!readInteger(object, "asn", 1, std::numeric_limits<std::uint32_t>::max(), &asn, error))
    {
        return false;
    }
    router->asn = static_cast<std::uint32_t>(asn);
    if (!readOptionalString(object, "cluster_id", &router->clusterId, error))
    {
        return false;
    }
    if (object.contains("cluster_id") && router->clusterId.empty())
    {
        router->clusterId = router->routerId;
    }
    const auto prefixKey = object.contains("originated_prefixes") ? "originated_prefixes" : "prefixes";
    if (object.contains(prefixKey) && !readStringList(object, prefixKey, &router->originatedPrefixes, error))
    {
        return false;
    }
    if (!readCoordinate(object, "x", &router->position.x, error) ||
        !readCoordinate(object, "y", &router->position.y, error))
    {
        return false;
    }
    if (const auto* position = field(object, "position"))
    {
        if (!position->is_object() || !readCoordinate(*position, "x", &router->position.x, error, true) ||
            !readCoordinate(*position, "y", &router->position.y, error, true))
        {
            if (error->empty())
            {
                *error = "字段 position 必须是包含 x/y 的对象";
            }
            return false;
        }
    }
    if (const auto* plugin = field(object, "plugin"))
    {
        if (plugin->is_string())
        {
            router->pluginId = trim(plugin->get<std::string>());
        }
        else if (plugin->is_object())
        {
            if (!readRequiredString(*plugin, "id", &router->pluginId, error))
            {
                return false;
            }
            if (const auto* settings = field(*plugin, "settings"))
            {
                if (!settings->is_object())
                {
                    *error = "字段 plugin.settings 必须是 JSON 对象";
                    return false;
                }
                router->pluginSettings = *settings;
            }
        }
        else
        {
            *error = "字段 plugin 必须是插件 ID 字符串或 JSON 对象";
            return false;
        }
    }
    if (object.contains("plugin_id") && !readRequiredString(object, "plugin_id", &router->pluginId, error))
    {
        return false;
    }
    if (const auto* settings = field(object, "plugin_settings"))
    {
        if (!settings->is_object())
        {
            *error = "字段 plugin_settings 必须是 JSON 对象";
            return false;
        }
        router->pluginSettings = *settings;
    }
    if (trim(router->pluginId).empty())
    {
        *error = "路由器插件 ID 不能为空";
        return false;
    }
    return true;
}

bool hasExplicitPluginSettings(const Json& object)
{
    if (object.contains("plugin_settings"))
    {
        return true;
    }
    const auto* plugin = field(object, "plugin");
    return plugin && plugin->is_object() && plugin->contains("settings");
}

bool applyLinkFields(const Json& object, LinkConfig* link, bool externalSession, std::string* error)
{
    if (!readOptionalBool(object, "enabled", &link->enabled, error) ||
        !readOptionalBool(object, "rr_client_from_a", &link->rrClientFromA, error) ||
        !readOptionalBool(object, "rr_client_from_b", &link->rrClientFromB, error))
    {
        return false;
    }
    std::int64_t delay = link->delayMs;
    std::int64_t mraiA = link->mraiMsFromA;
    std::int64_t mraiB = link->mraiMsFromB;
    if (!readInteger(object, "delay_ms", 0, MaximumIntervalMs, &delay, error) ||
        !readInteger(object, "mrai_ms_from_a", 0, MaximumIntervalMs, &mraiA, error) ||
        !readInteger(object, "mrai_ms_from_b", 0, MaximumIntervalMs, &mraiB, error))
    {
        return false;
    }
    link->delayMs = static_cast<int>(delay);
    link->mraiMsFromA = static_cast<int>(mraiA);
    link->mraiMsFromB = static_cast<int>(mraiB);
    if (object.contains("relationship"))
    {
        std::string relationship;
        if (!readRequiredString(object, "relationship", &relationship, error))
        {
            return false;
        }
        const auto parsed = linkBusinessRelationshipFromString(relationship);
        if (!parsed)
        {
            *error = "relationship 必须是 unspecified、peer、a_provider 或 b_provider";
            return false;
        }
        link->businessRelationship = *parsed;
    }
    if (!externalSession && link->businessRelationship != LinkBusinessRelationship::Unspecified)
    {
        *error = "同一 AS 内的链路不能设置商业关系";
        return false;
    }
    return true;
}

Json commandDescription(std::string command, std::string summary, Json fields = Json::array())
{
    return Json{{"command", std::move(command)}, {"summary", std::move(summary)}, {"fields", std::move(fields)}};
}

class SplitMix64 final
{
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next()
    {
        auto value = (state_ += 0x9e3779b97f4a7c15ULL);
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    int boundedInclusive(int minimum, int maximum)
    {
        const auto width = static_cast<std::uint64_t>(static_cast<std::int64_t>(maximum) - minimum) + 1U;
        return minimum + static_cast<int>(next() % width);
    }

private:
    std::uint64_t state_;
};

std::uint64_t stableSeed(std::string_view material)
{
    std::uint64_t result = 1469598103934665603ULL;
    for (const auto character : material)
    {
        result ^= static_cast<unsigned char>(character);
        result *= 1099511628211ULL;
    }
    return result;
}

std::string isoTimestamp(std::int64_t milliseconds)
{
    const auto seconds = milliseconds / 1000;
    const auto remainder = static_cast<int>((milliseconds % 1000 + 1000) % 1000);
    const auto time = static_cast<std::time_t>(seconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << remainder << 'Z';
    return output.str();
}

bool writeJsonFile(const std::filesystem::path& path, const Json& document, std::string* error)
{
    std::error_code filesystemError;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), filesystemError);
        if (filesystemError)
        {
            *error = "无法创建快照目录：" + path.parent_path().string();
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        *error = "无法写入快照：" + path.string();
        return false;
    }
    output << document.dump(2) << '\n';
    if (!output)
    {
        *error = "无法完整写入快照：" + path.string();
        return false;
    }
    return true;
}

} // namespace

HeadlessSession::HeadlessSession()
    : topology_(Topology::starter()), engine_(std::make_unique<SimulationEngine>()), eventStore_(std::make_unique<EventStore>())
{
    engine_->eventsGenerated.connect([this](const std::vector<SimulationEvent>& events)
    {
        if (eventRunOpen_ && eventStore_)
        {
            eventStore_->enqueueEvents(events);
            refreshEventStoreStatus();
        }
    });
    engine_->errorOccurred.connect([this](const std::string& error) { lastEngineError_ = error; });
    engine_->statsChanged.connect([this](const SimulationStats& stats)
    {
        latestStats_ = stats;
        simulationRunning_ = stats.running;
        simulationConverged_ = stats.converged;
    });
}

HeadlessSession::~HeadlessSession()
{
    shutdown();
}

bool HeadlessSession::isRunning() const
{
    return engine_ && engine_->isRunning();
}

void HeadlessSession::refreshRuntimeStatus()
{
    if (!engine_)
    {
        return;
    }
    latestStats_ = engine_->statsSnapshot();
    simulationRunning_ = latestStats_.running;
    simulationConverged_ = latestStats_.converged;
    lastEngineError_ = engine_->lastError();
}

bool HeadlessSession::stabilizeRuntime(std::string* error)
{
    if (!engine_ || !engine_->isRunning())
    {
        if (error)
        {
            *error = "仿真尚未运行";
        }
        return false;
    }
    if (interruptionFlag_ && interruptionFlag_->load(std::memory_order_relaxed))
    {
        if (error)
        {
            *error = "操作被中断";
        }
        return false;
    }
    const auto drained = engine_->runUntilConverged();
    refreshRuntimeStatus();
    if (!drained || !latestStats_.running || !latestStats_.converged)
    {
        if (error)
        {
            *error = !lastEngineError_.empty() ? lastEngineError_ : "仿真未能到达收敛边界";
        }
        return false;
    }
    return true;
}

void HeadlessSession::refreshEventStoreStatus()
{
    if (!eventStore_ || !eventRunOpen_)
    {
        return;
    }
    lastStoreError_ = eventStore_->lastError();
    committedEventId_ = eventStore_->committedEventId();
    eventRunSerial_ = eventStore_->runSerial();
}

HeadlessCommandResult HeadlessSession::execute(const Json& command)
{
    if (!command.is_object())
    {
        return failure("命令必须是 JSON 对象");
    }
    refreshRuntimeStatus();
    std::string name;
    if (const auto* value = field(command, "command"); value && value->is_string())
    {
        name = value->get<std::string>();
    }
    if (trim(name).empty())
    {
        if (const auto* value = field(command, "op"); value && value->is_string())
        {
            name = value->get<std::string>();
        }
    }
    name = lower(trim(std::move(name)));
    std::replace(name.begin(), name.end(), '-', '_');
    if (name.empty())
    {
        return failure("命令对象必须包含非空字符串字段 command");
    }
    return dispatch(name, command);
}

HeadlessCommandResult HeadlessSession::dispatch(const std::string& name, const Json& command)
{
    static const std::map<std::string, Handler, std::less<>> handlers{
        {"help", &HeadlessSession::helpCommand},
        {"status", &HeadlessSession::statusCommand},
        {"get_stats", &HeadlessSession::statusCommand},
        {"new", &HeadlessSession::newCommand},
        {"load", &HeadlessSession::loadCommand},
        {"open", &HeadlessSession::loadCommand},
        {"save", &HeadlessSession::saveCommand},
        {"save_as", &HeadlessSession::saveCommand},
        {"topology", &HeadlessSession::topologyCommand},
        {"get_topology", &HeadlessSession::topologyCommand},
        {"validate", &HeadlessSession::validateCommand},
        {"plugins", &HeadlessSession::pluginsCommand},
        {"list_plugins", &HeadlessSession::pluginsCommand},
        {"set_simulation", &HeadlessSession::setSimulationCommand},
        {"add_router", &HeadlessSession::addRouterCommand},
        {"update_router", &HeadlessSession::updateRouterCommand},
        {"move_router", &HeadlessSession::moveRouterCommand},
        {"delete_router", &HeadlessSession::deleteRouterCommand},
        {"remove_router", &HeadlessSession::deleteRouterCommand},
        {"add_link", &HeadlessSession::addLinkCommand},
        {"update_link", &HeadlessSession::updateLinkCommand},
        {"delete_link", &HeadlessSession::deleteLinkCommand},
        {"remove_link", &HeadlessSession::deleteLinkCommand},
        {"batch_update", &HeadlessSession::batchUpdateCommand},
        {"start", &HeadlessSession::startCommand},
        {"stop", &HeadlessSession::stopCommand},
        {"wait", &HeadlessSession::waitCommand},
        {"wait_converged", &HeadlessSession::waitConvergedCommand},
        {"set_router_state", &HeadlessSession::setRouterStateCommand},
        {"toggle_router", &HeadlessSession::toggleRouterCommand},
        {"set_link_state", &HeadlessSession::setLinkStateCommand},
        {"toggle_link", &HeadlessSession::toggleLinkCommand},
        {"advertise_prefix", &HeadlessSession::advertisePrefixCommand},
        {"withdraw_prefix", &HeadlessSession::withdrawPrefixCommand},
        {"routers", &HeadlessSession::routersCommand},
        {"get_routers", &HeadlessSession::routersCommand},
        {"rib", &HeadlessSession::ribCommand},
        {"get_rib", &HeadlessSession::ribCommand},
        {"peers", &HeadlessSession::peersCommand},
        {"get_peers", &HeadlessSession::peersCommand},
        {"path", &HeadlessSession::pathCommand},
        {"get_path", &HeadlessSession::pathCommand},
        {"snapshot", &HeadlessSession::snapshotCommand},
        {"export_snapshot", &HeadlessSession::snapshotCommand},
        {"query_events", &HeadlessSession::queryEventsCommand},
        {"events", &HeadlessSession::queryEventsCommand},
        {"query_convergence", &HeadlessSession::queryConvergenceCommand},
        {"convergence", &HeadlessSession::queryConvergenceCommand},
        {"flush_logs", &HeadlessSession::flushLogsCommand},
        {"exit", &HeadlessSession::exitCommand},
        {"quit", &HeadlessSession::exitCommand},
    };
    const auto handler = handlers.find(name);
    return handler == handlers.end() ? failure("未知命令：" + name + "；使用 help 查看命令清单")
                                     : (this->*handler->second)(command);
}

Json HeadlessSession::commandHelp() const
{
    return Json::array({
        commandDescription("help", "列出稳定 JSONL 命令协议"),
        commandDescription("status", "查看文档、运行、收敛和日志状态"),
        commandDescription("new", "新建双路由器起始拓扑", Json::array({"force=false"})),
        commandDescription("load", "加载并校验拓扑 JSON", Json::array({"path", "force=false"})),
        commandDescription("save", "原子保存当前拓扑", Json::array({"path=当前路径"})),
        commandDescription("topology", "返回完整拓扑 JSON"),
        commandDescription("validate", "执行完整拓扑和路由器策略校验"),
        commandDescription("plugins", "列出策略元数据、默认设置和注册错误"),
        commandDescription("set_simulation", "修改全局仿真设置",
                           Json::array({"name", "log_directory", "worker_threads", "convergence_quiet_ms",
                                        "withdrawal_ignores_mrai"})),
        commandDescription("add_router", "添加路由器，未给 ID/Router ID 时自动生成",
                           Json::array({"id", "router_id", "asn", "cluster_id", "prefixes[]", "x/y", "plugin_id",
                                        "plugin_settings{}"})),
        commandDescription("update_router", "编辑或重命名路由器并级联更新链路",
                           Json::array({"id", "new_id", "其余字段同 add_router"})),
        commandDescription("move_router", "持久化路由器画布位置", Json::array({"id", "x", "y"})),
        commandDescription("delete_router", "删除路由器并级联删除相邻链路", Json::array({"id 或 ids[]"})),
        commandDescription("add_link/update_link", "配置链路、双向 MRAI/RR Client 和商业关系",
                           Json::array({"a", "b", "enabled", "delay_ms", "mrai_ms_from_a/b",
                                        "rr_client_from_a/b", "relationship"})),
        commandDescription("delete_link", "按无向端点删除链路", Json::array({"a", "b"})),
        commandDescription("batch_update", "批量设置策略、出站 MRAI 和全链路延迟；随机值可重放",
                           Json::array({"router_ids[]", "plugin_id", "plugin_settings{}", "mrai{}", "delay{}", "seed"})),
        commandDescription("start/stop", "创建 BMP JSONL/SQLite 后启动或停止仿真"),
        commandDescription("wait", "等待指定墙钟时间", Json::array({"milliseconds"})),
        commandDescription("wait_converged", "确定性耗尽当前事件波", Json::array({"timeout_ms=30000（诊断）"})),
        commandDescription("set_router_state/toggle_router", "在稳定边界关闭或恢复节点",
                           Json::array({"router", "enabled"})),
        commandDescription("set_link_state/toggle_link", "在稳定边界断开或恢复链路",
                           Json::array({"a", "b", "enabled"})),
        commandDescription("advertise_prefix/withdraw_prefix", "在稳定边界发布或撤销 IPv4 前缀",
                           Json::array({"router", "prefix"})),
        commandDescription("routers/rib/peers/path", "查询完整运行快照"),
        commandDescription("snapshot", "导出所有 Router/RIB/Peer/逐跳路径和统计", Json::array({"path=可选"})),
        commandDescription("query_events", "查询当前或指定 SQLite 的完整事件和计数",
                           Json::array({"database=当前", "filter", "limit=20000"})),
        commandDescription("query_convergence", "查询当前或指定 SQLite 的收敛记录",
                           Json::array({"database=当前", "limit=5000"})),
        commandDescription("flush_logs", "阻塞刷新 BMP JSONL/SQLite"),
        commandDescription("exit", "安全停止、落盘并结束会话"),
    });
}

HeadlessCommandResult HeadlessSession::helpCommand(const Json&)
{
    return success(Json{{"protocol", "bgptester-cli-jsonl-v1"}, {"commands", commandHelp()}});
}

Json HeadlessSession::statusJson() const
{
    auto originated = Json::object();
    for (const auto& [router, prefixes] : runtimeOriginatedPrefixes_)
    {
        originated[router] = std::vector<std::string>(prefixes.begin(), prefixes.end());
    }
    return Json{{"topology_path", topologyPath_},
                {"dirty", dirty_},
                {"router_count", topology_.routers.size()},
                {"link_count", topology_.links.size()},
                {"running", isRunning()},
                {"converged", simulationConverged_},
                {"runtime_available", runtimeAvailable_},
                {"stats", statsToJson(latestStats_)},
                {"run_directory", runDirectory_},
                {"bmp_jsonl", logFilePath_},
                {"bmp_sqlite", databasePath_},
                {"event_run_serial", eventRunSerial_},
                {"committed_event_id", committedEventId_},
                {"runtime_originated_prefixes", std::move(originated)},
                {"event_store_error", lastStoreError_}};
}

HeadlessCommandResult HeadlessSession::statusCommand(const Json&)
{
    std::string error;
    if (isRunning() && !stabilizeRuntime(&error))
    {
        flushEventRun();
        return failure(error, statusJson());
    }
    if (eventRunOpen_ && !flushEventRun())
    {
        return failure(lastStoreError_, statusJson());
    }
    refreshEventStoreStatus();
    return success(statusJson());
}

HeadlessCommandResult HeadlessSession::rejectWhileRunning() const
{
    return failure("仿真运行期间拓扑编辑被锁定；请先执行 stop");
}

HeadlessCommandResult HeadlessSession::requireRuntime() const
{
    return runtimeAvailable_ ? success() : failure("尚无可查询的仿真运行时；请先执行 start");
}

bool HeadlessSession::beginEventRun(std::string* error)
{
    if (!eventStore_)
    {
        if (error)
        {
            *error = "日志存储不可用";
        }
        return false;
    }
    if (eventRunOpen_ && !endEventRun())
    {
        if (error)
        {
            *error = lastStoreError_;
        }
        return false;
    }
    lastStoreError_.clear();
    logFilePath_.clear();
    databasePath_.clear();
    runDirectory_.clear();
    eventRunSerial_ = 0;
    committedEventId_ = 0;
    std::string storeError;
    const auto started = eventStore_->beginRun(topology_.simulation, &storeError);
    if (!started)
    {
        lastStoreError_ = !storeError.empty() ? storeError : eventStore_->lastError();
        if (error)
        {
            *error = lastStoreError_;
        }
        return false;
    }
    eventRunOpen_ = true;
    logFilePath_ = absolutePath(eventStore_->logFilePath());
    databasePath_ = absolutePath(eventStore_->databasePath());
    runDirectory_ = absolutePath(eventStore_->runDirectory());
    refreshEventStoreStatus();
    return true;
}

bool HeadlessSession::flushEventRun()
{
    if (!eventRunOpen_)
    {
        return lastStoreError_.empty();
    }
    if (!eventStore_)
    {
        lastStoreError_ = "日志存储在刷新前已停止";
        return false;
    }
    std::string error;
    const auto flushed = eventStore_->flush(&error);
    refreshEventStoreStatus();
    if (!flushed || !error.empty())
    {
        lastStoreError_ = !error.empty() ? error : eventStore_->lastError();
        return false;
    }
    return lastStoreError_.empty();
}

bool HeadlessSession::endEventRun()
{
    if (!eventRunOpen_)
    {
        return lastStoreError_.empty();
    }
    if (!eventStore_)
    {
        lastStoreError_ = "日志存储在关闭运行前已停止";
        eventRunOpen_ = false;
        return false;
    }
    eventStore_->endRun();
    committedEventId_ = eventStore_->committedEventId();
    lastStoreError_ = eventStore_->lastError();
    eventRunOpen_ = false;
    return lastStoreError_.empty();
}

bool HeadlessSession::shutdown(std::string* error)
{
    if (shuttingDown_)
    {
        if (error)
        {
            *error = lastStoreError_;
        }
        return lastStoreError_.empty();
    }
    shuttingDown_ = true;
    if (engine_ && engine_->isRunning())
    {
        std::string ignored;
        stabilizeRuntime(&ignored);
        engine_->stopSimulation();
        refreshRuntimeStatus();
    }
    const auto flushed = flushEventRun();
    const auto ended = endEventRun();
    if (error)
    {
        *error = lastStoreError_;
    }
    return flushed && ended && lastStoreError_.empty();
}

HeadlessCommandResult HeadlessSession::newCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    const auto* forceValue = field(command, "force");
    const auto force = forceValue && forceValue->is_boolean() && forceValue->get<bool>();
    if (dirty_ && !force)
    {
        return failure("当前拓扑有未保存更改；若确定丢弃，请设置 force=true");
    }
    topology_ = Topology::starter();
    topologyPath_.clear();
    dirty_ = false;
    invalidateRuntime();
    return success(Json{{"router_count", topology_.routers.size()},
                        {"link_count", topology_.links.size()},
                        {"topology_sha256", topologySha256(topology_)},
                        {"topology", topology_.toJson()}});
}

HeadlessCommandResult HeadlessSession::loadCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    const auto* forceValue = field(command, "force");
    const auto force = forceValue && forceValue->is_boolean() && forceValue->get<bool>();
    if (dirty_ && !force)
    {
        return failure("当前拓扑有未保存更改；若确定丢弃，请设置 force=true");
    }
    std::string path;
    std::string error;
    if (!readRequiredString(command, "path", &path, &error))
    {
        return failure(error);
    }
    const auto absolute = std::filesystem::path(absolutePath(path));
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(absolute, fileError))
    {
        return failure("无法读取拓扑文件：" + absolute.string());
    }
    auto loaded = Topology::load(absolute, &error);
    if (!loaded)
    {
        return failure(error);
    }
    topology_ = std::move(*loaded);
    topologyPath_ = absolute.string();
    dirty_ = false;
    invalidateRuntime();
    return success(Json{{"path", topologyPath_},
                        {"router_count", topology_.routers.size()},
                        {"link_count", topology_.links.size()},
                        {"topology_sha256", topologySha256(topology_)}});
}

HeadlessCommandResult HeadlessSession::saveCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    auto path = topologyPath_;
    std::string error;
    if (!readOptionalString(command, "path", &path, &error) || trim(path).empty())
    {
        return failure(error.empty() ? "尚无保存路径；请提供 path" : error);
    }
    if (path.size() < 5U || lower(path.substr(path.size() - 5U)) != ".json")
    {
        path += ".json";
    }
    path = absolutePath(path);
    if (!topology_.save(path, &error))
    {
        return failure(error);
    }
    topologyPath_ = path;
    dirty_ = false;
    return success(Json{{"path", topologyPath_},
                        {"router_count", topology_.routers.size()},
                        {"link_count", topology_.links.size()},
                        {"topology_sha256", topologySha256(topology_)}});
}

HeadlessCommandResult HeadlessSession::topologyCommand(const Json&)
{
    return success(Json{{"path", topologyPath_},
                        {"dirty", dirty_},
                        {"topology_sha256", topologySha256(topology_)},
                        {"topology", topology_.toJson()}});
}

HeadlessCommandResult HeadlessSession::validateCommand(const Json&)
{
    auto problems = topology_.validate();
    if (problems.empty())
    {
        const auto neighbors = topology_.buildNeighborIndex();
        const auto routers = std::make_shared<const RouterConfigMap>(topology_.routers.begin(), topology_.routers.end());
        for (const auto& [id, config] : topology_.routers)
        {
            std::string creationError;
            RouterPolicyContext context{.config = config, .topologyRouters = routers};
            if (const auto found = neighbors.find(id); found != neighbors.end())
            {
                context.neighbors.insert(found->second.begin(), found->second.end());
            }
            auto policy = RouterPolicyRegistry::instance().createRouterPolicy(std::move(context), &creationError);
            if (!policy)
            {
                problems.push_back("路由器 " + id + " 的插件无法创建：" + creationError);
                continue;
            }
            for (const auto& problem : policy->validateConfiguration())
            {
                problems.push_back("路由器 " + id + " 的插件配置无效：" + problem);
            }
        }
    }
    const auto data = Json{{"valid", problems.empty()}, {"problems", problems}};
    return problems.empty() ? success(data) : failure(join(problems, "\n"), data);
}

HeadlessCommandResult HeadlessSession::pluginsCommand(const Json&)
{
    auto policies = Json::array();
    for (const auto& policy : RouterPolicyRegistry::instance().policies())
    {
        policies.push_back(Json{{"id", policy.metadata.id},
                                {"display_name", policy.metadata.displayName},
                                {"version", policy.metadata.version},
                                {"description", policy.metadata.description},
                                {"api_version", policy.metadata.apiVersion},
                                {"default_settings", policy.metadata.defaultSettings},
                                {"source", policy.source}});
    }
    return success(Json{{"plugins", std::move(policies)},
                        {"registration_errors", RouterPolicyRegistry::instance().registrationErrors()}});
}

HeadlessCommandResult HeadlessSession::setSimulationCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    auto settings = topology_.simulation;
    std::string error;
    if (!readOptionalString(command, "name", &settings.name, &error) ||
        !readOptionalString(command, "log_directory", &settings.logDirectory, &error) ||
        !readOptionalBool(command, "withdrawal_ignores_mrai", &settings.withdrawalIgnoresMrai, &error))
    {
        return failure(error);
    }
    if (command.contains("log_dir") && !readOptionalString(command, "log_dir", &settings.logDirectory, &error))
    {
        return failure(error);
    }
    std::int64_t workers = settings.workerThreads;
    std::int64_t quiet = settings.convergenceQuietMs;
    if (!readInteger(command, "worker_threads", 0, 256, &workers, &error) ||
        !readInteger(command, "convergence_quiet_ms", 0, MaximumQuietMs, &quiet, &error))
    {
        return failure(error);
    }
    settings.workerThreads = static_cast<int>(workers);
    settings.convergenceQuietMs = static_cast<int>(quiet);
    if (trim(settings.name).empty() || trim(settings.logDirectory).empty())
    {
        return failure("实验名称和日志目录不能为空");
    }
    const auto changed = settings != topology_.simulation;
    topology_.simulation = std::move(settings);
    dirty_ = dirty_ || changed;
    if (changed)
    {
        invalidateRuntime();
    }
    return success(Json{{"changed", changed}, {"simulation", topology_.toJson().at("simulation")}});
}

HeadlessCommandResult HeadlessSession::addRouterCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    RouterConfig router;
    router.id = topology_.nextRouterName();
    router.routerId = topology_.nextBgpRouterId();
    router.clusterId = router.routerId;
    const auto initialPluginId = router.pluginId;
    std::string error;
    if (!applyRouterFields(command, &router, &error, true))
    {
        return failure(error);
    }
    if (router.pluginId != initialPluginId && !RouterPolicyRegistry::instance().contains(router.pluginId))
    {
        return failure("添加路由器只能选择已注册插件：" + router.pluginId);
    }
    if (!command.contains("cluster_id"))
    {
        router.clusterId = router.routerId;
    }
    if (router.pluginId != initialPluginId && !hasExplicitPluginSettings(command))
    {
        const auto metadata = RouterPolicyRegistry::instance().metadata(router.pluginId);
        router.pluginSettings = metadata ? metadata->defaultSettings : Json::object();
    }
    if (router.id.empty())
    {
        return failure("路由器 ID 不能为空");
    }
    if (topology_.routers.contains(router.id))
    {
        return failure("路由器 ID 已存在：" + router.id);
    }
    auto candidate = topology_;
    candidate.routers.emplace(router.id, router);
    const auto problems = candidate.validate();
    if (!problems.empty())
    {
        return failure(join(problems, "\n"), Json{{"problems", problems}});
    }
    topology_ = std::move(candidate);
    dirty_ = true;
    invalidateRuntime();
    return success(Json{{"changed", true}, {"router", routerToJson(router)}});
}

HeadlessCommandResult HeadlessSession::updateRouterCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    std::string id;
    std::string error;
    if (!readRequiredString(command, "id", &id, &error))
    {
        return failure(error);
    }
    const auto current = topology_.routers.find(id);
    if (current == topology_.routers.end())
    {
        return failure("路由器不存在：" + id);
    }
    auto updated = current->second;
    const auto initialPluginId = updated.pluginId;
    if (!applyRouterFields(command, &updated, &error, false))
    {
        return failure(error);
    }
    if (updated.pluginId != initialPluginId && !RouterPolicyRegistry::instance().contains(updated.pluginId))
    {
        return failure("编辑路由器只能选择已注册插件：" + updated.pluginId);
    }
    if (updated.pluginId != initialPluginId && !hasExplicitPluginSettings(command))
    {
        const auto metadata = RouterPolicyRegistry::instance().metadata(updated.pluginId);
        updated.pluginSettings = metadata ? metadata->defaultSettings : Json::object();
    }
    auto newId = id;
    if (!readOptionalString(command, "new_id", &newId, &error) || newId.empty())
    {
        return failure(error.empty() ? "new_id 不能为空" : error);
    }
    if (newId != id && topology_.routers.contains(newId))
    {
        return failure("路由器 ID 已存在：" + newId);
    }
    updated.id = newId;
    auto candidate = topology_;
    candidate.routers.erase(id);
    for (auto& link : candidate.links)
    {
        if (link.a == id)
        {
            link.a = newId;
        }
        if (link.b == id)
        {
            link.b = newId;
        }
    }
    candidate.routers.emplace(newId, updated);
    for (auto& link : candidate.links)
    {
        if (link.businessRelationship == LinkBusinessRelationship::Unspecified || (link.a != newId && link.b != newId))
        {
            continue;
        }
        const auto a = candidate.routers.find(link.a);
        const auto b = candidate.routers.find(link.b);
        if (a != candidate.routers.end() && b != candidate.routers.end() && a->second.asn == b->second.asn)
        {
            link.businessRelationship = LinkBusinessRelationship::Unspecified;
        }
    }
    const auto problems = candidate.validate();
    if (!problems.empty())
    {
        return failure(join(problems, "\n"), Json{{"problems", problems}});
    }
    const auto changed = candidate.toJson() != topology_.toJson();
    topology_ = std::move(candidate);
    dirty_ = dirty_ || changed;
    if (changed)
    {
        invalidateRuntime();
    }
    return success(Json{{"changed", changed}, {"old_id", id}, {"id", newId}});
}

HeadlessCommandResult HeadlessSession::moveRouterCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    std::string id;
    std::string error;
    if (!readRequiredString(command, "id", &id, &error))
    {
        return failure(error);
    }
    const auto router = topology_.routers.find(id);
    if (router == topology_.routers.end())
    {
        return failure("路由器不存在：" + id);
    }
    auto position = router->second.position;
    if (!readCoordinate(command, "x", &position.x, &error, true) ||
        !readCoordinate(command, "y", &position.y, &error, true))
    {
        return failure(error);
    }
    const auto changed = position != router->second.position;
    router->second.position = position;
    dirty_ = dirty_ || changed;
    return success(Json{{"changed", changed},
                        {"id", id},
                        {"position", Json{{"x", position.x}, {"y", position.y}}}});
}

HeadlessCommandResult HeadlessSession::deleteRouterCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    std::vector<std::string> ids;
    std::string error;
    if (command.contains("ids"))
    {
        if (!readStringList(command, "ids", &ids, &error) || ids.empty())
        {
            return failure(error.empty() ? "ids 不能为空" : error);
        }
    }
    else
    {
        std::string id;
        if (!readRequiredString(command, "id", &id, &error))
        {
            return failure(error);
        }
        ids.push_back(std::move(id));
    }
    std::set<std::string, std::less<>> removed;
    for (const auto& id : ids)
    {
        if (!topology_.routers.contains(id))
        {
            return failure("路由器不存在：" + id);
        }
        removed.insert(id);
    }
    for (const auto& id : ids)
    {
        topology_.routers.erase(id);
    }
    const auto oldLinkCount = topology_.links.size();
    std::erase_if(topology_.links, [&](const LinkConfig& link)
                  { return removed.contains(link.a) || removed.contains(link.b); });
    dirty_ = true;
    invalidateRuntime();
    return success(Json{{"changed", true},
                        {"deleted_routers", ids},
                        {"deleted_links", oldLinkCount - topology_.links.size()}});
}

HeadlessCommandResult HeadlessSession::addLinkCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    std::string a;
    std::string b;
    std::string error;
    if (!readRequiredString(command, "a", &a, &error) || !readRequiredString(command, "b", &b, &error))
    {
        return failure(error);
    }
    if (a == b)
    {
        return failure("链路不能连接路由器自身：" + a);
    }
    if (!topology_.routers.contains(a) || !topology_.routers.contains(b))
    {
        return failure("链路端点不存在：" + a + " - " + b);
    }
    if (topology_.findLink(a, b))
    {
        return failure("链路已存在：" + a + " - " + b);
    }
    LinkConfig link{.a = a, .b = b};
    const auto external = topology_.routers.at(a).asn != topology_.routers.at(b).asn;
    if (!applyLinkFields(command, &link, external, &error))
    {
        return failure(error);
    }
    auto candidate = topology_;
    candidate.links.push_back(link);
    const auto problems = candidate.validate();
    if (!problems.empty())
    {
        return failure(join(problems, "\n"), Json{{"problems", problems}});
    }
    topology_ = std::move(candidate);
    dirty_ = true;
    invalidateRuntime();
    return success(Json{{"changed", true}, {"link", linkToJson(link)}});
}

HeadlessCommandResult HeadlessSession::updateLinkCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    std::string a;
    std::string b;
    std::string error;
    if (!readRequiredString(command, "a", &a, &error) || !readRequiredString(command, "b", &b, &error))
    {
        return failure(error);
    }
    const auto* current = topology_.findLink(a, b);
    if (!current)
    {
        return failure("链路不存在：" + a + " - " + b);
    }
    auto updated = *current;
    const auto reverseDirection = [](LinkConfig* link)
    {
        std::swap(link->a, link->b);
        std::swap(link->rrClientFromA, link->rrClientFromB);
        std::swap(link->mraiMsFromA, link->mraiMsFromB);
        if (link->businessRelationship == LinkBusinessRelationship::AProviderOfB)
        {
            link->businessRelationship = LinkBusinessRelationship::BProviderOfA;
        }
        else if (link->businessRelationship == LinkBusinessRelationship::BProviderOfA)
        {
            link->businessRelationship = LinkBusinessRelationship::AProviderOfB;
        }
    };
    const auto reversed = updated.a != a;
    if (reversed)
    {
        reverseDirection(&updated);
    }
    const auto external = topology_.routers.at(a).asn != topology_.routers.at(b).asn;
    if (!applyLinkFields(command, &updated, external, &error))
    {
        return failure(error);
    }
    if (reversed)
    {
        reverseDirection(&updated);
    }
    auto candidate = topology_;
    auto* candidateLink = candidate.findLink(a, b);
    const auto changed = candidateLink && *candidateLink != updated;
    if (candidateLink)
    {
        *candidateLink = updated;
    }
    const auto problems = candidate.validate();
    if (!problems.empty())
    {
        return failure(join(problems, "\n"), Json{{"problems", problems}});
    }
    topology_ = std::move(candidate);
    dirty_ = dirty_ || changed;
    if (changed)
    {
        invalidateRuntime();
    }
    return success(Json{{"changed", changed}, {"link", linkToJson(updated)}});
}

HeadlessCommandResult HeadlessSession::deleteLinkCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    std::string a;
    std::string b;
    std::string error;
    if (!readRequiredString(command, "a", &a, &error) || !readRequiredString(command, "b", &b, &error))
    {
        return failure(error);
    }
    const auto key = Topology::edgeKey(a, b);
    const auto originalSize = topology_.links.size();
    std::erase_if(topology_.links,
                  [&](const LinkConfig& link) { return Topology::edgeKey(link.a, link.b) == key; });
    if (topology_.links.size() == originalSize)
    {
        return failure("链路不存在：" + a + " - " + b);
    }
    dirty_ = true;
    invalidateRuntime();
    return success(Json{{"changed", true}, {"a", a}, {"b", b}});
}

HeadlessCommandResult HeadlessSession::batchUpdateCommand(const Json& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    std::string error;
    std::vector<std::string> routerIds;
    routerIds.reserve(topology_.routers.size());
    for (const auto& [id, router] : topology_.routers)
    {
        static_cast<void>(router);
        routerIds.push_back(id);
    }
    if (command.contains("router_ids") &&
        (!readStringList(command, "router_ids", &routerIds, &error) || routerIds.empty()))
    {
        return failure(error.empty() ? "router_ids 不能为空" : error);
    }
    for (const auto& id : routerIds)
    {
        if (!topology_.routers.contains(id))
        {
            return failure("路由器不存在：" + id);
        }
    }

    std::string pluginId;
    Json pluginSettings = Json::object();
    const auto changesPlugin = command.contains("plugin_id");
    const auto explicitPluginSettings = command.contains("plugin_settings");
    if (changesPlugin)
    {
        if (!readRequiredString(command, "plugin_id", &pluginId, &error))
        {
            return failure(error);
        }
        const auto metadata = RouterPolicyRegistry::instance().metadata(pluginId);
        if (!metadata)
        {
            return failure("批量配置只能选择已注册插件：" + pluginId);
        }
        pluginSettings = metadata->defaultSettings;
        if (explicitPluginSettings)
        {
            const auto* settings = field(command, "plugin_settings");
            if (!settings || !settings->is_object())
            {
                return failure("plugin_settings 必须是 JSON 对象");
            }
            pluginSettings = *settings;
        }
    }

    enum class BatchMode
    {
        Unchanged,
        Fixed,
        Random
    };
    struct BatchInterval
    {
        BatchMode mode = BatchMode::Unchanged;
        int minimum = 0;
        int maximum = 0;
    };
    const auto parseInterval = [&](std::string_view key, BatchInterval* result)
    {
        const auto* value = field(command, key);
        if (!value)
        {
            return true;
        }
        if (!value->is_object())
        {
            error = "字段 " + std::string(key) + " 必须是对象";
            return false;
        }
        std::string mode;
        if (!readRequiredString(*value, "mode", &mode, &error))
        {
            return false;
        }
        mode = lower(std::move(mode));
        if (mode == "unchanged")
        {
            return true;
        }
        if (mode == "fixed")
        {
            std::int64_t fixed = 0;
            if (!readInteger(*value, "value_ms", 0, MaximumIntervalMs, &fixed, &error, true))
            {
                return false;
            }
            result->mode = BatchMode::Fixed;
            result->minimum = result->maximum = static_cast<int>(fixed);
            return true;
        }
        if (mode == "random" || mode == "random_range")
        {
            std::int64_t minimum = 0;
            std::int64_t maximum = 0;
            if (!readInteger(*value, "min_ms", 0, MaximumIntervalMs, &minimum, &error, true) ||
                !readInteger(*value, "max_ms", 0, MaximumIntervalMs, &maximum, &error, true))
            {
                return false;
            }
            if (minimum > maximum)
            {
                error = std::string(key) + ".min_ms 不能大于 max_ms";
                return false;
            }
            result->mode = BatchMode::Random;
            result->minimum = static_cast<int>(minimum);
            result->maximum = static_cast<int>(maximum);
            return true;
        }
        error = std::string(key) + ".mode 必须是 unchanged、fixed 或 random";
        return false;
    };

    BatchInterval mrai;
    BatchInterval delay;
    if (!parseInterval("mrai", &mrai) || !parseInterval("delay", &delay))
    {
        return failure(error);
    }
    std::int64_t seedValue = 0;
    const auto hasSeed = command.contains("seed");
    if (hasSeed && !readInteger(command, "seed", 0, std::numeric_limits<std::uint32_t>::max(), &seedValue, &error, true))
    {
        return failure(error);
    }
    auto seedTopology = topology_;
    seedTopology.simulation.name.clear();
    seedTopology.simulation.logDirectory.clear();
    seedTopology.simulation.workerThreads = 0;
    for (auto& [id, router] : seedTopology.routers)
    {
        static_cast<void>(id);
        router.position = {};
    }
    auto material = seedTopology.toJson().dump();
    material.push_back('\0');
    material += command.dump();
    const auto seed = hasSeed ? static_cast<std::uint32_t>(seedValue)
                              : static_cast<std::uint32_t>(stableSeed(material));
    SplitMix64 generator(seed);
    const auto randomValue = [&](const BatchInterval& interval)
    { return generator.boundedInclusive(interval.minimum, interval.maximum); };

    auto candidate = topology_;
    int changedRouters = 0;
    int changedMraiDirections = 0;
    int changedLinks = 0;
    auto actualMraiByRouter = Json::object();
    auto actualDelays = Json::array();
    if (changesPlugin)
    {
        for (const auto& id : routerIds)
        {
            auto& router = candidate.routers.at(id);
            if (router.pluginId != pluginId || (explicitPluginSettings && router.pluginSettings != pluginSettings))
            {
                router.pluginId = pluginId;
                router.pluginSettings = pluginSettings;
                ++changedRouters;
            }
        }
    }
    std::map<std::string, int, std::less<>> mraiValues;
    if (mrai.mode != BatchMode::Unchanged)
    {
        for (const auto& id : routerIds)
        {
            const auto value = mrai.mode == BatchMode::Fixed ? mrai.minimum : randomValue(mrai);
            mraiValues.emplace(id, value);
            actualMraiByRouter[id] = value;
        }
        for (auto& link : candidate.links)
        {
            if (const auto value = mraiValues.find(link.a); value != mraiValues.end() && link.mraiMsFromA != value->second)
            {
                link.mraiMsFromA = value->second;
                ++changedMraiDirections;
            }
            if (const auto value = mraiValues.find(link.b); value != mraiValues.end() && link.mraiMsFromB != value->second)
            {
                link.mraiMsFromB = value->second;
                ++changedMraiDirections;
            }
        }
    }
    if (delay.mode != BatchMode::Unchanged)
    {
        for (auto& link : candidate.links)
        {
            const auto value = delay.mode == BatchMode::Fixed ? delay.minimum : randomValue(delay);
            if (link.delayMs != value)
            {
                link.delayMs = value;
                ++changedLinks;
            }
            actualDelays.push_back(Json{{"a", link.a}, {"b", link.b}, {"delay_ms", value}});
        }
    }
    const auto problems = candidate.validate();
    if (!problems.empty())
    {
        return failure(join(problems, "\n"), Json{{"problems", problems}});
    }
    const auto changed = changedRouters > 0 || changedMraiDirections > 0 || changedLinks > 0;
    topology_ = std::move(candidate);
    dirty_ = dirty_ || changed;
    if (changed)
    {
        invalidateRuntime();
    }
    Json data{{"changed", changed},
              {"changed_routers", changedRouters},
              {"changed_mrai_directions", changedMraiDirections},
              {"changed_links", changedLinks},
              {"seed", seed},
              {"seed_source", hasSeed ? "explicit" : "derived_from_input"},
              {"random_algorithm", "bgptester-splitmix64-v1"},
              {"seed_material_encoding", "bgptester-canonical-json-v1"},
              {"actual_mrai_by_router", std::move(actualMraiByRouter)},
              {"actual_delays", std::move(actualDelays)}};
    if (changesPlugin)
    {
        data["applied_plugin"] = Json{{"id", pluginId}, {"settings", pluginSettings}};
    }
    return success(std::move(data));
}

void HeadlessSession::updateRuntimeState()
{
    runtimeLinks_.clear();
    for (const auto& link : topology_.links)
    {
        runtimeLinks_.insert_or_assign(Topology::edgeKey(link.a, link.b), link.enabled);
    }
    runtimeOriginatedPrefixes_.clear();
    for (const auto& [id, router] : topology_.routers)
    {
        auto& prefixes = runtimeOriginatedPrefixes_[id];
        prefixes.insert(router.originatedPrefixes.begin(), router.originatedPrefixes.end());
    }
}

void HeadlessSession::invalidateRuntime()
{
    runtimeAvailable_ = false;
    latestStats_ = {};
    simulationRunning_ = false;
    simulationConverged_ = false;
    runtimeLinks_.clear();
    runtimeOriginatedPrefixes_.clear();
}

HeadlessCommandResult HeadlessSession::startCommand(const Json&)
{
    if (isRunning())
    {
        std::string error;
        if (!stabilizeRuntime(&error))
        {
            return failure(error, statusJson());
        }
        return failure("仿真已经在运行", statusJson());
    }
    std::string error;
    if (!beginEventRun(&error))
    {
        return failure(error);
    }
    runtimeAvailable_ = false;
    latestStats_ = {};
    lastEngineError_.clear();
    updateRuntimeState();
    engine_->prepareStartup();

    std::atomic_bool startupFinished{false};
    std::thread cancellationWatcher;
    if (interruptionFlag_)
    {
        cancellationWatcher = std::thread([this, &startupFinished]
        {
            while (!startupFinished.load(std::memory_order_acquire))
            {
                if (interruptionFlag_->load(std::memory_order_relaxed))
                {
                    engine_->requestStartupCancellation();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    engine_->startSimulation(topology_);
    startupFinished.store(true, std::memory_order_release);
    if (cancellationWatcher.joinable())
    {
        cancellationWatcher.join();
    }
    refreshRuntimeStatus();
    if (!engine_->isRunning())
    {
        flushEventRun();
        endEventRun();
        const auto message = !lastEngineError_.empty() ? lastEngineError_ : "仿真启动失败";
        return failure(lastStoreError_.empty() ? message : message + "；日志落盘失败：" + lastStoreError_, statusJson());
    }
    runtimeAvailable_ = true;
    if (!stabilizeRuntime(&error))
    {
        flushEventRun();
        return failure(error, statusJson());
    }
    auto data = statusJson();
    data["topology_sha256"] = topologySha256(topology_);
    data["plugin_registration_errors"] = RouterPolicyRegistry::instance().registrationErrors();
    return success(std::move(data));
}

HeadlessCommandResult HeadlessSession::stopCommand(const Json&)
{
    if (!isRunning())
    {
        if (!flushEventRun())
        {
            return failure(lastStoreError_, statusJson());
        }
        auto data = statusJson();
        data["changed"] = false;
        return success(std::move(data));
    }
    std::string stabilizationError;
    if (!stabilizeRuntime(&stabilizationError))
    {
        flushEventRun();
        return failure(stabilizationError, statusJson());
    }
    engine_->stopSimulation();
    refreshRuntimeStatus();
    if (!flushEventRun())
    {
        return failure(lastStoreError_, statusJson());
    }
    auto data = statusJson();
    data["changed"] = true;
    data["graceful"] = lastEngineError_.empty();
    return lastEngineError_.empty() ? success(std::move(data)) : failure(lastEngineError_, std::move(data));
}

HeadlessCommandResult HeadlessSession::waitCommand(const Json& command)
{
    std::string error;
    std::int64_t milliseconds = 0;
    if (!readInteger(command, "milliseconds", 0, MaximumIntervalMs, &milliseconds, &error, true))
    {
        return failure(error);
    }
    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() <
           milliseconds)
    {
        if (interruptionFlag_ && interruptionFlag_->load(std::memory_order_relaxed))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    const auto stabilized = !isRunning() || stabilizeRuntime(&error);
    refreshRuntimeStatus();
    const auto data = Json{{"requested_ms", milliseconds},
                           {"elapsed_ms", elapsed},
                           {"running", isRunning()},
                           {"converged", simulationConverged_},
                           {"stats", statsToJson(latestStats_)}};
    if (interruptionFlag_ && interruptionFlag_->load(std::memory_order_relaxed))
    {
        return failure("操作被中断", data);
    }
    return stabilized ? success(data) : failure(error, data);
}

HeadlessCommandResult HeadlessSession::waitConvergedCommand(const Json& command)
{
    if (!isRunning())
    {
        return failure("仿真尚未运行");
    }
    std::string error;
    std::int64_t timeout = 30000;
    if (!readInteger(command, "timeout_ms", 1, MaximumIntervalMs, &timeout, &error))
    {
        return failure(error);
    }
    const auto started = std::chrono::steady_clock::now();
    const auto stabilized = stabilizeRuntime(&error);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    Json data{{"converged", simulationConverged_},
              {"elapsed_ms", elapsed},
              {"wall_elapsed_ms", elapsed},
              {"timeout_ms", timeout},
              {"timeout_clock", "diagnostic_wall_clock"},
              {"stats", statsToJson(latestStats_)}};
    if (interruptionFlag_ && interruptionFlag_->load(std::memory_order_relaxed))
    {
        return failure("操作被中断", data);
    }
    if (!stabilized)
    {
        return failure(error, data);
    }
    if (!flushEventRun())
    {
        return failure(lastStoreError_, data);
    }
    data["event_run_serial"] = eventRunSerial_;
    data["committed_event_id"] = committedEventId_;
    data["bmp_sqlite"] = databasePath_;
    return success(std::move(data));
}

HeadlessCommandResult HeadlessSession::setRouterStateCommand(const Json& command)
{
    if (!isRunning())
    {
        return failure("仿真尚未运行");
    }
    std::string router;
    std::string error;
    if (!readRequiredString(command, "router", &router, &error) && !readRequiredString(command, "id", &router, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure("路由器不存在：" + router);
    }
    const auto* enabledValue = field(command, "enabled");
    if (!enabledValue || !enabledValue->is_boolean())
    {
        return failure("字段 enabled 必须是布尔值");
    }
    const auto enabled = enabledValue->get<bool>();
    if (!stabilizeRuntime(&error))
    {
        return failure(error);
    }
    const auto snapshots = engine_->routerSnapshots();
    const auto current = std::find_if(snapshots.begin(), snapshots.end(),
                                      [&](const RouterSnapshot& value) { return value.id == router; });
    const auto changed = current != snapshots.end() && current->active != enabled;
    engine_->setRouterState(router, enabled);
    refreshRuntimeStatus();
    if (!lastEngineError_.empty())
    {
        return failure(lastEngineError_);
    }
    if (!stabilizeRuntime(&error))
    {
        return failure(error);
    }
    return success(Json{{"changed", changed}, {"router", router}, {"enabled", enabled}});
}

HeadlessCommandResult HeadlessSession::toggleRouterCommand(const Json& command)
{
    if (!isRunning())
    {
        return failure("仿真尚未运行");
    }
    std::string router;
    std::string error;
    if (!readRequiredString(command, "router", &router, &error) && !readRequiredString(command, "id", &router, &error))
    {
        return failure(error);
    }
    if (!stabilizeRuntime(&error))
    {
        return failure(error);
    }
    const auto snapshots = engine_->routerSnapshots();
    const auto current = std::find_if(snapshots.begin(), snapshots.end(),
                                      [&](const RouterSnapshot& value) { return value.id == router; });
    if (current == snapshots.end())
    {
        return failure("路由器不存在：" + router);
    }
    auto forwarded = command;
    forwarded["router"] = router;
    forwarded["enabled"] = !current->active;
    return setRouterStateCommand(forwarded);
}

HeadlessCommandResult HeadlessSession::setLinkStateCommand(const Json& command)
{
    if (!isRunning())
    {
        return failure("仿真尚未运行");
    }
    std::string a;
    std::string b;
    std::string error;
    if (!readRequiredString(command, "a", &a, &error) || !readRequiredString(command, "b", &b, &error))
    {
        return failure(error);
    }
    const auto* link = topology_.findLink(a, b);
    if (!link)
    {
        return failure("链路不存在：" + a + " - " + b);
    }
    const auto* enabledValue = field(command, "enabled");
    if (!enabledValue || !enabledValue->is_boolean())
    {
        return failure("字段 enabled 必须是布尔值");
    }
    const auto enabled = enabledValue->get<bool>();
    if (!stabilizeRuntime(&error))
    {
        return failure(error);
    }
    const auto key = Topology::edgeKey(a, b);
    const auto current = runtimeLinks_.contains(key) ? runtimeLinks_.at(key) : link->enabled;
    const auto changed = current != enabled;
    engine_->setLinkState(a, b, enabled);
    refreshRuntimeStatus();
    if (!lastEngineError_.empty())
    {
        return failure(lastEngineError_);
    }
    runtimeLinks_.insert_or_assign(key, enabled);
    if (!stabilizeRuntime(&error))
    {
        return failure(error);
    }
    return success(Json{{"changed", changed}, {"a", a}, {"b", b}, {"enabled", enabled}});
}

HeadlessCommandResult HeadlessSession::toggleLinkCommand(const Json& command)
{
    std::string a;
    std::string b;
    std::string error;
    if (!readRequiredString(command, "a", &a, &error) || !readRequiredString(command, "b", &b, &error))
    {
        return failure(error);
    }
    const auto* link = topology_.findLink(a, b);
    if (!link)
    {
        return failure("链路不存在：" + a + " - " + b);
    }
    const auto key = Topology::edgeKey(a, b);
    const auto enabled = runtimeLinks_.contains(key) ? runtimeLinks_.at(key) : link->enabled;
    auto forwarded = command;
    forwarded["enabled"] = !enabled;
    return setLinkStateCommand(forwarded);
}

HeadlessCommandResult HeadlessSession::advertisePrefixCommand(const Json& command)
{
    if (!isRunning())
    {
        return failure("仿真尚未运行");
    }
    std::string router;
    std::string prefix;
    std::string error;
    if (!readRequiredString(command, "router", &router, &error) ||
        !readRequiredString(command, "prefix", &prefix, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure("路由器不存在：" + router);
    }
    if (!stabilizeRuntime(&error))
    {
        return failure(error);
    }
    const auto configChanged = !runtimeOriginatedPrefixes_[router].contains(prefix);
    const auto routers = engine_->routerSnapshots();
    const auto current = std::find_if(routers.begin(), routers.end(),
                                      [&](const RouterSnapshot& value) { return value.id == router; });
    const auto routerActive = current != routers.end() && current->active;
    const auto before = engine_->ribSnapshot(router);
    const auto routePresent = before.localRoutes.contains(prefix);
    engine_->originatePrefix(router, prefix);
    refreshRuntimeStatus();
    if (!lastEngineError_.empty())
    {
        return failure(lastEngineError_);
    }
    runtimeOriginatedPrefixes_[router].insert(prefix);
    if (!stabilizeRuntime(&error))
    {
        return failure(error);
    }
    const auto routeEffective = routerActive && !routePresent;
    return success(Json{{"changed", configChanged || routeEffective},
                        {"config_changed", configChanged},
                        {"route_effective", routeEffective},
                        {"router", router},
                        {"prefix", prefix}});
}

HeadlessCommandResult HeadlessSession::withdrawPrefixCommand(const Json& command)
{
    if (!isRunning())
    {
        return failure("仿真尚未运行");
    }
    std::string router;
    std::string prefix;
    std::string error;
    if (!readRequiredString(command, "router", &router, &error) ||
        !readRequiredString(command, "prefix", &prefix, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure("路由器不存在：" + router);
    }
    if (!stabilizeRuntime(&error))
    {
        return failure(error);
    }
    const auto configChanged = runtimeOriginatedPrefixes_[router].contains(prefix);
    const auto routePresent = engine_->ribSnapshot(router).localRoutes.contains(prefix);
    engine_->withdrawPrefix(router, prefix);
    refreshRuntimeStatus();
    if (!lastEngineError_.empty())
    {
        return failure(lastEngineError_);
    }
    runtimeOriginatedPrefixes_[router].erase(prefix);
    if (!stabilizeRuntime(&error))
    {
        return failure(error);
    }
    return success(Json{{"changed", configChanged || routePresent},
                        {"config_changed", configChanged},
                        {"route_effective", routePresent},
                        {"router", router},
                        {"prefix", prefix}});
}

Json HeadlessSession::statsToJson(const SimulationStats& stats)
{
    return Json{{"running", stats.running},
                {"converged", stats.converged},
                {"pending_events", stats.pendingEvents},
                {"delivered_messages", stats.deliveredMessages},
                {"elapsed_ms", stats.elapsedMs},
                {"convergence_elapsed_ms", stats.convergenceElapsedMs},
                {"convergence_trigger_event", stats.convergenceTriggerEvent},
                {"convergence_trigger_context", stats.convergenceTriggerContext}};
}

Json HeadlessSession::routeToJson(const RouteEntry& route)
{
    Json attributes{{"origin", route.attributes.origin},
                    {"as_path", route.attributes.asPath},
                    {"next_hop", route.attributes.nextHop},
                    {"local_pref", route.attributes.localPref},
                    {"med", route.attributes.med},
                    {"originator_id", route.attributes.originatorId},
                    {"cluster_list", route.attributes.clusterList},
                    {"communities", route.attributes.communities}};
    if (route.attributes.tfpVersionInfo)
    {
        attributes["tfp_version_info"] =
            Json{{"dependency_vector", tfpVectorToJson(route.attributes.tfpVersionInfo->dependencyVector)},
                 {"trigger_vector", tfpVectorToJson(route.attributes.tfpVersionInfo->triggerVector)}};
    }
    return Json{{"learned_from", route.learnedFrom},
                {"source_session", toString(route.sourceSession)},
                {"local_origin", route.localOrigin},
                {"source", routeSourceName(route.source)},
                {"attributes", std::move(attributes)}};
}

Json HeadlessSession::routerSnapshotToJson(const RouterSnapshot& snapshot)
{
    return Json{{"id", snapshot.id},
                {"router_id", snapshot.routerId},
                {"asn", snapshot.asn},
                {"active", snapshot.active},
                {"route_reflector", snapshot.routeReflector},
                {"best_route_count", snapshot.bestRouteCount}};
}

Json HeadlessSession::peerSnapshotToJson(const PeerSnapshot& snapshot)
{
    return Json{{"id", snapshot.id},
                {"remote_asn", snapshot.remoteAsn},
                {"session_type", toString(snapshot.sessionType)},
                {"relationship", toString(snapshot.relationship)},
                {"state", toString(snapshot.state)},
                {"enabled", snapshot.enabled},
                {"rr_client", snapshot.rrClient},
                {"mrai_ms", snapshot.mraiMs}};
}

Json HeadlessSession::ribSnapshotToJson(const RibSnapshot& snapshot, const std::string& prefixFilter)
{
    const auto routeMap = [&](const RouteTable& routes)
    {
        std::vector<std::string> prefixes;
        prefixes.reserve(routes.size());
        for (const auto& [prefix, route] : routes)
        {
            static_cast<void>(route);
            if (prefixFilter.empty() || prefix == prefixFilter)
            {
                prefixes.push_back(prefix);
            }
        }
        std::sort(prefixes.begin(), prefixes.end());
        auto result = Json::object();
        for (const auto& prefix : prefixes)
        {
            result[prefix] = routeToJson(routes.at(prefix));
        }
        return result;
    };

    std::vector<std::string> peerIds;
    peerIds.reserve(snapshot.adjRibIn.size());
    for (const auto& [peer, routes] : snapshot.adjRibIn)
    {
        static_cast<void>(routes);
        peerIds.push_back(peer);
    }
    std::sort(peerIds.begin(), peerIds.end());
    auto adjRibIn = Json::object();
    for (const auto& peer : peerIds)
    {
        auto routes = routeMap(snapshot.adjRibIn.at(peer));
        if (!routes.empty() || prefixFilter.empty())
        {
            adjRibIn[peer] = std::move(routes);
        }
    }
    return Json{{"router", snapshot.router},
                {"local_routes", routeMap(snapshot.localRoutes)},
                {"loc_rib", routeMap(snapshot.locRib)},
                {"adj_rib_in", std::move(adjRibIn)}};
}

HeadlessCommandResult HeadlessSession::routersCommand(const Json&)
{
    std::string error;
    if (isRunning() && !stabilizeRuntime(&error))
    {
        return failure(error);
    }
    auto routers = Json::array();
    if (runtimeAvailable_)
    {
        auto snapshots = engine_->routerSnapshots();
        std::sort(snapshots.begin(), snapshots.end(),
                  [](const RouterSnapshot& lhs, const RouterSnapshot& rhs) { return lhs.id < rhs.id; });
        for (const auto& snapshot : snapshots)
        {
            routers.push_back(routerSnapshotToJson(snapshot));
        }
    }
    else
    {
        routers = topology_.toJson().at("routers");
    }
    return success(Json{{"runtime", runtimeAvailable_}, {"routers", std::move(routers)}});
}

HeadlessCommandResult HeadlessSession::ribCommand(const Json& command)
{
    if (!runtimeAvailable_)
    {
        return requireRuntime();
    }
    std::string router;
    std::string prefix;
    std::string error;
    if (!readRequiredString(command, "router", &router, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure("路由器不存在：" + router);
    }
    if (!readOptionalString(command, "prefix", &prefix, &error))
    {
        return failure(error);
    }
    if (isRunning() && !stabilizeRuntime(&error))
    {
        return failure(error);
    }
    return success(Json{{"rib", ribSnapshotToJson(engine_->ribSnapshot(router), prefix)}});
}

HeadlessCommandResult HeadlessSession::peersCommand(const Json& command)
{
    if (!runtimeAvailable_)
    {
        return requireRuntime();
    }
    std::string router;
    std::string error;
    if (!readRequiredString(command, "router", &router, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure("路由器不存在：" + router);
    }
    if (isRunning() && !stabilizeRuntime(&error))
    {
        return failure(error);
    }
    auto snapshots = engine_->peerSnapshots(router);
    std::sort(snapshots.begin(), snapshots.end(),
              [](const PeerSnapshot& lhs, const PeerSnapshot& rhs) { return lhs.id < rhs.id; });
    auto peers = Json::array();
    for (const auto& snapshot : snapshots)
    {
        peers.push_back(peerSnapshotToJson(snapshot));
    }
    return success(Json{{"router", router}, {"peers", std::move(peers)}});
}

HeadlessCommandResult HeadlessSession::pathCommand(const Json& command)
{
    if (!runtimeAvailable_)
    {
        return requireRuntime();
    }
    std::string router;
    std::string prefix;
    std::string error;
    if (!readRequiredString(command, "router", &router, &error) ||
        !readRequiredString(command, "prefix", &prefix, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure("路由器不存在：" + router);
    }
    if (isRunning() && !stabilizeRuntime(&error))
    {
        return failure(error);
    }
    const auto found = engine_->ribSnapshot(router).locRib.contains(prefix);
    const auto path = found ? engine_->pathSnapshot(router, prefix) : std::vector<std::string>{};
    return success(Json{{"router", router}, {"prefix", prefix}, {"found", found}, {"path", path}});
}

HeadlessCommandResult HeadlessSession::snapshotCommand(const Json& command)
{
    if (!runtimeAvailable_)
    {
        return requireRuntime();
    }
    std::string error;
    if (isRunning() && !stabilizeRuntime(&error))
    {
        return failure(error);
    }
    if (eventRunOpen_ && !flushEventRun())
    {
        return failure(lastStoreError_, statusJson());
    }
    refreshRuntimeStatus();
    auto summaries = engine_->routerSnapshots();
    std::sort(summaries.begin(), summaries.end(),
              [](const RouterSnapshot& lhs, const RouterSnapshot& rhs) { return lhs.id < rhs.id; });
    auto routers = Json::array();
    for (const auto& summary : summaries)
    {
        const auto rib = engine_->ribSnapshot(summary.id);
        auto peerSnapshots = engine_->peerSnapshots(summary.id);
        std::sort(peerSnapshots.begin(), peerSnapshots.end(),
                  [](const PeerSnapshot& lhs, const PeerSnapshot& rhs) { return lhs.id < rhs.id; });
        auto peers = Json::array();
        for (const auto& peer : peerSnapshots)
        {
            peers.push_back(peerSnapshotToJson(peer));
        }
        std::vector<std::string> prefixes;
        prefixes.reserve(rib.locRib.size());
        for (const auto& [prefix, route] : rib.locRib)
        {
            static_cast<void>(route);
            prefixes.push_back(prefix);
        }
        std::sort(prefixes.begin(), prefixes.end());
        auto paths = Json::object();
        for (const auto& prefix : prefixes)
        {
            paths[prefix] = engine_->pathSnapshot(summary.id, prefix);
        }
        const auto originated = runtimeOriginatedPrefixes_.contains(summary.id)
                                    ? std::vector<std::string>(runtimeOriginatedPrefixes_.at(summary.id).begin(),
                                                               runtimeOriginatedPrefixes_.at(summary.id).end())
                                    : std::vector<std::string>{};
        routers.push_back(Json{{"summary", routerSnapshotToJson(summary)},
                               {"rib", ribSnapshotToJson(rib, {})},
                               {"peers", std::move(peers)},
                               {"paths", std::move(paths)},
                               {"runtime_originated_prefixes", originated}});
    }

    auto stableTopology = topology_;
    std::sort(stableTopology.links.begin(), stableTopology.links.end(), [](const LinkConfig& lhs, const LinkConfig& rhs)
              { return Topology::edgeKey(lhs.a, lhs.b) < Topology::edgeKey(rhs.a, rhs.b); });
    auto links = Json::array();
    for (const auto& link : stableTopology.links)
    {
        auto object = linkToJson(link);
        const auto key = Topology::edgeKey(link.a, link.b);
        object["runtime_enabled"] = runtimeLinks_.contains(key) ? runtimeLinks_.at(key) : link.enabled;
        links.push_back(std::move(object));
    }
    Json snapshot{{"schema", "bgptester-runtime-snapshot-v1"},
                  {"captured_at", isoTimestamp(SimulationEpochMilliseconds + latestStats_.elapsedMs)},
                  {"captured_simulation_ms", latestStats_.elapsedMs},
                  {"topology_path", topologyPath_},
                  {"topology", stableTopology.toJson()},
                  {"topology_sha256", topologySha256(stableTopology)},
                  {"stats", statsToJson(latestStats_)},
                  {"routers", std::move(routers)},
                  {"links", std::move(links)},
                  {"bmp_jsonl", logFilePath_},
                  {"bmp_sqlite", databasePath_},
                  {"event_run_serial", eventRunSerial_},
                  {"committed_event_id", committedEventId_}};

    std::string path;
    if (!readOptionalString(command, "path", &path, &error))
    {
        return failure(error);
    }
    if (!path.empty())
    {
        path = absolutePath(path);
        if (!writeJsonFile(path, snapshot, &error))
        {
            return failure(error);
        }
    }
    return success(Json{{"path", path}, {"snapshot", std::move(snapshot)}});
}

HeadlessCommandResult HeadlessSession::queryEventsCommand(const Json& command)
{
    auto database = databasePath_;
    std::string filter;
    std::string error;
    if (!readOptionalString(command, "database", &database, &error) ||
        !readOptionalString(command, "filter", &filter, &error, false))
    {
        return failure(error);
    }
    if (database.empty())
    {
        return failure("尚无当前 BMP SQLite；请先执行 start 或提供 database");
    }
    database = absolutePath(database);
    std::error_code fileError;
    if (!std::filesystem::exists(database, fileError))
    {
        return failure("SQLite 历史不存在：" + database);
    }
    std::int64_t limit = 20000;
    if (!readInteger(command, "limit", 1, 1000000, &limit, &error))
    {
        return failure(error);
    }
    if (eventRunOpen_ && isRunning() && !stabilizeRuntime(&error))
    {
        return failure(error);
    }
    if (eventRunOpen_ && !flushEventRun())
    {
        return failure(lastStoreError_, statusJson());
    }
    const auto cancelled = [this]
    { return interruptionFlag_ && interruptionFlag_->load(std::memory_order_relaxed); };
    const auto page = EventStore::queryDatabase(database, static_cast<int>(limit), filter, &error, {}, cancelled);
    if (!error.empty())
    {
        return failure(error);
    }
    auto events = Json::array();
    for (const auto& event : page.events)
    {
        events.push_back(EventStore::eventToJson(event));
    }
    Json data{{"database", database},
              {"filter", filter},
              {"limit", limit},
              {"total_count", page.totalCount},
              {"filtered_count", page.filteredCount},
              {"message_total_count", page.messageTotalCount},
              {"filtered_message_count", page.filteredMessageCount},
              {"database_max_event_id", page.maxEventId},
              {"events", std::move(events)}};
    if (database == databasePath_)
    {
        data["event_run_serial"] = eventRunSerial_;
        data["committed_event_id"] = committedEventId_;
    }
    return success(std::move(data));
}

HeadlessCommandResult HeadlessSession::queryConvergenceCommand(const Json& command)
{
    auto database = databasePath_;
    std::string error;
    if (!readOptionalString(command, "database", &database, &error))
    {
        return failure(error);
    }
    if (database.empty())
    {
        return failure("尚无当前 BMP SQLite；请先执行 start 或提供 database");
    }
    database = absolutePath(database);
    std::error_code fileError;
    if (!std::filesystem::exists(database, fileError))
    {
        return failure("SQLite 历史不存在：" + database);
    }
    std::int64_t limit = 5000;
    if (!readInteger(command, "limit", 1, 1000000, &limit, &error))
    {
        return failure(error);
    }
    if (eventRunOpen_ && isRunning() && !stabilizeRuntime(&error))
    {
        return failure(error);
    }
    if (eventRunOpen_ && !flushEventRun())
    {
        return failure(lastStoreError_, statusJson());
    }
    const auto cancelled = [this]
    { return interruptionFlag_ && interruptionFlag_->load(std::memory_order_relaxed); };
    const auto page = EventStore::queryConvergenceDatabase(database, static_cast<int>(limit), &error, cancelled);
    if (!error.empty())
    {
        return failure(error);
    }
    auto events = Json::array();
    for (const auto& event : page.events)
    {
        events.push_back(EventStore::eventToJson(event));
    }
    Json data{{"database", database},
              {"limit", limit},
              {"total_count", page.totalCount},
              {"database_max_event_id", page.maxEventId},
              {"events", std::move(events)}};
    if (database == databasePath_)
    {
        data["event_run_serial"] = eventRunSerial_;
        data["committed_event_id"] = committedEventId_;
    }
    return success(std::move(data));
}

HeadlessCommandResult HeadlessSession::flushLogsCommand(const Json&)
{
    if (databasePath_.empty())
    {
        return failure("尚未创建 BMP 日志");
    }
    std::string error;
    if (isRunning() && !stabilizeRuntime(&error))
    {
        flushEventRun();
        return failure(error, statusJson());
    }
    if (!flushEventRun())
    {
        return failure(lastStoreError_, statusJson());
    }
    return success(Json{{"bmp_jsonl", logFilePath_},
                        {"bmp_sqlite", databasePath_},
                        {"run_directory", runDirectory_},
                        {"event_run_serial", eventRunSerial_},
                        {"committed_event_id", committedEventId_}});
}

HeadlessCommandResult HeadlessSession::exitCommand(const Json&)
{
    std::string error;
    HeadlessCommandResult result;
    if (isRunning() && !stabilizeRuntime(&error))
    {
        flushEventRun();
        result = failure(error, statusJson());
    }
    else if (eventRunOpen_ && !flushEventRun())
    {
        result = failure(lastStoreError_, statusJson());
    }
    else
    {
        refreshEventStoreStatus();
        result = success(statusJson());
    }
    result.exitRequested = true;
    return result;
}

} // namespace bgptester
