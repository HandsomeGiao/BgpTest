#include "engine/SimulationEngine.hpp"
#include "headless/HeadlessSession.hpp"
#include "model/CanonicalJson.hpp"
#include "model/DeterministicRandom.hpp"
#include "model/StrictIpv4.hpp"
#include "persistence/EventStore.hpp"
#include "persistence/SimulationEventCodec.hpp"
#include "plugin/RouterPluginRegistry.hpp"
#include "router_plugins/StandardBgpRouterPlugin.hpp"
#include "router_plugins/TfpVersionRouterPlugin.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{

using namespace bgptester;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate())
        {
            return true;
        }
        QThread::msleep(2);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

QJsonArray versionVectorToJson(const TfpVersionVector& vector)
{
    QJsonArray result;
    for (auto it = vector.cbegin(); it != vector.cend(); ++it)
    {
        result.append(QJsonObject{{QStringLiteral("asn"), static_cast<qint64>(it.key().asn)},
                                  {QStringLiteral("entity"), it.key().entityId},
                                  {QStringLiteral("version"), QString::number(it.value())}});
    }
    return result;
}

QJsonObject routeToCanonicalJson(const RouteEntry& route)
{
    QJsonArray asPath;
    for (const auto asn : route.attributes.asPath)
    {
        asPath.append(static_cast<qint64>(asn));
    }
    QJsonArray clusters;
    for (const auto& cluster : route.attributes.clusterList)
    {
        clusters.append(cluster);
    }
    QJsonObject communities;
    for (auto it = route.attributes.communities.cbegin(); it != route.attributes.communities.cend(); ++it)
    {
        communities.insert(it.key(), it.value());
    }
    QJsonObject attributes{{QStringLiteral("origin"), route.attributes.origin},
                           {QStringLiteral("as_path"), asPath},
                           {QStringLiteral("next_hop"), route.attributes.nextHop},
                           {QStringLiteral("local_pref"), static_cast<qint64>(route.attributes.localPref)},
                           {QStringLiteral("med"), static_cast<qint64>(route.attributes.med)},
                           {QStringLiteral("originator_id"), route.attributes.originatorId},
                           {QStringLiteral("cluster_list"), clusters},
                           {QStringLiteral("communities"), communities}};
    if (route.attributes.tfpVersionInfo)
    {
        attributes.insert(QStringLiteral("tfp"),
                          QJsonObject{{QStringLiteral("dependency"),
                                       versionVectorToJson(route.attributes.tfpVersionInfo->dependencyVector)},
                                      {QStringLiteral("trigger"),
                                       versionVectorToJson(route.attributes.tfpVersionInfo->triggerVector)}});
    }
    return QJsonObject{{QStringLiteral("attributes"), attributes},
                       {QStringLiteral("learned_from"), route.learnedFrom},
                       {QStringLiteral("source_session"), static_cast<int>(route.sourceSession)},
                       {QStringLiteral("local_origin"), route.localOrigin},
                       {QStringLiteral("source"), static_cast<int>(route.source)}};
}

void appendCanonicalRoutes(QByteArray& output, const QByteArray& scope, const QHash<QString, RouteEntry>& routes)
{
    auto prefixes = routes.keys();
    prefixes.sort(Qt::CaseSensitive);
    output += scope;
    output += "|COUNT|";
    output += QByteArray::number(prefixes.size());
    output += '\n';
    for (const auto& prefix : prefixes)
    {
        output += scope;
        output += '|';
        output += prefix.toUtf8();
        output += '|';
        output += QJsonDocument(routeToCanonicalJson(routes.value(prefix))).toJson(QJsonDocument::Compact);
        output += '\n';
    }
}

QByteArray canonicalOutcome(SimulationEngine& engine, const QVector<SimulationEvent>& events)
{
    QByteArray output;
    for (const auto& event : events)
    {
        output += "EVENT|";
        output += QJsonDocument(SimulationEventCodec::toJson(event)).toJson(QJsonDocument::Compact);
        output += '\n';
    }

    auto routers = engine.routerSnapshots();
    std::sort(routers.begin(), routers.end(),
              [](const RouterSnapshot& lhs, const RouterSnapshot& rhs) { return lhs.id < rhs.id; });
    for (const auto& router : routers)
    {
        output += "ROUTER|";
        output += router.id.toUtf8();
        output += '|';
        output += router.routerId.toUtf8();
        output += '|';
        output += QByteArray::number(router.asn);
        output += '|';
        output += QByteArray::number(router.active);
        output += '|';
        output += QByteArray::number(router.routeReflector);
        output += '|';
        output += QByteArray::number(router.bestRouteCount);
        output += '\n';
        const auto rib = engine.ribSnapshot(router.id);
        const auto routerScope = QByteArray("RIB|") + router.id.toUtf8();
        appendCanonicalRoutes(output, routerScope + "|LOCAL", rib.localRoutes);
        appendCanonicalRoutes(output, routerScope + "|BEST", rib.locRib);
        const auto peers = engine.peerSnapshots(router.id);
        QSet<QString> peerIdSet;
        for (const auto& peer : peers)
        {
            peerIdSet.insert(peer.id);
            output += "PEER|";
            output += router.id.toUtf8();
            output += '|';
            output += peer.id.toUtf8();
            output += '|';
            output += QByteArray::number(peer.remoteAsn);
            output += '|';
            output += QByteArray::number(static_cast<int>(peer.sessionType));
            output += '|';
            output += QByteArray::number(peer.rrClient);
            output += '|';
            output += QByteArray::number(peer.enabled);
            output += '|';
            output += QByteArray::number(peer.mraiMs);
            output += '|';
            output += QByteArray::number(static_cast<int>(peer.state));
            output += '|';
            output += QByteArray::number(static_cast<int>(peer.relationship));
            output += '\n';
        }
        for (const auto& peerId : rib.adjRibIn.keys())
        {
            peerIdSet.insert(peerId);
        }
        auto peerIds = peerIdSet.values();
        peerIds.sort(Qt::CaseSensitive);
        for (const auto& peerId : peerIds)
        {
            appendCanonicalRoutes(output, routerScope + "|ADJ|" + peerId.toUtf8(), rib.adjRibIn.value(peerId));
        }
    }

    const auto stats = engine.statsSnapshot();
    output += "STATS|";
    output += QByteArray::number(stats.running);
    output += '|';
    output += QByteArray::number(stats.converged);
    output += '|';
    output += QByteArray::number(stats.pendingEvents);
    output += '|';
    output += QByteArray::number(stats.deliveredMessages);
    output += '|';
    output += QByteArray::number(stats.elapsedMs);
    output += '\n';
    return output;
}

Topology deterministicDiamondTopology(bool useTfp = false)
{
    Topology topology;
    topology.simulation.name = QStringLiteral("determinism-regression");
    topology.simulation.convergenceQuietMs = 20;
    for (int index = 1; index <= 4; ++index)
    {
        RouterConfig router;
        router.id = QStringLiteral("R%1").arg(index);
        router.routerId = QStringLiteral("10.20.0.%1").arg(index);
        router.clusterId = router.routerId;
        router.asn = 65200U + static_cast<quint32>(index);
        router.pluginId = useTfp ? TfpVersionRouterPluginId : StandardRouterPluginId;
        if (useTfp)
        {
            router.pluginSettings.insert(QStringLiteral("initial_version"), QStringLiteral("1000"));
        }
        if (index == 1)
        {
            for (int prefix = 0; prefix < 48; ++prefix)
            {
                router.originatedPrefixes.append(QStringLiteral("198.51.%1.0/24").arg(prefix));
            }
        }
        topology.routers.insert(router.id, router);
    }
    topology.links = {
        LinkConfig{.a = QStringLiteral("R1"), .b = QStringLiteral("R2"), .delayMs = 2, .mraiMsFromA = 7},
        LinkConfig{.a = QStringLiteral("R1"), .b = QStringLiteral("R3"), .delayMs = 2, .mraiMsFromA = 7},
        LinkConfig{.a = QStringLiteral("R2"), .b = QStringLiteral("R4"), .delayMs = 3, .mraiMsFromA = 11},
        LinkConfig{.a = QStringLiteral("R3"), .b = QStringLiteral("R4"), .delayMs = 3, .mraiMsFromA = 11},
    };
    return topology;
}

struct DeterminismRun
{
    QByteArray canonical;
    int statsEmissions = 0;
    int eventBatches = 0;
};

DeterminismRun runDeterminismScenario(qsizetype processingQuantum, int startupBlockMs, int eventConsumerBlockMs,
                                      bool synchronousDrain = false, bool useTfp = false)
{
    const auto topology = deterministicDiamondTopology(useTfp);
    QVector<SimulationEvent> events;
    SimulationEngine engine(nullptr, processingQuantum);
    int statsEmissions = 0;
    int eventBatches = 0;
    QObject::connect(&engine, &SimulationEngine::statsChanged, [&](const SimulationStats&) { ++statsEmissions; });
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& batch)
                     {
                         ++eventBatches;
                         events += batch;
                         if (eventConsumerBlockMs > 0)
                         {
                             QThread::msleep(static_cast<unsigned long>(eventConsumerBlockMs));
                         }
                     });
    const auto settle = [&](const char* error)
    {
        if (synchronousDrain)
        {
            require(engine.runUntilConverged(), "synchronous deterministic drain failed");
        }
        require(waitFor([&] { return engine.isConverged(); }, 10000), error);
    };
    engine.startSimulation(topology);
    if (startupBlockMs > 0)
    {
        QThread::msleep(static_cast<unsigned long>(startupBlockMs));
    }
    settle("determinism scenario did not initially converge");

    engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
    settle("determinism scenario did not converge after link down");
    engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), true);
    settle("determinism scenario did not converge after link up");
    engine.withdrawPrefix(QStringLiteral("R1"), QStringLiteral("198.51.0.0/24"));
    settle("determinism scenario did not converge after withdrawal");
    engine.originatePrefix(QStringLiteral("R1"), QStringLiteral("198.51.0.0/24"));
    settle("determinism scenario did not converge after re-advertisement");

    const auto result = canonicalOutcome(engine, events);
    engine.stopSimulation();
    return DeterminismRun{.canonical = result, .statsEmissions = statsEmissions, .eventBatches = eventBatches};
}

