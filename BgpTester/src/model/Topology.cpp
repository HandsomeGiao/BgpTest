#include "model/Topology.hpp"
#include "model/StrictIpv4.hpp"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>

namespace bgptester
{
namespace
{

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

Topology Topology::starter()
{
    Topology topology;
    topology.simulation.name = QStringLiteral("quick-lab");
    RouterConfig r1{.id = QStringLiteral("R1"),
                    .routerId = QStringLiteral("10.0.0.1"),
                    .asn = 65001,
                    .clusterId = QStringLiteral("10.0.0.1"),
                    .originatedPrefixes = {QStringLiteral("10.1.0.0/24")},
                    .position = QPointF(180, 220),
                    .pluginId = StandardRouterPluginId,
                    .pluginSettings = {}};
    RouterConfig r2{.id = QStringLiteral("R2"),
                    .routerId = QStringLiteral("10.0.0.2"),
                    .asn = 65002,
                    .clusterId = QStringLiteral("10.0.0.2"),
                    .originatedPrefixes = {QStringLiteral("10.2.0.0/24")},
                    .position = QPointF(450, 220),
                    .pluginId = StandardRouterPluginId,
                    .pluginSettings = {}};
    topology.routers.insert(r1.id, r1);
    topology.routers.insert(r2.id, r2);
    topology.links.append(LinkConfig{.a = r1.id, .b = r2.id, .delayMs = 10});
    return topology;
}

QJsonObject Topology::toJson() const
{
    QJsonObject simulationObject{
        {QStringLiteral("name"), simulation.name},
        {QStringLiteral("log_dir"), simulation.logDirectory},
        {QStringLiteral("worker_threads"), simulation.workerThreads},
        {QStringLiteral("convergence_quiet_ms"), simulation.convergenceQuietMs},
        {QStringLiteral("withdrawal_ignores_mrai"), simulation.withdrawalIgnoresMrai},
        {QStringLiteral("router_class"), QStringLiteral("BgpRouter")},
    };

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

namespace
{

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
    QString routerId;
    QJsonObject object;
};

QJsonValue detachedJsonValue(const QJsonValue& value);

QJsonObject detachedJsonObject(const QJsonObject& object)
{
    QJsonObject result;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
    {
        result.insert(it.key(), detachedJsonValue(it.value()));
    }
    return result;
}

QJsonArray detachedJsonArray(const QJsonArray& array)
{
    QJsonArray result;
    for (const auto& value : array)
    {
        result.append(detachedJsonValue(value));
    }
    return result;
}

QJsonValue detachedJsonValue(const QJsonValue& value)
{
    switch (value.type())
    {
        case QJsonValue::Null:
            return QJsonValue(QJsonValue::Null);
        case QJsonValue::Bool:
            return QJsonValue(value.toBool());
        case QJsonValue::Double:
            return QJsonValue(value.toDouble());
        case QJsonValue::String:
            return QJsonValue(value.toString());
        case QJsonValue::Array:
            return QJsonValue(detachedJsonArray(value.toArray()));
        case QJsonValue::Object:
            return QJsonValue(detachedJsonObject(value.toObject()));
        case QJsonValue::Undefined:
            return QJsonValue(QJsonValue::Undefined);
    }
    return QJsonValue(QJsonValue::Undefined);
}

class TopologyJsonBuilder
{
public:
    void reserve(qsizetype linkCount)
    {
        if (linkCount > 0)
        {
            topology_.links.reserve(linkCount);
            explicitLinkFieldsByIndex_.reserve(linkCount);
        }
    }

    void setSimulation(const QJsonObject& object)
    {
        topology_.simulation.name = object.value(QStringLiteral("name")).toString(QStringLiteral("bgp-lab"));
        topology_.simulation.logDirectory = object.value(QStringLiteral("log_dir")).toString(QStringLiteral("tmp"));
        topology_.simulation.workerThreads = jsonNonNegativeInt(object, QStringLiteral("worker_threads"), 0);
        topology_.simulation.convergenceQuietMs = jsonNonNegativeInt(object, QStringLiteral("convergence_quiet_ms"), 1000);
        topology_.simulation.withdrawalIgnoresMrai = object.value(QStringLiteral("withdrawal_ignores_mrai")).toBool(true);
    }

    bool addRouter(const QJsonObject& entry, QString* error)
    {
        const auto index = topology_.routers.size();
        if (index >= std::numeric_limits<int>::max())
        {
            setError(error, QStringLiteral("路由器数量超过支持上限"));
            return false;
        }
        const auto oneBasedIndex = static_cast<int>(index) + 1;
        RouterConfig router;
        router.id = entry.value(QStringLiteral("id")).toString(QStringLiteral("R%1").arg(oneBasedIndex)).trimmed();
        if (topology_.routers.contains(router.id))
        {
            setError(error, QStringLiteral("路由器 ID 重复：%1").arg(router.id));
            return false;
        }
        router.routerId = entry.value(QStringLiteral("router_id")).toString(Topology::routerIdFromIndex(oneBasedIndex)).trimmed();
        router.asn = jsonUint(entry, QStringLiteral("asn"), 65000);
        router.clusterId = entry.value(QStringLiteral("cluster_id")).toString(router.routerId).trimmed();
        router.originatedPrefixes = readStringArray(entry.value(QStringLiteral("originated_prefixes")));
        const auto position = entry.value(QStringLiteral("position")).toObject();
        router.position = QPointF(position.value(QStringLiteral("x")).toDouble(140.0 + (index % 5) * 150.0),
                                  position.value(QStringLiteral("y")).toDouble(120.0 + (index / 5) * 120.0));

        const auto pluginValue = entry.value(QStringLiteral("plugin"));
        if (!pluginValue.isUndefined() && !pluginValue.isNull() && !pluginValue.isString() && !pluginValue.isObject())
        {
            setError(error, QStringLiteral("路由器 %1 的 plugin 必须是字符串或对象").arg(router.id));
            return false;
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
            setError(error, QStringLiteral("路由器 %1 的 plugin.settings 必须是对象").arg(router.id));
            return false;
        }
        if (settingsValue.isObject())
        {
            router.pluginSettings = detachedJsonObject(settingsValue.toObject());
        }
        else if (entry.value(QStringLiteral("plugin_settings")).isObject())
        {
            router.pluginSettings = detachedJsonObject(entry.value(QStringLiteral("plugin_settings")).toObject());
        }

        for (const auto& neighbor : entry.value(QStringLiteral("neighbors")).toArray())
        {
            legacyNeighbors_.append(LegacyNeighborEntry{router.id, detachedJsonObject(neighbor.toObject())});
        }
        const auto routerId = router.id;
        topology_.routers.insert(routerId, std::move(router));
        return true;
    }

    bool addLink(const QJsonObject& entry, QString* error)
    {
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
                setError(error, QStringLiteral("链路 %1 - %2 的 relationship 必须是字符串").arg(link.a, link.b));
                return false;
            }
            const auto relationship = linkBusinessRelationshipFromString(relationshipValue.toString().trimmed());
            if (!relationship)
            {
                setError(error, QStringLiteral("链路 %1 - %2 的 relationship 无效：%3")
                                    .arg(link.a, link.b, relationshipValue.toString()));
                return false;
            }
            link.businessRelationship = *relationship;
        }
        explicitLinkFieldsByIndex_.append(ExplicitLinkFields{
            .enabled = entry.value(QStringLiteral("enabled")).isBool(),
            .rrClientFromA = entry.value(QStringLiteral("rr_client_from_a")).isBool(),
            .rrClientFromB = entry.value(QStringLiteral("rr_client_from_b")).isBool(),
            .mraiMsFromA = entry.value(QStringLiteral("mrai_ms_from_a")).isDouble(),
            .mraiMsFromB = entry.value(QStringLiteral("mrai_ms_from_b")).isDouble(),
            .relationship = !relationshipValue.isUndefined() && !relationshipValue.isNull(),
        });
        topology_.links.append(std::move(link));
        return true;
    }

