#include "headless/HeadlessSession.hpp"

#include "engine/SimulationEngine.hpp"
#include "persistence/EventStore.hpp"
#include "plugin/RouterPluginRegistry.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <thread>

namespace bgptester
{
namespace
{

constexpr int MaximumIntervalMs = 24 * 60 * 60 * 1000;
constexpr int MaximumQuietMs = 600000;

HeadlessCommandResult success(QJsonObject data = {})
{
    return HeadlessCommandResult{.ok = true, .data = std::move(data), .error = {}, .exitRequested = false};
}

HeadlessCommandResult failure(QString error, QJsonObject data = {})
{
    return HeadlessCommandResult{.ok = false, .data = std::move(data), .error = std::move(error), .exitRequested = false};
}

QJsonArray stringsToJson(const QStringList& values)
{
    QJsonArray result;
    for (const auto& value : values)
    {
        result.append(value);
    }
    return result;
}

QJsonArray uint32sToJson(const QVector<quint32>& values)
{
    QJsonArray result;
    for (const auto value : values)
    {
        result.append(static_cast<qint64>(value));
    }
    return result;
}

QJsonValue uint64ToJson(quint64 value)
{
    if (value <= static_cast<quint64>(std::numeric_limits<qint64>::max()))
    {
        return static_cast<qint64>(value);
    }
    return QString::number(value);
}

bool readRequiredString(const QJsonObject& object, const QString& key, QString* value, QString* error)
{
    const auto entry = object.value(key);
    if (!entry.isString() || entry.toString().trimmed().isEmpty())
    {
        *error = QStringLiteral("字段 %1 必须是非空字符串").arg(key);
        return false;
    }
    *value = entry.toString().trimmed();
    return true;
}

bool readOptionalString(const QJsonObject& object, const QString& key, QString* value, QString* error, bool trim = true)
{
    const auto entry = object.constFind(key);
    if (entry == object.constEnd())
    {
        return true;
    }
    if (!entry.value().isString())
    {
        *error = QStringLiteral("字段 %1 必须是字符串").arg(key);
        return false;
    }
    *value = trim ? entry.value().toString().trimmed() : entry.value().toString();
    return true;
}

bool readOptionalBool(const QJsonObject& object, const QString& key, bool* value, QString* error)
{
    const auto entry = object.constFind(key);
    if (entry == object.constEnd())
    {
        return true;
    }
    if (!entry.value().isBool())
    {
        *error = QStringLiteral("字段 %1 必须是布尔值").arg(key);
        return false;
    }
    *value = entry.value().toBool();
    return true;
}

bool readInteger(const QJsonObject& object, const QString& key, qint64 minimum, qint64 maximum, qint64* value, QString* error,
                 bool required = false)
{
    const auto entry = object.constFind(key);
    if (entry == object.constEnd())
    {
        if (required)
        {
            *error = QStringLiteral("缺少必需字段 %1").arg(key);
            return false;
        }
        return true;
    }
    if (!entry.value().isDouble())
    {
        *error = QStringLiteral("字段 %1 必须是整数").arg(key);
        return false;
    }
    const auto number = entry.value().toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum))
    {
        *error = QStringLiteral("字段 %1 必须是 %2 到 %3 之间的整数").arg(key).arg(minimum).arg(maximum);
        return false;
    }
    *value = static_cast<qint64>(number);
    return true;
}

bool readCoordinate(const QJsonObject& object, const QString& key, qreal* value, QString* error, bool required = false)
{
    const auto entry = object.constFind(key);
    if (entry == object.constEnd())
    {
        if (required)
        {
            *error = QStringLiteral("缺少必需字段 %1").arg(key);
            return false;
        }
        return true;
    }
    if (!entry.value().isDouble() || !std::isfinite(entry.value().toDouble()))
    {
        *error = QStringLiteral("字段 %1 必须是有限数值").arg(key);
        return false;
    }
    *value = entry.value().toDouble();
    return true;
}

bool readStringList(const QJsonObject& object, const QString& key, QStringList* values, QString* error)
{
    const auto entry = object.constFind(key);
    if (entry == object.constEnd())
    {
        return true;
    }
    if (!entry.value().isArray())
    {
        *error = QStringLiteral("字段 %1 必须是字符串数组").arg(key);
        return false;
    }
    QStringList result;
    for (const auto& item : entry.value().toArray())
    {
        if (!item.isString())
        {
            *error = QStringLiteral("字段 %1 必须是字符串数组").arg(key);
            return false;
        }
        const auto value = item.toString().trimmed();
        if (!value.isEmpty() && !result.contains(value))
        {
            result.append(value);
        }
    }
    *values = std::move(result);
    return true;
}

QString routeSourceName(RouteSource source)
{
    switch (source)
    {
        case RouteSource::Local:
            return QStringLiteral("local");
        case RouteSource::Customer:
            return QStringLiteral("customer");
        case RouteSource::Peer:
            return QStringLiteral("peer");
        case RouteSource::Provider:
            return QStringLiteral("provider");
        case RouteSource::Unspecified:
            return QStringLiteral("unspecified");
    }
    return QStringLiteral("unspecified");
}

QJsonArray tfpVectorToJson(const TfpVersionVector& vector)
{
    QJsonArray result;
    for (auto it = vector.cbegin(); it != vector.cend(); ++it)
    {
        result.append(QJsonObject{{QStringLiteral("asn"), static_cast<qint64>(it.key().asn)},
                                  {QStringLiteral("entity_id"), it.key().entityId},
                                  {QStringLiteral("version"), uint64ToJson(it.value())}});
    }
    return result;
}

QJsonObject stringMapToJson(const QMap<QString, QString>& values)
{
    QJsonObject result;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
    {
        result.insert(it.key(), it.value());
    }
    return result;
}

QJsonObject linkToJson(const LinkConfig& link)
{
    return QJsonObject{{QStringLiteral("a"), link.a},
                       {QStringLiteral("b"), link.b},
                       {QStringLiteral("enabled"), link.enabled},
                       {QStringLiteral("delay_ms"), link.delayMs},
                       {QStringLiteral("rr_client_from_a"), link.rrClientFromA},
                       {QStringLiteral("rr_client_from_b"), link.rrClientFromB},
                       {QStringLiteral("mrai_ms_from_a"), link.mraiMsFromA},
                       {QStringLiteral("mrai_ms_from_b"), link.mraiMsFromB},
                       {QStringLiteral("relationship"), toString(link.businessRelationship)}};
}

QJsonObject routerToJson(const RouterConfig& router)
{
    return QJsonObject{{QStringLiteral("id"), router.id},
                       {QStringLiteral("router_id"), router.routerId},
                       {QStringLiteral("asn"), static_cast<qint64>(router.asn)},
                       {QStringLiteral("cluster_id"), router.clusterId},
                       {QStringLiteral("originated_prefixes"), stringsToJson(router.originatedPrefixes)},
                       {QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), router.position.x()},
                                                                {QStringLiteral("y"), router.position.y()}}},
                       {QStringLiteral("plugin"), QJsonObject{{QStringLiteral("id"), router.pluginId},
                                                              {QStringLiteral("settings"), router.pluginSettings}}}};
}

QString topologySha256(const Topology& topology)
{
    auto canonicalTopology = topology;
    std::sort(canonicalTopology.links.begin(), canonicalTopology.links.end(), [](const LinkConfig& lhs, const LinkConfig& rhs)
              { return Topology::edgeKey(lhs.a, lhs.b) < Topology::edgeKey(rhs.a, rhs.b); });
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(canonicalTopology.toJson()).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}

bool applyRouterFields(const QJsonObject& object, RouterConfig* router, QString* error, bool adding)
{
    if (adding && object.contains(QStringLiteral("id")) &&
        !readRequiredString(object, QStringLiteral("id"), &router->id, error))
    {
        return false;
    }
    if (object.contains(QStringLiteral("router_id")) &&
        !readRequiredString(object, QStringLiteral("router_id"), &router->routerId, error))
    {
        return false;
    }
    qint64 asn = router->asn;
    if (!readInteger(object, QStringLiteral("asn"), 1, std::numeric_limits<quint32>::max(), &asn, error))
    {
        return false;
    }
    router->asn = static_cast<quint32>(asn);
    if (!readOptionalString(object, QStringLiteral("cluster_id"), &router->clusterId, error))
    {
        return false;
    }
    if (object.contains(QStringLiteral("cluster_id")) && router->clusterId.isEmpty())
    {
        router->clusterId = router->routerId;
    }

    const auto prefixKey = object.contains(QStringLiteral("originated_prefixes")) ? QStringLiteral("originated_prefixes")
                                                                                   : QStringLiteral("prefixes");
    if (object.contains(prefixKey) && !readStringList(object, prefixKey, &router->originatedPrefixes, error))
    {
        return false;
    }

    if (!readCoordinate(object, QStringLiteral("x"), &router->position.rx(), error) ||
        !readCoordinate(object, QStringLiteral("y"), &router->position.ry(), error))
    {
        return false;
    }
    if (object.contains(QStringLiteral("position")))
    {
        if (!object.value(QStringLiteral("position")).isObject())
        {
            *error = QStringLiteral("字段 position 必须是包含 x/y 的对象");
            return false;
        }
        const auto position = object.value(QStringLiteral("position")).toObject();
        if (!readCoordinate(position, QStringLiteral("x"), &router->position.rx(), error, true) ||
            !readCoordinate(position, QStringLiteral("y"), &router->position.ry(), error, true))
        {
            return false;
        }
    }

    if (object.contains(QStringLiteral("plugin")))
    {
        const auto plugin = object.value(QStringLiteral("plugin"));
        if (plugin.isString())
        {
            router->pluginId = plugin.toString().trimmed();
        }
        else if (plugin.isObject())
        {
            const auto pluginObject = plugin.toObject();
            if (!readRequiredString(pluginObject, QStringLiteral("id"), &router->pluginId, error))
            {
                return false;
            }
            if (pluginObject.contains(QStringLiteral("settings")))
            {
                if (!pluginObject.value(QStringLiteral("settings")).isObject())
                {
                    *error = QStringLiteral("字段 plugin.settings 必须是 JSON 对象");
                    return false;
                }
                router->pluginSettings = pluginObject.value(QStringLiteral("settings")).toObject();
            }
        }
        else
        {
            *error = QStringLiteral("字段 plugin 必须是插件 ID 字符串或 JSON 对象");
            return false;
        }
    }
    if (object.contains(QStringLiteral("plugin_id")) &&
        !readRequiredString(object, QStringLiteral("plugin_id"), &router->pluginId, error))
    {
        return false;
    }
    if (object.contains(QStringLiteral("plugin_settings")))
    {
        if (!object.value(QStringLiteral("plugin_settings")).isObject())
        {
            *error = QStringLiteral("字段 plugin_settings 必须是 JSON 对象");
            return false;
        }
        router->pluginSettings = object.value(QStringLiteral("plugin_settings")).toObject();
    }
    if (router->pluginId.trimmed().isEmpty())
    {
        *error = QStringLiteral("路由器插件 ID 不能为空");
        return false;
    }
    return true;
}

bool hasExplicitPluginSettings(const QJsonObject& object)
{
    if (object.contains(QStringLiteral("plugin_settings")))
    {
        return true;
    }
    const auto plugin = object.value(QStringLiteral("plugin"));
    return plugin.isObject() && plugin.toObject().contains(QStringLiteral("settings"));
}

