#pragma once

#include <QJsonObject>
#include <QMap>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

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

struct SimulationSettings
{
    QString name = QStringLiteral("bgp-lab");
    QString logDirectory = QStringLiteral("tmp");
    int convergenceQuietMs = 1000;
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

    bool operator==(const NeighborConfig&) const = default;
};

class Topology
{
public:
    SimulationSettings simulation;
    QMap<QString, RouterConfig> routers;
    QVector<LinkConfig> links;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static std::optional<Topology> fromJson(const QJsonObject& object, QString* error = nullptr);
    [[nodiscard]] static std::optional<Topology> load(const QString& path, QString* error = nullptr);
    [[nodiscard]] bool save(const QString& path, QString* error = nullptr) const;

    [[nodiscard]] QStringList validate() const;
    [[nodiscard]] QVector<NeighborConfig> neighborsFor(const QString& routerId) const;
    [[nodiscard]] const LinkConfig* findLink(const QString& a, const QString& b) const;
    [[nodiscard]] LinkConfig* findLink(const QString& a, const QString& b);
    [[nodiscard]] QString nextRouterName() const;
    [[nodiscard]] QString nextBgpRouterId() const;
    [[nodiscard]] static QString routerIdFromIndex(int oneBasedIndex);
    [[nodiscard]] static QString edgeKey(const QString& a, const QString& b);
};

} // namespace bgptester

Q_DECLARE_METATYPE(bgptester::Topology)
Q_DECLARE_METATYPE(bgptester::RouterConfig)
Q_DECLARE_METATYPE(bgptester::LinkConfig)
Q_DECLARE_METATYPE(bgptester::NeighborConfig)