    qsizetype routerCount() const
    {
        return topology_.routers.size();
    }

    qsizetype linkCount() const
    {
        return topology_.links.size();
    }

    std::optional<Topology> finish(QString* error)
    {
        // Legacy files kept session values in neighbor entries. Compactly
        // retaining only those entries avoids keeping every router JSON object
        // alive while a large topology is parsed.
        QHash<QString, LinkBusinessRelationship> legacyRelationships;
        legacyRelationships.reserve(legacyNeighbors_.size());
        if (!legacyNeighbors_.isEmpty())
        {
            linkIndexes_.reserve(topology_.links.size());
            explicitLinkFields_.reserve(topology_.links.size());
            for (qsizetype index = 0; index < topology_.links.size(); ++index)
            {
                const auto& link = topology_.links[index];
                const auto edge = Topology::edgeKey(link.a, link.b);
                if (!linkIndexes_.contains(edge))
                {
                    linkIndexes_.insert(edge, index);
                    explicitLinkFields_.insert(edge, explicitLinkFieldsByIndex_.value(index));
                }
            }
        }
        for (const auto& legacy : std::as_const(legacyNeighbors_))
        {
            const auto& routerId = legacy.routerId;
            const auto& neighborObject = legacy.object;
            const auto peerId = neighborObject.value(QStringLiteral("id")).toString().trimmed();
            if (!topology_.routers.contains(routerId) || !topology_.routers.contains(peerId) || routerId == peerId)
            {
                continue;
            }
            const auto edge = Topology::edgeKey(routerId, peerId);
            auto linkIndex = linkIndexes_.constFind(edge);
            if (linkIndex == linkIndexes_.cend())
            {
                LinkConfig generated;
                generated.a = routerId;
                generated.b = peerId;
                linkIndexes_.insert(edge, topology_.links.size());
                explicitLinkFields_.insert(edge, ExplicitLinkFields{});
                topology_.links.append(std::move(generated));
                linkIndex = linkIndexes_.constFind(edge);
            }
            auto& link = topology_.links[linkIndex.value()];
            const auto fields = explicitLinkFields_.value(edge);
            const auto rr = neighborObject.value(QStringLiteral("rr_client")).toBool(false);
            const auto mrai = jsonNonNegativeInt(neighborObject, QStringLiteral("mrai_ms"), 0);
            const auto enabled = neighborObject.value(QStringLiteral("enabled")).toBool(true);
            const auto relationshipValue = neighborObject.value(QStringLiteral("relationship"));
            if (!fields.relationship && !relationshipValue.isUndefined() && !relationshipValue.isNull())
            {
                if (!relationshipValue.isString())
                {
                    setError(error, QStringLiteral("邻居 %1 → %2 的 relationship 必须是字符串").arg(routerId, peerId));
                    return std::nullopt;
                }
                const auto relationship = neighborRelationshipFromString(relationshipValue.toString().trimmed());
                if (!relationship)
                {
                    setError(error, QStringLiteral("邻居 %1 → %2 的 relationship 无效：%3")
                                        .arg(routerId, peerId, relationshipValue.toString()));
                    return std::nullopt;
                }
                const auto linkRelationship = linkRelationshipForNeighbor(*relationship, link.a == routerId);
                const auto previous = legacyRelationships.constFind(edge);
                if (previous != legacyRelationships.cend() && previous.value() != linkRelationship)
                {
                    setError(error, QStringLiteral("链路 %1 - %2 的双向邻居 relationship 不一致").arg(link.a, link.b));
                    return std::nullopt;
                }
                legacyRelationships.insert(edge, linkRelationship);
                link.businessRelationship = linkRelationship;
            }
            if (!fields.enabled)
            {
                link.enabled = link.enabled && enabled;
            }
            if (link.a == routerId)
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
        if (!problems.isEmpty())
        {
            setError(error, problems.join(u'\n'));
            return std::nullopt;
        }
        return std::move(topology_);
    }

private:
    static void setError(QString* error, QString message)
    {
        if (error)
        {
            *error = std::move(message);
        }
    }

    Topology topology_;
    QHash<QString, qsizetype> linkIndexes_;
    QHash<QString, ExplicitLinkFields> explicitLinkFields_;
    QVector<ExplicitLinkFields> explicitLinkFieldsByIndex_;
    QVector<LegacyNeighborEntry> legacyNeighbors_;
};

class MappedTopologyJsonReader
{
public:
    MappedTopologyJsonReader(const char* data, qsizetype size, QString* error, TopologyLoadProgressCallback progress)
        : data_(data), size_(size), error_(error), progress_(std::move(progress))
    {
    }