QByteArray runBudgetFaultScenario(qsizetype processingQuantum)
{
    QVector<SimulationEvent> events;
    constexpr quint64 budget = 256;
    const auto topology = deterministicDiamondTopology(false);
    SimulationEngine engine(nullptr, processingQuantum, budget);
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& batch) { events += batch; });
    engine.startSimulation(topology);
    require(engine.runUntilConverged(), "fixed-budget scenario did not reach its initial stable boundary");
    for (int cycle = 0; cycle < 129; ++cycle)
    {
        engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
        engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), true);
    }
    engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
    require(!engine.runUntilConverged() && !engine.isConverged(), "stale-event budget scenario unexpectedly converged");
    const auto error = engine.lastError();
    require(error.contains(QStringLiteral("事件预算")), "stale-event scenario did not report budget exhaustion");
    engine.stopSimulation();
    require(!engine.isRunning(), "fixed-budget scenario did not stop");
    const auto faultCanonical = canonicalOutcome(engine, events);

    events.clear();
    engine.startSimulation(topology);
    require(engine.runUntilConverged(), "reused engine did not recover from its stopped budget fault");
    const auto recoveredCanonical = canonicalOutcome(engine, events);
    engine.stopSimulation();

    QVector<SimulationEvent> freshEvents;
    SimulationEngine fresh(nullptr, processingQuantum, budget);
    QObject::connect(&fresh, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& batch) { freshEvents += batch; });
    fresh.startSimulation(topology);
    require(fresh.runUntilConverged(), "fresh engine did not converge in budget recovery comparison");
    const auto freshCanonical = canonicalOutcome(fresh, freshEvents);
    fresh.stopSimulation();
    require(recoveredCanonical == freshCanonical, "stop/start did not fully recover a budget-faulted engine");

    return QByteArrayLiteral("ERROR|") + error.toUtf8() + QByteArrayLiteral("\nFAULT\n") + faultCanonical +
           QByteArrayLiteral("RECOVERY\n") + recoveredCanonical;
}

Topology twoRouterTopology(int mraiMs = 0, bool sameAs = false)
{
    Topology topology;
    topology.simulation.name = QStringLiteral("core-test");
    topology.simulation.convergenceQuietMs = 20;
    RouterConfig r1{.id = QStringLiteral("R1"),
                    .routerId = QStringLiteral("10.0.0.1"),
                    .asn = 65001,
                    .clusterId = QStringLiteral("10.0.0.1"),
                    .originatedPrefixes = {QStringLiteral("203.0.113.0/24")},
                    .position = QPointF(100, 100),
                    .pluginId = StandardRouterPluginId,
                    .pluginSettings = {}};
    RouterConfig r2{.id = QStringLiteral("R2"),
                    .routerId = QStringLiteral("10.0.0.2"),
                    .asn = sameAs ? 65001U : 65002U,
                    .clusterId = QStringLiteral("10.0.0.2"),
                    .originatedPrefixes = {},
                    .position = QPointF(300, 100),
                    .pluginId = StandardRouterPluginId,
                    .pluginSettings = {}};
    topology.routers.insert(r1.id, r1);
    topology.routers.insert(r2.id, r2);
    topology.links.append(LinkConfig{.a = r1.id, .b = r2.id, .enabled = true, .delayMs = 2, .mraiMsFromA = mraiMs});
    return topology;
}

void topologyRoundTripPreservesDirectionalFields()
{
    auto topology = twoRouterTopology(123, true);
    topology.links[0].rrClientFromA = true;
    topology.links[0].rrClientFromB = false;
    topology.links[0].mraiMsFromB = 456;
    topology.routers[QStringLiteral("R1")].pluginId = QStringLiteral("org.example.round-trip");
    topology.routers[QStringLiteral("R1")].pluginSettings = QJsonObject{{QStringLiteral("preference"), 321}};
    QString error;
    const auto parsed = Topology::fromJson(topology.toJson(), &error);
    require(parsed.has_value(), "topology JSON round trip failed");
    require(error.isEmpty(), "topology round trip returned an error");
    require(parsed->links.size() == 1, "round trip changed link count");
    const auto& link = parsed->links.front();
    require(link.rrClientFromA && !link.rrClientFromB, "round trip lost directional RR flags");
    require(link.mraiMsFromA == 123 && link.mraiMsFromB == 456, "round trip lost directional MRAI values");
    const auto& router = parsed->routers.value(QStringLiteral("R1"));
    require(router.pluginId == QStringLiteral("org.example.round-trip") &&
                router.pluginSettings.value(QStringLiteral("preference")).toInt() == 321,
            "round trip lost router plugin configuration");
}

void topologyValidationRejectsBrokenData()
{
    auto topology = twoRouterTopology();
    topology.routers[QStringLiteral("R2")].routerId = topology.routers.value(QStringLiteral("R1")).routerId;
    topology.links.append(topology.links.front());
    const auto problems = topology.validate();
    require(problems.size() >= 2, "validation did not reject duplicate router id and link");
}

void strictIpv4ParsingIsPlatformIndependent()
{
    require(isCanonicalIpv4Address(QStringLiteral("192.0.2.1")) &&
                isCanonicalIpv4Prefix(QStringLiteral("192.0.2.0/24")),
            "canonical IPv4 input was rejected");
    require(!isCanonicalIpv4Address(QStringLiteral("192.0.2.01")) &&
                !isCanonicalIpv4Address(QStringLiteral("127.1")) &&
                !isCanonicalIpv4Address(QStringLiteral("+192.0.2.1")) &&
                !isCanonicalIpv4Prefix(QStringLiteral("192.0.2.0/024")) &&
                !isCanonicalIpv4Prefix(QStringLiteral("192.0.2.0/+24")),
            "non-canonical IPv4 syntax was accepted");
}

void deterministicRandomHasStableAlgorithm()
{
    DeterministicRandom random(12345);
    const QVector<int> expected{88, 31, 4, 13, 57, 45, 37, 35};
    for (const auto value : expected)
    {
        require(random.boundedInclusive(0, 100) == value, "deterministic random algorithm changed");
    }
    require(DeterministicRandom::seedFromBytes(QByteArrayLiteral("bgptester-determinism")) == 16903449300306930449ULL,
            "deterministic input-derived seed changed");
}

void canonicalJsonEncodingHasStableFormat()
{
    QJsonObject first;
    first.insert(QStringLiteral("b"), true);
    first.insert(QStringLiteral("a"), 1.0);
    QJsonObject second;
    second.insert(QStringLiteral("a"), 1.0);
    second.insert(QStringLiteral("b"), true);

    const auto encoded = canonicalJsonEncoding(first);
    require(encoded == canonicalJsonEncoding(second), "canonical JSON encoding depended on object insertion order");
    require(encoded.toHex() == QByteArrayLiteral(
                                     "42434a014f02000000000000000100000000000000610044000000000000f03f"
                                     "010000000000000062004201"),
            "canonical JSON encoding version changed");
    require(canonicalJsonEncoding(QJsonValue(-0.0)) == canonicalJsonEncoding(QJsonValue(0.0)),
            "canonical JSON encoding did not normalize negative zero");
}

void eventTimestampRoundTripPreservesUtc()
{
    SimulationEvent event;
    event.timestamp = QDateTime::fromMSecsSinceEpoch(SimulationEpochMilliseconds + 12345, QTimeZone::UTC);
    event.event = QStringLiteral("timestamp_test");
    const auto json = QJsonDocument(SimulationEventCodec::toJson(event)).toJson(QJsonDocument::Compact);
    require(json.contains("2000-01-01T00:00:12.345Z"), "event timestamp did not encode an explicit UTC zone");
    QString error;
    const auto decoded = SimulationEventCodec::fromJson(json, &error);
    require(decoded && error.isEmpty(), "UTC event timestamp did not decode");
    require(decoded->timestamp.toMSecsSinceEpoch() == event.timestamp.toMSecsSinceEpoch() && decoded->timestamp.offsetFromUtc() == 0,
            "event timestamp changed across UTC serialization");

    auto legacyObject = SimulationEventCodec::toJson(event);
    legacyObject.insert(QStringLiteral("timestamp"), QStringLiteral("2000-01-01 00:00:12.345"));
    const auto legacyDecoded =
        SimulationEventCodec::fromJson(QJsonDocument(legacyObject).toJson(QJsonDocument::Compact), &error);
    require(legacyDecoded && error.isEmpty(), "legacy UTC event timestamp did not decode");
    require(legacyDecoded->timestamp.toMSecsSinceEpoch() == event.timestamp.toMSecsSinceEpoch() &&
                legacyDecoded->timestamp.offsetFromUtc() == 0,
            "legacy timestamp interpretation depends on the machine time zone");
}

