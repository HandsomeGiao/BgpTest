#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bgptester
{

using Json = nlohmann::json;

inline const std::string StandardRouterPluginId = "org.bgptester.router.standard-bgp";
inline constexpr std::int64_t SimulationEpochMilliseconds = 946684800000LL; // 2000-01-01T00:00:00.000Z

enum class SessionType
{
    Ibgp,
    Ebgp
};

enum class LinkBusinessRelationship
{
    Unspecified,
    PeerToPeer,
    AProviderOfB,
    BProviderOfA
};

// The relationship of the remote neighbor to the local router.
enum class NeighborRelationship
{
    Unspecified,
    Peer,
    Provider,
    Customer
};

enum class MessageType
{
    Open,
    Update,
    Notification
};

enum class PeerState
{
    Idle,
    OpenSent,
    Established
};

enum class RouteSource
{
    Unspecified,
    Local,
    Customer,
    Peer,
    Provider
};

std::string toString(SessionType type);
std::optional<SessionType> sessionTypeFromString(std::string_view value);
std::string toString(LinkBusinessRelationship relationship);
std::optional<LinkBusinessRelationship> linkBusinessRelationshipFromString(std::string_view value);
std::string toString(NeighborRelationship relationship);
std::optional<NeighborRelationship> neighborRelationshipFromString(std::string_view value);

inline std::string toString(MessageType type)
{
    switch (type)
    {
        case MessageType::Open:
            return "OPEN";
        case MessageType::Update:
            return "UPDATE";
        case MessageType::Notification:
            return "NOTIFICATION";
    }
    return "UNKNOWN";
}

inline std::string toString(PeerState state)
{
    switch (state)
    {
        case PeerState::Idle:
            return "Idle";
        case PeerState::OpenSent:
            return "OpenSent";
        case PeerState::Established:
            return "Established";
    }
    return "Unknown";
}

struct Position
{
    double x = 100.0;
    double y = 100.0;

    bool operator==(const Position&) const = default;
};

struct SimulationSettings
{
    std::string name = "bgp-lab";
    std::string logDirectory = "tmp";
    int workerThreads = 0;
    int convergenceQuietMs = 1000;
    bool withdrawalIgnoresMrai = true;

    bool operator==(const SimulationSettings&) const = default;
};

struct RouterConfig
{
    std::string id;
    std::string routerId;
    std::uint32_t asn = 65000;
    std::string clusterId;
    std::vector<std::string> originatedPrefixes;
    Position position{};
    std::string pluginId = StandardRouterPluginId;
    Json pluginSettings = Json::object();

    bool operator==(const RouterConfig&) const = default;
};

struct LinkConfig
{
    std::string a;
    std::string b;
    bool enabled = true;
    int delayMs = 0;
    bool rrClientFromA = false;
    bool rrClientFromB = false;
    int mraiMsFromA = 0;
    int mraiMsFromB = 0;
    LinkBusinessRelationship businessRelationship = LinkBusinessRelationship::Unspecified;

    bool operator==(const LinkConfig&) const = default;
};

struct NeighborConfig
{
    std::string id;
    std::uint32_t remoteAsn = 0;
    SessionType sessionType = SessionType::Ibgp;
    bool rrClient = false;
    bool enabled = true;
    int mraiMs = 0;
    NeighborRelationship relationship = NeighborRelationship::Unspecified;

    bool operator==(const NeighborConfig&) const = default;
};

using RouterMap = std::map<std::string, RouterConfig>;
using NeighborMap = std::map<std::string, NeighborConfig>;
using NeighborIndex = std::unordered_map<std::string, NeighborMap>;

// A TFP entity is scoped to one logical BGP router, rather than an entire AS.
struct TfpEntity
{
    std::uint32_t asn = 0;
    std::string entityId;

    bool operator==(const TfpEntity&) const = default;
    bool operator<(const TfpEntity& other) const
    {
        return asn != other.asn ? asn < other.asn : entityId < other.entityId;
    }
};

struct TfpEntityHash
{
    std::size_t operator()(const TfpEntity& entity) const noexcept
    {
        const auto first = std::hash<std::uint32_t>{}(entity.asn);
        const auto second = std::hash<std::string>{}(entity.entityId);
        return first ^ (second + static_cast<std::size_t>(0x9e3779b9U) + (first << 6U) + (first >> 2U));
    }
};

using TfpVersionVector = std::map<TfpEntity, std::uint64_t>;

struct TfpVersionInfo
{
    TfpVersionVector dependencyVector;
    TfpVersionVector triggerVector;

    bool operator==(const TfpVersionInfo&) const = default;
};

struct PathAttributes
{
    std::string origin = "igp";
    std::vector<std::uint32_t> asPath;
    std::string nextHop;
    std::uint32_t localPref = 100;
    std::uint32_t med = 0;
    std::string originatorId;
    std::vector<std::string> clusterList;
    std::map<std::string, std::string> communities;
    std::optional<TfpVersionInfo> tfpVersionInfo;

    bool operator==(const PathAttributes&) const = default;
};

struct RouteEntry
{
    PathAttributes attributes;
    std::string learnedFrom;
    SessionType sourceSession = SessionType::Ibgp;
    bool localOrigin = false;
    RouteSource source = RouteSource::Unspecified;

    bool operator==(const RouteEntry&) const = default;
};

struct BgpMessage
{
    MessageType type = MessageType::Open;
    std::string from;
    std::string to;
    std::uint64_t sequence = 0;

    std::uint32_t openAsn = 0;
    std::string openRouterId;
    std::vector<std::string> nlri;
    std::vector<std::string> withdrawn;
    std::optional<RouteEntry> advertisedRoute;
    PathAttributes withdrawalAttributes;
    std::map<std::string, TfpVersionInfo> tfpVersionInfoByPrefix;
    int errorCode = 0;
    int errorSubcode = 0;
    std::string errorData;

    std::map<std::string, std::uint64_t> generations;
    bool guarded = false;
};

struct RouterSnapshot
{
    std::string id;
    std::string routerId;
    std::uint32_t asn = 0;
    bool active = false;
    bool routeReflector = false;
    int bestRouteCount = 0;
};

struct PeerSnapshot
{
    std::string id;
    std::uint32_t remoteAsn = 0;
    SessionType sessionType = SessionType::Ibgp;
    bool rrClient = false;
    bool enabled = true;
    int mraiMs = 0;
    PeerState state = PeerState::Idle;
    NeighborRelationship relationship = NeighborRelationship::Unspecified;
};

using RouteTable = std::unordered_map<std::string, RouteEntry>;

struct RibSnapshot
{
    std::string router;
    RouteTable localRoutes;
    RouteTable locRib;
    std::unordered_map<std::string, RouteTable> adjRibIn;
};

struct SimulationEvent
{
    std::uint64_t id = 0;
    // UTC milliseconds since the Unix epoch. The deterministic simulation
    // starts at SimulationEpochMilliseconds and never reads wall time.
    std::int64_t timestamp = SimulationEpochMilliseconds;
    std::string event;
    std::string router;
    std::string from;
    std::string to;
    std::optional<std::uint32_t> fromAs;
    std::optional<std::uint32_t> toAs;
    std::string messageType;
    std::string action;
    std::uint64_t sequence = 0;
    std::vector<std::string> prefixes;
    std::vector<std::string> withdrawn;
    std::string nextHop;
    std::vector<std::uint32_t> asPath;
    std::optional<std::uint32_t> localPref;
    std::optional<std::uint32_t> med;
    std::map<std::string, std::string> details;
};

struct SimulationStats
{
    bool running = false;
    bool converged = false;
    std::size_t pendingEvents = 0;
    std::uint64_t deliveredMessages = 0;
    std::int64_t elapsedMs = 0;
    std::int64_t convergenceElapsedMs = 0;
    std::string convergenceTriggerEvent;
    std::string convergenceTriggerContext;
};

} // namespace bgptester

namespace std
{
template <>
struct hash<bgptester::TfpEntity>
{
    size_t operator()(const bgptester::TfpEntity& entity) const noexcept
    {
        return bgptester::TfpEntityHash{}(entity);
    }
};
} // namespace std
