#include "core/Topology.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bgptester
{
namespace
{

bool asciiWhitespace(char value)
{
    switch (value)
    {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '\f':
        case '\v':
            return true;
        default:
            return false;
    }
}

std::string trimCopy(std::string_view value)
{
    while (!value.empty() && asciiWhitespace(value.front()))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && asciiWhitespace(value.back()))
    {
        value.remove_suffix(1);
    }
    return std::string(value);
}

char asciiLower(char value)
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool asciiCaseEqual(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index)
    {
        if (asciiLower(lhs[index]) != asciiLower(rhs[index]))
        {
            return false;
        }
    }
    return true;
}

std::string pathText(const std::filesystem::path& path)
{
    const auto utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::string joinStrings(const std::vector<std::string>& values, std::string_view separator)
{
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
        {
            result.append(separator);
        }
        result.append(values[index]);
    }
    return result;
}

void setError(std::string* error, std::string message)
{
    if (error)
    {
        *error = std::move(message);
    }
}

const Json* member(const Json& object, std::string_view key)
{
    if (!object.is_object())
    {
        return nullptr;
    }
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

std::string jsonString(const Json& object, std::string_view key, std::string fallback = {})
{
    const auto* value = member(object, key);
    return value && value->is_string() ? value->get<std::string>() : std::move(fallback);
}

bool jsonBool(const Json& object, std::string_view key, bool fallback)
{
    const auto* value = member(object, key);
    return value && value->is_boolean() ? value->get<bool>() : fallback;
}

double jsonDouble(const Json& object, std::string_view key, double fallback)
{
    const auto* value = member(object, key);
    if (!value || !value->is_number())
    {
        return fallback;
    }
    try
    {
        const auto number = value->get<double>();
        return std::isfinite(number) ? number : fallback;
    }
    catch (const Json::exception&)
    {
        return fallback;
    }
}

std::uint32_t jsonUint(const Json& object, std::string_view key, std::uint32_t fallback)
{
    const auto* value = member(object, key);
    if (!value || !value->is_number())
    {
        return fallback;
    }
    try
    {
        const auto number = value->get<double>();
        if (!std::isfinite(number) || number < 0.0 || number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
        {
            return fallback;
        }
        return static_cast<std::uint32_t>(number);
    }
    catch (const Json::exception&)
    {
        return fallback;
    }
}

int jsonNonNegativeInt(const Json& object, std::string_view key, int fallback)
{
    const auto* value = member(object, key);
    if (!value || !value->is_number())
    {
        return fallback;
    }
    try
    {
        const auto number = value->get<double>();
        if (!std::isfinite(number) || std::floor(number) != number || number < 0.0 ||
            number > static_cast<double>(std::numeric_limits<int>::max()))
        {
            return fallback;
        }
        return static_cast<int>(number);
    }
    catch (const Json::exception&)
    {
        return fallback;
    }
}

std::vector<std::string> readStringArray(const Json* value)
{
    std::vector<std::string> result;
    if (!value || !value->is_array())
    {
        return result;
    }
    std::unordered_set<std::string> seen;
    result.reserve(value->size());
    for (const auto& entry : *value)
    {
        if (!entry.is_string())
        {
            continue;
        }
        auto text = trimCopy(entry.get_ref<const std::string&>());
        if (!text.empty() && seen.insert(text).second)
        {
            result.push_back(std::move(text));
        }
    }
    return result;
}

bool parseDecimal(std::string_view text, int maximum, int* parsed = nullptr)
{
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
    {
        return false;
    }
    int value = 0;
    for (const auto character : text)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        const auto digit = static_cast<int>(character - '0');
        if (value > (maximum - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    if (parsed)
    {
        *parsed = value;
    }
    return true;
}

bool canonicalIpv4Address(std::string_view text, bool allowZero)
{
    std::uint32_t address = 0;
    std::size_t componentStart = 0;
    int componentCount = 0;
    for (std::size_t index = 0; index <= text.size(); ++index)
    {
        if (index != text.size() && text[index] != '.')
        {
            continue;
        }
        if (componentCount >= 4)
        {
            return false;
        }
        int component = 0;
        if (!parseDecimal(text.substr(componentStart, index - componentStart), 255, &component))
        {
            return false;
        }
        address = (address << 8U) | static_cast<std::uint32_t>(component);
        ++componentCount;
        componentStart = index + 1;
    }
    return componentCount == 4 && (allowZero || address != 0);
}

bool canonicalIpv4Prefix(std::string_view text)
{
    const auto slash = text.rfind('/');
    if (slash == std::string_view::npos || slash == 0 || slash == text.size() - 1 || text.substr(0, slash).find('/') != std::string_view::npos)
    {
        return false;
    }
    return parseDecimal(text.substr(slash + 1), 32) && canonicalIpv4Address(text.substr(0, slash), true);
}

NeighborRelationship neighborRelationshipFor(const LinkConfig& link, bool fromA)
{
    switch (link.businessRelationship)
    {
        case LinkBusinessRelationship::Unspecified:
            return NeighborRelationship::Unspecified;
        case LinkBusinessRelationship::PeerToPeer:
            return NeighborRelationship::Peer;
        case LinkBusinessRelationship::AProviderOfB:
            return fromA ? NeighborRelationship::Customer : NeighborRelationship::Provider;
        case LinkBusinessRelationship::BProviderOfA:
            return fromA ? NeighborRelationship::Provider : NeighborRelationship::Customer;
    }
    return NeighborRelationship::Unspecified;
}

LinkBusinessRelationship linkRelationshipForNeighbor(NeighborRelationship relationship, bool localIsA)
{
    switch (relationship)
    {
        case NeighborRelationship::Unspecified:
            return LinkBusinessRelationship::Unspecified;
        case NeighborRelationship::Peer:
            return LinkBusinessRelationship::PeerToPeer;
        case NeighborRelationship::Provider:
            return localIsA ? LinkBusinessRelationship::BProviderOfA : LinkBusinessRelationship::AProviderOfB;
        case NeighborRelationship::Customer:
            return localIsA ? LinkBusinessRelationship::AProviderOfB : LinkBusinessRelationship::BProviderOfA;
    }
    return LinkBusinessRelationship::Unspecified;
}

struct ExplicitLinkFields
{
    bool enabled = false;
    bool rrClientFromA = false;
    bool rrClientFromB = false;
    bool mraiMsFromA = false;
    bool mraiMsFromB = false;
    bool relationship = false;
};

struct LegacyNeighborEntry
{
    std::string routerId;
    Json object = Json::object();
};

class TopologyJsonBuilder
{
public:
    void reserve(std::size_t linkCount)
    {
        topology_.links.reserve(linkCount);
        explicitLinkFieldsByIndex_.reserve(linkCount);
    }

    void setSimulation(const Json& object)
    {
        topology_.simulation.name = jsonString(object, "name", "bgp-lab");
        topology_.simulation.logDirectory = jsonString(object, "log_dir", "tmp");
        topology_.simulation.workerThreads = jsonNonNegativeInt(object, "worker_threads", 0);
        topology_.simulation.convergenceQuietMs = jsonNonNegativeInt(object, "convergence_quiet_ms", 1000);
        topology_.simulation.withdrawalIgnoresMrai = jsonBool(object, "withdrawal_ignores_mrai", true);
    }

    bool addRouter(const Json& entry, std::string* error)
    {
        const auto index = topology_.routers.size();
        if (index >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            setError(error, "路由器数量超过支持上限");
            return false;
        }
        const auto oneBasedIndex = static_cast<int>(index) + 1;
        RouterConfig router;
        router.id = trimCopy(jsonString(entry, "id", "R" + std::to_string(oneBasedIndex)));
        if (topology_.routers.contains(router.id))
        {
            setError(error, "路由器 ID 重复：" + router.id);
            return false;
        }
        router.routerId = trimCopy(jsonString(entry, "router_id", Topology::routerIdFromIndex(oneBasedIndex)));
        router.asn = jsonUint(entry, "asn", 65000);
        router.clusterId = trimCopy(jsonString(entry, "cluster_id", router.routerId));
        router.originatedPrefixes = readStringArray(member(entry, "originated_prefixes"));

        const auto* position = member(entry, "position");
        const auto defaultX = 140.0 + static_cast<double>(index % 5) * 150.0;
        const auto defaultY = 120.0 + static_cast<double>(index / 5) * 120.0;
        router.position = Position{.x = position && position->is_object() ? jsonDouble(*position, "x", defaultX) : defaultX,
                                   .y = position && position->is_object() ? jsonDouble(*position, "y", defaultY) : defaultY};

        const auto* pluginValue = member(entry, "plugin");
        if (pluginValue && !pluginValue->is_null() && !pluginValue->is_string() && !pluginValue->is_object())
        {
            setError(error, "路由器 " + router.id + " 的 plugin 必须是字符串或对象");
            return false;
        }
        const auto pluginIdFallback = jsonString(entry, "plugin_id", StandardRouterPluginId);
        if (pluginValue && pluginValue->is_string())
        {
            router.pluginId = trimCopy(pluginValue->get_ref<const std::string&>());
            if (const auto* legacySettings = member(entry, "plugin_settings"); legacySettings && legacySettings->is_object())
            {
                router.pluginSettings = *legacySettings;
            }
        }
        else if (pluginValue && pluginValue->is_object())
        {
            router.pluginId = trimCopy(jsonString(*pluginValue, "id", pluginIdFallback));
            const auto* settings = member(*pluginValue, "settings");
            if (settings && !settings->is_object())
            {
                setError(error, "路由器 " + router.id + " 的 plugin.settings 必须是对象");
                return false;
            }
            if (settings)
            {
                router.pluginSettings = *settings;
            }
            else if (const auto* legacySettings = member(entry, "plugin_settings"); legacySettings && legacySettings->is_object())
            {
                router.pluginSettings = *legacySettings;
            }
        }
        else
        {
            router.pluginId = trimCopy(pluginIdFallback);
            if (const auto* legacySettings = member(entry, "plugin_settings"); legacySettings && legacySettings->is_object())
            {
                router.pluginSettings = *legacySettings;
            }
        }

        if (const auto* neighbors = member(entry, "neighbors"); neighbors && neighbors->is_array())
        {
            for (const auto& neighbor : *neighbors)
            {
                if (neighbor.is_object())
                {
                    legacyNeighbors_.push_back(LegacyNeighborEntry{router.id, neighbor});
                }
            }
        }
        const auto routerId = router.id;
        topology_.routers.emplace(routerId, std::move(router));
        return true;
    }

    bool addLink(const Json& entry, std::string* error)
    {
        LinkConfig link;
        link.a = trimCopy(jsonString(entry, "a"));
        link.b = trimCopy(jsonString(entry, "b"));
        link.enabled = jsonBool(entry, "enabled", true);
        link.delayMs = jsonNonNegativeInt(entry, "delay_ms", 0);
        link.rrClientFromA = jsonBool(entry, "rr_client_from_a", false);
        link.rrClientFromB = jsonBool(entry, "rr_client_from_b", false);
        link.mraiMsFromA = jsonNonNegativeInt(entry, "mrai_ms_from_a", 0);
        link.mraiMsFromB = jsonNonNegativeInt(entry, "mrai_ms_from_b", 0);

        const auto* relationshipValue = member(entry, "relationship");
        if (relationshipValue && !relationshipValue->is_null())
        {
            if (!relationshipValue->is_string())
            {
                setError(error, "链路 " + link.a + " - " + link.b + " 的 relationship 必须是字符串");
                return false;
            }
            const auto relationship = linkBusinessRelationshipFromString(relationshipValue->get_ref<const std::string&>());
            if (!relationship)
            {
                setError(error, "链路 " + link.a + " - " + link.b + " 的 relationship 无效：" +
                                    relationshipValue->get<std::string>());
                return false;
            }
            link.businessRelationship = *relationship;
        }

        const auto* enabled = member(entry, "enabled");
        const auto* rrClientFromA = member(entry, "rr_client_from_a");
        const auto* rrClientFromB = member(entry, "rr_client_from_b");
        const auto* mraiMsFromA = member(entry, "mrai_ms_from_a");
        const auto* mraiMsFromB = member(entry, "mrai_ms_from_b");
        explicitLinkFieldsByIndex_.push_back(ExplicitLinkFields{
            .enabled = enabled && enabled->is_boolean(),
            .rrClientFromA = rrClientFromA && rrClientFromA->is_boolean(),
            .rrClientFromB = rrClientFromB && rrClientFromB->is_boolean(),
            .mraiMsFromA = mraiMsFromA && mraiMsFromA->is_number(),
            .mraiMsFromB = mraiMsFromB && mraiMsFromB->is_number(),
            .relationship = relationshipValue && !relationshipValue->is_null(),
        });
        topology_.links.push_back(std::move(link));
        return true;
    }

    std::size_t routerCount() const
    {
        return topology_.routers.size();
    }

    std::size_t linkCount() const
    {
        return topology_.links.size();
    }

    std::optional<Topology> finish(std::string* error)
    {
        std::unordered_map<std::string, LinkBusinessRelationship> legacyRelationships;
        std::unordered_map<std::string, std::size_t> linkIndexes;
        std::unordered_map<std::string, ExplicitLinkFields> explicitLinkFields;
        if (!legacyNeighbors_.empty())
        {
            linkIndexes.reserve(topology_.links.size());
            explicitLinkFields.reserve(topology_.links.size());
            for (std::size_t index = 0; index < topology_.links.size(); ++index)
            {
                const auto& link = topology_.links[index];
                const auto edge = Topology::edgeKey(link.a, link.b);
                if (!linkIndexes.contains(edge))
                {
                    linkIndexes.emplace(edge, index);
                    explicitLinkFields.emplace(edge, explicitLinkFieldsByIndex_[index]);
                }
            }
        }

        for (const auto& legacy : legacyNeighbors_)
        {
            const auto peerId = trimCopy(jsonString(legacy.object, "id"));
            if (!topology_.routers.contains(legacy.routerId) || !topology_.routers.contains(peerId) || legacy.routerId == peerId)
            {
                continue;
            }
            const auto edge = Topology::edgeKey(legacy.routerId, peerId);
            auto indexIt = linkIndexes.find(edge);
            if (indexIt == linkIndexes.end())
            {
                LinkConfig generated;
                generated.a = legacy.routerId;
                generated.b = peerId;
                const auto index = topology_.links.size();
                topology_.links.push_back(std::move(generated));
                linkIndexes.emplace(edge, index);
                explicitLinkFields.emplace(edge, ExplicitLinkFields{});
                indexIt = linkIndexes.find(edge);
            }

            auto& link = topology_.links[indexIt->second];
            const auto fields = explicitLinkFields.at(edge);
            const auto rr = jsonBool(legacy.object, "rr_client", false);
            const auto mrai = jsonNonNegativeInt(legacy.object, "mrai_ms", 0);
            const auto enabled = jsonBool(legacy.object, "enabled", true);
            const auto* relationshipValue = member(legacy.object, "relationship");
            if (!fields.relationship && relationshipValue && !relationshipValue->is_null())
            {
                if (!relationshipValue->is_string())
                {
                    setError(error, "邻居 " + legacy.routerId + " → " + peerId + " 的 relationship 必须是字符串");
                    return std::nullopt;
                }
                const auto relationship = neighborRelationshipFromString(relationshipValue->get_ref<const std::string&>());
                if (!relationship)
                {
                    setError(error, "邻居 " + legacy.routerId + " → " + peerId + " 的 relationship 无效：" +
                                        relationshipValue->get<std::string>());
                    return std::nullopt;
                }
                const auto linkRelationship = linkRelationshipForNeighbor(*relationship, link.a == legacy.routerId);
                const auto previous = legacyRelationships.find(edge);
                if (previous != legacyRelationships.end() && previous->second != linkRelationship)
                {
                    setError(error, "链路 " + link.a + " - " + link.b + " 的双向邻居 relationship 不一致");
                    return std::nullopt;
                }
                legacyRelationships.insert_or_assign(edge, linkRelationship);
                link.businessRelationship = linkRelationship;
            }
            if (!fields.enabled)
            {
                link.enabled = link.enabled && enabled;
            }
            if (link.a == legacy.routerId)
            {
                if (!fields.rrClientFromA)
                {
                    link.rrClientFromA = rr;
                }
                if (!fields.mraiMsFromA)
                {
                    link.mraiMsFromA = mrai;
                }
            }
            else
            {
                if (!fields.rrClientFromB)
                {
                    link.rrClientFromB = rr;
                }
                if (!fields.mraiMsFromB)
                {
                    link.mraiMsFromB = mrai;
                }
            }
        }

        const auto problems = topology_.validate();
        if (!problems.empty())
        {
            setError(error, joinStrings(problems, "\n"));
            return std::nullopt;
        }
        return std::move(topology_);
    }

private:
    Topology topology_;
    std::vector<ExplicitLinkFields> explicitLinkFieldsByIndex_;
    std::vector<LegacyNeighborEntry> legacyNeighbors_;
};

bool reportProgress(const TopologyLoadProgressCallback& progress, TopologyLoadStage stage, std::int64_t totalBytes,
                    std::size_t routersLoaded, std::size_t linksLoaded, std::size_t completedItems, std::size_t totalItems,
                    std::string* error)
{
    if (!progress)
    {
        return true;
    }
    const auto bytesProcessed = totalItems == 0
                                    ? totalBytes
                                    : static_cast<std::int64_t>((static_cast<long double>(totalBytes) * completedItems) / totalItems);
    if (progress(TopologyLoadProgress{.stage = stage,
                                      .bytesProcessed = bytesProcessed,
                                      .totalBytes = totalBytes,
                                      .routersLoaded = routersLoaded,
                                      .linksLoaded = linksLoaded}))
    {
        return true;
    }
    setError(error, "拓扑加载已取消");
    return false;
}

std::optional<Topology> buildTopology(const Json& object, std::string* error, const TopologyLoadProgressCallback& progress,
                                      std::int64_t totalBytes)
{
    if (!object.is_object())
    {
        setError(error, "拓扑 JSON 顶层必须是对象");
        return std::nullopt;
    }
    const auto* routers = member(object, "routers");
    const auto* links = member(object, "links");
    const auto routerCount = routers && routers->is_array() ? routers->size() : 0;
    const auto linkCount = links && links->is_array() ? links->size() : 0;
    const auto totalItems = routerCount + linkCount + 1;

    TopologyJsonBuilder builder;
    builder.reserve(linkCount);
    const auto* simulation = member(object, "simulation");
    builder.setSimulation(simulation && simulation->is_object() ? *simulation : Json::object());

    if (!reportProgress(progress, TopologyLoadStage::ReadingRouters, totalBytes, 0, 0, 0, totalItems, error))
    {
        return std::nullopt;
    }
    if (routers && routers->is_array())
    {
        for (std::size_t index = 0; index < routers->size(); ++index)
        {
            const auto& value = (*routers)[index];
            if (!value.is_object())
            {
                setError(error, "routers 数组包含非对象成员");
                return std::nullopt;
            }
            if (!builder.addRouter(value, error))
            {
                return std::nullopt;
            }
            if ((index + 1 == routers->size() || (index + 1) % 1024 == 0) &&
                !reportProgress(progress, TopologyLoadStage::ReadingRouters, totalBytes, builder.routerCount(), 0, index + 1,
                                totalItems, error))
            {
                return std::nullopt;
            }
        }
    }

    if (!reportProgress(progress, TopologyLoadStage::ReadingLinks, totalBytes, builder.routerCount(), 0, routerCount, totalItems,
                        error))
    {
        return std::nullopt;
    }
    if (links && links->is_array())
    {
        for (std::size_t index = 0; index < links->size(); ++index)
        {
            const auto& value = (*links)[index];
            if (!value.is_object())
            {
                setError(error, "links 数组包含非对象成员");
                return std::nullopt;
            }
            if (!builder.addLink(value, error))
            {
                return std::nullopt;
            }
            if ((index + 1 == links->size() || (index + 1) % 1024 == 0) &&
                !reportProgress(progress, TopologyLoadStage::ReadingLinks, totalBytes, builder.routerCount(), builder.linkCount(),
                                routerCount + index + 1, totalItems, error))
            {
                return std::nullopt;
            }
        }
    }

    if (!reportProgress(progress, TopologyLoadStage::Validating, totalBytes, builder.routerCount(), builder.linkCount(), totalItems,
                        totalItems, error))
    {
        return std::nullopt;
    }
    return builder.finish(error);
}

std::filesystem::path temporarySibling(const std::filesystem::path& path, std::string_view label)
{
    static std::atomic<std::uint64_t> sequence{0};
    auto temporary = path;
    temporary += ".";
    temporary += label;
    temporary += ".";
    temporary += std::to_string(++sequence);
    return temporary;
}

bool replaceFile(const std::filesystem::path& temporary, const std::filesystem::path& destination, std::string* error)
{
    std::error_code renameError;
    std::filesystem::rename(temporary, destination, renameError);
    if (!renameError)
    {
        return true;
    }

    std::error_code existsError;
    if (!std::filesystem::exists(destination, existsError) || existsError)
    {
        setError(error, "保存 " + pathText(destination) + " 失败：" + renameError.message());
        return false;
    }

    const auto backup = temporarySibling(destination, "backup");
    std::error_code backupError;
    std::filesystem::rename(destination, backup, backupError);
    if (backupError)
    {
        setError(error, "保存 " + pathText(destination) + " 失败：" + backupError.message());
        return false;
    }

    std::error_code installError;
    std::filesystem::rename(temporary, destination, installError);
    if (installError)
    {
        std::error_code restoreError;
        std::filesystem::rename(backup, destination, restoreError);
        setError(error, "保存 " + pathText(destination) + " 失败：" + installError.message() +
                            (restoreError ? "；恢复原文件也失败：" + restoreError.message() : std::string{}));
        return false;
    }

    std::error_code cleanupError;
    std::filesystem::remove(backup, cleanupError);
    return true;
}

} // namespace

bool isCanonicalIpv4Address(std::string_view text, bool allowZero)
{
    return canonicalIpv4Address(text, allowZero);
}

bool isCanonicalIpv4Prefix(std::string_view text)
{
    return canonicalIpv4Prefix(text);
}

std::string toString(SessionType type)
{
    return type == SessionType::Ebgp ? "ebgp" : "ibgp";
}

std::optional<SessionType> sessionTypeFromString(std::string_view value)
{
    const auto normalized = trimCopy(value);
    if (asciiCaseEqual(normalized, "ibgp"))
    {
        return SessionType::Ibgp;
    }
    if (asciiCaseEqual(normalized, "ebgp"))
    {
        return SessionType::Ebgp;
    }
    return std::nullopt;
}

std::string toString(LinkBusinessRelationship relationship)
{
    switch (relationship)
    {
        case LinkBusinessRelationship::Unspecified:
            return "unspecified";
        case LinkBusinessRelationship::PeerToPeer:
            return "peer";
        case LinkBusinessRelationship::AProviderOfB:
            return "a_provider";
        case LinkBusinessRelationship::BProviderOfA:
            return "b_provider";
    }
    return "unspecified";
}

std::optional<LinkBusinessRelationship> linkBusinessRelationshipFromString(std::string_view value)
{
    const auto normalized = trimCopy(value);
    if (asciiCaseEqual(normalized, "unspecified"))
    {
        return LinkBusinessRelationship::Unspecified;
    }
    if (asciiCaseEqual(normalized, "peer"))
    {
        return LinkBusinessRelationship::PeerToPeer;
    }
    if (asciiCaseEqual(normalized, "a_provider"))
    {
        return LinkBusinessRelationship::AProviderOfB;
    }
    if (asciiCaseEqual(normalized, "b_provider"))
    {
        return LinkBusinessRelationship::BProviderOfA;
    }
    return std::nullopt;
}

std::string toString(NeighborRelationship relationship)
{
    switch (relationship)
    {
        case NeighborRelationship::Unspecified:
            return "unspecified";
        case NeighborRelationship::Peer:
            return "peer";
        case NeighborRelationship::Provider:
            return "provider";
        case NeighborRelationship::Customer:
            return "customer";
    }
    return "unspecified";
}

std::optional<NeighborRelationship> neighborRelationshipFromString(std::string_view value)
{
    const auto normalized = trimCopy(value);
    if (asciiCaseEqual(normalized, "unspecified"))
    {
        return NeighborRelationship::Unspecified;
    }
    if (asciiCaseEqual(normalized, "peer"))
    {
        return NeighborRelationship::Peer;
    }
    if (asciiCaseEqual(normalized, "provider"))
    {
        return NeighborRelationship::Provider;
    }
    if (asciiCaseEqual(normalized, "customer"))
    {
        return NeighborRelationship::Customer;
    }
    return std::nullopt;
}

Topology Topology::starter()
{
    Topology topology;
    topology.simulation.name = "quick-lab";
    RouterConfig r1{.id = "R1",
                    .routerId = "10.0.0.1",
                    .asn = 65001,
                    .clusterId = "10.0.0.1",
                    .originatedPrefixes = {"10.1.0.0/24"},
                    .position = Position{180.0, 220.0},
                    .pluginId = StandardRouterPluginId,
                    .pluginSettings = Json::object()};
    RouterConfig r2{.id = "R2",
                    .routerId = "10.0.0.2",
                    .asn = 65002,
                    .clusterId = "10.0.0.2",
                    .originatedPrefixes = {"10.2.0.0/24"},
                    .position = Position{450.0, 220.0},
                    .pluginId = StandardRouterPluginId,
                    .pluginSettings = Json::object()};
    topology.routers.emplace(r1.id, r1);
    topology.routers.emplace(r2.id, r2);
    topology.links.push_back(LinkConfig{.a = r1.id, .b = r2.id, .delayMs = 10});
    return topology;
}

Json Topology::toJson() const
{
    Json routerArray = Json::array();
    for (const auto& [id, router] : routers)
    {
        static_cast<void>(id);
        routerArray.push_back(Json{
            {"id", router.id},
            {"router_id", router.routerId},
            {"asn", router.asn},
            {"cluster_id", router.clusterId.empty() ? router.routerId : router.clusterId},
            {"originated_prefixes", router.originatedPrefixes},
            {"position", Json{{"x", router.position.x}, {"y", router.position.y}}},
            {"plugin", Json{{"id", router.pluginId}, {"settings", router.pluginSettings}}},
        });
    }

    Json linkArray = Json::array();
    for (const auto& link : links)
    {
        linkArray.push_back(Json{
            {"a", link.a},
            {"b", link.b},
            {"enabled", link.enabled},
            {"delay_ms", link.delayMs},
            {"rr_client_from_a", link.rrClientFromA},
            {"rr_client_from_b", link.rrClientFromB},
            {"mrai_ms_from_a", link.mraiMsFromA},
            {"mrai_ms_from_b", link.mraiMsFromB},
            {"relationship", toString(link.businessRelationship)},
        });
    }

    return Json{{"simulation",
                 Json{{"name", simulation.name},
                      {"log_dir", simulation.logDirectory},
                      {"worker_threads", simulation.workerThreads},
                      {"convergence_quiet_ms", simulation.convergenceQuietMs},
                      {"withdrawal_ignores_mrai", simulation.withdrawalIgnoresMrai},
                      {"router_class", "BgpRouter"}}},
                {"routers", std::move(routerArray)},
                {"links", std::move(linkArray)}};
}

std::optional<Topology> Topology::fromJson(const Json& object, std::string* error)
{
    if (error)
    {
        error->clear();
    }
    return buildTopology(object, error, {}, 0);
}

std::optional<Topology> Topology::load(const std::filesystem::path& path, std::string* error,
                                       const TopologyLoadProgressCallback& progress)
{
    if (error)
    {
        error->clear();
    }
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    if (sizeError)
    {
        setError(error, "无法读取 " + pathText(path) + "：" + sizeError.message());
        return std::nullopt;
    }
    if (size == 0)
    {
        setError(error, "拓扑文件为空");
        return std::nullopt;
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
    {
        setError(error, "拓扑文件过大，无法读取到当前进程地址空间");
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        setError(error, "无法读取 " + pathText(path));
        return std::nullopt;
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file || file.gcount() != static_cast<std::streamsize>(contents.size()))
    {
        setError(error, "读取 " + pathText(path) + " 失败");
        return std::nullopt;
    }

    Json object;
    try
    {
        object = Json::parse(contents);
    }
    catch (const Json::parse_error& exception)
    {
        setError(error, "拓扑 JSON 解析失败（偏移 " + std::to_string(exception.byte) + "）：" + exception.what());
        return std::nullopt;
    }
    catch (const Json::exception& exception)
    {
        setError(error, "拓扑 JSON 解析失败：" + std::string(exception.what()));
        return std::nullopt;
    }
    return buildTopology(object, error, progress, static_cast<std::int64_t>(size));
}

bool Topology::save(const std::filesystem::path& path, std::string* error) const
{
    if (error)
    {
        error->clear();
    }
    const auto problems = validate();
    if (!problems.empty())
    {
        setError(error, joinStrings(problems, "\n"));
        return false;
    }

    const auto temporary = temporarySibling(path, "tmp");
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        setError(error, "无法写入 " + pathText(path));
        return false;
    }
    auto contents = toJson().dump(4);
    contents.push_back('\n');
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.flush();
    if (!file)
    {
        file.close();
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        setError(error, "无法完整写入 " + pathText(path));
        return false;
    }
    file.close();
    if (!replaceFile(temporary, path, error))
    {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    return true;
}

std::vector<std::string> Topology::validate() const
{
    std::vector<std::string> problems;
    if (trimCopy(simulation.name).empty())
    {
        problems.emplace_back("仿真名称不能为空");
    }
    if (trimCopy(simulation.logDirectory).empty())
    {
        problems.emplace_back("日志目录不能为空");
    }
    if (simulation.convergenceQuietMs < 0)
    {
        problems.emplace_back("收敛静默时间不能为负数");
    }
    if (simulation.workerThreads < 0 || simulation.workerThreads > 256)
    {
        problems.emplace_back("后台工作线程数必须在 0 到 256 之间");
    }
    if (routers.empty())
    {
        problems.emplace_back("拓扑至少需要一台路由器");
    }

    std::unordered_set<std::string> bgpRouterIds;
    bgpRouterIds.reserve(routers.size());
    for (const auto& [key, router] : routers)
    {
        if (trimCopy(router.id).empty())
        {
            problems.emplace_back("路由器 ID 不能为空");
        }
        if (router.id != key)
        {
            problems.push_back("路由器映射键与 ID 不一致：" + key + " / " + router.id);
        }
        if (!isCanonicalIpv4Address(router.routerId, false))
        {
            problems.push_back(router.id + " 的 BGP Router ID 无效：" + router.routerId);
        }
        else if (!bgpRouterIds.insert(router.routerId).second)
        {
            problems.push_back("BGP Router ID 重复：" + router.routerId);
        }
        if (router.asn == 0)
        {
            problems.push_back(router.id + " 的 ASN 必须大于 0");
        }
        if (trimCopy(router.pluginId).empty())
        {
            problems.push_back(router.id + " 的路由器插件 ID 不能为空");
        }
        if (!router.clusterId.empty() && !isCanonicalIpv4Address(router.clusterId))
        {
            problems.push_back(router.id + " 的 Cluster ID 无效：" + router.clusterId);
        }
        for (const auto& prefix : router.originatedPrefixes)
        {
            if (!isCanonicalIpv4Prefix(prefix))
            {
                problems.push_back(router.id + " 的前缀无效：" + prefix);
            }
        }
    }

    std::unordered_set<std::string> edges;
    edges.reserve(links.size());
    for (const auto& link : links)
    {
        const auto a = routers.find(link.a);
        const auto b = routers.find(link.b);
        if (link.a == link.b)
        {
            problems.push_back("链路不能连接路由器自身：" + link.a);
        }
        if (a == routers.end() || b == routers.end())
        {
            problems.push_back("链路端点不存在：" + link.a + " - " + link.b);
        }
        else if (a->second.asn == b->second.asn && link.businessRelationship != LinkBusinessRelationship::Unspecified)
        {
            problems.push_back("同一 AS 内的链路不能设置商业关系：" + link.a + " - " + link.b);
        }
        const auto key = edgeKey(link.a, link.b);
        if (!edges.insert(key).second)
        {
            problems.push_back("链路重复：" + link.a + " - " + link.b);
        }
        if (link.delayMs < 0 || link.mraiMsFromA < 0 || link.mraiMsFromB < 0)
        {
            problems.push_back("链路 " + link.a + " - " + link.b + " 的延迟/MRAI 不能为负数");
        }
    }
    return problems;
}

std::vector<NeighborConfig> Topology::neighborsFor(const std::string& routerId) const
{
    std::vector<NeighborConfig> result;
    const auto local = routers.find(routerId);
    if (local == routers.end())
    {
        return result;
    }
    for (const auto& link : links)
    {
        std::string peerId;
        bool fromA = false;
        if (link.a == routerId)
        {
            peerId = link.b;
            fromA = true;
        }
        else if (link.b == routerId)
        {
            peerId = link.a;
        }
        else
        {
            continue;
        }
        const auto peer = routers.find(peerId);
        if (peer == routers.end())
        {
            continue;
        }
        result.push_back(NeighborConfig{
            .id = peerId,
            .remoteAsn = peer->second.asn,
            .sessionType = local->second.asn == peer->second.asn ? SessionType::Ibgp : SessionType::Ebgp,
            .rrClient = fromA ? link.rrClientFromA : link.rrClientFromB,
            .enabled = link.enabled,
            .mraiMs = fromA ? link.mraiMsFromA : link.mraiMsFromB,
            .relationship = neighborRelationshipFor(link, fromA),
        });
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) { return lhs.id < rhs.id; });
    return result;
}

NeighborIndex Topology::buildNeighborIndex() const
{
    NeighborIndex result;
    result.reserve(routers.size());
    for (const auto& link : links)
    {
        const auto a = routers.find(link.a);
        const auto b = routers.find(link.b);
        if (a == routers.end() || b == routers.end())
        {
            continue;
        }
        const auto sessionType = a->second.asn == b->second.asn ? SessionType::Ibgp : SessionType::Ebgp;
        result[link.a].emplace(link.b, NeighborConfig{
                                                .id = link.b,
                                                .remoteAsn = b->second.asn,
                                                .sessionType = sessionType,
                                                .rrClient = link.rrClientFromA,
                                                .enabled = link.enabled,
                                                .mraiMs = link.mraiMsFromA,
                                                .relationship = neighborRelationshipFor(link, true),
                                            });
        result[link.b].emplace(link.a, NeighborConfig{
                                                .id = link.a,
                                                .remoteAsn = a->second.asn,
                                                .sessionType = sessionType,
                                                .rrClient = link.rrClientFromB,
                                                .enabled = link.enabled,
                                                .mraiMs = link.mraiMsFromB,
                                                .relationship = neighborRelationshipFor(link, false),
                                            });
    }
    return result;
}

const LinkConfig* Topology::findLink(const std::string& a, const std::string& b) const
{
    const auto key = edgeKey(a, b);
    const auto it = std::find_if(links.cbegin(), links.cend(), [&](const auto& link) { return edgeKey(link.a, link.b) == key; });
    return it == links.cend() ? nullptr : &*it;
}

LinkConfig* Topology::findLink(const std::string& a, const std::string& b)
{
    const auto key = edgeKey(a, b);
    const auto it = std::find_if(links.begin(), links.end(), [&](const auto& link) { return edgeKey(link.a, link.b) == key; });
    return it == links.end() ? nullptr : &*it;
}

std::string Topology::nextRouterName() const
{
    for (int index = 1; index < std::numeric_limits<int>::max(); ++index)
    {
        auto candidate = "R" + std::to_string(index);
        if (!routers.contains(candidate))
        {
            return candidate;
        }
    }
    return "Router";
}

std::string Topology::nextBgpRouterId() const
{
    std::unordered_set<std::string> used;
    used.reserve(routers.size());
    for (const auto& [id, router] : routers)
    {
        static_cast<void>(id);
        used.insert(router.routerId);
    }
    for (int index = 1; index <= 256 * 256 * 254; ++index)
    {
        auto candidate = routerIdFromIndex(index);
        if (!used.contains(candidate))
        {
            return candidate;
        }
    }
    return "10.255.255.254";
}

std::string Topology::routerIdFromIndex(int oneBasedIndex)
{
    if (oneBasedIndex < 1)
    {
        oneBasedIndex = 1;
    }
    constexpr int usableLastOctets = 254;
    const auto zeroBased = oneBasedIndex - 1;
    const auto second = zeroBased / (256 * usableLastOctets);
    const auto remainder = zeroBased % (256 * usableLastOctets);
    const auto third = remainder / usableLastOctets;
    const auto fourth = remainder % usableLastOctets + 1;
    return "10." + std::to_string(second) + "." + std::to_string(third) + "." + std::to_string(fourth);
}

std::string Topology::edgeKey(const std::string& a, const std::string& b)
{
    return a < b ? a + '\x1f' + b : b + '\x1f' + a;
}

} // namespace bgptester