    std::optional<Topology> parse()
    {
        if (size_ >= 3 && static_cast<unsigned char>(data_[0]) == 0xef && static_cast<unsigned char>(data_[1]) == 0xbb &&
            static_cast<unsigned char>(data_[2]) == 0xbf)
        {
            position_ = 3;
        }
        skipWhitespace();
        if (!consume('{', QStringLiteral("拓扑 JSON 顶层必须是对象")))
        {
            return std::nullopt;
        }

        bool first = true;
        while (true)
        {
            skipWhitespace();
            if (position_ < size_ && data_[position_] == '}')
            {
                ++position_;
                break;
            }
            if (!first && !consume(',', QStringLiteral("JSON 对象成员之间缺少逗号")))
            {
                return std::nullopt;
            }
            first = false;
            skipWhitespace();

            QString key;
            if (!readString(&key) || !consume(':', QStringLiteral("JSON 对象键后缺少冒号")))
            {
                return std::nullopt;
            }
            skipWhitespace();
            const auto valueStart = position_;
            qsizetype valueEnd = valueStart;
            if (!scanValueEnd(size_, &valueEnd))
            {
                return std::nullopt;
            }

            if (key == QStringLiteral("simulation"))
            {
                const auto object = parseObject(valueStart, valueEnd, QStringLiteral("simulation"));
                if (!object)
                {
                    return std::nullopt;
                }
                builder_.setSimulation(*object);
            }
            else if (key == QStringLiteral("routers") || key == QStringLiteral("links"))
            {
                if (!parseObjectArray(valueStart, valueEnd, key == QStringLiteral("routers")))
                {
                    return std::nullopt;
                }
            }
            position_ = valueEnd;
        }

        skipWhitespace();
        if (position_ != size_)
        {
            fail(QStringLiteral("顶层 JSON 对象后存在多余内容"), position_);
            return std::nullopt;
        }
        if (!report(TopologyLoadStage::Validating, true))
        {
            return std::nullopt;
        }
        return builder_.finish(error_);
    }

private:
    void skipWhitespace()
    {
        while (position_ < size_ &&
               (data_[position_] == ' ' || data_[position_] == '\t' || data_[position_] == '\r' || data_[position_] == '\n'))
        {
            ++position_;
        }
    }

