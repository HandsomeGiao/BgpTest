#include "engine/SimulationEngine.hpp"
#include "persistence/EventStore.hpp"
#include "plugin/RouterPluginRegistry.hpp"
#include "router_plugins/TfpVersionRouterPlugin.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <functional>
#include <iostream>
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
        bool durationOk = false;
        const auto sequence = event.details.value(QStringLiteral("convergence_sequence")).toULongLong(&sequenceOk);
        const auto startedAt = event.details.value(QStringLiteral("started_at_ms")).toLongLong(&startOk);
        const auto completedAt = event.details.value(QStringLiteral("completed_at_ms")).toLongLong(&completionOk);
        const auto duration = event.details.value(QStringLiteral("duration_ms")).toLongLong(&durationOk);
        require(sequenceOk && sequence == static_cast<quint64>(index + 1), "convergence sequence is incorrect");
        require(startOk && completionOk && durationOk && duration == completedAt - startedAt,
                "convergence duration fields are inconsistent");
        require(duration >= topology.simulation.convergenceQuietMs, "convergence duration omitted the quiet window");
        require(startedAt >= previousCompletion, "convergence cycles overlap unexpectedly");
        require(event.details.value(QStringLiteral("trigger_event")) == expectedTriggers.at(index),
                "convergence trigger event is incorrect");
        require(!event.details.value(QStringLiteral("trigger_context")).isEmpty(), "convergence trigger context is missing");
        previousCompletion = completedAt;
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

    SimulationEngine engine;
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

    // A's short path delivers S's newer trigger hundreds of milliseconds
    // before B's normal withdrawal.  D must reject the still-present B
    // candidate because it explicitly depends on S's older version.
    require(waitFor(
                [&]
                {
                    const auto rib = engine.ribSnapshot(QStringLiteral("D"));
                    return !rib.locRib.contains(prefix) && rib.adjRibIn.value(QStringLiteral("B")).contains(prefix);
                },
                300),
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

    SimulationEngine engine;
    engine.startSimulation(topology);
    require(waitFor([&] { return engine.ribSnapshot(QStringLiteral("R2")).locRib.size() == prefixCount && engine.isConverged(); }, 3000),
            "delayed bulk route set did not converge");
    for (const auto& prefix : originated)
    {
        engine.withdrawPrefix(QStringLiteral("R1"), prefix);
    }

    // Let the zero-delay withdrawal flush build one in-flight message, then
    // supersede only one prefix while the message is crossing the link.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    engine.originatePrefix(QStringLiteral("R1"), originated.front());
    require(waitFor(
                [&]
                {
                    const auto rib = engine.ribSnapshot(QStringLiteral("R2")).locRib;
                    return rib.size() == 1 && rib.contains(originated.front()) && engine.isConverged();
                },
                3000),
            "one stale prefix discarded valid siblings from an aggregated withdrawal");
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
    bool started = false;
    QString error;
    QString databasePath;
    QMetaObject::invokeMethod(
        store,
        [&]
        {
            started = store->beginRun(settings, &error);
            databasePath = store->databasePath();
        },
        Qt::BlockingQueuedConnection);
    require(started, "event store worker failed to start");

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
            std::cout << "stress elapsed_ms=" << elapsed.elapsed() << " messages=" << latestStats.deliveredMessages
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
    std::cout << "stress done elapsed_ms=" << elapsed.elapsed() << " converged=" << converged
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
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--stress"))
    {
        const auto durationMs = argc >= 4 ? QString::fromLocal8Bit(argv[3]).toInt() : 10000;
        return runTopologyStress(QString::fromLocal8Bit(argv[2]), std::max(100, durationMs));
    }
    try
    {
        topologyRoundTripPreservesDirectionalFields();
        topologyValidationRejectsBrokenData();
        missingRouterPluginPreventsSimulationStart();
        ebgpRoutesPropagateAndWithdraw();
        routeReflectorPropagatesClientRoute();
        staleMraiAdvertisementDoesNotReappear();
        convergenceHistoryCapturesEveryCycle();
        sourceRouterPluginControlsRouteExport();
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