bool applyLinkFields(const QJsonObject& object, LinkConfig* link, bool externalSession, QString* error)
{
    if (!readOptionalBool(object, QStringLiteral("enabled"), &link->enabled, error) ||
        !readOptionalBool(object, QStringLiteral("rr_client_from_a"), &link->rrClientFromA, error) ||
        !readOptionalBool(object, QStringLiteral("rr_client_from_b"), &link->rrClientFromB, error))
    {
        return false;
    }
    qint64 delay = link->delayMs;
    qint64 mraiA = link->mraiMsFromA;
    qint64 mraiB = link->mraiMsFromB;
    if (!readInteger(object, QStringLiteral("delay_ms"), 0, MaximumIntervalMs, &delay, error) ||
        !readInteger(object, QStringLiteral("mrai_ms_from_a"), 0, MaximumIntervalMs, &mraiA, error) ||
        !readInteger(object, QStringLiteral("mrai_ms_from_b"), 0, MaximumIntervalMs, &mraiB, error))
    {
        return false;
    }
    link->delayMs = static_cast<int>(delay);
    link->mraiMsFromA = static_cast<int>(mraiA);
    link->mraiMsFromB = static_cast<int>(mraiB);

    if (object.contains(QStringLiteral("relationship")))
    {
        QString relationship;
        if (!readRequiredString(object, QStringLiteral("relationship"), &relationship, error))
        {
            return false;
        }
        const auto parsed = linkBusinessRelationshipFromString(relationship);
        if (!parsed)
        {
            *error = QStringLiteral("relationship 必须是 unspecified、peer、a_provider 或 b_provider");
            return false;
        }
        link->businessRelationship = *parsed;
    }
    if (!externalSession && link->businessRelationship != LinkBusinessRelationship::Unspecified)
    {
        *error = QStringLiteral("同一 AS 内的链路不能设置商业关系");
        return false;
    }
    return true;
}

QJsonObject makeCommandDescription(const QString& command, const QString& summary, QJsonArray fields = {})
{
    return QJsonObject{{QStringLiteral("command"), command}, {QStringLiteral("summary"), summary}, {QStringLiteral("fields"), fields}};
}

} // namespace

HeadlessSession::HeadlessSession(QObject* parent) : QObject(parent), topology_(Topology::starter())
{
    engine_ = new SimulationEngine;
    engine_->moveToThread(&engineThread_);
    engineThread_.setObjectName(QStringLiteral("BgpCliSimulationThread"));
    connect(&engineThread_, &QThread::finished, engine_, &QObject::deleteLater);
    eventStore_ = new EventStore;
    eventStore_->moveToThread(&eventStoreThread_);
    eventStoreThread_.setObjectName(QStringLiteral("BmpCliEventStoreThread"));
    connect(&eventStoreThread_, &QThread::finished, eventStore_, &QObject::deleteLater);
    connect(engine_, &SimulationEngine::eventsGenerated, eventStore_, &EventStore::enqueueEvents, Qt::DirectConnection);
    engineThread_.start();
    eventStoreThread_.start();
}

HeadlessSession::~HeadlessSession()
{
    shutdown();
}

bool HeadlessSession::isRunning() const
{
    return simulationRunning_;
}

void HeadlessSession::refreshRuntimeStatus()
{
    if (!engine_ || !engineThread_.isRunning() || shuttingDown_)
    {
        return;
    }
    SimulationStats stats;
    QMetaObject::invokeMethod(engine_, [engine = engine_, &stats] { stats = engine->statsSnapshot(); },
                              Qt::BlockingQueuedConnection);
    latestStats_ = stats;
    simulationRunning_ = stats.running;
    simulationConverged_ = stats.converged;
}

void HeadlessSession::refreshEventStoreStatus()
{
    if (!eventStore_ || !eventStoreThread_.isRunning() || !eventRunOpen_)
    {
        return;
    }
    QString storeError;
    quint64 committedEventId = 0;
    quint64 runSerial = 0;
    QMetaObject::invokeMethod(
        eventStore_,
        [store = eventStore_, &storeError, &committedEventId, &runSerial]
        {
            storeError = store->lastError();
            committedEventId = store->committedEventId();
            runSerial = store->runSerial();
        },
        Qt::BlockingQueuedConnection);
    lastStoreError_ = std::move(storeError);
    committedEventId_ = committedEventId;
    eventRunSerial_ = runSerial;
}

HeadlessCommandResult HeadlessSession::execute(const QJsonObject& command)
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    refreshRuntimeStatus();
    auto name = command.value(QStringLiteral("command")).toString().trimmed().toLower();
    if (name.isEmpty())
    {
        name = command.value(QStringLiteral("op")).toString().trimmed().toLower();
    }
    if (name.isEmpty())
    {
        return failure(QStringLiteral("命令对象必须包含非空字符串字段 command"));
    }
    name.replace(u'-', u'_');
    return dispatch(name, command);
}

HeadlessCommandResult HeadlessSession::dispatch(const QString& name, const QJsonObject& command)
{
    static const QMap<QString, Handler> handlers{
        {QStringLiteral("help"), &HeadlessSession::helpCommand},
        {QStringLiteral("status"), &HeadlessSession::statusCommand},
        {QStringLiteral("get_stats"), &HeadlessSession::statusCommand},
        {QStringLiteral("new"), &HeadlessSession::newCommand},
        {QStringLiteral("load"), &HeadlessSession::loadCommand},
        {QStringLiteral("open"), &HeadlessSession::loadCommand},
        {QStringLiteral("save"), &HeadlessSession::saveCommand},
        {QStringLiteral("save_as"), &HeadlessSession::saveCommand},
        {QStringLiteral("topology"), &HeadlessSession::topologyCommand},
        {QStringLiteral("get_topology"), &HeadlessSession::topologyCommand},
        {QStringLiteral("validate"), &HeadlessSession::validateCommand},
        {QStringLiteral("plugins"), &HeadlessSession::pluginsCommand},
        {QStringLiteral("list_plugins"), &HeadlessSession::pluginsCommand},
        {QStringLiteral("set_simulation"), &HeadlessSession::setSimulationCommand},
        {QStringLiteral("add_router"), &HeadlessSession::addRouterCommand},
        {QStringLiteral("update_router"), &HeadlessSession::updateRouterCommand},
        {QStringLiteral("move_router"), &HeadlessSession::moveRouterCommand},
        {QStringLiteral("delete_router"), &HeadlessSession::deleteRouterCommand},
        {QStringLiteral("remove_router"), &HeadlessSession::deleteRouterCommand},
        {QStringLiteral("add_link"), &HeadlessSession::addLinkCommand},
        {QStringLiteral("update_link"), &HeadlessSession::updateLinkCommand},
        {QStringLiteral("delete_link"), &HeadlessSession::deleteLinkCommand},
        {QStringLiteral("remove_link"), &HeadlessSession::deleteLinkCommand},
        {QStringLiteral("batch_update"), &HeadlessSession::batchUpdateCommand},
        {QStringLiteral("start"), &HeadlessSession::startCommand},
        {QStringLiteral("stop"), &HeadlessSession::stopCommand},
        {QStringLiteral("wait"), &HeadlessSession::waitCommand},
        {QStringLiteral("wait_converged"), &HeadlessSession::waitConvergedCommand},
        {QStringLiteral("set_router_state"), &HeadlessSession::setRouterStateCommand},
        {QStringLiteral("toggle_router"), &HeadlessSession::toggleRouterCommand},
        {QStringLiteral("set_link_state"), &HeadlessSession::setLinkStateCommand},
        {QStringLiteral("toggle_link"), &HeadlessSession::toggleLinkCommand},
        {QStringLiteral("advertise_prefix"), &HeadlessSession::advertisePrefixCommand},
        {QStringLiteral("withdraw_prefix"), &HeadlessSession::withdrawPrefixCommand},
        {QStringLiteral("routers"), &HeadlessSession::routersCommand},
        {QStringLiteral("get_routers"), &HeadlessSession::routersCommand},
        {QStringLiteral("rib"), &HeadlessSession::ribCommand},
        {QStringLiteral("get_rib"), &HeadlessSession::ribCommand},
        {QStringLiteral("peers"), &HeadlessSession::peersCommand},
        {QStringLiteral("get_peers"), &HeadlessSession::peersCommand},
        {QStringLiteral("path"), &HeadlessSession::pathCommand},
        {QStringLiteral("get_path"), &HeadlessSession::pathCommand},
        {QStringLiteral("snapshot"), &HeadlessSession::snapshotCommand},
        {QStringLiteral("export_snapshot"), &HeadlessSession::snapshotCommand},
        {QStringLiteral("query_events"), &HeadlessSession::queryEventsCommand},
        {QStringLiteral("events"), &HeadlessSession::queryEventsCommand},
        {QStringLiteral("query_convergence"), &HeadlessSession::queryConvergenceCommand},
        {QStringLiteral("convergence"), &HeadlessSession::queryConvergenceCommand},
        {QStringLiteral("flush_logs"), &HeadlessSession::flushLogsCommand},
        {QStringLiteral("exit"), &HeadlessSession::exitCommand},
        {QStringLiteral("quit"), &HeadlessSession::exitCommand},
    };
    const auto handler = handlers.constFind(name);
    if (handler == handlers.cend())
    {
        return failure(QStringLiteral("未知命令：%1；使用 help 查看命令清单").arg(name));
    }
    return (this->*handler.value())(command);
}