    bool consume(char expected, const QString& message)
    {
        skipWhitespace();
        if (position_ >= size_ || data_[position_] != expected)
        {
            return fail(message, position_);
        }
        ++position_;
        return true;
    }

    bool readString(QString* value)
    {
        skipWhitespace();
        if (position_ >= size_ || data_[position_] != '"')
        {
            return fail(QStringLiteral("JSON 对象键必须是字符串"), position_);
        }
        const auto start = position_;
        qsizetype end = start;
        if (!scanStringEnd(size_, &end))
        {
            return false;
        }

        QByteArray wrapped;
        wrapped.reserve(end - start + 2);
        wrapped.append('[');
        wrapped.append(data_ + start, end - start);
        wrapped.append(']');
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(wrapped, &parseError);
        if (document.isNull() || !document.isArray() || document.array().size() != 1 || !document.array().at(0).isString())
        {
            return fail(QStringLiteral("JSON 对象键无效：%1").arg(parseError.errorString()), start + parseError.offset);
        }
        *value = document.array().at(0).toString();
        position_ = end;
        return true;
    }

    bool scanStringEnd(qsizetype limit, qsizetype* end)
    {
        auto cursor = position_;
        if (cursor >= limit || data_[cursor] != '"')
        {
            return fail(QStringLiteral("此处应为 JSON 字符串"), cursor);
        }
        ++cursor;
        bool escaped = false;
        while (cursor < limit)
        {
            const auto character = data_[cursor++];
            if (escaped)
            {
                escaped = false;
            }
            else if (character == '\\')
            {
                escaped = true;
            }
            else if (character == '"')
            {
                *end = cursor;
                return true;
            }
        }
        return fail(QStringLiteral("JSON 字符串未闭合"), position_);
    }

