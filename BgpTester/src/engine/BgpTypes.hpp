#pragma once

#include "model/Topology.hpp"

#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace bgptester
{

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

inline QString toString(MessageType type)
{
    switch (type)
    {
        case MessageType::Open:
            return QStringLiteral("OPEN");
        case MessageType::Update:
            return QStringLiteral("UPDATE");
        case MessageType::Notification:
            return QStringLiteral("NOTIFICATION");
    }
    return QStringLiteral("UNKNOWN");
}

inline QString toString(PeerState state)
{
    switch (state)
    {
        case PeerState::Idle:
            return QStringLiteral("Idle");
        case PeerState::OpenSent:
            return QStringLiteral("OpenSent");
        case PeerState::Established:
            return QStringLiteral("Established");
    }
    return QStringLiteral("Unknown");
}

// A TFP entity is scoped to one logical BGP router, not to the AS as a
// whole.  Keeping ASN and Entity ID as separate fields prevents two border
// routers in the same AS from sharing a version space accidentally.
struct TfpEntity
{
    quint32 asn = 0;
    QString entityId;

    bool operator==(const TfpEntity&) const = default;
    bool operator<(const TfpEntity& other) const
    {
        if (asn != other.asn)
        {
            return asn < other.asn;
        }
        return entityId < other.entityId;
    }
};

using TfpVersionVector = QMap<TfpEntity, quint64>;

// Unified TFP_VERSION_INFO path attribute.  It is also carried in the
// PathAttributes member of a withdrawal-only BgpMessage.
struct TfpVersionInfo
{
    TfpVersionVector dependencyVector;
    TfpVersionVector triggerVector;

    bool operator==(const TfpVersionInfo&) const = default;
};

struct PathAttributes
{
    QString origin = QStringLiteral("igp");
    QVector<quint32> asPath;
    QString nextHop;
    quint32 localPref = 100;
    quint32 med = 0;
    QString originatorId;
    QStringList clusterList;
    QMap<QString, QString> communities;
    std::optional<TfpVersionInfo> tfpVersionInfo;

    bool operator==(const PathAttributes&) const = default;
};

struct RouteEntry
{
    PathAttributes attributes;
    QString learnedFrom;
    SessionType sourceSession = SessionType::Ibgp;
    bool localOrigin = false;
    RouteSource source = RouteSource::Unspecified;

    bool operator==(const RouteEntry&) const = default;
};

struct BgpMessage
{
    MessageType type = MessageType::Open;
    QString from;
    QString to;
    quint64 sequence = 0;

    quint32 openAsn = 0;
    QString openRouterId;
    QStringList nlri;
    QStringList withdrawn;
    PathAttributes attributes;
    std::optional<RouteEntry> advertisedRoute;
    int errorCode = 0;
    int errorSubcode = 0;
    QString errorData;

    QMap<QString, quint64> generations;
    bool guarded = false;
};

struct RouterSnapshot
{
    QString id;
    QString routerId;
    quint32 asn = 0;
    bool active = false;
    bool routeReflector = false;
    int bestRouteCount = 0;
};

struct PeerSnapshot
{
    QString id;
    quint32 remoteAsn = 0;
    SessionType sessionType = SessionType::Ibgp;
    bool rrClient = false;
    bool enabled = true;
    int mraiMs = 0;
    PeerState state = PeerState::Idle;
    NeighborRelationship relationship = NeighborRelationship::Unspecified;
};

struct RibSnapshot
{
    QString router;
    QHash<QString, RouteEntry> localRoutes;
    QHash<QString, RouteEntry> locRib;
    QHash<QString, QHash<QString, RouteEntry>> adjRibIn;
};

struct SimulationEvent
{
    quint64 id = 0;
    QDateTime timestamp;
    QString event;
    QString router;
    QString from;
    QString to;
    std::optional<quint32> fromAs;
    std::optional<quint32> toAs;
    QString messageType;
    QString action;
    quint64 sequence = 0;
    QStringList prefixes;
    QStringList withdrawn;
    QString nextHop;
    QVector<quint32> asPath;
    std::optional<quint32> localPref;
    std::optional<quint32> med;
    QMap<QString, QString> details;
};

struct SimulationStats
{
    bool running = false;
    bool converged = false;
    qsizetype pendingEvents = 0;
    quint64 deliveredMessages = 0;
    qint64 elapsedMs = 0;
    qint64 convergenceElapsedMs = 0;
    QString convergenceTriggerEvent;
    QString convergenceTriggerContext;
};

} // namespace bgptester

Q_DECLARE_METATYPE(bgptester::PathAttributes)
Q_DECLARE_METATYPE(bgptester::RouteEntry)
Q_DECLARE_METATYPE(bgptester::TfpEntity)
Q_DECLARE_METATYPE(bgptester::TfpVersionInfo)
Q_DECLARE_METATYPE(bgptester::RouterSnapshot)
Q_DECLARE_METATYPE(bgptester::PeerSnapshot)
Q_DECLARE_METATYPE(bgptester::RibSnapshot)
Q_DECLARE_METATYPE(bgptester::SimulationEvent)
Q_DECLARE_METATYPE(bgptester::SimulationStats)