QJsonArray HeadlessSession::commandHelp() const
{
    return QJsonArray{
        makeCommandDescription(QStringLiteral("help"), QStringLiteral("列出稳定 JSONL 命令协议")),
        makeCommandDescription(QStringLiteral("status"), QStringLiteral("查看文档、运行、收敛和日志状态")),
        makeCommandDescription(QStringLiteral("new"), QStringLiteral("新建与 GUI 相同的双路由器起始拓扑"),
                               {QStringLiteral("force=false")}),
        makeCommandDescription(QStringLiteral("load"), QStringLiteral("加载并校验拓扑 JSON"),
                               {QStringLiteral("path"), QStringLiteral("force=false")}),
        makeCommandDescription(QStringLiteral("save"), QStringLiteral("原子保存当前拓扑"), {QStringLiteral("path=当前路径")} ),
        makeCommandDescription(QStringLiteral("topology"), QStringLiteral("返回完整拓扑 JSON")),
        makeCommandDescription(QStringLiteral("validate"), QStringLiteral("执行与 GUI 启动相同的完整校验")),
        makeCommandDescription(QStringLiteral("plugins"), QStringLiteral("列出插件元数据、默认设置和注册错误")),
        makeCommandDescription(QStringLiteral("set_simulation"), QStringLiteral("修改全局仿真设置"),
                               {QStringLiteral("name"), QStringLiteral("log_directory"), QStringLiteral("worker_threads"),
                                QStringLiteral("convergence_quiet_ms"), QStringLiteral("withdrawal_ignores_mrai")}),
        makeCommandDescription(QStringLiteral("add_router"), QStringLiteral("添加路由器，未给 ID/Router ID 时自动生成"),
                               {QStringLiteral("id"), QStringLiteral("router_id"), QStringLiteral("asn"),
                                QStringLiteral("cluster_id"), QStringLiteral("prefixes[]"), QStringLiteral("x/y"),
                                QStringLiteral("plugin_id"), QStringLiteral("plugin_settings{}")}),
        makeCommandDescription(QStringLiteral("update_router"), QStringLiteral("编辑或重命名路由器并级联更新链路"),
                               {QStringLiteral("id"), QStringLiteral("new_id"), QStringLiteral("其余字段同 add_router")}),
        makeCommandDescription(QStringLiteral("move_router"), QStringLiteral("持久化路由器画布位置"),
                               {QStringLiteral("id"), QStringLiteral("x"), QStringLiteral("y")}),
        makeCommandDescription(QStringLiteral("delete_router"), QStringLiteral("删除路由器并级联删除相邻链路"),
                               {QStringLiteral("id 或 ids[]")}),
        makeCommandDescription(QStringLiteral("add_link/update_link"), QStringLiteral("配置链路、双向 MRAI/RR Client 和商业关系"),
                               {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("enabled"), QStringLiteral("delay_ms"),
                                QStringLiteral("mrai_ms_from_a/b"), QStringLiteral("rr_client_from_a/b"),
                                QStringLiteral("relationship")}),
        makeCommandDescription(QStringLiteral("delete_link"), QStringLiteral("按无向端点删除链路"),
                               {QStringLiteral("a"), QStringLiteral("b")}),
        makeCommandDescription(QStringLiteral("batch_update"), QStringLiteral("批量设置插件、出站 MRAI 和全链路延迟；随机值可重放"),
                               {QStringLiteral("router_ids[]"), QStringLiteral("plugin_id"), QStringLiteral("plugin_settings{}"),
                                QStringLiteral("mrai{mode,value_ms|min_ms,max_ms}"),
                                QStringLiteral("delay{mode,value_ms|min_ms,max_ms}"), QStringLiteral("seed")}),
        makeCommandDescription(QStringLiteral("start/stop"), QStringLiteral("创建 BMP JSONL/SQLite 后启动或停止仿真")),
        makeCommandDescription(QStringLiteral("wait"), QStringLiteral("保持事件循环运行指定时间"), {QStringLiteral("milliseconds")}),
        makeCommandDescription(QStringLiteral("wait_converged"), QStringLiteral("等待当前收敛轮完成"),
                               {QStringLiteral("timeout_ms=30000")}),
        makeCommandDescription(QStringLiteral("set_router_state/toggle_router"), QStringLiteral("运行时关闭或恢复节点"),
                               {QStringLiteral("router"), QStringLiteral("enabled")}),
        makeCommandDescription(QStringLiteral("set_link_state/toggle_link"), QStringLiteral("运行时断开或恢复链路"),
                               {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("enabled")}),
        makeCommandDescription(QStringLiteral("advertise_prefix/withdraw_prefix"), QStringLiteral("运行时发布或撤销 IPv4 前缀"),
                               {QStringLiteral("router"), QStringLiteral("prefix")}),
        makeCommandDescription(QStringLiteral("routers/rib/peers/path"), QStringLiteral("查询与 GUI 检查器对应的完整运行快照")),
        makeCommandDescription(QStringLiteral("snapshot"), QStringLiteral("稳定排序导出所有 Router/RIB/Peer/逐跳路径和统计"),
                               {QStringLiteral("path=可选 JSON 文件")}),
        makeCommandDescription(QStringLiteral("query_events"), QStringLiteral("查询当前或指定 SQLite 的完整事件和计数"),
                               {QStringLiteral("database=当前"), QStringLiteral("filter"), QStringLiteral("limit=20000")}),
        makeCommandDescription(QStringLiteral("query_convergence"), QStringLiteral("查询当前或指定 SQLite 的收敛记录"),
                               {QStringLiteral("database=当前"), QStringLiteral("limit=5000")}),
        makeCommandDescription(QStringLiteral("flush_logs"), QStringLiteral("阻塞刷新 BMP JSONL/SQLite")),
        makeCommandDescription(QStringLiteral("exit"), QStringLiteral("安全停止、落盘并结束会话")),
    };
}

HeadlessCommandResult HeadlessSession::helpCommand(const QJsonObject&)
{
    return success(QJsonObject{{QStringLiteral("protocol"), QStringLiteral("bgptester-cli-jsonl-v1")},
                               {QStringLiteral("commands"), commandHelp()}});
}

QJsonObject HeadlessSession::statusJson() const
{
    QJsonObject runtimeOriginatedPrefixes;
    for (auto it = runtimeOriginatedPrefixes_.cbegin(); it != runtimeOriginatedPrefixes_.cend(); ++it)
    {
        auto prefixes = it.value().values();
        prefixes.sort(Qt::CaseSensitive);
        runtimeOriginatedPrefixes.insert(it.key(), stringsToJson(prefixes));
    }
    return QJsonObject{{QStringLiteral("topology_path"), topologyPath_},
                       {QStringLiteral("dirty"), dirty_},
                       {QStringLiteral("router_count"), topology_.routers.size()},
                       {QStringLiteral("link_count"), topology_.links.size()},
                       {QStringLiteral("running"), isRunning()},
                       {QStringLiteral("converged"), simulationConverged_},
                       {QStringLiteral("runtime_available"), runtimeAvailable_},
                       {QStringLiteral("stats"), statsToJson(latestStats_)},
                       {QStringLiteral("run_directory"), runDirectory_},
                       {QStringLiteral("bmp_jsonl"), logFilePath_},
                       {QStringLiteral("bmp_sqlite"), databasePath_},
                       {QStringLiteral("event_run_serial"), uint64ToJson(eventRunSerial_)},
                       {QStringLiteral("committed_event_id"), uint64ToJson(committedEventId_)},
                       {QStringLiteral("runtime_originated_prefixes"), runtimeOriginatedPrefixes},
                       {QStringLiteral("event_store_error"), lastStoreError_}};
}

HeadlessCommandResult HeadlessSession::statusCommand(const QJsonObject&)
{
    refreshEventStoreStatus();
    return success(statusJson());
}

HeadlessCommandResult HeadlessSession::rejectWhileRunning() const
{
    return failure(QStringLiteral("仿真运行期间拓扑编辑被锁定；请先执行 stop"));
}

HeadlessCommandResult HeadlessSession::requireRuntime() const
{
    return runtimeAvailable_ ? success() : failure(QStringLiteral("尚无可查询的仿真运行时；请先执行 start"));
}

bool HeadlessSession::beginEventRun(QString* error)
{
    if (!eventStore_ || !eventStoreThread_.isRunning())
    {
        *error = QStringLiteral("日志存储线程未运行");
        return false;
    }
    if (eventRunOpen_ && !endEventRun())
    {
        *error = lastStoreError_;
        return false;
    }
    lastStoreError_.clear();
    logFilePath_.clear();
    databasePath_.clear();
    runDirectory_.clear();
    eventRunSerial_ = 0;
    committedEventId_ = 0;
    bool started = false;
    QString logFilePath;
    QString databasePath;
    QString runDirectory;
    quint64 runSerial = 0;
    const auto settings = topology_.simulation;
    QMetaObject::invokeMethod(
        eventStore_,
        [store = eventStore_, settings, error, &started, &logFilePath, &databasePath, &runDirectory, &runSerial]
        {
            started = store->beginRun(settings, error);
            if (started)
            {
                logFilePath = QFileInfo(store->logFilePath()).absoluteFilePath();
                databasePath = QFileInfo(store->databasePath()).absoluteFilePath();
                runDirectory = QFileInfo(store->runDirectory()).absoluteFilePath();
                runSerial = store->runSerial();
            }
        },
        Qt::BlockingQueuedConnection);
    if (started)
    {
        logFilePath_ = std::move(logFilePath);
        databasePath_ = std::move(databasePath);
        runDirectory_ = std::move(runDirectory);
        eventRunSerial_ = runSerial;
    }
    eventRunOpen_ = started;
    return started;
}

bool HeadlessSession::flushEventRun()
{
    if (eventRunOpen_ && (!eventStore_ || !eventStoreThread_.isRunning()))
    {
        lastStoreError_ = QStringLiteral("日志存储线程在刷新前已停止");
        return false;
    }
    if (eventRunOpen_)
    {
        QString storeError;
        quint64 committedEventId = 0;
        QMetaObject::invokeMethod(
            eventStore_,
            [store = eventStore_, &storeError, &committedEventId]
            {
                store->flush();
                committedEventId = store->committedEventId();
                storeError = store->lastError();
            },
            Qt::BlockingQueuedConnection);
        committedEventId_ = committedEventId;
        lastStoreError_ = storeError;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return lastStoreError_.isEmpty();
}

bool HeadlessSession::endEventRun()
{
    if (eventRunOpen_ && (!eventStore_ || !eventStoreThread_.isRunning()))
    {
        lastStoreError_ = QStringLiteral("日志存储线程在关闭运行前已停止");
        eventRunOpen_ = false;
        return false;
    }
    if (eventRunOpen_)
    {
        QString storeError;
        quint64 committedEventId = 0;
        QMetaObject::invokeMethod(
            eventStore_,
            [store = eventStore_, &storeError, &committedEventId]
            {
                store->endRun();
                committedEventId = store->committedEventId();
                storeError = store->lastError();
            },
            Qt::BlockingQueuedConnection);
        committedEventId_ = committedEventId;
        lastStoreError_ = storeError;
    }
    eventRunOpen_ = false;
    return lastStoreError_.isEmpty();
}

bool HeadlessSession::shutdown(QString* error)
{
    if (shuttingDown_)
    {
        if (error)
        {
            *error = lastStoreError_;
        }
        return lastStoreError_.isEmpty();
    }
    shuttingDown_ = true;
    if (engine_ && engineThread_.isRunning())
    {
        SimulationStats stats;
        QMetaObject::invokeMethod(
            engine_,
            [engine = engine_, &stats]
            {
                engine->stopSimulation();
                stats = engine->statsSnapshot();
            },
            Qt::BlockingQueuedConnection);
        latestStats_ = stats;
        simulationRunning_ = stats.running;
        simulationConverged_ = stats.converged;
    }
    const auto flushed = flushEventRun();
    const auto ended = endEventRun();
    if (eventStoreThread_.isRunning())
    {
        eventStoreThread_.quit();
        eventStoreThread_.wait();
    }
    if (engineThread_.isRunning())
    {
        engineThread_.quit();
        engineThread_.wait();
    }
    if (error)
    {
        *error = lastStoreError_;
    }
    return flushed && ended && lastStoreError_.isEmpty();
}

HeadlessCommandResult HeadlessSession::newCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    const auto force = command.value(QStringLiteral("force")).toBool(false);
    if (dirty_ && !force)
    {
        return failure(QStringLiteral("当前拓扑有未保存更改；若确定丢弃，请设置 force=true"));
    }
    topology_ = Topology::starter();
    topologyPath_.clear();
    dirty_ = false;
    invalidateRuntime();
    return success(QJsonObject{{QStringLiteral("router_count"), topology_.routers.size()},
                               {QStringLiteral("link_count"), topology_.links.size()},
                               {QStringLiteral("topology_sha256"), topologySha256(topology_)},
                               {QStringLiteral("topology"), topology_.toJson()}});
}

HeadlessCommandResult HeadlessSession::loadCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    const auto force = command.value(QStringLiteral("force")).toBool(false);
    if (dirty_ && !force)
    {
        return failure(QStringLiteral("当前拓扑有未保存更改；若确定丢弃，请设置 force=true"));
    }
    QString path;
    QString error;
    if (!readRequiredString(command, QStringLiteral("path"), &path, &error))
    {
        return failure(error);
    }
    const QFileInfo file(path);
    if (!file.exists() || !file.isFile() || !file.isReadable())
    {
        return failure(QStringLiteral("无法读取拓扑文件：%1").arg(file.absoluteFilePath()));
    }
    auto loaded = Topology::load(file.absoluteFilePath(), &error);
    if (!loaded)
    {
        return failure(error);
    }
    topology_ = std::move(*loaded);
    topologyPath_ = file.absoluteFilePath();
    dirty_ = false;
    invalidateRuntime();
    return success(QJsonObject{{QStringLiteral("path"), topologyPath_},
                               {QStringLiteral("router_count"), topology_.routers.size()},
                               {QStringLiteral("link_count"), topology_.links.size()},
                               {QStringLiteral("topology_sha256"), topologySha256(topology_)}});
}