    bool scanValueEnd(qsizetype limit, qsizetype* end)
    {
        skipWhitespace();
        if (position_ >= limit)
        {
            return fail(QStringLiteral("JSON 值缺失"), position_);
        }
        auto cursor = position_;
        const auto first = data_[cursor];
        if (first == '"')
        {
            return scanStringEnd(limit, end);
        }
        if (first != '{' && first != '[')
        {
            while (cursor < limit && data_[cursor] != ',' && data_[cursor] != '}' && data_[cursor] != ']')
            {
                ++cursor;
            }
            auto trimmedEnd = cursor;
            while (trimmedEnd > position_ && (data_[trimmedEnd - 1] == ' ' || data_[trimmedEnd - 1] == '\t' ||
                                              data_[trimmedEnd - 1] == '\r' || data_[trimmedEnd - 1] == '\n'))
            {
                --trimmedEnd;
            }
            if (trimmedEnd == position_)
            {
                return fail(QStringLiteral("JSON 值缺失"), position_);
            }
            *end = trimmedEnd;
            return true;
        }

        QVector<char> closers;
        closers.reserve(16);
        closers.append(first == '{' ? '}' : ']');
        ++cursor;
        while (cursor < limit)
        {
            const auto character = data_[cursor];
            if (character == '"')
            {
                const auto savedPosition = position_;
                position_ = cursor;
                qsizetype stringEnd = cursor;
                const auto ok = scanStringEnd(limit, &stringEnd);
                position_ = savedPosition;
                if (!ok)
                {
                    return false;
                }
                cursor = stringEnd;
                continue;
            }
            if (character == '{' || character == '[')
            {
                closers.append(character == '{' ? '}' : ']');
            }
            else if (character == '}' || character == ']')
            {
                if (closers.isEmpty() || closers.back() != character)
                {
                    return fail(QStringLiteral("JSON 括号不匹配"), cursor);
                }
                closers.removeLast();
                if (closers.isEmpty())
                {
                    *end = cursor + 1;
                    return true;
                }
            }
            ++cursor;
        }
        return fail(QStringLiteral("JSON 对象或数组未闭合"), position_);
    }

    std::optional<QJsonObject> parseObject(qsizetype start, qsizetype end, const QString& context)
    {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(QByteArray(data_ + start, end - start), &parseError);
        if (document.isNull() || !document.isObject())
        {
            fail(QStringLiteral("%1 解析失败：%2").arg(context, parseError.errorString()), start + parseError.offset);
            return std::nullopt;
        }
        return document.object();
    }

    bool parseObjectArray(qsizetype start, qsizetype end, bool routers)
    {
        position_ = start;
        const auto name = routers ? QStringLiteral("routers") : QStringLiteral("links");
        if (!consume('[', QStringLiteral("%1 必须是数组").arg(name)))
        {
            return false;
        }
        if (!routers)
        {
            builder_.reserve(std::clamp<qsizetype>((end - start) / 200, 0, 1'000'000));
        }
        bool first = true;
        qsizetype batchStart = -1;
        qsizetype batchEnd = -1;
        qsizetype batchCount = 0;
        while (true)
        {
            skipWhitespace();
            if (position_ < end && data_[position_] == ']')
            {
                if (!parseObjectBatch(batchStart, batchEnd, batchCount, name, routers))
                {
                    return false;
                }
                ++position_;
                return position_ == end || fail(QStringLiteral("%1 数组后存在多余内容").arg(name), position_);
            }
            if (!first && !consume(',', QStringLiteral("%1 数组成员之间缺少逗号").arg(name)))
            {
                return false;
            }
            first = false;
            skipWhitespace();
            const auto objectStart = position_;
            qsizetype objectEnd = objectStart;
            if (!scanValueEnd(end, &objectEnd))
            {
                return false;
            }
            if (batchCount == 0)
            {
                batchStart = objectStart;
            }
            batchEnd = objectEnd;
            ++batchCount;
            position_ = objectEnd;
            constexpr qsizetype maximumBatchEntries = 256;
            constexpr qsizetype maximumBatchBytes = 1024 * 1024;
            if ((batchCount >= maximumBatchEntries || batchEnd - batchStart >= maximumBatchBytes) &&
                !parseObjectBatch(batchStart, batchEnd, batchCount, name, routers))
            {
                return false;
            }
            if (batchCount >= maximumBatchEntries || batchEnd - batchStart >= maximumBatchBytes)
            {
                batchStart = -1;
                batchEnd = -1;
                batchCount = 0;
            }
        }
    }