void discreteEventClockIgnoresWallTime()
{
    auto topology = twoRouterTopology(10);
    QVector<SimulationEvent> events;
    SimulationEngine engine;
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& batch) { events += batch; });

    engine.startSimulation(topology);
    QThread::msleep(40);
    require(engine.statsSnapshot().elapsedMs == 0, "wall-clock blocking advanced virtual simulation time");
    require(waitFor([&] { return engine.isConverged(); }, 1500), "discrete clock topology did not converge");

    const auto stats = engine.statsSnapshot();
    require(stats.elapsedMs == 32, "virtual clock did not follow the exact MRAI + link-delay + quiet timeline");
    require(stats.pendingEvents == 0 && stats.deliveredMessages > 0, "discrete clock finished with pending work");

    qint64 previousSimulationTime = -1;
    bool sawExpectedUpdate = false;
    bool sawExpectedConvergence = false;
    for (const auto& event : events)
    {
        bool timeOk = false;
        const auto simulationTime = event.details.value(QStringLiteral("simulation_time_ms")).toLongLong(&timeOk);
        require(timeOk && simulationTime >= previousSimulationTime, "event simulation timestamps are not monotonic");
        previousSimulationTime = simulationTime;
        if (event.from == QStringLiteral("R1") && event.to == QStringLiteral("R2") && event.action == QStringLiteral("UPDATE"))
        {
            sawExpectedUpdate = simulationTime == 12;
        }
        if (event.event == QStringLiteral("converged"))
        {
            sawExpectedConvergence = simulationTime == 32 &&
                                     event.details.value(QStringLiteral("completed_at_ms")) == QStringLiteral("12") &&
                                     event.details.value(QStringLiteral("confirmed_at_ms")) == QStringLiteral("32");
        }
    }
    require(sawExpectedUpdate, "MRAI/link-delay UPDATE arrived at the wrong virtual time");
    require(sawExpectedConvergence, "convergence confirmation arrived at the wrong virtual time");
    engine.stopSimulation();
}

void repeatedRunsAreStrictlyDeterministic()
{
    const auto baseline = runDeterminismScenario(SimulationEngine::DefaultProcessingQuantum, 0, 0);
    const auto singleEvent = runDeterminismScenario(1, 35, 1);
    require(singleEvent.canonical == baseline.canonical,
            "single-event dispatch with wall-clock stalls changed the canonical result");
    require(singleEvent.statsEmissions > baseline.statsEmissions && singleEvent.eventBatches > baseline.eventBatches,
            "processing quantum did not exercise a different dispatch path");
    require(runDeterminismScenario(7, 0, 2).canonical == baseline.canonical,
            "small dispatch batches with a slow event consumer changed the canonical result");
    require(runDeterminismScenario(3, 0, 0, true).canonical == baseline.canonical,
            "synchronous deterministic drain changed the canonical result");
    const auto tfpBaseline = runDeterminismScenario(SimulationEngine::DefaultProcessingQuantum, 0, 0, false, true);
    require(runDeterminismScenario(1, 25, 1, false, true).canonical == tfpBaseline.canonical,
            "TFP result changed with single-event dispatch and wall-clock stalls");
    for (int repetition = 0; repetition < 3; ++repetition)
    {
        require(runDeterminismScenario(SimulationEngine::DefaultProcessingQuantum, 10 * repetition, 0).canonical == baseline.canonical,
                "repeated deterministic simulation produced a different canonical result");
    }
}

void concurrentSubprocessesRemainDeterministicUnderLoad()
{
    const QVector<QStringList> variants{
        {QStringLiteral("--determinism-report"), QStringLiteral("1"), QStringLiteral("60"), QStringLiteral("2")},
        {QStringLiteral("--determinism-report"), QStringLiteral("3"), QStringLiteral("0"), QStringLiteral("3")},
        {QStringLiteral("--determinism-report"), QStringLiteral("7"), QStringLiteral("25"), QStringLiteral("1")},
        {QStringLiteral("--determinism-report"), QString::number(SimulationEngine::DefaultProcessingQuantum),
         QStringLiteral("0"), QStringLiteral("0")},
    };
    std::vector<std::unique_ptr<QProcess>> processes;
    processes.reserve(static_cast<size_t>(variants.size()));
    for (qsizetype index = 0; index < variants.size(); ++index)
    {
        auto process = std::make_unique<QProcess>();
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("TZ"), index % 2 == 0 ? QStringLiteral("UTC") : QStringLiteral("Asia/Shanghai"));
        if (index % 2 == 0)
        {
            environment.remove(QStringLiteral("QT_HASH_SEED"));
        }
        else
        {
            environment.insert(QStringLiteral("QT_HASH_SEED"), QStringLiteral("0"));
        }
        process->setProcessEnvironment(environment);
        process->setProgram(QCoreApplication::applicationFilePath());
        process->setArguments(variants.at(index));
        process->start();
        require(process->waitForStarted(10000), "determinism subprocess did not start");
        processes.push_back(std::move(process));
    }

    QByteArray reference;
    for (auto& process : processes)
    {
        require(process->waitForFinished(60000), "determinism subprocess did not finish");
        require(process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0,
                QStringLiteral("determinism subprocess failed: %1")
                    .arg(QString::fromUtf8(process->readAllStandardError()))
                    .toStdString());
        const auto digest = process->readAllStandardOutput().trimmed();
        require(digest.size() == 64, "determinism subprocess returned an invalid digest");
        if (reference.isEmpty())
        {
            reference = digest;
        }
        else
        {
            require(digest == reference, "concurrent CPU contention changed the deterministic digest");
        }
    }
}

void convergenceEventBudgetIsExactAndDeterministic()
{
    struct BudgetOutcome
    {
        QByteArray canonical;
        bool drained = false;
        bool converged = false;
        QString error;
        quint64 processedEvents = 0;
    };
    const auto run = [](qsizetype quantum, quint64 budget)
    {
        QVector<SimulationEvent> events;
        SimulationEngine engine(nullptr, quantum, budget);
        QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                         [&](const QVector<SimulationEvent>& batch) { events += batch; });
        engine.startSimulation(twoRouterTopology(10));
        const auto drained = engine.runUntilConverged();
        const auto canonical = canonicalOutcome(engine, events);
        const auto statsBeforeWallWait = engine.statsSnapshot();
        const auto eventCountBeforeWallWait = events.size();
        QThread::msleep(10);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        require(canonicalOutcome(engine, events) == canonical && events.size() == eventCountBeforeWallWait &&
                    engine.statsSnapshot().pendingEvents == statsBeforeWallWait.pendingEvents,
                "budget-faulted simulation continued after its deterministic boundary");
        if (!drained)
        {
            const auto faultError = engine.lastError();
            engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
            engine.setRouterState(QStringLiteral("R1"), false);
            engine.originatePrefix(QStringLiteral("R1"), QStringLiteral("198.51.100.0/24"));
            engine.withdrawPrefix(QStringLiteral("R1"), QStringLiteral("203.0.113.0/24"));
            require(!engine.runUntilConverged() && engine.lastError() == faultError &&
                        canonicalOutcome(engine, events) == canonical,
                    "budget-faulted simulation accepted a control mutation before stop/start recovery");
        }
        quint64 processedEvents = 0;
        for (const auto& event : events)
        {
            if (event.event == QStringLiteral("converged") || event.event == QStringLiteral("convergence_failed"))
            {
                processedEvents = event.details.value(QStringLiteral("processed_events")).toULongLong();
            }
        }
        const BudgetOutcome outcome{.canonical = canonical,
                                    .drained = drained,
                                    .converged = engine.isConverged(),
                                    .error = engine.lastError(),
                                    .processedEvents = processedEvents};
        engine.stopSimulation();
        require(!engine.isRunning(), "stop did not terminate a budget-faulted simulation");
        if (!drained)
        {
            require(events.size() >= 2 && events.at(events.size() - 2).event == QStringLiteral("simulation_aborted") &&
                        events.back().event == QStringLiteral("simulation_stopped"),
                    "budget-faulted stop did not record a deterministic abort before stopping");
        }
        return outcome;
    };

    const auto baseline = run(3, SimulationEngine::DefaultConvergenceEventBudget);
    require(baseline.drained && baseline.converged && baseline.processedEvents > 1,
            "baseline did not expose a usable convergence event count");
    const auto exact = run(1, baseline.processedEvents);
    require(exact.drained && exact.converged && exact.canonical == baseline.canonical,
            "the event that exactly exhausted the budget did not converge deterministically");

    const auto faultBudget = baseline.processedEvents - 1;
    const auto singleEventFault = run(1, faultBudget);
    const auto batchedFault = run(7, faultBudget);
    require(!singleEventFault.drained && !singleEventFault.converged &&
                singleEventFault.processedEvents == faultBudget &&
                singleEventFault.error.contains(QStringLiteral("事件预算")),
            "event budget did not stop at its exact deterministic boundary");
    require(batchedFault.canonical == singleEventFault.canonical && batchedFault.error == singleEventFault.error,
            "event-budget failure changed with the processing quantum");
}

void controlReentryIsRejectedWithoutDeferredMutation()
{
    auto topology = twoRouterTopology();
    SimulationEngine engine(nullptr, 1);
    bool attempted = false;
    bool nestedDrainResult = true;
    int linkStateChanges = 0;
    QObject::connect(&engine, &SimulationEngine::linkStateChanged, [&] { ++linkStateChanges; });
    QObject::connect(
        &engine, &SimulationEngine::eventsGenerated, &engine,
        [&](const QVector<SimulationEvent>&)
        {
            if (attempted)
            {
                return;
            }
            attempted = true;
            engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
            nestedDrainResult = engine.runUntilConverged();
        },
        Qt::DirectConnection);
    engine.startSimulation(topology);
    require(engine.runUntilConverged(), "reentry test did not converge");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const auto peers = engine.peerSnapshots(QStringLiteral("R1"));
    require(attempted && !nestedDrainResult && linkStateChanges == 0 && !peers.isEmpty() && peers.front().enabled,
            "event callback left a hidden deferred control mutation");
    engine.stopSimulation();
}