HeadlessCommandResult HeadlessSession::saveCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    QString path = topologyPath_;
    QString error;
    if (!readOptionalString(command, QStringLiteral("path"), &path, &error) || path.trimmed().isEmpty())
    {
        return failure(error.isEmpty() ? QStringLiteral("尚无保存路径；请提供 path") : error);
    }
    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
    {
        path += QStringLiteral(".json");
    }
    path = QFileInfo(path).absoluteFilePath();
    auto directory = QFileInfo(path).absoluteDir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral(".")))
    {
        return failure(QStringLiteral("无法创建拓扑目录：%1").arg(directory.absolutePath()));
    }
    if (!topology_.save(path, &error))
    {
        return failure(error);
    }
    topologyPath_ = path;
    dirty_ = false;
    return success(QJsonObject{{QStringLiteral("path"), topologyPath_},
                               {QStringLiteral("router_count"), topology_.routers.size()},
                               {QStringLiteral("link_count"), topology_.links.size()},
                               {QStringLiteral("topology_sha256"), topologySha256(topology_)}});
}

HeadlessCommandResult HeadlessSession::topologyCommand(const QJsonObject&)
{
    return success(QJsonObject{{QStringLiteral("path"), topologyPath_},
                               {QStringLiteral("dirty"), dirty_},
                               {QStringLiteral("topology_sha256"), topologySha256(topology_)},
                               {QStringLiteral("topology"), topology_.toJson()}});
}

HeadlessCommandResult HeadlessSession::validateCommand(const QJsonObject&)
{
    auto problems = topology_.validate();
    if (problems.isEmpty())
    {
        const auto neighborIndex = topology_.buildNeighborIndex();
        for (auto it = topology_.routers.cbegin(); it != topology_.routers.cend(); ++it)
        {
            QString creationError;
            const RouterNodeContext context{.config = it.value(),
                                            .topologyRouters = topology_.routers,
                                            .neighbors = neighborIndex.value(it.key())};
            std::unique_ptr<RouterNode> node(
                RouterPluginRegistry::instance().createRouterNode(context, nullptr, &creationError));
            if (!node)
            {
                problems.append(QStringLiteral("路由器 %1 的插件无法创建：%2").arg(it.key(), creationError));
                continue;
            }
            const auto configurationProblems = node->validateConfiguration();
            for (const auto& problem : configurationProblems)
            {
                problems.append(QStringLiteral("路由器 %1 的插件配置无效：%2").arg(it.key(), problem));
            }
        }
    }
    const auto data = QJsonObject{{QStringLiteral("valid"), problems.isEmpty()},
                                  {QStringLiteral("problems"), stringsToJson(problems)}};
    return problems.isEmpty() ? success(data) : failure(problems.join(u'\n'), data);
}

HeadlessCommandResult HeadlessSession::pluginsCommand(const QJsonObject&)
{
    QJsonArray plugins;
    for (const auto& plugin : RouterPluginRegistry::instance().plugins())
    {
        plugins.append(QJsonObject{{QStringLiteral("id"), plugin.metadata.id},
                                   {QStringLiteral("display_name"), plugin.metadata.displayName},
                                   {QStringLiteral("version"), plugin.metadata.version},
                                   {QStringLiteral("description"), plugin.metadata.description},
                                   {QStringLiteral("api_version"), plugin.metadata.apiVersion},
                                   {QStringLiteral("default_settings"), plugin.metadata.defaultSettings},
                                   {QStringLiteral("source"), plugin.source}});
    }
    return success(QJsonObject{{QStringLiteral("plugins"), plugins},
                               {QStringLiteral("registration_errors"),
                                stringsToJson(RouterPluginRegistry::instance().registrationErrors())}});
}

HeadlessCommandResult HeadlessSession::setSimulationCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    auto settings = topology_.simulation;
    QString error;
    if (!readOptionalString(command, QStringLiteral("name"), &settings.name, &error) ||
        !readOptionalString(command, QStringLiteral("log_directory"), &settings.logDirectory, &error) ||
        !readOptionalBool(command, QStringLiteral("withdrawal_ignores_mrai"), &settings.withdrawalIgnoresMrai, &error))
    {
        return failure(error);
    }
    if (command.contains(QStringLiteral("log_dir")) &&
        !readOptionalString(command, QStringLiteral("log_dir"), &settings.logDirectory, &error))
    {
        return failure(error);
    }
    qint64 workers = settings.workerThreads;
    qint64 quiet = settings.convergenceQuietMs;
    if (!readInteger(command, QStringLiteral("worker_threads"), 0, 256, &workers, &error) ||
        !readInteger(command, QStringLiteral("convergence_quiet_ms"), 0, MaximumQuietMs, &quiet, &error))
    {
        return failure(error);
    }
    settings.workerThreads = static_cast<int>(workers);
    settings.convergenceQuietMs = static_cast<int>(quiet);
    if (settings.name.trimmed().isEmpty() || settings.logDirectory.trimmed().isEmpty())
    {
        return failure(QStringLiteral("实验名称和日志目录不能为空"));
    }
    const auto changed = settings.name != topology_.simulation.name || settings.logDirectory != topology_.simulation.logDirectory ||
                         settings.workerThreads != topology_.simulation.workerThreads ||
                         settings.convergenceQuietMs != topology_.simulation.convergenceQuietMs ||
                         settings.withdrawalIgnoresMrai != topology_.simulation.withdrawalIgnoresMrai;
    topology_.simulation = std::move(settings);
    dirty_ = dirty_ || changed;
    if (changed)
    {
        invalidateRuntime();
    }
    return success(QJsonObject{{QStringLiteral("changed"), changed},
                               {QStringLiteral("simulation"), topology_.toJson().value(QStringLiteral("simulation"))}});
}

HeadlessCommandResult HeadlessSession::addRouterCommand(const QJsonObject& command)
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
    QString error;
    if (!applyRouterFields(command, &router, &error, true))
    {
        return failure(error);
    }
    if (router.pluginId != initialPluginId && !RouterPluginRegistry::instance().contains(router.pluginId))
    {
        return failure(QStringLiteral("添加路由器只能选择已注册插件：%1").arg(router.pluginId));
    }
    if (!command.contains(QStringLiteral("cluster_id")))
    {
        router.clusterId = router.routerId;
    }
    if (router.pluginId != initialPluginId && !hasExplicitPluginSettings(command))
    {
        const auto metadata = RouterPluginRegistry::instance().metadata(router.pluginId);
        router.pluginSettings = metadata ? metadata->defaultSettings : QJsonObject{};
    }
    if (router.id.trimmed().isEmpty())
    {
        return failure(QStringLiteral("路由器 ID 不能为空"));
    }
    if (topology_.routers.contains(router.id))
    {
        return failure(QStringLiteral("路由器 ID 已存在：%1").arg(router.id));
    }
    auto candidate = topology_;
    candidate.routers.insert(router.id, router);
    const auto problems = candidate.validate();
    if (!problems.isEmpty())
    {
        return failure(problems.join(u'\n'), QJsonObject{{QStringLiteral("problems"), stringsToJson(problems)}});
    }
    topology_ = std::move(candidate);
    dirty_ = true;
    invalidateRuntime();
    return success(QJsonObject{{QStringLiteral("changed"), true}, {QStringLiteral("router"), routerToJson(router)}});
}

HeadlessCommandResult HeadlessSession::updateRouterCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    QString id;
    QString error;
    if (!readRequiredString(command, QStringLiteral("id"), &id, &error))
    {
        return failure(error);
    }
    const auto current = topology_.routers.constFind(id);
    if (current == topology_.routers.cend())
    {
        return failure(QStringLiteral("路由器不存在：%1").arg(id));
    }
    auto updated = current.value();
    const auto initialPluginId = updated.pluginId;
    if (!applyRouterFields(command, &updated, &error, false))
    {
        return failure(error);
    }
    if (updated.pluginId != initialPluginId && !RouterPluginRegistry::instance().contains(updated.pluginId))
    {
        return failure(QStringLiteral("编辑路由器只能选择已注册插件：%1").arg(updated.pluginId));
    }
    if (updated.pluginId != initialPluginId && !hasExplicitPluginSettings(command))
    {
        const auto metadata = RouterPluginRegistry::instance().metadata(updated.pluginId);
        updated.pluginSettings = metadata ? metadata->defaultSettings : QJsonObject{};
    }
    QString newId = id;
    if (!readOptionalString(command, QStringLiteral("new_id"), &newId, &error) || newId.isEmpty())
    {
        return failure(error.isEmpty() ? QStringLiteral("new_id 不能为空") : error);
    }
    if (newId != id && topology_.routers.contains(newId))
    {
        return failure(QStringLiteral("路由器 ID 已存在：%1").arg(newId));
    }
    updated.id = newId;

    auto candidate = topology_;
    candidate.routers.remove(id);
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
    candidate.routers.insert(newId, updated);
    for (auto& link : candidate.links)
    {
        if (link.businessRelationship == LinkBusinessRelationship::Unspecified || (link.a != newId && link.b != newId))
        {
            continue;
        }
        const auto a = candidate.routers.constFind(link.a);
        const auto b = candidate.routers.constFind(link.b);
        if (a != candidate.routers.cend() && b != candidate.routers.cend() && a->asn == b->asn)
        {
            link.businessRelationship = LinkBusinessRelationship::Unspecified;
        }
    }
    const auto problems = candidate.validate();
    if (!problems.isEmpty())
    {
        return failure(problems.join(u'\n'), QJsonObject{{QStringLiteral("problems"), stringsToJson(problems)}});
    }
    const auto changed = candidate.toJson() != topology_.toJson();
    topology_ = std::move(candidate);
    dirty_ = dirty_ || changed;
    if (changed)
    {
        invalidateRuntime();
    }
    return success(QJsonObject{{QStringLiteral("changed"), changed},
                               {QStringLiteral("old_id"), id},
                               {QStringLiteral("id"), newId}});
}

HeadlessCommandResult HeadlessSession::moveRouterCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    QString id;
    QString error;
    if (!readRequiredString(command, QStringLiteral("id"), &id, &error))
    {
        return failure(error);
    }
    auto router = topology_.routers.find(id);
    if (router == topology_.routers.end())
    {
        return failure(QStringLiteral("路由器不存在：%1").arg(id));
    }
    auto position = router->position;
    if (!readCoordinate(command, QStringLiteral("x"), &position.rx(), &error, true) ||
        !readCoordinate(command, QStringLiteral("y"), &position.ry(), &error, true))
    {
        return failure(error);
    }
    const auto changed = position != router->position;
    router->position = position;
    dirty_ = dirty_ || changed;
    return success(QJsonObject{{QStringLiteral("changed"), changed},
                               {QStringLiteral("id"), id},
                               {QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), position.x()},
                                                                        {QStringLiteral("y"), position.y()}}}});
}

HeadlessCommandResult HeadlessSession::deleteRouterCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    QStringList ids;
    QString error;
    if (command.contains(QStringLiteral("ids")))
    {
        if (!readStringList(command, QStringLiteral("ids"), &ids, &error) || ids.isEmpty())
        {
            return failure(error.isEmpty() ? QStringLiteral("ids 不能为空") : error);
        }
    }
    else
    {
        QString id;
        if (!readRequiredString(command, QStringLiteral("id"), &id, &error))
        {
            return failure(error);
        }
        ids.append(id);
    }
    for (const auto& id : ids)
    {
        if (!topology_.routers.contains(id))
        {
            return failure(QStringLiteral("路由器不存在：%1").arg(id));
        }
    }
    QSet<QString> remove;
    for (const auto& id : ids)
    {
        remove.insert(id);
    }
    for (const auto& id : ids)
    {
        topology_.routers.remove(id);
    }
    const auto oldLinkCount = topology_.links.size();
    topology_.links.erase(std::remove_if(topology_.links.begin(), topology_.links.end(),
                                         [&](const LinkConfig& link) { return remove.contains(link.a) || remove.contains(link.b); }),
                          topology_.links.end());
    dirty_ = true;
    invalidateRuntime();
    return success(QJsonObject{{QStringLiteral("changed"), true},
                               {QStringLiteral("deleted_routers"), stringsToJson(ids)},
                               {QStringLiteral("deleted_links"), oldLinkCount - topology_.links.size()}});
}