    bool parseObjectBatch(qsizetype start, qsizetype end, qsizetype count, const QString& name, bool routers)
    {
        if (count == 0)
        {
            return true;
        }
        QByteArray wrapped;
        // Parse a bounded slice instead of materializing the complete root
        // document. Batching keeps QJsonDocument efficient while capping its
        // transient DOM to at most 256 entries / roughly one MiB.
        wrapped.reserve(end - start + 2);
        wrapped.append('[');
        wrapped.append(data_ + start, end - start);
        wrapped.append(']');
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(wrapped, &parseError);
        if (document.isNull() || !document.isArray() || document.array().size() != count)
        {
            const auto relativeOffset = std::max<qsizetype>(0, parseError.offset - 1);
            return fail(QStringLiteral("%1 数组成员解析失败：%2").arg(name, parseError.errorString()), start + relativeOffset);
        }
        for (const auto& value : document.array())
        {
            if (!value.isObject())
            {
                return fail(QStringLiteral("%1 数组包含非对象成员").arg(name), start);
            }
            if (routers ? !builder_.addRouter(value.toObject(), error_) : !builder_.addLink(value.toObject(), error_))
            {
                return false;
            }
        }
        return report(routers ? TopologyLoadStage::ReadingRouters : TopologyLoadStage::ReadingLinks, false);
    }

    bool report(TopologyLoadStage stage, bool force)
    {
        if (!progress_)
        {
            return true;
        }
        constexpr qsizetype reportInterval = 1024 * 1024;
        if (!force && position_ - lastReportedPosition_ < reportInterval &&
            ((builder_.routerCount() + builder_.linkCount()) % 512) != 0)
        {
            return true;
        }
        lastReportedPosition_ = position_;
        const TopologyLoadProgress value{
            .stage = stage,
            .bytesProcessed = position_,
            .totalBytes = size_,
            .routersLoaded = builder_.routerCount(),
            .linksLoaded = builder_.linkCount(),
        };
        if (!progress_(value))
        {
            return fail(QStringLiteral("拓扑加载已取消"), position_);
        }
        return true;
    }

    bool fail(const QString& message, qsizetype offset)
    {
        if (error_)
        {
            *error_ = QStringLiteral("%1（偏移 %2）").arg(message).arg(offset);
        }
        return false;
    }

    const char* data_ = nullptr;
    qsizetype size_ = 0;
    qsizetype position_ = 0;
    qsizetype lastReportedPosition_ = 0;
    QString* error_ = nullptr;
    TopologyLoadProgressCallback progress_;
    TopologyJsonBuilder builder_;
};

} // namespace

std::optional<Topology> Topology::fromJson(const QJsonObject& object, QString* error)
{
    if (error)
    {
        error->clear();
    }
    TopologyJsonBuilder builder;
    const auto routerValues = object.value(QStringLiteral("routers")).toArray();
    const auto linkValues = object.value(QStringLiteral("links")).toArray();
    builder.reserve(linkValues.size());
    builder.setSimulation(object.value(QStringLiteral("simulation")).toObject());
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
        if (!builder.addRouter(value.toObject(), error))
        {
            return std::nullopt;
        }
    }
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
        if (!builder.addLink(value.toObject(), error))
        {
            return std::nullopt;
        }
    }
    return builder.finish(error);
}