void lifecycleReentryIsRejectedWithoutRecursion()
{
    const auto topology = twoRouterTopology();
    SimulationEngine engine;
    QVector<SimulationEvent> events;
    bool startCallbackAttempted = false;
    bool stopCallbackAttempted = false;
    QObject::connect(
        &engine, &SimulationEngine::runningChanged, &engine,
        [&](bool running)
        {
            if (running && !startCallbackAttempted)
            {
                startCallbackAttempted = true;
                engine.setRouterState(QStringLiteral("R1"), false);
                engine.startSimulation(topology);
            }
        },
        Qt::DirectConnection);
    QObject::connect(
        &engine, &SimulationEngine::eventsGenerated, &engine,
        [&](const QVector<SimulationEvent>& batch)
        {
            events += batch;
            if (!stopCallbackAttempted &&
                std::any_of(batch.cbegin(), batch.cend(),
                            [](const SimulationEvent& event) { return event.event == QStringLiteral("simulation_stopped"); }))
            {
                stopCallbackAttempted = true;
                engine.startSimulation(topology);
                engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
            }
        },
        Qt::DirectConnection);

    engine.startSimulation(topology);
    require(engine.runUntilConverged(), "lifecycle reentry test did not converge");
    const auto routers = engine.routerSnapshots();
    require(startCallbackAttempted &&
                std::count_if(events.cbegin(), events.cend(), [](const SimulationEvent& event)
                              { return event.event == QStringLiteral("simulation_started"); }) == 1 &&
                std::all_of(routers.cbegin(), routers.cend(), [](const RouterSnapshot& router) { return router.active; }),
            "start lifecycle callback changed or recursively restarted the simulation");

    engine.stopSimulation();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(stopCallbackAttempted && !engine.isRunning() &&
                std::count_if(events.cbegin(), events.cend(), [](const SimulationEvent& event)
                              { return event.event == QStringLiteral("simulation_stopped"); }) == 1,
            "stop lifecycle callback changed or recursively restarted the simulation");
}

void controlCallbacksCannotAdvanceTheEventQueue()
{
    SimulationEngine engine;
    engine.startSimulation(twoRouterTopology());
    require(engine.runUntilConverged(), "control callback queue-advance test did not initially converge");

    bool processAttempted = false;
    bool processInvokeSucceeded = false;
    bool convergedInsideControl = false;
    bool linkSignalObserved = false;
    QObject::connect(
        &engine, &SimulationEngine::eventsGenerated, &engine,
        [&](const QVector<SimulationEvent>& batch)
        {
            if (!processAttempted &&
                std::any_of(batch.cbegin(), batch.cend(),
                            [](const SimulationEvent& event) { return event.event == QStringLiteral("link_down"); }))
            {
                processAttempted = true;
                processInvokeSucceeded = QMetaObject::invokeMethod(&engine, "processDueEvents", Qt::DirectConnection);
                convergedInsideControl = engine.isConverged();
            }
        },
        Qt::DirectConnection);
    QObject::connect(&engine, &SimulationEngine::linkStateChanged, &engine,
                     [&](const QString&, const QString&, bool) { linkSignalObserved = true; }, Qt::DirectConnection);

    engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
    require(processAttempted && processInvokeSucceeded && linkSignalObserved && !convergedInsideControl && !engine.isConverged(),
            "a control callback advanced the event queue before the outer operation completed");
    require(engine.runUntilConverged(), "control callback queue-advance test did not subsequently converge");
    engine.stopSimulation();
}

void nestedControlEventLoopCannotLoseTheEventPump()
{
    SimulationEngine engine;
    engine.startSimulation(twoRouterTopology());
    require(engine.runUntilConverged(), "nested-loop event-pump test did not initially converge");

    bool nestedLoopRan = false;
    QObject::connect(
        &engine, &SimulationEngine::eventsGenerated, &engine,
        [&](const QVector<SimulationEvent>& batch)
        {
            if (!nestedLoopRan &&
                std::any_of(batch.cbegin(), batch.cend(),
                            [](const SimulationEvent& event) { return event.event == QStringLiteral("link_down"); }))
            {
                nestedLoopRan = true;
                QEventLoop nestedLoop;
                QTimer::singleShot(5, &nestedLoop, &QEventLoop::quit);
                nestedLoop.exec();
            }
        },
        Qt::DirectConnection);

    engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
    require(nestedLoopRan && !engine.isConverged(), "nested control callback advanced the simulation reentrantly");
    require(waitFor([&] { return engine.isConverged(); }, 1500),
            "nested control callback consumed the single-shot event-pump wakeup");
    engine.stopSimulation();
}

void restartingOneEngineReproducesTheSameRun()
{
    const auto topology = deterministicDiamondTopology(true);
    SimulationEngine engine(nullptr, 5);
    QVector<SimulationEvent> events;
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& batch) { events += batch; });
    QByteArray baseline;
    for (int repetition = 0; repetition < 2; ++repetition)
    {
        events.clear();
        engine.startSimulation(topology);
        require(engine.runUntilConverged(), "reused engine deterministic drain failed");
        require(engine.isConverged(), "reused engine did not converge");
        const auto outcome = canonicalOutcome(engine, events);
        if (repetition == 0)
        {
            baseline = outcome;
        }
        else
        {
            require(outcome == baseline, "restarting the same engine leaked state between deterministic runs");
        }
        engine.stopSimulation();
    }
}

QByteArray runHeadlessStableBoundaryScenario(const QString& logDirectory, int startupWallBlockMs, int workerThreads)
{
    HeadlessSession session;
    const auto execute = [&](QJsonObject command)
    {
        const auto result = session.execute(command);
        require(result.ok, QStringLiteral("headless command failed: %1").arg(result.error).toStdString());
        return result;
    };

    execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("set_simulation")},
                        {QStringLiteral("name"), QStringLiteral("headless-determinism")},
                        {QStringLiteral("log_directory"), logDirectory},
                        {QStringLiteral("worker_threads"), workerThreads},
                        {QStringLiteral("convergence_quiet_ms"), 20}});
    const auto randomBatch = execute(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("batch_update")},
                    {QStringLiteral("mrai"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("random")},
                                                         {QStringLiteral("min_ms"), 2},
                                                         {QStringLiteral("max_ms"), 8}}},
                    {QStringLiteral("delay"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("random")},
                                                          {QStringLiteral("min_ms"), 1},
                                                          {QStringLiteral("max_ms"), 5}}}});
    const auto started = execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("start")}});
    const auto jsonlPath = started.data.value(QStringLiteral("bmp_jsonl")).toString();
    if (startupWallBlockMs > 0)
    {
        QThread::msleep(static_cast<unsigned long>(startupWallBlockMs));
    }
    const auto initialStatus = execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("status")}});
    const auto initialEvents = execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("query_events")},
                                                    {QStringLiteral("limit"), 1}});
    const auto initialCommittedEventId = initialStatus.data.value(QStringLiteral("committed_event_id"));
    require(initialCommittedEventId == initialEvents.data.value(QStringLiteral("database_max_event_id")),
            "status returned before EventStore reached the deterministic engine boundary");

    QJsonArray controlResults;
    controlResults.append(execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("set_link_state")},
                                               {QStringLiteral("a"), QStringLiteral("R1")},
                                               {QStringLiteral("b"), QStringLiteral("R2")},
                                               {QStringLiteral("enabled"), false}})
                              .data);
    controlResults.append(execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("set_link_state")},
                                               {QStringLiteral("a"), QStringLiteral("R1")},
                                               {QStringLiteral("b"), QStringLiteral("R2")},
                                               {QStringLiteral("enabled"), true}})
                              .data);
    controlResults.append(execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("advertise_prefix")},
                                               {QStringLiteral("router"), QStringLiteral("R1")},
                                               {QStringLiteral("prefix"), QStringLiteral("203.0.113.0/24")}})
                              .data);
    controlResults.append(execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("withdraw_prefix")},
                                               {QStringLiteral("router"), QStringLiteral("R1")},
                                               {QStringLiteral("prefix"), QStringLiteral("203.0.113.0/24")}})
                              .data);

    const auto waited = execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("wait_converged")},
                                             {QStringLiteral("timeout_ms"), 1}});
    const auto rib = execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("get_rib")},
                                         {QStringLiteral("router"), QStringLiteral("R2")}});
    const auto eventQuery = execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("query_events")},
                                                {QStringLiteral("limit"), 1000}});
    const auto convergenceQuery = execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("query_convergence")},
                                                       {QStringLiteral("limit"), 100}});
    const auto flushed = execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("flush_logs")}});
    require(flushed.data.value(QStringLiteral("committed_event_id")) ==
                eventQuery.data.value(QStringLiteral("database_max_event_id")),
            "flush_logs returned before the deterministic EventStore waterline");
    const auto exited = execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("exit")}});
    require(exited.exitRequested && exited.data.value(QStringLiteral("committed_event_id")) ==
                                        eventQuery.data.value(QStringLiteral("database_max_event_id")),
            "exit did not return a stable committed boundary");

    QJsonObject canonical{{QStringLiteral("random_batch"), randomBatch.data},
                          {QStringLiteral("initial_status_stats"), initialStatus.data.value(QStringLiteral("stats"))},
                          {QStringLiteral("initial_event_waterline"), initialCommittedEventId},
                          {QStringLiteral("controls"), controlResults},
                          {QStringLiteral("stats"), waited.data.value(QStringLiteral("stats"))},
                          {QStringLiteral("rib"), rib.data},
                          {QStringLiteral("events"), eventQuery.data.value(QStringLiteral("events"))},
                          {QStringLiteral("event_total_count"), eventQuery.data.value(QStringLiteral("total_count"))},
                          {QStringLiteral("event_max_id"), eventQuery.data.value(QStringLiteral("database_max_event_id"))},
                          {QStringLiteral("convergences"), convergenceQuery.data.value(QStringLiteral("events"))},
                          {QStringLiteral("convergence_total_count"),
                           convergenceQuery.data.value(QStringLiteral("total_count"))}};
    execute(QJsonObject{{QStringLiteral("command"), QStringLiteral("stop")}});
    QFile jsonl(jsonlPath);
    require(jsonl.open(QIODevice::ReadOnly), "headless deterministic JSONL log could not be opened");
    const auto jsonlContents = jsonl.readAll();
    require(!jsonlContents.isEmpty(), "headless deterministic JSONL log is empty");
    require(!jsonlContents.contains("\r\n"), "deterministic JSONL log used a platform-native line ending");
    QString shutdownError;
    require(session.shutdown(&shutdownError),
            QStringLiteral("headless session shutdown failed: %1").arg(shutdownError).toStdString());
    return QJsonDocument(canonical).toJson(QJsonDocument::Compact) + QByteArrayLiteral("\nJSONL\n") + jsonlContents;
}

