#pragma once

#include <QJsonObject>
#include <QHash>
#include <QMap>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

namespace bgptester
{

inline const QString StandardRouterPluginId = QStringLiteral("org.bgptester.router.standard-bgp");

enum class SessionType
{
    Ibgp,
    Ebgp
};

QString toString(SessionType type);
std::optional<SessionType> sessionTypeFromString(const QString& value);

enum class LinkBusinessRelationship
{
    Unspecified,
    PeerToPeer,
    AProviderOfB,
    BProviderOfA
};

QString toString(LinkBusinessRelationship relationship);
std::optional<LinkBusinessRelationship> linkBusinessRelationshipFromString(const QString& value);

// The relationship of the remote neighbor to the local router.
enum class NeighborRelationship
{
    Unspecified,
    Peer,
    Provider,
    Customer
};

QString toString(NeighborRelationship relationship);
std::optional<NeighborRelationship> neighborRelationshipFromString(const QString& value);

struct SimulationSettings
{
    QString name = QStringLiteral("bgp-lab");
    QString logDirectory = QStringLiteral("tmp");
    int workerThreads = 0;
    int convergenceQuietMs = 1000;
    bool withdrawalIgnoresMrai = true;
};

struct RouterConfig
{
    QString id;
    QString routerId;
    quint32 asn = 65000;
    QString clusterId;
    QStringList originatedPrefixes;
    QPointF position{100.0, 100.0};
    QString pluginId = StandardRouterPluginId;
    QJsonObject pluginSettings;

    bool operator==(const RouterConfig&) const = default;
};

struct LinkConfig
{
    QString a;
    QString b;
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
    QString id;
    quint32 remoteAsn = 0;
    SessionType sessionType = SessionType::Ibgp;
    bool rrClient = false;
    bool enabled = true;
    int mraiMs = 0;
    NeighborRelationship relationship = NeighborRelationship::Unspecified;

    bool operator==(const NeighborConfig&) const = default;
};

using NeighborIndex = QHash<QString, QMap<QString, NeighborConfig>>;

enum class TopologyLoadStage
{
    ReadingRouters,
    ReadingLinks,
    Validating
};

struct TopologyLoadProgress
{
    TopologyLoadStage stage = TopologyLoadStage::ReadingRouters;
    qint64 bytesProcessed = 0;
    qint64 totalBytes = 0;
    qsizetype routersLoaded = 0;
    qsizetype linksLoaded = 0;
};

using TopologyLoadProgressCallback = std::function<bool(const TopologyLoadProgress&)>;

class Topology
{
public:
    SimulationSettings simulation;
    QMap<QString, RouterConfig> routers;
    QVector<LinkConfig> links;

    QJsonObject toJson() const;
    static Topology starter();
    static std::optional<Topology> fromJson(const QJsonObject& object, QString* error = nullptr);
    // The callback runs in the calling thread; return false to cancel.
    static std::optional<Topology> load(const QString& path, QString* error = nullptr,
                                        const TopologyLoadProgressCallback& progress = {});
    bool save(const QString& path, QString* error = nullptr) const;

    QStringList validate() const;
    NeighborIndex buildNeighborIndex() const;
    QVector<NeighborConfig> neighborsFor(const QString& routerId) const;
    const LinkConfig* findLink(const QString& a, const QString& b) const;
    LinkConfig* findLink(const QString& a, const QString& b);
    QString nextRouterName() const;
    QString nextBgpRouterId() const;
    static QString routerIdFromIndex(int oneBasedIndex);
    static QString edgeKey(const QString& a, const QString& b);
};

} // namespace bgptester

Q_DECLARE_METATYPE(bgptester::Topology)
Q_DECLARE_METATYPE(bgptester::RouterConfig)
Q_DECLARE_METATYPE(bgptester::LinkConfig)
Q_DECLARE_METATYPE(bgptester::NeighborConfig)