std::optional<Topology> Topology::load(const QString& path, QString* error, const TopologyLoadProgressCallback& progress)
{
    if (error)
    {
        error->clear();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
        {
            *error = QStringLiteral("无法读取 %1：%2").arg(path, file.errorString());
        }
        return std::nullopt;
    }
    const auto fileSize = file.size();
    if (fileSize <= 0 || static_cast<quint64>(fileSize) > static_cast<quint64>(std::numeric_limits<qsizetype>::max()))
    {
        if (error)
        {
            *error = fileSize == 0 ? QStringLiteral("拓扑文件为空") : QStringLiteral("拓扑文件过大，无法映射到当前进程地址空间");
        }
        return std::nullopt;
    }

    auto* mapped = file.map(0, fileSize);
    QByteArray fallback;
    const char* data = nullptr;
    if (mapped)
    {
        data = reinterpret_cast<const char*>(mapped);
    }
    else
    {
        file.seek(0);
        fallback = file.readAll();
        if (fallback.size() != fileSize)
        {
            if (error)
            {
                *error = QStringLiteral("读取 %1 失败：%2").arg(path, file.errorString());
            }
            return std::nullopt;
        }
        data = fallback.constData();
    }

    MappedTopologyJsonReader reader(data, static_cast<qsizetype>(fileSize), error, progress);
    auto topology = reader.parse();
    if (mapped)
    {
        file.unmap(mapped);
    }
    return topology;
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
    if (simulation.workerThreads < 0 || simulation.workerThreads > 256)
    {
        problems.append(QStringLiteral("后台工作线程数必须在 0 到 256 之间"));
    }
    if (routers.isEmpty())
    {
        problems.append(QStringLiteral("拓扑至少需要一台路由器"));
    }

    QSet<QString> bgpRouterIds;
    bgpRouterIds.reserve(routers.size());
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
        if (!isCanonicalIpv4Address(router.routerId, false))
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
        if (!router.clusterId.isEmpty() && !isCanonicalIpv4Address(router.clusterId))
        {
            problems.append(QStringLiteral("%1 的 Cluster ID 无效：%2").arg(router.id, router.clusterId));
        }
        for (const auto& prefix : router.originatedPrefixes)
        {
            if (!isCanonicalIpv4Prefix(prefix))
            {
                problems.append(QStringLiteral("%1 的前缀无效：%2").arg(router.id, prefix));
            }
        }
    }

    QSet<QString> edges;
    edges.reserve(links.size());
    for (const auto& link : links)
    {
        const auto a = routers.constFind(link.a);
        const auto b = routers.constFind(link.b);
        if (link.a == link.b)
        {
            problems.append(QStringLiteral("链路不能连接路由器自身：%1").arg(link.a));
        }
        if (a == routers.cend() || b == routers.cend())
        {
            problems.append(QStringLiteral("链路端点不存在：%1 - %2").arg(link.a, link.b));
        }
        else if (a->asn == b->asn && link.businessRelationship != LinkBusinessRelationship::Unspecified)
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

NeighborIndex Topology::buildNeighborIndex() const
{
    NeighborIndex result;
    result.reserve(routers.size());
    for (const auto& link : links)
    {
        const auto a = routers.constFind(link.a);
        const auto b = routers.constFind(link.b);
        if (a == routers.cend() || b == routers.cend())
        {
            continue;
        }
        const auto sessionType = a->asn == b->asn ? SessionType::Ibgp : SessionType::Ebgp;
        result[link.a].insert(link.b, NeighborConfig{
            .id = link.b,
            .remoteAsn = b->asn,
            .sessionType = sessionType,
            .rrClient = link.rrClientFromA,
            .enabled = link.enabled,
            .mraiMs = link.mraiMsFromA,
            .relationship = neighborRelationshipFor(link, true),
        });
        result[link.b].insert(link.a, NeighborConfig{
            .id = link.a,
            .remoteAsn = a->asn,
            .sessionType = sessionType,
            .rrClient = link.rrClientFromB,
            .enabled = link.enabled,
            .mraiMs = link.mraiMsFromB,
            .relationship = neighborRelationshipFor(link, false),
        });
    }
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