void headlessCommandsUseStableSimulationBoundaries()
{
    QTemporaryDir directory(QDir::current().filePath(QStringLiteral("headless-determinism-test-XXXXXX")));
    require(directory.isValid(), "headless deterministic test directory is unavailable");
    const auto baseline = runHeadlessStableBoundaryScenario(QDir(directory.path()).filePath(QStringLiteral("worker-1")), 0, 1);
    const auto blocked = runHeadlessStableBoundaryScenario(QDir(directory.path()).filePath(QStringLiteral("worker-4")), 50, 4);
    require(blocked == baseline,
            "headless thread scheduling, worker count, or wall-clock startup delay changed the canonical result");
}

void missingRouterPluginPreventsSimulationStart()
{
    auto topology = twoRouterTopology();
    topology.routers[QStringLiteral("R1")].pluginId = QStringLiteral("org.example.missing");
    QString error;
    SimulationEngine engine;
    QObject::connect(&engine, &SimulationEngine::errorOccurred, [&error](const QString& message) { error = message; });
    engine.startSimulation(topology);
    require(!engine.isRunning() && error.contains(QStringLiteral("org.example.missing")),
            "missing router plugin did not prevent simulation start");
}

void ebgpRoutesPropagateAndWithdraw()
{
    auto topology = twoRouterTopology();
    SimulationEngine engine;
    engine.startSimulation(topology);
    require(
        waitFor(
            [&]
            { return engine.ribSnapshot(QStringLiteral("R2")).locRib.contains(QStringLiteral("203.0.113.0/24")) && engine.isConverged(); },
            1500),
        "EBGP route did not converge");
    const auto route = engine.ribSnapshot(QStringLiteral("R2")).locRib.value(QStringLiteral("203.0.113.0/24"));
    require(route.attributes.asPath == QVector<quint32>{65001}, "EBGP export did not prepend the source ASN");
    require(route.attributes.nextHop == QStringLiteral("10.0.0.1"), "EBGP NEXT_HOP is wrong");

    engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
    require(
        waitFor(
            [&]
            { return !engine.ribSnapshot(QStringLiteral("R2")).locRib.contains(QStringLiteral("203.0.113.0/24")) && engine.isConverged(); },
            1000),
        "link down did not withdraw the learned route");
    engine.stopSimulation();
}

void routeReflectorPropagatesClientRoute()
{
    Topology topology;
    topology.simulation.name = QStringLiteral("rr-test");
    topology.simulation.convergenceQuietMs = 20;
    for (int index = 1; index <= 3; ++index)
    {
        RouterConfig router;
        router.id = QStringLiteral("R%1").arg(index);
        router.routerId = QStringLiteral("10.0.0.%1").arg(index);
        router.clusterId = router.routerId;
        router.asn = 65000;
        router.position = QPointF(index * 150, 100);
        if (index == 2)
        {
            router.originatedPrefixes.append(QStringLiteral("198.51.100.0/24"));
        }
        topology.routers.insert(router.id, router);
    }
    topology.links.append(LinkConfig{.a = QStringLiteral("R1"), .b = QStringLiteral("R2"), .delayMs = 1, .rrClientFromA = true});
    topology.links.append(LinkConfig{.a = QStringLiteral("R1"), .b = QStringLiteral("R3"), .delayMs = 1, .rrClientFromA = true});
    SimulationEngine engine;
    engine.startSimulation(topology);
    require(waitFor([&] { return engine.ribSnapshot(QStringLiteral("R3")).locRib.contains(QStringLiteral("198.51.100.0/24")); }, 1500),
            "route reflector did not propagate a client route");
    const auto route = engine.ribSnapshot(QStringLiteral("R3")).locRib.value(QStringLiteral("198.51.100.0/24"));
    require(route.learnedFrom == QStringLiteral("R1"), "reflected route came from the wrong peer");
    require(!route.attributes.originatorId.isEmpty(), "reflected route has no ORIGINATOR_ID");
    engine.stopSimulation();
}

void staleMraiAdvertisementDoesNotReappear()
{
    auto topology = twoRouterTopology(180);
    topology.routers[QStringLiteral("R1")].originatedPrefixes.clear();
    SimulationEngine engine;
    engine.startSimulation(topology);
    require(waitFor([&] { return engine.isConverged(); }, 800), "empty topology did not converge");
    engine.originatePrefix(QStringLiteral("R1"), QStringLiteral("192.0.2.0/24"));
    engine.withdrawPrefix(QStringLiteral("R1"), QStringLiteral("192.0.2.0/24"));
    require(waitFor([&] { return engine.isConverged(); }, 1000), "MRAI cancellation did not converge");
    require(!engine.ribSnapshot(QStringLiteral("R2")).locRib.contains(QStringLiteral("192.0.2.0/24")),
            "stale MRAI advertisement revived a withdrawn route");
    engine.stopSimulation();
}

void convergenceHistoryCapturesEveryCycle()
{
    auto topology = twoRouterTopology();
    QVector<SimulationEvent> convergences;
    SimulationEngine engine;
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& events)
                     {
                         for (const auto& event : events)
                         {
                             if (event.event == QStringLiteral("converged"))
                             {
                                 convergences.append(event);
                             }
                         }
                     });

    engine.startSimulation(topology);
    require(waitFor([&] { return engine.isConverged() && convergences.size() == 1; }, 1500), "initial convergence was not recorded");
    engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
    require(waitFor([&] { return engine.isConverged() && convergences.size() == 2; }, 1500), "reconvergence was not recorded");

    qint64 previousCompletion = -1;
    const QStringList expectedTriggers{QStringLiteral("simulation_started"), QStringLiteral("link_down")};
    for (qsizetype index = 0; index < convergences.size(); ++index)
    {
        const auto& event = convergences.at(index);
        bool sequenceOk = false;
        bool startOk = false;
        bool completionOk = false;
        bool confirmationOk = false;
        bool durationOk = false;
        bool simulatedDurationOk = false;
        const auto sequence = event.details.value(QStringLiteral("convergence_sequence")).toULongLong(&sequenceOk);
        const auto startedAt = event.details.value(QStringLiteral("started_at_ms")).toLongLong(&startOk);
        const auto completedAt = event.details.value(QStringLiteral("completed_at_ms")).toLongLong(&completionOk);
        const auto confirmedAt = event.details.value(QStringLiteral("confirmed_at_ms")).toLongLong(&confirmationOk);
        const auto duration = event.details.value(QStringLiteral("duration_ms")).toLongLong(&durationOk);
        const auto simulatedDuration =
            event.details.value(QStringLiteral("simulated_duration_ms")).toLongLong(&simulatedDurationOk);
        require(sequenceOk && sequence == static_cast<quint64>(index + 1), "convergence sequence is incorrect");
        require(startOk && completionOk && confirmationOk && durationOk && simulatedDurationOk &&
                    duration == completedAt - startedAt && simulatedDuration == confirmedAt - startedAt,
                "convergence virtual-time fields are inconsistent");
        require(simulatedDuration == duration + topology.simulation.convergenceQuietMs,
                "convergence confirmation did not advance by the configured quiet window");
        require(startedAt >= previousCompletion, "convergence cycles overlap unexpectedly");
        require(event.details.value(QStringLiteral("trigger_event")) == expectedTriggers.at(index),
                "convergence trigger event is incorrect");
        require(!event.details.value(QStringLiteral("trigger_context")).isEmpty(), "convergence trigger context is missing");
        previousCompletion = confirmedAt;
    }
    engine.stopSimulation();
}