HeadlessCommandResult HeadlessSession::addLinkCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    QString a;
    QString b;
    QString error;
    if (!readRequiredString(command, QStringLiteral("a"), &a, &error) ||
        !readRequiredString(command, QStringLiteral("b"), &b, &error))
    {
        return failure(error);
    }
    if (a == b)
    {
        return failure(QStringLiteral("链路不能连接路由器自身：%1").arg(a));
    }
    if (!topology_.routers.contains(a) || !topology_.routers.contains(b))
    {
        return failure(QStringLiteral("链路端点不存在：%1 - %2").arg(a, b));
    }
    if (topology_.findLink(a, b))
    {
        return failure(QStringLiteral("链路已存在：%1 - %2").arg(a, b));
    }
    LinkConfig link{.a = a, .b = b};
    const auto external = topology_.routers.value(a).asn != topology_.routers.value(b).asn;
    if (!applyLinkFields(command, &link, external, &error))
    {
        return failure(error);
    }
    auto candidate = topology_;
    candidate.links.append(link);
    const auto problems = candidate.validate();
    if (!problems.isEmpty())
    {
        return failure(problems.join(u'\n'), QJsonObject{{QStringLiteral("problems"), stringsToJson(problems)}});
    }
    topology_ = std::move(candidate);
    dirty_ = true;
    invalidateRuntime();
    return success(QJsonObject{{QStringLiteral("changed"), true}, {QStringLiteral("link"), linkToJson(link)}});
}

HeadlessCommandResult HeadlessSession::updateLinkCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    QString a;
    QString b;
    QString error;
    if (!readRequiredString(command, QStringLiteral("a"), &a, &error) ||
        !readRequiredString(command, QStringLiteral("b"), &b, &error))
    {
        return failure(error);
    }
    auto* current = topology_.findLink(a, b);
    if (!current)
    {
        return failure(QStringLiteral("链路不存在：%1 - %2").arg(a, b));
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
    // Directional fields are interpreted relative to the a/b supplied by the
    // command, while the saved edge keeps its original orientation.
    const auto reversed = updated.a != a;
    if (reversed)
    {
        reverseDirection(&updated);
    }
    const auto external = topology_.routers.value(a).asn != topology_.routers.value(b).asn;
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
    *candidateLink = updated;
    const auto problems = candidate.validate();
    if (!problems.isEmpty())
    {
        return failure(problems.join(u'\n'), QJsonObject{{QStringLiteral("problems"), stringsToJson(problems)}});
    }
    topology_ = std::move(candidate);
    dirty_ = dirty_ || changed;
    if (changed)
    {
        invalidateRuntime();
    }
    return success(QJsonObject{{QStringLiteral("changed"), changed}, {QStringLiteral("link"), linkToJson(updated)}});
}

HeadlessCommandResult HeadlessSession::deleteLinkCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    QString a;
    QString b;
    QString error;
    if (!readRequiredString(command, QStringLiteral("a"), &a, &error) ||
        !readRequiredString(command, QStringLiteral("b"), &b, &error))
    {
        return failure(error);
    }
    const auto key = Topology::edgeKey(a, b);
    const auto originalSize = topology_.links.size();
    topology_.links.erase(std::remove_if(topology_.links.begin(), topology_.links.end(),
                                         [&](const LinkConfig& link) { return Topology::edgeKey(link.a, link.b) == key; }),
                          topology_.links.end());
    if (topology_.links.size() == originalSize)
    {
        return failure(QStringLiteral("链路不存在：%1 - %2").arg(a, b));
    }
    dirty_ = true;
    invalidateRuntime();
    return success(QJsonObject{{QStringLiteral("changed"), true}, {QStringLiteral("a"), a}, {QStringLiteral("b"), b}});
}

