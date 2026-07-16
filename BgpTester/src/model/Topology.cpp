#include "model/Topology.hpp"

#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <limits>

namespace bgptester
{
namespace
{

bool isIpv4(const QString& value, bool allowZero = true)
{
    QHostAddress address;
    if (!address.setAddress(value) || address.protocol() != QAbstractSocket::IPv4Protocol)
    {
        return false;
    }
    return allowZero || address.toIPv4Address() != 0;
}

bool isIpv4Cidr(const QString& value)
{
    const auto slash = value.lastIndexOf(u'/');
    if (slash <= 0 || slash == value.size() - 1)
    {
        return false;
    }
    bool prefixOk = false;
    const auto prefixLength = value.sliced(slash + 1).toInt(&prefixOk);
    return prefixOk && prefixLength >= 0 && prefixLength <= 32 && isIpv4(value.first(slash));
}

quint32 jsonUint(const QJsonObject& object, const QString& key, quint32 fallback)
{
    const auto value = object.value(key);
    if (!value.isDouble())
    {
        return fallback;
    }
    const auto number = value.toDouble();
    if (number < 0.0 || number > static_cast<double>(std::numeric_limits<quint32>::max()))
    {
        return fallback;
    }
    return static_cast<quint32>(number);
}

int jsonNonNegativeInt(const QJsonObject& object, const QString& key, int fallback)
{
    const auto value = object.value(key);
    if (!value.isDouble())
    {
        return fallback;
    }
    const auto number = value.toInteger(fallback);
    if (number < 0 || number > std::numeric_limits<int>::max())
    {
        return fallback;
    }
    return static_cast<int>(number);
}

QJsonArray stringArray(const QStringList& values)
{
    QJsonArray result;
    for (const auto& value : values)
    {
        result.append(value);
    }
    return result;
}

QStringList readStringArray(const QJsonValue& value)
{
    QStringList result;
    if (!value.isArray())
    {
        return result;
    }
    for (const auto& entry : value.toArray())
    {
        if (entry.isString())
        {
            result.append(entry.toString().trimmed());
        }
    }
    result.removeAll(QString{});
    result.removeDuplicates();
    return result;
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

} // namespace

QString toString(SessionType type)
{
    return type == SessionType::Ebgp ? QStringLiteral("ebgp") : QStringLiteral("ibgp");
}

std::optional<SessionType> sessionTypeFromString(const QString& value)
{
    if (value.compare(QStringLiteral("ibgp"), Qt::CaseInsensitive) == 0)
    {
        return SessionType::Ibgp;
    }
    if (value.compare(QStringLiteral("ebgp"), Qt::CaseInsensitive) == 0)
    {
        return SessionType::Ebgp;
    }
    return std::nullopt;
}

QString toString(LinkBusinessRelationship relationship)
{
    switch (relationship)
    {
        case LinkBusinessRelationship::Unspecified:
            return QStringLiteral("unspecified");
        case LinkBusinessRelationship::PeerToPeer:
            return QStringLiteral("peer");
        case LinkBusinessRelationship::AProviderOfB:
            return QStringLiteral("a_provider");
        case LinkBusinessRelationship::BProviderOfA:
            return QStringLiteral("b_provider");
    }
    return QStringLiteral("unspecified");
}

std::optional<LinkBusinessRelationship> linkBusinessRelationshipFromString(const QString& value)
{
    const auto normalized = value.trimmed();
    if (normalized.compare(QStringLiteral("unspecified"), Qt::CaseInsensitive) == 0)
    {
        return LinkBusinessRelationship::Unspecified;
    }
    if (normalized.compare(QStringLiteral("peer"), Qt::CaseInsensitive) == 0)
    {
        return LinkBusinessRelationship::PeerToPeer;
    }
    if (normalized.compare(QStringLiteral("a_provider"), Qt::CaseInsensitive) == 0)
    {
        return LinkBusinessRelationship::AProviderOfB;
    }
    if (normalized.compare(QStringLiteral("b_provider"), Qt::CaseInsensitive) == 0)
    {
        return LinkBusinessRelationship::BProviderOfA;
    }
    return std::nullopt;
}

QString toString(NeighborRelationship relationship)
{
    switch (relationship)
    {
        case NeighborRelationship::Unspecified:
            return QStringLiteral("unspecified");
        case NeighborRelationship::Peer:
            return QStringLiteral("peer");
        case NeighborRelationship::Provider:
            return QStringLiteral("provider");
        case NeighborRelationship::Customer:
            return QStringLiteral("customer");
    }
    return QStringLiteral("unspecified");
}

std::optional<NeighborRelationship> neighborRelationshipFromString(const QString& value)
{
    const auto normalized = value.trimmed();
    if (normalized.compare(QStringLiteral("unspecified"), Qt::CaseInsensitive) == 0)
    {
        return NeighborRelationship::Unspecified;
    }
    if (normalized.compare(QStringLiteral("peer"), Qt::CaseInsensitive) == 0)
    {
        return NeighborRelationship::Peer;
    }
    if (normalized.compare(QStringLiteral("provider"), Qt::CaseInsensitive) == 0)
    {
        return NeighborRelationship::Provider;
    }
    if (normalized.compare(QStringLiteral("customer"), Qt::CaseInsensitive) == 0)
    {
        return NeighborRelationship::Customer;
    }
    return std::nullopt;
}

QJsonObject Topology::toJson() const
{
    QJsonObject simulationObject{
        {QStringLiteral("name"), simulation.name},
        {QStringLiteral("log_dir"), simulation.logDirectory},
        {QStringLiteral("worker_threads"), 0},
        {QStringLiteral("convergence_quiet_ms"), simulation.convergenceQuietMs},
        {QStringLiteral("router_class"), QStringLiteral("BgpRouter")},
    };

    QMap<QString, QJsonArray> neighborArrays;
    for (auto it = routers.cbegin(); it != routers.cend(); ++it)
    {
        neighborArrays.insert(it.key(), {});
    }
    for (const auto& link : links)
    {
        const auto aIt = routers.constFind(link.a);
        const auto bIt = routers.constFind(link.b);
        if (aIt == routers.cend() || bIt == routers.cend())
        {
            continue;
        }
        const auto session = aIt->asn == bIt->asn ? QStringLiteral("ibgp") : QStringLiteral("ebgp");
        neighborArrays[link.a].append(QJsonObject{
            {QStringLiteral("id"), link.b},
            {QStringLiteral("remote_asn"), static_cast<qint64>(bIt->asn)},
            {QStringLiteral("session_type"), session},
            {QStringLiteral("rr_client"), link.rrClientFromA},
            {QStringLiteral("mrai_ms"), link.mraiMsFromA},
            {QStringLiteral("enabled"), link.enabled},
            {QStringLiteral("relationship"), toString(neighborRelationshipFor(link, true))},
        });
        neighborArrays[link.b].append(QJsonObject{
            {QStringLiteral("id"), link.a},
            {QStringLiteral("remote_asn"), static_cast<qint64>(aIt->asn)},
            {QStringLiteral("session_type"), session},
            {QStringLiteral("rr_client"), link.rrClientFromB},
            {QStringLiteral("mrai_ms"), link.mraiMsFromB},
            {QStringLiteral("enabled"), link.enabled},
            {QStringLiteral("relationship"), toString(neighborRelationshipFor(link, false))},
        });
    }

    QJsonArray routerArray;
    for (auto it = routers.cbegin(); it != routers.cend(); ++it)
    {
        const auto& router = it.value();
        routerArray.append(QJsonObject{
            {QStringLiteral("id"), router.id},
            {QStringLiteral("router_id"), router.routerId},
            {QStringLiteral("asn"), static_cast<qint64>(router.asn)},
            {QStringLiteral("cluster_id"), router.clusterId.isEmpty() ? router.routerId : router.clusterId},
            {QStringLiteral("originated_prefixes"), stringArray(router.originatedPrefixes)},
            {QStringLiteral("position"),
             QJsonObject{{QStringLiteral("x"), router.position.x()}, {QStringLiteral("y"), router.position.y()}}},
            {QStringLiteral("plugin"),
             QJsonObject{{QStringLiteral("id"), router.pluginId}, {QStringLiteral("settings"), router.pluginSettings}}},
            {QStringLiteral("neighbors"), neighborArrays.value(router.id)},
        });
    }

    QJsonArray linkArray;
    for (const auto& link : links)
    {
        linkArray.append(QJsonObject{
            {QStringLiteral("a"), link.a},
            {QStringLiteral("b"), link.b},
            {QStringLiteral("enabled"), link.enabled},
            {QStringLiteral("delay_ms"), link.delayMs},
            {QStringLiteral("rr_client_from_a"), link.rrClientFromA},
            {QStringLiteral("rr_client_from_b"), link.rrClientFromB},
            {QStringLiteral("mrai_ms_from_a"), link.mraiMsFromA},
            {QStringLiteral("mrai_ms_from_b"), link.mraiMsFromB},
            {QStringLiteral("relationship"), toString(link.businessRelationship)},
        });
    }

    return {
        {QStringLiteral("simulation"), simulationObject}, {QStringLiteral("routers"), routerArray}, {QStringLiteral("links"), linkArray}};
}

std::optional<Topology> Topology::fromJson(const QJsonObject& object, QString* error)
{
    Topology topology;
    const auto simulationObject = object.value(QStringLiteral("simulation")).toObject();
    topology.simulation.name = simulationObject.value(QStringLiteral("name")).toString(QStringLiteral("bgp-lab"));
    topology.simulation.logDirectory = simulationObject.value(QStringLiteral("log_dir")).toString(QStringLiteral("tmp"));
    topology.simulation.convergenceQuietMs = jsonNonNegativeInt(simulationObject, QStringLiteral("convergence_quiet_ms"), 1000);

    const auto routerValues = object.value(QStringLiteral("routers")).toArray();
    QVector<QJsonObject> originalRouters;
    originalRouters.reserve(routerValues.size());
    int index = 0;
    for (const auto& value : routerValues)
    {
        if (!value.isObject())
        {
            if (error)
            {
                *error = QStringLiteral("routers 数组包含非对象成员");
            }
            return std::nullopt;
        }
        const auto entry = value.toObject();
        originalRouters.append(entry);
        RouterConfig router;
        router.id = entry.value(QStringLiteral("id")).toString(QStringLiteral("R%1").arg(index + 1)).trimmed();
        if (topology.routers.contains(router.id))
        {
            if (error)
            {
                *error = QStringLiteral("路由器 ID 重复：%1").arg(router.id);
            }
            return std::nullopt;
        }
        router.routerId = entry.value(QStringLiteral("router_id")).toString(routerIdFromIndex(index + 1)).trimmed();
        router.asn = jsonUint(entry, QStringLiteral("asn"), 65000);
        router.clusterId = entry.value(QStringLiteral("cluster_id")).toString(router.routerId).trimmed();
        router.originatedPrefixes = readStringArray(entry.value(QStringLiteral("originated_prefixes")));
        const auto position = entry.value(QStringLiteral("position")).toObject();
        router.position = QPointF(position.value(QStringLiteral("x")).toDouble(140.0 + (index % 5) * 150.0),
                                  position.value(QStringLiteral("y")).toDouble(120.0 + (index / 5) * 120.0));
        const auto pluginValue = entry.value(QStringLiteral("plugin"));
        if (!pluginValue.isUndefined() && !pluginValue.isNull() && !pluginValue.isString() && !pluginValue.isObject())
        {
            if (error)
            {
                *error = QStringLiteral("路由器 %1 的 plugin 必须是字符串或对象").arg(router.id);
            }
            return std::nullopt;
        }
        const auto pluginObject = pluginValue.toObject();
        router.pluginId =
            (pluginValue.isString() ? pluginValue.toString()
                                    : pluginObject.value(QStringLiteral("id"))
                                          .toString(entry.value(QStringLiteral("plugin_id")).toString(StandardRouterPluginId)))
                .trimmed();
        const auto settingsValue = pluginObject.value(QStringLiteral("settings"));
        if (!settingsValue.isUndefined() && !settingsValue.isObject())
        {
            if (error)
            {
                *error = QStringLiteral("路由器 %1 的 plugin.settings 必须是对象").arg(router.id);
            }
            return std::nullopt;
        }
        if (settingsValue.isObject())
        {
            router.pluginSettings = settingsValue.toObject();
        }
        else if (entry.value(QStringLiteral("plugin_settings")).isObject())
        {
            router.pluginSettings = entry.value(QStringLiteral("plugin_settings")).toObject();
        }
        topology.routers.insert(router.id, router);
        ++index;
    }

    const auto linkValues = object.value(QStringLiteral("links")).toArray();
    QSet<QString> linksWithExplicitRelationship;
    for (const auto& value : linkValues)
    {
        if (!value.isObject())
        {
            if (error)
            {
                *error = QStringLiteral("links 数组包含非对象成员");
            }
            return std::nullopt;
        }
        const auto entry = value.toObject();
        LinkConfig link;
        link.a = entry.value(QStringLiteral("a")).toString().trimmed();
        link.b = entry.value(QStringLiteral("b")).toString().trimmed();
        link.enabled = entry.value(QStringLiteral("enabled")).toBool(true);
        link.delayMs = jsonNonNegativeInt(entry, QStringLiteral("delay_ms"), 0);
        link.rrClientFromA = entry.value(QStringLiteral("rr_client_from_a")).toBool(false);
        link.rrClientFromB = entry.value(QStringLiteral("rr_client_from_b")).toBool(false);
        link.mraiMsFromA = jsonNonNegativeInt(entry, QStringLiteral("mrai_ms_from_a"), 0);
        link.mraiMsFromB = jsonNonNegativeInt(entry, QStringLiteral("mrai_ms_from_b"), 0);
        const auto relationshipValue = entry.value(QStringLiteral("relationship"));
        if (!relationshipValue.isUndefined() && !relationshipValue.isNull())
        {
            if (!relationshipValue.isString())
            {
                if (error)
                {
                    *error = QStringLiteral("链路 %1 - %2 的 relationship 必须是字符串").arg(link.a, link.b);
                }
                return std::nullopt;
            }
            const auto relationship = linkBusinessRelationshipFromString(relationshipValue.toString().trimmed());
            if (!relationship)
            {
                if (error)
                {
                    *error = QStringLiteral("链路 %1 - %2 的 relationship 无效：%3")
                                 .arg(link.a, link.b, relationshipValue.toString());
                }
                return std::nullopt;
            }
            link.businessRelationship = *relationship;
            linksWithExplicitRelationship.insert(edgeKey(link.a, link.b));
        }
        topology.links.append(link);
    }

    // Old topology files kept directional MRAI/RR values in neighbor entries;
    // relationship is accepted there as a compatibility format as well.
    // Also synthesize a link when a valid neighbor pair has no links entry.
    QMap<QString, LinkBusinessRelationship> legacyRelationships;
    for (const auto& routerObject : originalRouters)
    {
        const auto routerId = routerObject.value(QStringLiteral("id")).toString();
        for (const auto& neighborValue : routerObject.value(QStringLiteral("neighbors")).toArray())
        {
            const auto neighborObject = neighborValue.toObject();
            const auto peerId = neighborObject.value(QStringLiteral("id")).toString().trimmed();
            if (!topology.routers.contains(routerId) || !topology.routers.contains(peerId) || routerId == peerId)
            {
                continue;
            }
            auto* link = topology.findLink(routerId, peerId);
            if (!link)
            {
                LinkConfig generated;
                generated.a = routerId;
                generated.b = peerId;
                generated.enabled = neighborObject.value(QStringLiteral("enabled")).toBool(true);
                topology.links.append(generated);
                link = &topology.links.last();
            }
            const auto rr = neighborObject.value(QStringLiteral("rr_client")).toBool(false);
            const auto mrai = jsonNonNegativeInt(neighborObject, QStringLiteral("mrai_ms"), 0);
            const auto enabled = neighborObject.value(QStringLiteral("enabled")).toBool(true);
            const auto relationshipValue = neighborObject.value(QStringLiteral("relationship"));
            if (!relationshipValue.isUndefined() && !relationshipValue.isNull())
            {
                if (!relationshipValue.isString())
                {
                    if (error)
                    {
                        *error = QStringLiteral("邻居 %1 → %2 的 relationship 必须是字符串").arg(routerId, peerId);
                    }
                    return std::nullopt;
                }
                const auto relationship = neighborRelationshipFromString(relationshipValue.toString().trimmed());
                if (!relationship)
                {
                    if (error)
                    {
                        *error = QStringLiteral("邻居 %1 → %2 的 relationship 无效：%3")
                                     .arg(routerId, peerId, relationshipValue.toString());
                    }
                    return std::nullopt;
                }
                const auto edge = edgeKey(routerId, peerId);
                const auto linkRelationship = linkRelationshipForNeighbor(*relationship, link->a == routerId);
                if (linksWithExplicitRelationship.contains(edge))
                {
                    if (link->businessRelationship != linkRelationship)
                    {
                        if (error)
                        {
                            *error = QStringLiteral("链路 %1 - %2 与邻居条目的 relationship 不一致").arg(link->a, link->b);
                        }
                        return std::nullopt;
                    }
                }
                else
                {
                    const auto previous = legacyRelationships.constFind(edge);
                    if (previous != legacyRelationships.cend() && previous.value() != linkRelationship)
                    {
                        if (error)
                        {
                            *error = QStringLiteral("链路 %1 - %2 的双向邻居 relationship 不一致").arg(link->a, link->b);
                        }
                        return std::nullopt;
                    }
                    legacyRelationships.insert(edge, linkRelationship);
                    link->businessRelationship = linkRelationship;
                }
            }
            link->enabled = link->enabled && enabled;
            if (link->a == routerId)
            {
                link->rrClientFromA = rr;
                link->mraiMsFromA = mrai;
            }
            else
            {
                link->rrClientFromB = rr;
                link->mraiMsFromB = mrai;
            }
        }
    }

    const auto problems = topology.validate();
    if (!problems.isEmpty())
    {
        if (error)
        {
            *error = problems.join(u'\n');
        }
        return std::nullopt;
    }
    return topology;
}

std::optional<Topology> Topology::load(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
        {
            *error = QStringLiteral("无法读取 %1：%2").arg(path, file.errorString());
        }
        return std::nullopt;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (document.isNull() || !document.isObject())
    {
        if (error)
        {
            *error = QStringLiteral("JSON 解析失败（偏移 %1）：%2").arg(parseError.offset).arg(parseError.errorString());
        }
        return std::nullopt;
    }
    return fromJson(document.object(), error);
}

bool Topology::save(const QString& path, QString* error) const
{
    const auto problems = validate();
    if (!problems.isEmpty())
    {
        if (error)
        {
            *error = problems.join(u'\n');
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        if (error)
        {
            *error = QStringLiteral("无法写入 %1：%2").arg(path, file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit())
    {
        if (error)
        {
            *error = QStringLiteral("保存 %1 失败：%2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

QStringList Topology::validate() const
{
    QStringList problems;
    if (simulation.name.trimmed().isEmpty())
    {
        problems.append(QStringLiteral("仿真名称不能为空"));
    }
    if (simulation.logDirectory.trimmed().isEmpty())
    {
        problems.append(QStringLiteral("日志目录不能为空"));
    }
    if (simulation.convergenceQuietMs < 0)
    {
        problems.append(QStringLiteral("收敛静默时间不能为负数"));
    }
    if (routers.isEmpty())
    {
        problems.append(QStringLiteral("拓扑至少需要一台路由器"));
    }

    QSet<QString> bgpRouterIds;
    for (auto it = routers.cbegin(); it != routers.cend(); ++it)
    {
        const auto& router = it.value();
        if (router.id.trimmed().isEmpty())
        {
            problems.append(QStringLiteral("路由器 ID 不能为空"));
        }
        if (router.id != it.key())
        {
            problems.append(QStringLiteral("路由器映射键与 ID 不一致：%1 / %2").arg(it.key(), router.id));
        }
        if (!isIpv4(router.routerId, false))
        {
            problems.append(QStringLiteral("%1 的 BGP Router ID 无效：%2").arg(router.id, router.routerId));
        }
        else if (bgpRouterIds.contains(router.routerId))
        {
            problems.append(QStringLiteral("BGP Router ID 重复：%1").arg(router.routerId));
        }
        bgpRouterIds.insert(router.routerId);
        if (router.asn == 0)
        {
            problems.append(QStringLiteral("%1 的 ASN 必须大于 0").arg(router.id));
        }
        if (router.pluginId.trimmed().isEmpty())
        {
            problems.append(QStringLiteral("%1 的路由器插件 ID 不能为空").arg(router.id));
        }
        if (!router.clusterId.isEmpty() && !isIpv4(router.clusterId))
        {
            problems.append(QStringLiteral("%1 的 Cluster ID 无效：%2").arg(router.id, router.clusterId));
        }
        for (const auto& prefix : router.originatedPrefixes)
        {
            if (!isIpv4Cidr(prefix))
            {
                problems.append(QStringLiteral("%1 的前缀无效：%2").arg(router.id, prefix));
            }
        }
    }

    QSet<QString> edges;
    for (const auto& link : links)
    {
        if (link.a == link.b)
        {
            problems.append(QStringLiteral("链路不能连接路由器自身：%1").arg(link.a));
        }
        if (!routers.contains(link.a) || !routers.contains(link.b))
        {
            problems.append(QStringLiteral("链路端点不存在：%1 - %2").arg(link.a, link.b));
        }
        else if (routers.value(link.a).asn == routers.value(link.b).asn &&
                 link.businessRelationship != LinkBusinessRelationship::Unspecified)
        {
            problems.append(QStringLiteral("同一 AS 内的链路不能设置商业关系：%1 - %2").arg(link.a, link.b));
        }
        const auto key = edgeKey(link.a, link.b);
        if (edges.contains(key))
        {
            problems.append(QStringLiteral("链路重复：%1 - %2").arg(link.a, link.b));
        }
        edges.insert(key);
        if (link.delayMs < 0 || link.mraiMsFromA < 0 || link.mraiMsFromB < 0)
        {
            problems.append(QStringLiteral("链路 %1 - %2 的延迟/MRAI 不能为负数").arg(link.a, link.b));
        }
    }
    return problems;
}

QVector<NeighborConfig> Topology::neighborsFor(const QString& routerId) const
{
    QVector<NeighborConfig> result;
    const auto localIt = routers.constFind(routerId);
    if (localIt == routers.cend())
    {
        return result;
    }
    for (const auto& link : links)
    {
        QString peerId;
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
        const auto peerIt = routers.constFind(peerId);
        if (peerIt == routers.cend())
        {
            continue;
        }
        result.append(NeighborConfig{
            .id = peerId,
            .remoteAsn = peerIt->asn,
            .sessionType = localIt->asn == peerIt->asn ? SessionType::Ibgp : SessionType::Ebgp,
            .rrClient = fromA ? link.rrClientFromA : link.rrClientFromB,
            .enabled = link.enabled,
            .mraiMs = fromA ? link.mraiMsFromA : link.mraiMsFromB,
            .relationship = neighborRelationshipFor(link, fromA),
        });
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) { return lhs.id < rhs.id; });
    return result;
}

const LinkConfig* Topology::findLink(const QString& a, const QString& b) const
{
    const auto key = edgeKey(a, b);
    const auto it = std::find_if(links.cbegin(), links.cend(), [&](const auto& l) { return edgeKey(l.a, l.b) == key; });
    return it == links.cend() ? nullptr : &*it;
}

LinkConfig* Topology::findLink(const QString& a, const QString& b)
{
    const auto key = edgeKey(a, b);
    const auto it = std::find_if(links.begin(), links.end(), [&](const auto& l) { return edgeKey(l.a, l.b) == key; });
    return it == links.end() ? nullptr : &*it;
}

QString Topology::nextRouterName() const
{
    for (int index = 1; index < std::numeric_limits<int>::max(); ++index)
    {
        const auto candidate = QStringLiteral("R%1").arg(index);
        if (!routers.contains(candidate))
        {
            return candidate;
        }
    }
    return QStringLiteral("Router");
}

QString Topology::nextBgpRouterId() const
{
    QSet<QString> used;
    for (const auto& router : routers)
    {
        used.insert(router.routerId);
    }
    for (int index = 1; index <= 256 * 256 * 254; ++index)
    {
        const auto candidate = routerIdFromIndex(index);
        if (!used.contains(candidate))
        {
            return candidate;
        }
    }
    return QStringLiteral("10.255.255.254");
}

QString Topology::routerIdFromIndex(int oneBasedIndex)
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
    return QStringLiteral("10.%1.%2.%3").arg(second).arg(third).arg(fourth);
}

QString Topology::edgeKey(const QString& a, const QString& b)
{
    return a < b ? a + u'\x1f' + b : b + u'\x1f' + a;
}

} // namespace bgptester