void sourceRouterPluginControlsRouteExport()
{
    require(RouterPluginRegistry::instance().contains(QStringLiteral("org.bgptester.example.configurable-export")),
            "source router plugin was not registered automatically");

    auto topology = twoRouterTopology();
    auto& customRouter = topology.routers[QStringLiteral("R1")];
    customRouter.pluginId = QStringLiteral("org.bgptester.example.configurable-export");
    customRouter.pluginSettings = QJsonObject{{QStringLiteral("export_routes"), false}, {QStringLiteral("local_preference"), 250}};

    SimulationEngine engine;
    engine.startSimulation(topology);
    require(waitFor([&] { return engine.isConverged(); }, 1500), "custom router topology did not converge");
    require(engine.ribSnapshot(QStringLiteral("R1")).locRib.contains(QStringLiteral("203.0.113.0/24")),
            "custom router did not install its local route");
    require(!engine.ribSnapshot(QStringLiteral("R2")).locRib.contains(QStringLiteral("203.0.113.0/24")),
            "custom router ignored export_routes=false");
    engine.stopSimulation();

    customRouter.pluginSettings[QStringLiteral("export_routes")] = true;
    engine.startSimulation(topology);
    require(waitFor([&] { return engine.ribSnapshot(QStringLiteral("R2")).locRib.contains(QStringLiteral("203.0.113.0/24")); }, 1500),
            "custom router did not export routes when enabled");
    const auto route = engine.ribSnapshot(QStringLiteral("R2")).locRib.value(QStringLiteral("203.0.113.0/24"));
    require(route.attributes.localPref == 250, "custom router settings did not affect route attributes");
    engine.stopSimulation();

    QString startError;
    QObject::connect(&engine, &SimulationEngine::errorOccurred, [&startError](const QString& message) { startError = message; });
    customRouter.pluginSettings[QStringLiteral("export_routes")] = QStringLiteral("not-a-boolean");
    engine.startSimulation(topology);
    require(!engine.isRunning() && startError.contains(QStringLiteral("export_routes")),
            "invalid plugin configuration did not prevent simulation start");
}

void tfpVersionRouterExtendsStandardBgpRouter()
{
    auto topology = twoRouterTopology();
    auto& config = topology.routers[QStringLiteral("R1")];
    config.pluginId = TfpVersionRouterPluginId;

    QString error;
    std::unique_ptr<RouterNode> node(RouterPluginRegistry::instance().createRouterNode(config, topology, nullptr, &error));
    require(node != nullptr && error.isEmpty(), "TFP version router node could not be created");
    require(dynamic_cast<StandardBgpRouterNode*>(node.get()) != nullptr, "TFP version router node does not extend StandardBgpRouterNode");
}

Topology tfpDiamondTopology()
{
    Topology topology;
    topology.simulation.name = QStringLiteral("tfp-version-test");
    topology.simulation.convergenceQuietMs = 20;

    const auto addRouter = [&](const QString& id, const QString& routerId, quint32 asn, bool originates)
    {
        RouterConfig router;
        router.id = id;
        router.routerId = routerId;
        router.clusterId = routerId;
        router.asn = asn;
        router.pluginId = TfpVersionRouterPluginId;
        router.pluginSettings =
            QJsonObject{{QStringLiteral("entity_id"), routerId}, {QStringLiteral("initial_version"), QStringLiteral("0")}};
        if (originates)
        {
            router.originatedPrefixes.append(QStringLiteral("198.18.0.0/24"));
        }
        topology.routers.insert(id, router);
    };

    addRouter(QStringLiteral("S"), QStringLiteral("10.0.0.1"), 65001, true);
    // A and B deliberately share an ASN.  Their Entity IDs must still keep
    // their version spaces independent.
    addRouter(QStringLiteral("A"), QStringLiteral("10.0.0.2"), 65002, false);
    addRouter(QStringLiteral("B"), QStringLiteral("10.0.0.3"), 65002, false);
    addRouter(QStringLiteral("D"), QStringLiteral("10.0.0.4"), 65003, false);

    topology.links.append(LinkConfig{.a = QStringLiteral("S"), .b = QStringLiteral("A"), .delayMs = 1});
    topology.links.append(LinkConfig{.a = QStringLiteral("S"), .b = QStringLiteral("B"), .delayMs = 1});
    topology.links.append(LinkConfig{.a = QStringLiteral("A"), .b = QStringLiteral("D"), .delayMs = 5});
    topology.links.append(LinkConfig{.a = QStringLiteral("B"), .b = QStringLiteral("D"), .delayMs = 500});
    return topology;
}

void tfpVersionInfoSurvivesRouteReflection()
{
    Topology topology;
    topology.simulation.name = QStringLiteral("tfp-rr-test");
    topology.simulation.convergenceQuietMs = 20;
    for (int index = 1; index <= 3; ++index)
    {
        RouterConfig router;
        router.id = QStringLiteral("R%1").arg(index);
        router.routerId = QStringLiteral("10.10.0.%1").arg(index);
        router.clusterId = router.routerId;
        router.asn = 65100;
        router.pluginId = TfpVersionRouterPluginId;
        if (index == 2)
        {
            router.originatedPrefixes.append(QStringLiteral("198.19.0.0/24"));
        }
        topology.routers.insert(router.id, router);
    }
    topology.links.append(LinkConfig{.a = QStringLiteral("R1"), .b = QStringLiteral("R2"), .delayMs = 1, .rrClientFromA = true});
    topology.links.append(LinkConfig{.a = QStringLiteral("R1"), .b = QStringLiteral("R3"), .delayMs = 1, .rrClientFromA = true});

    SimulationEngine engine;
    engine.startSimulation(topology);
    require(waitFor([&] { return engine.ribSnapshot(QStringLiteral("R3")).locRib.contains(QStringLiteral("198.19.0.0/24")); }, 1500),
            "TFP route did not survive route reflection");
    const auto route = engine.ribSnapshot(QStringLiteral("R3")).locRib.value(QStringLiteral("198.19.0.0/24"));
    require(route.attributes.tfpVersionInfo.has_value(), "route reflector dropped TFP_VERSION_INFO");
    require(route.attributes.tfpVersionInfo->dependencyVector.contains(TfpEntity{65100, QStringLiteral("10.10.0.2")}) &&
                route.attributes.tfpVersionInfo->dependencyVector.contains(TfpEntity{65100, QStringLiteral("10.10.0.1")}),
            "reflected route is missing origin or RR entity dependency");
    engine.stopSimulation();
}

void tfpVersionPluginInvalidatesOnlyExplicitOldDependencies()
{
    require(RouterPluginRegistry::instance().contains(TfpVersionRouterPluginId), "TFP version router plugin was not registered");

    const auto prefix = QStringLiteral("198.18.0.0/24");
    const TfpEntity sourceEntity{65001, QStringLiteral("10.0.0.1")};
    const TfpEntity aEntity{65002, QStringLiteral("10.0.0.2")};
    const TfpEntity bEntity{65002, QStringLiteral("10.0.0.3")};
    bool sawVersionedWithdrawalFromA = false;
    bool sawPolicyWithdrawalFromD = false;
    bool policyWithdrawalHadTfp = false;

    SimulationEngine engine(nullptr, 1);
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& events)
                     {
                         for (const auto& event : events)
                         {
                             if (event.from == QStringLiteral("A") && event.to == QStringLiteral("D") &&
                                 event.action == QStringLiteral("WITHDRAW") && event.withdrawn.contains(prefix) &&
                                 event.details.value(QStringLiteral("tfp_trigger_vector")).contains(QStringLiteral("(65001,10.0.0.1)=")))
                             {
                                 sawVersionedWithdrawalFromA = true;
                             }
                             if (event.from == QStringLiteral("D") && event.to == QStringLiteral("B") &&
                                 event.action == QStringLiteral("WITHDRAW") && event.withdrawn.contains(prefix))
                             {
                                 sawPolicyWithdrawalFromD = true;
                                 policyWithdrawalHadTfp = event.details.contains(QStringLiteral("tfp_dependency_vector")) ||
                                                          event.details.contains(QStringLiteral("tfp_trigger_vector"));
                             }
                         }
                     });
    engine.startSimulation(tfpDiamondTopology());
    require(waitFor(
                [&]
                {
                    const auto rib = engine.ribSnapshot(QStringLiteral("D"));
                    return rib.adjRibIn.value(QStringLiteral("A")).contains(prefix) &&
                           rib.adjRibIn.value(QStringLiteral("B")).contains(prefix) && engine.isConverged();
                },
                5000),
            "TFP diamond topology did not converge with both candidate paths");

    const auto initial = engine.ribSnapshot(QStringLiteral("D"));
    const auto routeFromA = initial.adjRibIn.value(QStringLiteral("A")).value(prefix);
    const auto routeFromB = initial.adjRibIn.value(QStringLiteral("B")).value(prefix);
    require(routeFromA.attributes.tfpVersionInfo.has_value() && routeFromB.attributes.tfpVersionInfo.has_value(),
            "TFP_VERSION_INFO was not propagated on UPDATE");
    const auto& dependenciesA = routeFromA.attributes.tfpVersionInfo->dependencyVector;
    const auto& dependenciesB = routeFromB.attributes.tfpVersionInfo->dependencyVector;
    require(dependenciesA.contains(sourceEntity) && dependenciesA.contains(aEntity), "A path is missing source or A router dependency");
    require(dependenciesB.contains(sourceEntity) && dependenciesB.contains(bEntity), "B path is missing source or B router dependency");
    require(!dependenciesA.contains(bEntity) && !dependenciesB.contains(aEntity),
            "routers in the same AS incorrectly shared one version entity");

    // Advancing only A's router-level version must not stale B's path even
    // though A and B have the same ASN.
    engine.setLinkState(QStringLiteral("S"), QStringLiteral("A"), false);
    require(waitFor(
                [&]
                {
                    const auto rib = engine.ribSnapshot(QStringLiteral("D"));
                    return rib.locRib.contains(prefix) && rib.locRib.value(prefix).learnedFrom == QStringLiteral("B") &&
                           engine.isConverged();
                },
                2000),
            "A entity version incorrectly invalidated B's independent path");
    require(sawPolicyWithdrawalFromD && !policyWithdrawalHadTfp,
            "policy/split-horizon withdrawal incorrectly carried TFP version information");

    engine.setLinkState(QStringLiteral("S"), QStringLiteral("A"), true);
    require(waitFor(
                [&]
                {
                    const auto rib = engine.ribSnapshot(QStringLiteral("D"));
                    return rib.adjRibIn.value(QStringLiteral("A")).contains(prefix) && engine.isConverged();
                },
                2500),
            "A path did not recover before the targeted withdrawal test");

    sawVersionedWithdrawalFromA = false;
    engine.withdrawPrefix(QStringLiteral("S"), prefix);

    // Advance one discrete event at a time. A's short path must deliver S's
    // newer trigger before B's normal withdrawal, independent of wall time.
    bool observedStaleBIntermediateState = false;
    for (int step = 0; step < 128 && !observedStaleBIntermediateState; ++step)
    {
        require(QMetaObject::invokeMethod(&engine, "processDueEvents", Qt::DirectConnection),
                "could not advance one discrete event");
        const auto rib = engine.ribSnapshot(QStringLiteral("D"));
        observedStaleBIntermediateState =
            !rib.locRib.contains(prefix) && rib.adjRibIn.value(QStringLiteral("B")).contains(prefix);
    }
    require(observedStaleBIntermediateState,
            "TFP trigger did not invalidate B's old dependency before B's withdrawal arrived");
    require(sawVersionedWithdrawalFromA, "withdrawal did not carry a TFP trigger vector");

    require(waitFor(
                [&]
                {
                    const auto rib = engine.ribSnapshot(QStringLiteral("D"));
                    return !rib.adjRibIn.value(QStringLiteral("B")).contains(prefix) && engine.isConverged();
                },
                2000),
            "TFP topology did not finish normal withdrawal convergence");
    engine.stopSimulation();
}