HeadlessCommandResult HeadlessSession::batchUpdateCommand(const QJsonObject& command)
{
    if (isRunning())
    {
        return rejectWhileRunning();
    }
    QString error;
    QStringList routerIds = topology_.routers.keys();
    if (command.contains(QStringLiteral("router_ids")))
    {
        if (!readStringList(command, QStringLiteral("router_ids"), &routerIds, &error) || routerIds.isEmpty())
        {
            return failure(error.isEmpty() ? QStringLiteral("router_ids 不能为空") : error);
        }
    }
    for (const auto& id : routerIds)
    {
        if (!topology_.routers.contains(id))
        {
            return failure(QStringLiteral("路由器不存在：%1").arg(id));
        }
    }

    QString pluginId;
    QJsonObject pluginSettings;
    const auto changesPlugin = command.contains(QStringLiteral("plugin_id"));
    const auto explicitPluginSettings = command.contains(QStringLiteral("plugin_settings"));
    if (changesPlugin)
    {
        if (!readRequiredString(command, QStringLiteral("plugin_id"), &pluginId, &error))
        {
            return failure(error);
        }
        const auto metadata = RouterPluginRegistry::instance().metadata(pluginId);
        if (!metadata)
        {
            return failure(QStringLiteral("批量配置只能选择已注册插件：%1").arg(pluginId));
        }
        pluginSettings = metadata->defaultSettings;
        if (explicitPluginSettings)
        {
            if (!command.value(QStringLiteral("plugin_settings")).isObject())
            {
                return failure(QStringLiteral("plugin_settings 必须是 JSON 对象"));
            }
            pluginSettings = command.value(QStringLiteral("plugin_settings")).toObject();
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
    const auto parseInterval = [&](const QString& key, BatchInterval* result) -> bool
    {
        if (!command.contains(key))
        {
            return true;
        }
        if (!command.value(key).isObject())
        {
            error = QStringLiteral("字段 %1 必须是对象").arg(key);
            return false;
        }
        const auto object = command.value(key).toObject();
        QString mode;
        if (!readRequiredString(object, QStringLiteral("mode"), &mode, &error))
        {
            return false;
        }
        mode = mode.toLower();
        if (mode == QStringLiteral("unchanged"))
        {
            return true;
        }
        if (mode == QStringLiteral("fixed"))
        {
            qint64 value = 0;
            if (!readInteger(object, QStringLiteral("value_ms"), 0, MaximumIntervalMs, &value, &error, true))
            {
                return false;
            }
            result->mode = BatchMode::Fixed;
            result->minimum = result->maximum = static_cast<int>(value);
            return true;
        }
        if (mode == QStringLiteral("random") || mode == QStringLiteral("random_range"))
        {
            qint64 minimum = 0;
            qint64 maximum = 0;
            if (!readInteger(object, QStringLiteral("min_ms"), 0, MaximumIntervalMs, &minimum, &error, true) ||
                !readInteger(object, QStringLiteral("max_ms"), 0, MaximumIntervalMs, &maximum, &error, true))
            {
                return false;
            }
            if (minimum > maximum)
            {
                error = QStringLiteral("%1.min_ms 不能大于 max_ms").arg(key);
                return false;
            }
            result->mode = BatchMode::Random;
            result->minimum = static_cast<int>(minimum);
            result->maximum = static_cast<int>(maximum);
            return true;
        }
        error = QStringLiteral("%1.mode 必须是 unchanged、fixed 或 random").arg(key);
        return false;
    };

    BatchInterval mrai;
    BatchInterval delay;
    if (!parseInterval(QStringLiteral("mrai"), &mrai) || !parseInterval(QStringLiteral("delay"), &delay))
    {
        return failure(error);
    }

    qint64 seedValue = 0;
    const auto hasSeed = command.contains(QStringLiteral("seed"));
    if (hasSeed && !readInteger(command, QStringLiteral("seed"), 0, std::numeric_limits<quint32>::max(), &seedValue, &error, true))
    {
        return failure(error);
    }
    const auto seed = hasSeed ? static_cast<quint32>(seedValue) : QRandomGenerator::global()->generate();
    std::mt19937 generator(seed);
    const auto randomValue = [&](const BatchInterval& interval)
    {
        return std::uniform_int_distribution<int>(interval.minimum, interval.maximum)(generator);
    };

    auto candidate = topology_;
    int changedRouters = 0;
    int changedMraiDirections = 0;
    int changedLinks = 0;
    QJsonObject actualMraiByRouter;
    QJsonArray actualDelayByLink;

    if (changesPlugin)
    {
        for (const auto& id : routerIds)
        {
            auto router = candidate.routers.find(id);
            if (router->pluginId != pluginId)
            {
                router->pluginId = pluginId;
                router->pluginSettings = pluginSettings;
                ++changedRouters;
            }
            else if (explicitPluginSettings && router->pluginSettings != pluginSettings)
            {
                router->pluginSettings = pluginSettings;
                ++changedRouters;
            }
        }
    }

    QMap<QString, int> mraiValues;
    if (mrai.mode != BatchMode::Unchanged)
    {
        for (const auto& id : routerIds)
        {
            const auto value = mrai.mode == BatchMode::Fixed ? mrai.minimum : randomValue(mrai);
            mraiValues.insert(id, value);
            actualMraiByRouter.insert(id, value);
        }
        for (auto& link : candidate.links)
        {
            if (mraiValues.contains(link.a) && link.mraiMsFromA != mraiValues.value(link.a))
            {
                link.mraiMsFromA = mraiValues.value(link.a);
                ++changedMraiDirections;
            }
            if (mraiValues.contains(link.b) && link.mraiMsFromB != mraiValues.value(link.b))
            {
                link.mraiMsFromB = mraiValues.value(link.b);
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
            actualDelayByLink.append(QJsonObject{{QStringLiteral("a"), link.a},
                                                 {QStringLiteral("b"), link.b},
                                                 {QStringLiteral("delay_ms"), value}});
        }
    }

    const auto problems = candidate.validate();
    if (!problems.isEmpty())
    {
        return failure(problems.join(u'\n'), QJsonObject{{QStringLiteral("problems"), stringsToJson(problems)}});
    }
    const auto changed = changedRouters > 0 || changedMraiDirections > 0 || changedLinks > 0;
    topology_ = std::move(candidate);
    dirty_ = dirty_ || changed;
    if (changed)
    {
        invalidateRuntime();
    }
    QJsonObject data{{QStringLiteral("changed"), changed},
                     {QStringLiteral("changed_routers"), changedRouters},
                     {QStringLiteral("changed_mrai_directions"), changedMraiDirections},
                     {QStringLiteral("changed_links"), changedLinks},
                     {QStringLiteral("seed"), static_cast<qint64>(seed)},
                     {QStringLiteral("actual_mrai_by_router"), actualMraiByRouter},
                     {QStringLiteral("actual_delays"), actualDelayByLink}};
    if (changesPlugin)
    {
        data.insert(QStringLiteral("applied_plugin"),
                    QJsonObject{{QStringLiteral("id"), pluginId}, {QStringLiteral("settings"), pluginSettings}});
    }
    return success(data);
}

void HeadlessSession::updateRuntimeLinks()
{
    runtimeLinks_.clear();
    for (const auto& link : topology_.links)
    {
        runtimeLinks_.insert(Topology::edgeKey(link.a, link.b), link.enabled);
    }
    runtimeOriginatedPrefixes_.clear();
    for (auto it = topology_.routers.cbegin(); it != topology_.routers.cend(); ++it)
    {
        auto& prefixes = runtimeOriginatedPrefixes_[it.key()];
        for (const auto& prefix : it->originatedPrefixes)
        {
            prefixes.insert(prefix);
        }
    }
}

void HeadlessSession::invalidateRuntime()
{
    runtimeAvailable_ = false;
    latestStats_ = {};
    simulationConverged_ = false;
    runtimeLinks_.clear();
    runtimeOriginatedPrefixes_.clear();
}

HeadlessCommandResult HeadlessSession::startCommand(const QJsonObject&)
{
    if (isRunning())
    {
        return failure(QStringLiteral("仿真已经在运行"), statusJson());
    }
    QString error;
    if (!beginEventRun(&error))
    {
        return failure(error);
    }
    runtimeAvailable_ = false;
    latestStats_ = {};
    lastEngineError_.clear();
    updateRuntimeLinks();
    engine_->prepareStartup();
    auto topology = topology_;
    bool started = false;
    bool converged = false;
    SimulationStats stats;
    QString engineError;
    std::atomic_bool startupFinished{false};
    std::thread cancellationWatcher;
    if (interruptionFlag_)
    {
        cancellationWatcher = std::thread(
            [this, &startupFinished]
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
    QMetaObject::invokeMethod(
        engine_,
        [engine = engine_, topology = std::move(topology), &started, &converged, &stats, &engineError]() mutable
        {
            engine->startSimulation(std::move(topology));
            started = engine->isRunning();
            converged = engine->isConverged();
            stats = engine->statsSnapshot();
            engineError = engine->lastError();
        },
        Qt::BlockingQueuedConnection);
    startupFinished.store(true, std::memory_order_release);
    if (cancellationWatcher.joinable())
    {
        cancellationWatcher.join();
    }
    simulationRunning_ = started;
    simulationConverged_ = converged;
    latestStats_ = stats;
    lastEngineError_ = engineError;
    if (!started)
    {
        const auto flushed = flushEventRun();
        const auto ended = endEventRun();
        const auto message = lastEngineError_.isEmpty() ? QStringLiteral("仿真启动失败") : lastEngineError_;
        const auto fullMessage = flushed && ended ? message : QStringLiteral("%1；日志落盘失败：%2").arg(message, lastStoreError_);
        return failure(fullMessage, statusJson());
    }
    runtimeAvailable_ = true;
    auto data = statusJson();
    data.insert(QStringLiteral("topology_sha256"), topologySha256(topology_));
    data.insert(QStringLiteral("plugin_registration_errors"),
                stringsToJson(RouterPluginRegistry::instance().registrationErrors()));
    return success(data);
}

HeadlessCommandResult HeadlessSession::stopCommand(const QJsonObject&)
{
    if (!isRunning())
    {
        if (!flushEventRun())
        {
            return failure(lastStoreError_, statusJson());
        }
        auto data = statusJson();
        data.insert(QStringLiteral("changed"), false);
        return success(data);
    }
    lastEngineError_.clear();
    QMetaObject::invokeMethod(engine_, &SimulationEngine::stopSimulation, Qt::BlockingQueuedConnection);
    simulationRunning_ = false;
    simulationConverged_ = false;
    refreshRuntimeStatus();
    if (!flushEventRun())
    {
        return failure(lastStoreError_, statusJson());
    }
    auto data = statusJson();
    data.insert(QStringLiteral("changed"), true);
    return success(data);
}

HeadlessCommandResult HeadlessSession::waitCommand(const QJsonObject& command)
{
    QString error;
    qint64 milliseconds = 0;
    if (!readInteger(command, QStringLiteral("milliseconds"), 0, 24LL * 60 * 60 * 1000, &milliseconds, &error, true))
    {
        return failure(error);
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds && (!interruptionFlag_ || !interruptionFlag_->load(std::memory_order_relaxed)))
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    refreshRuntimeStatus();
    const auto data = QJsonObject{{QStringLiteral("requested_ms"), milliseconds},
                                  {QStringLiteral("elapsed_ms"), timer.elapsed()},
                                  {QStringLiteral("running"), isRunning()},
                                  {QStringLiteral("converged"), simulationConverged_},
                                  {QStringLiteral("stats"), statsToJson(latestStats_)}};
    return interruptionFlag_ && interruptionFlag_->load(std::memory_order_relaxed) ? failure(QStringLiteral("操作被中断"), data)
                                                                                   : success(data);
}

HeadlessCommandResult HeadlessSession::waitConvergedCommand(const QJsonObject& command)
{
    if (!isRunning())
    {
        return failure(QStringLiteral("仿真尚未运行"));
    }
    QString error;
    qint64 timeout = 30000;
    if (!readInteger(command, QStringLiteral("timeout_ms"), 1, 24LL * 60 * 60 * 1000, &timeout, &error))
    {
        return failure(error);
    }
    QElapsedTimer timer;
    timer.start();
    while (simulationRunning_ && !simulationConverged_ && timer.elapsed() < timeout &&
           (!interruptionFlag_ || !interruptionFlag_->load(std::memory_order_relaxed)))
    {
        QThread::msleep(5);
        refreshRuntimeStatus();
    }
    refreshRuntimeStatus();
    auto data = QJsonObject{{QStringLiteral("converged"), simulationConverged_},
                            {QStringLiteral("elapsed_ms"), timer.elapsed()},
                            {QStringLiteral("timeout_ms"), timeout},
                            {QStringLiteral("stats"), statsToJson(latestStats_)}};
    if (interruptionFlag_ && interruptionFlag_->load(std::memory_order_relaxed))
    {
        return failure(QStringLiteral("操作被中断"), data);
    }
    if (!simulationConverged_)
    {
        return failure(simulationRunning_ ? QStringLiteral("等待收敛超时") : QStringLiteral("仿真在收敛前停止"),
                       data);
    }
    if (!flushEventRun())
    {
        return failure(lastStoreError_, data);
    }
    data.insert(QStringLiteral("event_run_serial"), uint64ToJson(eventRunSerial_));
    data.insert(QStringLiteral("committed_event_id"), uint64ToJson(committedEventId_));
    data.insert(QStringLiteral("bmp_sqlite"), databasePath_);
    return success(data);
}

HeadlessCommandResult HeadlessSession::setRouterStateCommand(const QJsonObject& command)
{
    if (!isRunning())
    {
        return failure(QStringLiteral("仿真尚未运行"));
    }
    QString router;
    QString error;
    if (!readRequiredString(command, QStringLiteral("router"), &router, &error))
    {
        if (!readRequiredString(command, QStringLiteral("id"), &router, &error))
        {
            return failure(error);
        }
    }
    if (!topology_.routers.contains(router))
    {
        return failure(QStringLiteral("路由器不存在：%1").arg(router));
    }
    if (!command.contains(QStringLiteral("enabled")) || !command.value(QStringLiteral("enabled")).isBool())
    {
        return failure(QStringLiteral("字段 enabled 必须是布尔值"));
    }
    const auto enabled = command.value(QStringLiteral("enabled")).toBool();
    QVector<RouterSnapshot> snapshots;
    QString engineError;
    SimulationStats stats;
    QMetaObject::invokeMethod(
        engine_,
        [engine = engine_, router, enabled, &snapshots, &engineError, &stats]
        {
            snapshots = engine->routerSnapshots();
            engine->setRouterState(router, enabled);
            engineError = engine->lastError();
            stats = engine->statsSnapshot();
        },
        Qt::BlockingQueuedConnection);
    const auto current = std::find_if(snapshots.cbegin(), snapshots.cend(),
                                      [&](const RouterSnapshot& snapshot) { return snapshot.id == router; });
    const auto changed = current != snapshots.cend() && current->active != enabled;
    lastEngineError_ = engineError;
    latestStats_ = stats;
    simulationRunning_ = stats.running;
    simulationConverged_ = stats.converged;
    if (!lastEngineError_.isEmpty())
    {
        return failure(lastEngineError_);
    }
    return success(QJsonObject{{QStringLiteral("changed"), changed},
                               {QStringLiteral("router"), router},
                               {QStringLiteral("enabled"), enabled}});
}

HeadlessCommandResult HeadlessSession::toggleRouterCommand(const QJsonObject& command)
{
    if (!isRunning())
    {
        return failure(QStringLiteral("仿真尚未运行"));
    }
    QString router;
    QString error;
    if (!readRequiredString(command, QStringLiteral("router"), &router, &error))
    {
        if (!readRequiredString(command, QStringLiteral("id"), &router, &error))
        {
            return failure(error);
        }
    }
    QVector<RouterSnapshot> snapshots;
    QMetaObject::invokeMethod(engine_, [engine = engine_, &snapshots] { snapshots = engine->routerSnapshots(); },
                              Qt::BlockingQueuedConnection);
    const auto current = std::find_if(snapshots.cbegin(), snapshots.cend(),
                                      [&](const RouterSnapshot& snapshot) { return snapshot.id == router; });
    if (current == snapshots.cend())
    {
        return failure(QStringLiteral("路由器不存在：%1").arg(router));
    }
    auto forwarded = command;
    forwarded.insert(QStringLiteral("router"), router);
    forwarded.insert(QStringLiteral("enabled"), !current->active);
    return setRouterStateCommand(forwarded);
}

HeadlessCommandResult HeadlessSession::setLinkStateCommand(const QJsonObject& command)
{
    if (!isRunning())
    {
        return failure(QStringLiteral("仿真尚未运行"));
    }
    QString a;
    QString b;
    QString error;
    if (!readRequiredString(command, QStringLiteral("a"), &a, &error) ||
        !readRequiredString(command, QStringLiteral("b"), &b, &error))
    {
        return failure(error);
    }
    if (!topology_.findLink(a, b))
    {
        return failure(QStringLiteral("链路不存在：%1 - %2").arg(a, b));
    }
    if (!command.contains(QStringLiteral("enabled")) || !command.value(QStringLiteral("enabled")).isBool())
    {
        return failure(QStringLiteral("字段 enabled 必须是布尔值"));
    }
    const auto enabled = command.value(QStringLiteral("enabled")).toBool();
    const auto key = Topology::edgeKey(a, b);
    const auto changed = runtimeLinks_.value(key, topology_.findLink(a, b)->enabled) != enabled;
    QString engineError;
    SimulationStats stats;
    QMetaObject::invokeMethod(
        engine_,
        [engine = engine_, a, b, enabled, &engineError, &stats]
        {
            engine->setLinkState(a, b, enabled);
            engineError = engine->lastError();
            stats = engine->statsSnapshot();
        },
        Qt::BlockingQueuedConnection);
    lastEngineError_ = engineError;
    latestStats_ = stats;
    simulationRunning_ = stats.running;
    simulationConverged_ = stats.converged;
    if (!lastEngineError_.isEmpty())
    {
        return failure(lastEngineError_);
    }
    runtimeLinks_.insert(key, enabled);
    return success(QJsonObject{{QStringLiteral("changed"), changed},
                               {QStringLiteral("a"), a},
                               {QStringLiteral("b"), b},
                               {QStringLiteral("enabled"), enabled}});
}

HeadlessCommandResult HeadlessSession::toggleLinkCommand(const QJsonObject& command)
{
    QString a;
    QString b;
    QString error;
    if (!readRequiredString(command, QStringLiteral("a"), &a, &error) ||
        !readRequiredString(command, QStringLiteral("b"), &b, &error))
    {
        return failure(error);
    }
    const auto* link = topology_.findLink(a, b);
    if (!link)
    {
        return failure(QStringLiteral("链路不存在：%1 - %2").arg(a, b));
    }
    auto forwarded = command;
    forwarded.insert(QStringLiteral("enabled"), !runtimeLinks_.value(Topology::edgeKey(a, b), link->enabled));
    return setLinkStateCommand(forwarded);
}

HeadlessCommandResult HeadlessSession::advertisePrefixCommand(const QJsonObject& command)
{
    if (!isRunning())
    {
        return failure(QStringLiteral("仿真尚未运行"));
    }
    QString router;
    QString prefix;
    QString error;
    if (!readRequiredString(command, QStringLiteral("router"), &router, &error) ||
        !readRequiredString(command, QStringLiteral("prefix"), &prefix, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure(QStringLiteral("路由器不存在：%1").arg(router));
    }
    const auto configChanged = !runtimeOriginatedPrefixes_.value(router).contains(prefix);
    bool routerActive = false;
    bool routePresent = false;
    QString engineError;
    SimulationStats stats;
    QMetaObject::invokeMethod(
        engine_,
        [engine = engine_, router, prefix, &routerActive, &routePresent, &engineError, &stats]
        {
            const auto routers = engine->routerSnapshots();
            const auto current = std::find_if(routers.cbegin(), routers.cend(),
                                              [&](const RouterSnapshot& snapshot) { return snapshot.id == router; });
            routerActive = current != routers.cend() && current->active;
            routePresent = engine->ribSnapshot(router).localRoutes.contains(prefix);
            engine->originatePrefix(router, prefix);
            engineError = engine->lastError();
            stats = engine->statsSnapshot();
        },
        Qt::BlockingQueuedConnection);
    lastEngineError_ = engineError;
    latestStats_ = stats;
    simulationRunning_ = stats.running;
    simulationConverged_ = stats.converged;
    if (!lastEngineError_.isEmpty())
    {
        return failure(lastEngineError_);
    }
    runtimeOriginatedPrefixes_[router].insert(prefix);
    const auto routeEffective = routerActive && !routePresent;
    return success(QJsonObject{{QStringLiteral("changed"), configChanged || routeEffective},
                               {QStringLiteral("config_changed"), configChanged},
                               {QStringLiteral("route_effective"), routeEffective},
                               {QStringLiteral("router"), router},
                               {QStringLiteral("prefix"), prefix}});
}

HeadlessCommandResult HeadlessSession::withdrawPrefixCommand(const QJsonObject& command)
{
    if (!isRunning())
    {
        return failure(QStringLiteral("仿真尚未运行"));
    }
    QString router;
    QString prefix;
    QString error;
    if (!readRequiredString(command, QStringLiteral("router"), &router, &error) ||
        !readRequiredString(command, QStringLiteral("prefix"), &prefix, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure(QStringLiteral("路由器不存在：%1").arg(router));
    }
    const auto configChanged = runtimeOriginatedPrefixes_.value(router).contains(prefix);
    bool routePresent = false;
    QString engineError;
    SimulationStats stats;
    QMetaObject::invokeMethod(
        engine_,
        [engine = engine_, router, prefix, &routePresent, &engineError, &stats]
        {
            routePresent = engine->ribSnapshot(router).localRoutes.contains(prefix);
            engine->withdrawPrefix(router, prefix);
            engineError = engine->lastError();
            stats = engine->statsSnapshot();
        },
        Qt::BlockingQueuedConnection);
    lastEngineError_ = engineError;
    latestStats_ = stats;
    simulationRunning_ = stats.running;
    simulationConverged_ = stats.converged;
    if (!lastEngineError_.isEmpty())
    {
        return failure(lastEngineError_);
    }
    runtimeOriginatedPrefixes_[router].remove(prefix);
    return success(QJsonObject{{QStringLiteral("changed"), configChanged || routePresent},
                               {QStringLiteral("config_changed"), configChanged},
                               {QStringLiteral("route_effective"), routePresent},
                               {QStringLiteral("router"), router},
                               {QStringLiteral("prefix"), prefix}});
}

QJsonObject HeadlessSession::statsToJson(const SimulationStats& stats)
{
    return QJsonObject{{QStringLiteral("running"), stats.running},
                       {QStringLiteral("converged"), stats.converged},
                       {QStringLiteral("pending_events"), static_cast<qint64>(stats.pendingEvents)},
                       {QStringLiteral("delivered_messages"), uint64ToJson(stats.deliveredMessages)},
                       {QStringLiteral("elapsed_ms"), stats.elapsedMs},
                       {QStringLiteral("convergence_elapsed_ms"), stats.convergenceElapsedMs},
                       {QStringLiteral("convergence_trigger_event"), stats.convergenceTriggerEvent},
                       {QStringLiteral("convergence_trigger_context"), stats.convergenceTriggerContext}};
}

QJsonObject HeadlessSession::routeToJson(const RouteEntry& route)
{
    QJsonObject attributes{{QStringLiteral("origin"), route.attributes.origin},
                           {QStringLiteral("as_path"), uint32sToJson(route.attributes.asPath)},
                           {QStringLiteral("next_hop"), route.attributes.nextHop},
                           {QStringLiteral("local_pref"), static_cast<qint64>(route.attributes.localPref)},
                           {QStringLiteral("med"), static_cast<qint64>(route.attributes.med)},
                           {QStringLiteral("originator_id"), route.attributes.originatorId},
                           {QStringLiteral("cluster_list"), stringsToJson(route.attributes.clusterList)},
                           {QStringLiteral("communities"), stringMapToJson(route.attributes.communities)}};
    if (route.attributes.tfpVersionInfo)
    {
        attributes.insert(QStringLiteral("tfp_version_info"),
                          QJsonObject{{QStringLiteral("dependency_vector"),
                                       tfpVectorToJson(route.attributes.tfpVersionInfo->dependencyVector)},
                                      {QStringLiteral("trigger_vector"),
                                       tfpVectorToJson(route.attributes.tfpVersionInfo->triggerVector)}});
    }
    return QJsonObject{{QStringLiteral("learned_from"), route.learnedFrom},
                       {QStringLiteral("source_session"), toString(route.sourceSession)},
                       {QStringLiteral("local_origin"), route.localOrigin},
                       {QStringLiteral("source"), routeSourceName(route.source)},
                       {QStringLiteral("attributes"), attributes}};
}

QJsonObject HeadlessSession::routerSnapshotToJson(const RouterSnapshot& snapshot)
{
    return QJsonObject{{QStringLiteral("id"), snapshot.id},
                       {QStringLiteral("router_id"), snapshot.routerId},
                       {QStringLiteral("asn"), static_cast<qint64>(snapshot.asn)},
                       {QStringLiteral("active"), snapshot.active},
                       {QStringLiteral("route_reflector"), snapshot.routeReflector},
                       {QStringLiteral("best_route_count"), snapshot.bestRouteCount}};
}

QJsonObject HeadlessSession::peerSnapshotToJson(const PeerSnapshot& snapshot)
{
    return QJsonObject{{QStringLiteral("id"), snapshot.id},
                       {QStringLiteral("remote_asn"), static_cast<qint64>(snapshot.remoteAsn)},
                       {QStringLiteral("session_type"), toString(snapshot.sessionType)},
                       {QStringLiteral("relationship"), toString(snapshot.relationship)},
                       {QStringLiteral("state"), toString(snapshot.state)},
                       {QStringLiteral("enabled"), snapshot.enabled},
                       {QStringLiteral("rr_client"), snapshot.rrClient},
                       {QStringLiteral("mrai_ms"), snapshot.mraiMs}};
}

QJsonObject HeadlessSession::ribSnapshotToJson(const RibSnapshot& snapshot, const QString& prefixFilter)
{
    const auto routeMap = [&](const QHash<QString, RouteEntry>& routes)
    {
        QJsonObject result;
        auto prefixes = routes.keys();
        prefixes.sort(Qt::CaseSensitive);
        for (const auto& prefix : prefixes)
        {
            if (prefixFilter.isEmpty() || prefix == prefixFilter)
            {
                result.insert(prefix, routeToJson(routes.value(prefix)));
            }
        }
        return result;
    };

    QJsonObject adjRibIn;
    auto peerIds = snapshot.adjRibIn.keys();
    peerIds.sort(Qt::CaseSensitive);
    for (const auto& peer : peerIds)
    {
        QJsonObject routes;
        auto prefixes = snapshot.adjRibIn.value(peer).keys();
        prefixes.sort(Qt::CaseSensitive);
        for (const auto& prefix : prefixes)
        {
            if (prefixFilter.isEmpty() || prefix == prefixFilter)
            {
                routes.insert(prefix, routeToJson(snapshot.adjRibIn.value(peer).value(prefix)));
            }
        }
        if (!routes.isEmpty() || prefixFilter.isEmpty())
        {
            adjRibIn.insert(peer, routes);
        }
    }
    return QJsonObject{{QStringLiteral("router"), snapshot.router},
                       {QStringLiteral("local_routes"), routeMap(snapshot.localRoutes)},
                       {QStringLiteral("loc_rib"), routeMap(snapshot.locRib)},
                       {QStringLiteral("adj_rib_in"), adjRibIn}};
}

HeadlessCommandResult HeadlessSession::routersCommand(const QJsonObject&)
{
    QJsonArray routers;
    if (runtimeAvailable_)
    {
        QVector<RouterSnapshot> snapshots;
        QMetaObject::invokeMethod(engine_, [engine = engine_, &snapshots] { snapshots = engine->routerSnapshots(); },
                                  Qt::BlockingQueuedConnection);
        std::sort(snapshots.begin(), snapshots.end(),
                  [](const RouterSnapshot& lhs, const RouterSnapshot& rhs) { return lhs.id < rhs.id; });
        for (const auto& snapshot : snapshots)
        {
            routers.append(routerSnapshotToJson(snapshot));
        }
    }
    else
    {
        const auto topologyJson = topology_.toJson();
        routers = topologyJson.value(QStringLiteral("routers")).toArray();
    }
    return success(QJsonObject{{QStringLiteral("runtime"), runtimeAvailable_}, {QStringLiteral("routers"), routers}});
}

HeadlessCommandResult HeadlessSession::ribCommand(const QJsonObject& command)
{
    if (!runtimeAvailable_)
    {
        return requireRuntime();
    }
    QString router;
    QString error;
    if (!readRequiredString(command, QStringLiteral("router"), &router, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure(QStringLiteral("路由器不存在：%1").arg(router));
    }
    QString prefix;
    if (!readOptionalString(command, QStringLiteral("prefix"), &prefix, &error))
    {
        return failure(error);
    }
    RibSnapshot snapshot;
    QMetaObject::invokeMethod(engine_, [engine = engine_, router, &snapshot] { snapshot = engine->ribSnapshot(router); },
                              Qt::BlockingQueuedConnection);
    return success(QJsonObject{{QStringLiteral("rib"), ribSnapshotToJson(snapshot, prefix)}});
}

HeadlessCommandResult HeadlessSession::peersCommand(const QJsonObject& command)
{
    if (!runtimeAvailable_)
    {
        return requireRuntime();
    }
    QString router;
    QString error;
    if (!readRequiredString(command, QStringLiteral("router"), &router, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure(QStringLiteral("路由器不存在：%1").arg(router));
    }
    QVector<PeerSnapshot> snapshots;
    QMetaObject::invokeMethod(engine_, [engine = engine_, router, &snapshots] { snapshots = engine->peerSnapshots(router); },
                              Qt::BlockingQueuedConnection);
    std::sort(snapshots.begin(), snapshots.end(),
              [](const PeerSnapshot& lhs, const PeerSnapshot& rhs) { return lhs.id < rhs.id; });
    QJsonArray peers;
    for (const auto& snapshot : snapshots)
    {
        peers.append(peerSnapshotToJson(snapshot));
    }
    return success(QJsonObject{{QStringLiteral("router"), router}, {QStringLiteral("peers"), peers}});
}

HeadlessCommandResult HeadlessSession::pathCommand(const QJsonObject& command)
{
    if (!runtimeAvailable_)
    {
        return requireRuntime();
    }
    QString router;
    QString prefix;
    QString error;
    if (!readRequiredString(command, QStringLiteral("router"), &router, &error) ||
        !readRequiredString(command, QStringLiteral("prefix"), &prefix, &error))
    {
        return failure(error);
    }
    if (!topology_.routers.contains(router))
    {
        return failure(QStringLiteral("路由器不存在：%1").arg(router));
    }
    QStringList path;
    bool found = false;
    QMetaObject::invokeMethod(
        engine_,
        [engine = engine_, router, prefix, &path, &found]
        {
            found = engine->ribSnapshot(router).locRib.contains(prefix);
            if (found)
            {
                path = engine->pathSnapshot(router, prefix);
            }
        },
        Qt::BlockingQueuedConnection);
    return success(QJsonObject{{QStringLiteral("router"), router},
                               {QStringLiteral("prefix"), prefix},
                               {QStringLiteral("found"), found},
                               {QStringLiteral("path"), stringsToJson(path)}});
}

HeadlessCommandResult HeadlessSession::snapshotCommand(const QJsonObject& command)
{
    if (!runtimeAvailable_)
    {
        return requireRuntime();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QVector<RouterSnapshot> routerSnapshots;
    QMap<QString, RibSnapshot> ribs;
    QMap<QString, QVector<PeerSnapshot>> peersByRouter;
    QMap<QString, QMap<QString, QStringList>> pathsByRouter;
    SimulationStats capturedStats;
    QString capturedAt;
    QString storeError;
    quint64 committedEventId = 0;
    const auto canFlushStore = eventRunOpen_ && eventStore_ && eventStoreThread_.isRunning();
    QMetaObject::invokeMethod(
        engine_,
        [engine = engine_, store = eventStore_, canFlushStore, &routerSnapshots, &ribs, &peersByRouter, &pathsByRouter,
         &capturedStats, &capturedAt, &storeError, &committedEventId]
        {
            capturedAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
            capturedStats = engine->statsSnapshot();
            routerSnapshots = engine->routerSnapshots();
            for (const auto& summary : routerSnapshots)
            {
                const auto rib = engine->ribSnapshot(summary.id);
                ribs.insert(summary.id, rib);
                peersByRouter.insert(summary.id, engine->peerSnapshots(summary.id));
                auto prefixes = rib.locRib.keys();
                prefixes.sort(Qt::CaseSensitive);
                auto& paths = pathsByRouter[summary.id];
                for (const auto& prefix : prefixes)
                {
                    paths.insert(prefix, engine->pathSnapshot(summary.id, prefix));
                }
            }
            if (!canFlushStore)
            {
                storeError = QStringLiteral("日志存储线程在快照刷新前已停止");
                return;
            }
            QMetaObject::invokeMethod(
                store,
                [store, &storeError, &committedEventId]
                {
                    store->flush();
                    storeError = store->lastError();
                    committedEventId = store->committedEventId();
                },
                Qt::BlockingQueuedConnection);
        },
        Qt::BlockingQueuedConnection);
    latestStats_ = capturedStats;
    simulationRunning_ = capturedStats.running;
    simulationConverged_ = capturedStats.converged;
    lastStoreError_ = storeError;
    committedEventId_ = committedEventId;
    if (!lastStoreError_.isEmpty())
    {
        return failure(lastStoreError_, statusJson());
    }
    std::sort(routerSnapshots.begin(), routerSnapshots.end(),
              [](const RouterSnapshot& lhs, const RouterSnapshot& rhs) { return lhs.id < rhs.id; });
    QJsonArray routers;
    for (const auto& summary : routerSnapshots)
    {
        const auto& rib = ribs[summary.id];
        auto peerSnapshots = peersByRouter.value(summary.id);
        std::sort(peerSnapshots.begin(), peerSnapshots.end(),
                  [](const PeerSnapshot& lhs, const PeerSnapshot& rhs) { return lhs.id < rhs.id; });
        QJsonArray peers;
        for (const auto& peer : peerSnapshots)
        {
            peers.append(peerSnapshotToJson(peer));
        }
        QJsonObject paths;
        const auto routerPaths = pathsByRouter.value(summary.id);
        for (auto it = routerPaths.cbegin(); it != routerPaths.cend(); ++it)
        {
            paths.insert(it.key(), stringsToJson(it.value()));
        }
        auto originatedPrefixes = runtimeOriginatedPrefixes_.value(summary.id).values();
        originatedPrefixes.sort(Qt::CaseSensitive);
        routers.append(QJsonObject{{QStringLiteral("summary"), routerSnapshotToJson(summary)},
                                   {QStringLiteral("rib"), ribSnapshotToJson(rib, {})},
                                   {QStringLiteral("peers"), peers},
                                   {QStringLiteral("paths"), paths},
                                   {QStringLiteral("runtime_originated_prefixes"), stringsToJson(originatedPrefixes)}});
    }

    auto stableTopology = topology_;
    std::sort(stableTopology.links.begin(), stableTopology.links.end(), [](const LinkConfig& lhs, const LinkConfig& rhs)
              { return Topology::edgeKey(lhs.a, lhs.b) < Topology::edgeKey(rhs.a, rhs.b); });
    QJsonArray links;
    for (const auto& link : stableTopology.links)
    {
        auto object = linkToJson(link);
        object.insert(QStringLiteral("runtime_enabled"),
                      runtimeLinks_.value(Topology::edgeKey(link.a, link.b), link.enabled));
        links.append(object);
    }
    QJsonObject snapshot{{QStringLiteral("schema"), QStringLiteral("bgptester-runtime-snapshot-v1")},
                         {QStringLiteral("captured_at"), capturedAt},
                         {QStringLiteral("topology_path"), topologyPath_},
                         {QStringLiteral("topology"), stableTopology.toJson()},
                         {QStringLiteral("topology_sha256"), topologySha256(stableTopology)},
                         {QStringLiteral("stats"), statsToJson(capturedStats)},
                         {QStringLiteral("routers"), routers},
                         {QStringLiteral("links"), links},
                         {QStringLiteral("bmp_jsonl"), logFilePath_},
                         {QStringLiteral("bmp_sqlite"), databasePath_},
                         {QStringLiteral("event_run_serial"), uint64ToJson(eventRunSerial_)},
                         {QStringLiteral("committed_event_id"), uint64ToJson(committedEventId_)}};

    QString path;
    QString error;
    if (!readOptionalString(command, QStringLiteral("path"), &path, &error))
    {
        return failure(error);
    }
    if (!path.isEmpty())
    {
        path = QFileInfo(path).absoluteFilePath();
        QDir directory = QFileInfo(path).absoluteDir();
        if (!directory.exists() && !directory.mkpath(QStringLiteral(".")))
        {
            return failure(QStringLiteral("无法创建快照目录：%1").arg(directory.absolutePath()));
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
        {
            return failure(QStringLiteral("无法写入快照：%1").arg(file.errorString()));
        }
        const auto contents = QJsonDocument(snapshot).toJson(QJsonDocument::Indented);
        if (file.write(contents) != contents.size())
        {
            file.cancelWriting();
            return failure(QStringLiteral("无法完整写入快照：%1").arg(file.errorString()));
        }
        if (!file.commit())
        {
            return failure(QStringLiteral("无法提交快照：%1").arg(file.errorString()));
        }
    }
    return success(QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("snapshot"), snapshot}});
}

HeadlessCommandResult HeadlessSession::queryEventsCommand(const QJsonObject& command)
{
    QString database = databasePath_;
    QString filter;
    QString error;
    if (!readOptionalString(command, QStringLiteral("database"), &database, &error) ||
        !readOptionalString(command, QStringLiteral("filter"), &filter, &error, false))
    {
        return failure(error);
    }
    if (database.isEmpty())
    {
        return failure(QStringLiteral("尚无当前 BMP SQLite；请先执行 start 或提供 database"));
    }
    database = QFileInfo(database).absoluteFilePath();
    if (!QFileInfo::exists(database))
    {
        return failure(QStringLiteral("SQLite 历史不存在：%1").arg(database));
    }
    qint64 limit = 20000;
    if (!readInteger(command, QStringLiteral("limit"), 1, 1000000, &limit, &error))
    {
        return failure(error);
    }
    if (database == databasePath_ && !flushEventRun())
    {
        return failure(lastStoreError_, statusJson());
    }
    const auto cancelled = [this]
    { return interruptionFlag_ && interruptionFlag_->load(std::memory_order_relaxed); };
    const auto page = EventStore::queryDatabase(database, static_cast<int>(limit), filter, &error, {}, cancelled);
    if (!error.isEmpty())
    {
        return failure(error);
    }
    QJsonArray events;
    for (const auto& event : page.events)
    {
        events.append(EventStore::eventToJson(event));
    }
    QJsonObject data{{QStringLiteral("database"), database},
                     {QStringLiteral("filter"), filter},
                     {QStringLiteral("limit"), limit},
                     {QStringLiteral("total_count"), page.totalCount},
                     {QStringLiteral("filtered_count"), page.filteredCount},
                     {QStringLiteral("message_total_count"), page.messageTotalCount},
                     {QStringLiteral("filtered_message_count"), page.filteredMessageCount},
                     {QStringLiteral("database_max_event_id"), uint64ToJson(page.maxEventId)},
                     {QStringLiteral("events"), events}};
    if (database == databasePath_)
    {
        data.insert(QStringLiteral("event_run_serial"), uint64ToJson(eventRunSerial_));
        data.insert(QStringLiteral("committed_event_id"), uint64ToJson(committedEventId_));
    }
    return success(data);
}

HeadlessCommandResult HeadlessSession::queryConvergenceCommand(const QJsonObject& command)
{
    QString database = databasePath_;
    QString error;
    if (!readOptionalString(command, QStringLiteral("database"), &database, &error))
    {
        return failure(error);
    }
    if (database.isEmpty())
    {
        return failure(QStringLiteral("尚无当前 BMP SQLite；请先执行 start 或提供 database"));
    }
    database = QFileInfo(database).absoluteFilePath();
    if (!QFileInfo::exists(database))
    {
        return failure(QStringLiteral("SQLite 历史不存在：%1").arg(database));
    }
    qint64 limit = 5000;
    if (!readInteger(command, QStringLiteral("limit"), 1, 1000000, &limit, &error))
    {
        return failure(error);
    }
    if (database == databasePath_ && !flushEventRun())
    {
        return failure(lastStoreError_, statusJson());
    }
    const auto cancelled = [this]
    { return interruptionFlag_ && interruptionFlag_->load(std::memory_order_relaxed); };
    const auto page = EventStore::queryConvergenceDatabase(database, static_cast<int>(limit), &error, cancelled);
    if (!error.isEmpty())
    {
        return failure(error);
    }
    QJsonArray events;
    for (const auto& event : page.events)
    {
        events.append(EventStore::eventToJson(event));
    }
    QJsonObject data{{QStringLiteral("database"), database},
                     {QStringLiteral("limit"), limit},
                     {QStringLiteral("total_count"), page.totalCount},
                     {QStringLiteral("database_max_event_id"), uint64ToJson(page.maxEventId)},
                     {QStringLiteral("events"), events}};
    if (database == databasePath_)
    {
        data.insert(QStringLiteral("event_run_serial"), uint64ToJson(eventRunSerial_));
        data.insert(QStringLiteral("committed_event_id"), uint64ToJson(committedEventId_));
    }
    return success(data);
}

HeadlessCommandResult HeadlessSession::flushLogsCommand(const QJsonObject&)
{
    if (databasePath_.isEmpty())
    {
        return failure(QStringLiteral("尚未创建 BMP 日志"));
    }
    if (!flushEventRun())
    {
        return failure(lastStoreError_, statusJson());
    }
    return success(QJsonObject{{QStringLiteral("bmp_jsonl"), logFilePath_},
                               {QStringLiteral("bmp_sqlite"), databasePath_},
                               {QStringLiteral("run_directory"), runDirectory_},
                               {QStringLiteral("event_run_serial"), uint64ToJson(eventRunSerial_)},
                               {QStringLiteral("committed_event_id"), uint64ToJson(committedEventId_)}});
}

HeadlessCommandResult HeadlessSession::exitCommand(const QJsonObject&)
{
    auto result = success(statusJson());
    result.exitRequested = true;
    return result;
}

} // namespace bgptester