void bulkUpdatesAreAggregated()
{
    constexpr int prefixCount = 4096;
    auto topology = twoRouterTopology();
    auto& prefixes = topology.routers[QStringLiteral("R1")].originatedPrefixes;
    prefixes.clear();
    prefixes.reserve(prefixCount);
    for (int index = 0; index < prefixCount; ++index)
    {
        prefixes.append(QStringLiteral("100.64.%1.%2/32").arg(index / 256).arg(index % 256));
    }

    int updateEvents = 0;
    int advertisedPrefixes = 0;
    SimulationEngine engine;
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& events)
                     {
                         for (const auto& event : events)
                         {
                             if (event.action == QStringLiteral("UPDATE"))
                             {
                                 ++updateEvents;
                                 advertisedPrefixes += event.prefixes.size();
                             }
                         }
                     });
    engine.startSimulation(topology);
    require(waitFor([&] { return engine.ribSnapshot(QStringLiteral("R2")).locRib.size() == prefixCount && engine.isConverged(); }, 10000),
            "bulk route set did not converge");
    require(advertisedPrefixes >= prefixCount, "bulk route events lost advertised prefixes");
    require(updateEvents == 1, "one advertisement flush was split into multiple UPDATE events");
    engine.stopSimulation();
}

void bulkWithdrawalsAreAggregated()
{
    constexpr int prefixCount = 4096;
    auto topology = twoRouterTopology();
    auto& prefixes = topology.routers[QStringLiteral("R1")].originatedPrefixes;
    prefixes.clear();
    prefixes.reserve(prefixCount);
    for (int index = 0; index < prefixCount; ++index)
    {
        prefixes.append(QStringLiteral("100.65.%1.%2/32").arg(index / 256).arg(index % 256));
    }
    const auto originated = prefixes;

    bool countWithdrawals = false;
    int withdrawalEvents = 0;
    int withdrawnPrefixes = 0;
    SimulationEngine engine;
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& events)
                     {
                         if (!countWithdrawals)
                         {
                             return;
                         }
                         for (const auto& event : events)
                         {
                             if (event.action == QStringLiteral("WITHDRAW"))
                             {
                                 ++withdrawalEvents;
                                 withdrawnPrefixes += event.withdrawn.size();
                             }
                         }
                     });
    engine.startSimulation(topology);
    require(waitFor([&] { return engine.ribSnapshot(QStringLiteral("R2")).locRib.size() == prefixCount && engine.isConverged(); }, 10000),
            "bulk route set did not converge before withdrawal");

    countWithdrawals = true;
    for (const auto& prefix : originated)
    {
        engine.withdrawPrefix(QStringLiteral("R1"), prefix);
    }
    require(waitFor([&] { return engine.ribSnapshot(QStringLiteral("R2")).locRib.isEmpty() && engine.isConverged(); }, 10000),
            "bulk withdrawals did not converge");
    require(withdrawnPrefixes == prefixCount, "bulk withdrawal lost prefixes");
    require(withdrawalEvents == 1, "one withdrawal flush was split into multiple UPDATE events");
    engine.stopSimulation();
}

void transientWithdrawalIsSupersededBeforeFlush()
{
    auto topology = twoRouterTopology();
    const auto prefix = topology.routers.value(QStringLiteral("R1")).originatedPrefixes.front();
    int withdrawalEvents = 0;
    bool countWithdrawals = false;
    SimulationEngine engine;
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& events)
                     {
                         if (!countWithdrawals)
                         {
                             return;
                         }
                         for (const auto& event : events)
                         {
                             if (event.action == QStringLiteral("WITHDRAW"))
                             {
                                 ++withdrawalEvents;
                             }
                         }
                     });
    engine.startSimulation(topology);
    require(waitFor([&] { return engine.ribSnapshot(QStringLiteral("R2")).locRib.contains(prefix) && engine.isConverged(); }, 1500),
            "route did not converge before transient withdrawal");

    countWithdrawals = true;
    engine.withdrawPrefix(QStringLiteral("R1"), prefix);
    engine.originatePrefix(QStringLiteral("R1"), prefix);
    require(waitFor([&] { return engine.ribSnapshot(QStringLiteral("R2")).locRib.contains(prefix) && engine.isConverged(); }, 1500),
            "superseding advertisement did not preserve the route");
    require(withdrawalEvents == 0, "superseded withdrawal was still emitted");
    engine.stopSimulation();
}

void stalePrefixDoesNotDiscardAggregatedWithdrawal()
{
    constexpr int prefixCount = 512;
    auto topology = twoRouterTopology();
    topology.links.front().delayMs = 100;
    auto& prefixes = topology.routers[QStringLiteral("R1")].originatedPrefixes;
    prefixes.clear();
    prefixes.reserve(prefixCount);
    for (int index = 0; index < prefixCount; ++index)
    {
        prefixes.append(QStringLiteral("100.66.%1.%2/32").arg(index / 256).arg(index % 256));
    }
    const auto originated = prefixes;

    SimulationEngine engine(nullptr, 1);
    QStringList deliveredWithdrawals;
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& events)
                     {
                         for (const auto& event : events)
                         {
                             if (event.from == QStringLiteral("R1") && event.to == QStringLiteral("R2") &&
                                 event.action == QStringLiteral("WITHDRAW"))
                             {
                                 deliveredWithdrawals += event.withdrawn;
                             }
                         }
                     });
    engine.startSimulation(topology);
    require(waitFor([&] { return engine.ribSnapshot(QStringLiteral("R2")).locRib.size() == prefixCount && engine.isConverged(); }, 3000),
            "delayed bulk route set did not converge");
    for (const auto& prefix : originated)
    {
        engine.withdrawPrefix(QStringLiteral("R1"), prefix);
    }

    // Process exactly the zero-delay flush so one withdrawal message is in
    // flight, then supersede only one prefix before its logical delivery time.
    require(QMetaObject::invokeMethod(&engine, "processDueEvents", Qt::DirectConnection),
            "could not advance the withdrawal flush event");
    require(deliveredWithdrawals.isEmpty() && engine.statsSnapshot().pendingEvents > 0,
            "withdrawal flush was not isolated from its delayed in-flight delivery");
    engine.originatePrefix(QStringLiteral("R1"), originated.front());
    require(waitFor(
                [&]
                {
                    const auto rib = engine.ribSnapshot(QStringLiteral("R2")).locRib;
                    return rib.size() == 1 && rib.contains(originated.front()) && engine.isConverged();
                },
                3000),
            "one stale prefix discarded valid siblings from an aggregated withdrawal");
    deliveredWithdrawals.sort(Qt::CaseSensitive);
    auto expectedWithdrawals = originated.mid(1);
    expectedWithdrawals.sort(Qt::CaseSensitive);
    require(deliveredWithdrawals == expectedWithdrawals,
            "generation filtering did not preserve every non-stale sibling in the in-flight withdrawal");
    engine.stopSimulation();
}

void eventStoreWritesJsonAndSqlite()
{
    QTemporaryDir directory(QDir::current().filePath(QStringLiteral("event-store-test-XXXXXX")));
    require(directory.isValid(), "temporary directory is unavailable");
    EventStore store;
    SimulationSettings settings;
    settings.name = QStringLiteral("event-test");
    settings.logDirectory = directory.path();
    QString error;
    if (!store.beginRun(settings, &error))
    {
        throw std::runtime_error(QStringLiteral("event store failed to start: %1").arg(error).toStdString());
    }
    SimulationEvent event;
    event.event = QStringLiteral("message_received");
    event.router = QStringLiteral("R2");
    event.from = QStringLiteral("R1");
    event.to = QStringLiteral("R2");
    event.action = QStringLiteral("UPDATE");
    event.messageType = QStringLiteral("UPDATE");
    event.prefixes = {QStringLiteral("203.0.113.0/24")};
    event.fromAs = 65001;
    event.toAs = 65002;
    event.details.insert(QStringLiteral("duration_ms"), QStringLiteral("37"));
    store.appendEvent(event);
    store.flush();
    const auto databasePath = store.databasePath();
    const auto logPath = store.logFilePath();
    store.endRun();
    require(QFileInfo::exists(databasePath), "SQLite log file was not created");
    require(QFileInfo(logPath).size() > 0, "JSONL log file is empty");
    const auto history = EventStore::readDatabase(databasePath, 10, &error);
    require(error.isEmpty(), "SQLite history query failed");
    require(history.size() == 1 && history.front().prefixes == QStringList{QStringLiteral("203.0.113.0/24")},
            "SQLite history did not preserve the event");
    require(history.front().details.value(QStringLiteral("duration_ms")) == QStringLiteral("37"),
            "SQLite history did not restore event details");
    require(history.front().timestamp.toMSecsSinceEpoch() == SimulationEpochMilliseconds &&
                history.front().timestamp.offsetFromUtc() == 0,
            "SQLite persistence did not assign the deterministic UTC epoch to an undated event");
}

void eventStoreWorkerQueuePersistsBatches()
{
    QTemporaryDir directory(QDir::current().filePath(QStringLiteral("event-store-worker-test-XXXXXX")));
    require(directory.isValid(), "worker event-store temporary directory is unavailable");

    QThread worker;
    auto* store = new EventStore;
    store->moveToThread(&worker);
    QObject::connect(&worker, &QThread::finished, store, &QObject::deleteLater);
    QObject receiver;
    int visibleEvents = 0;
    QObject::connect(store, &EventStore::eventsStored, &receiver,
                     [&](quint64, const QVector<SimulationEvent>& events) { visibleEvents += events.size(); });
    worker.start();

    SimulationSettings settings;
    settings.name = QStringLiteral("event-worker-test");
    settings.logDirectory = directory.path();
    settings.workerThreads = 4;
    bool started = false;
    int effectiveWorkerThreads = 0;
    QString error;
    QString databasePath;
    QMetaObject::invokeMethod(
        store,
        [&]
        {
            started = store->beginRun(settings, &error);
            databasePath = store->databasePath();
            effectiveWorkerThreads = store->encodingWorkerCount();
        },
        Qt::BlockingQueuedConnection);
    require(started, "event store worker failed to start");
    require(effectiveWorkerThreads == 4, "explicit worker count did not activate the parallel encoding branch");

    QVector<SimulationEvent> events;
    events.reserve(1000);
    for (int index = 0; index < 1000; ++index)
    {
        SimulationEvent event;
        event.event = QStringLiteral("message_received");
        event.action = QStringLiteral("UPDATE");
        event.messageType = QStringLiteral("UPDATE");
        event.prefixes = {QStringLiteral("198.18.%1.%2/32").arg(index / 256).arg(index % 256)};
        events.append(std::move(event));
    }
    store->enqueueEvents(std::move(events));
    QMetaObject::invokeMethod(store, &EventStore::flush, Qt::BlockingQueuedConnection);
    require(waitFor([&] { return visibleEvents == 1000; }, 2000), "GUI-facing stored event batches were not delivered");
    QMetaObject::invokeMethod(store, &EventStore::endRun, Qt::BlockingQueuedConnection);
    worker.quit();
    worker.wait();

    const auto history = EventStore::readDatabase(databasePath, 2000, &error);
    require(error.isEmpty() && history.size() == 1000, "worker event queue did not persist every event");
}

int runTopologyStress(const QString& path, int durationMs)
{
    QString error;
    const auto topology = Topology::load(path, &error);
    if (!topology)
    {
        std::cerr << "FAILED: " << error.toStdString() << '\n';
        return 1;
    }

    quint64 eventCount = 0;
    quint64 eventPrefixes = 0;
    quint64 updateEvents = 0;
    quint64 withdrawalEvents = 0;
    quint64 advertisedPrefixes = 0;
    quint64 withdrawnPrefixes = 0;
    SimulationStats latestStats;
    SimulationEngine engine;
    QObject::connect(&engine, &SimulationEngine::eventsGenerated,
                     [&](const QVector<SimulationEvent>& events)
                     {
                         eventCount += static_cast<quint64>(events.size());
                         for (const auto& event : events)
                         {
                             eventPrefixes += static_cast<quint64>(event.prefixes.size() + event.withdrawn.size());
                             if (event.action == QStringLiteral("UPDATE"))
                             {
                                 ++updateEvents;
                                 advertisedPrefixes += static_cast<quint64>(event.prefixes.size());
                             }
                             else if (event.action == QStringLiteral("WITHDRAW"))
                             {
                                 ++withdrawalEvents;
                                 withdrawnPrefixes += static_cast<quint64>(event.withdrawn.size());
                             }
                         }
                     });
    QObject::connect(&engine, &SimulationEngine::statsChanged, [&](const SimulationStats& stats) { latestStats = stats; });
    engine.startSimulation(*topology);

    QElapsedTimer elapsed;
    elapsed.start();
    qint64 nextReport = 1000;
    while (engine.isRunning() && elapsed.elapsed() < durationMs && !engine.isConverged())
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
        if (elapsed.elapsed() >= nextReport)
        {
            std::cout << "stress wall_elapsed_ms=" << elapsed.elapsed() << " messages=" << latestStats.deliveredMessages
                      << " scheduled=" << latestStats.pendingEvents << " log_events=" << eventCount << " event_prefixes=" << eventPrefixes
                      << std::endl;
            nextReport += 1000;
        }
    }
    const auto converged = engine.isConverged();
    const auto snapshots = engine.routerSnapshots();
    quint64 locRibEntries = 0;
    for (const auto& snapshot : snapshots)
    {
        locRibEntries += static_cast<quint64>(snapshot.bestRouteCount);
    }
    std::cout << "stress done wall_elapsed_ms=" << elapsed.elapsed() << " converged=" << converged
              << " messages=" << latestStats.deliveredMessages << " scheduled=" << latestStats.pendingEvents << " log_events=" << eventCount
              << " event_prefixes=" << eventPrefixes << " updates=" << updateEvents << " advertised_prefixes=" << advertisedPrefixes
              << " withdrawals=" << withdrawalEvents << " withdrawn_prefixes=" << withdrawnPrefixes << " loc_rib_entries=" << locRibEntries
              << '\n';
    engine.stopSimulation();
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--determinism-report"))
    {
        try
        {
            const auto quantum = argc >= 3 ? std::max<qint64>(1, QString::fromLocal8Bit(argv[2]).toLongLong())
                                           : SimulationEngine::DefaultProcessingQuantum;
            const auto startupBlockMs = argc >= 4 ? std::max(0, QString::fromLocal8Bit(argv[3]).toInt()) : 0;
            const auto consumerBlockMs = argc >= 5 ? std::max(0, QString::fromLocal8Bit(argv[4]).toInt()) : 0;
            const auto standard = runDeterminismScenario(static_cast<qsizetype>(quantum), startupBlockMs, consumerBlockMs);
            const auto tfp = runDeterminismScenario(static_cast<qsizetype>(quantum), startupBlockMs, consumerBlockMs, false, true);
            const auto budgetFault = runBudgetFaultScenario(static_cast<qsizetype>(quantum));
            const auto canonical = standard.canonical + QByteArrayLiteral("PROTOCOL|TFP\n") + tfp.canonical +
                                   QByteArrayLiteral("SCENARIO|BUDGET_FAULT\n") + budgetFault;
            std::cout << QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex().constData() << '\n';
            return 0;
        }
        catch (const std::exception& error)
        {
            std::cerr << "FAILED: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--stress"))
    {
        const auto durationMs = argc >= 4 ? QString::fromLocal8Bit(argv[3]).toInt() : 10000;
        return runTopologyStress(QString::fromLocal8Bit(argv[2]), std::max(100, durationMs));
    }
    try
    {
        topologyRoundTripPreservesDirectionalFields();
        topologyValidationRejectsBrokenData();
        strictIpv4ParsingIsPlatformIndependent();
        deterministicRandomHasStableAlgorithm();
        canonicalJsonEncodingHasStableFormat();
        eventTimestampRoundTripPreservesUtc();
        discreteEventClockIgnoresWallTime();
        convergenceEventBudgetIsExactAndDeterministic();
        controlReentryIsRejectedWithoutDeferredMutation();
        lifecycleReentryIsRejectedWithoutRecursion();
        controlCallbacksCannotAdvanceTheEventQueue();
        nestedControlEventLoopCannotLoseTheEventPump();
        repeatedRunsAreStrictlyDeterministic();
        concurrentSubprocessesRemainDeterministicUnderLoad();
        restartingOneEngineReproducesTheSameRun();
        headlessCommandsUseStableSimulationBoundaries();
        missingRouterPluginPreventsSimulationStart();
        ebgpRoutesPropagateAndWithdraw();
        routeReflectorPropagatesClientRoute();
        staleMraiAdvertisementDoesNotReappear();
        convergenceHistoryCapturesEveryCycle();
        sourceRouterPluginControlsRouteExport();
        tfpVersionRouterExtendsStandardBgpRouter();
        tfpVersionInfoSurvivesRouteReflection();
        tfpVersionPluginInvalidatesOnlyExplicitOldDependencies();
        bulkUpdatesAreAggregated();
        bulkWithdrawalsAreAggregated();
        transientWithdrawalIsSupersededBeforeFlush();
        stalePrefixDoesNotDiscardAggregatedWithdrawal();
        eventStoreWritesJsonAndSqlite();
        eventStoreWorkerQueuePersistsBatches();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All BgpTester core tests passed.\n";
    return 0;
}
