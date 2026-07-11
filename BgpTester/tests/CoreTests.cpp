#include "engine/SimulationEngine.hpp"
#include "persistence/EventStore.hpp"
#include "plugin/RouterPluginRegistry.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>
#include <stdexcept>

namespace {

using namespace bgptester;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool waitFor(const std::function<bool()> &predicate, int timeoutMs) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (predicate()) {
      return true;
    }
    QThread::msleep(2);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return predicate();
}

Topology twoRouterTopology(int mraiMs = 0, bool sameAs = false) {
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
  topology.links.append(LinkConfig{.a = r1.id,
                                   .b = r2.id,
                                   .enabled = true,
                                   .delayMs = 2,
                                   .mraiMsFromA = mraiMs});
  return topology;
}

void topologyRoundTripPreservesDirectionalFields() {
  auto topology = twoRouterTopology(123, true);
  topology.links[0].rrClientFromA = true;
  topology.links[0].rrClientFromB = false;
  topology.links[0].mraiMsFromB = 456;
  topology.routers[QStringLiteral("R1")].pluginId =
      QStringLiteral("org.example.round-trip");
  topology.routers[QStringLiteral("R1")].pluginSettings =
      QJsonObject{{QStringLiteral("preference"), 321}};
  QString error;
  const auto parsed = Topology::fromJson(topology.toJson(), &error);
  require(parsed.has_value(), "topology JSON round trip failed");
  require(error.isEmpty(), "topology round trip returned an error");
  require(parsed->links.size() == 1, "round trip changed link count");
  const auto &link = parsed->links.front();
  require(link.rrClientFromA && !link.rrClientFromB,
          "round trip lost directional RR flags");
  require(link.mraiMsFromA == 123 && link.mraiMsFromB == 456,
          "round trip lost directional MRAI values");
  const auto &router = parsed->routers.value(QStringLiteral("R1"));
  require(router.pluginId == QStringLiteral("org.example.round-trip") &&
              router.pluginSettings.value(QStringLiteral("preference"))
                      .toInt() == 321,
          "round trip lost router plugin configuration");
}

void topologyValidationRejectsBrokenData() {
  auto topology = twoRouterTopology();
  topology.routers[QStringLiteral("R2")].routerId =
      topology.routers.value(QStringLiteral("R1")).routerId;
  topology.links.append(topology.links.front());
  const auto problems = topology.validate();
  require(problems.size() >= 2,
          "validation did not reject duplicate router id and link");
}

void missingRouterPluginPreventsSimulationStart() {
  auto topology = twoRouterTopology();
  topology.routers[QStringLiteral("R1")].pluginId =
      QStringLiteral("org.example.missing");
  QString error;
  SimulationEngine engine;
  QObject::connect(&engine, &SimulationEngine::errorOccurred,
                   [&error](const QString &message) { error = message; });
  engine.startSimulation(topology);
  require(!engine.isRunning() &&
              error.contains(QStringLiteral("org.example.missing")),
          "missing router plugin did not prevent simulation start");
}

void ebgpRoutesPropagateAndWithdraw() {
  auto topology = twoRouterTopology();
  SimulationEngine engine;
  engine.startSimulation(topology);
  require(waitFor(
              [&] {
                return engine.ribSnapshot(QStringLiteral("R2"))
                           .locRib.contains(QStringLiteral("203.0.113.0/24")) &&
                       engine.isConverged();
              },
              1500),
          "EBGP route did not converge");
  const auto route = engine.ribSnapshot(QStringLiteral("R2"))
                         .locRib.value(QStringLiteral("203.0.113.0/24"));
  require(route.attributes.asPath == QVector<quint32>{65001},
          "EBGP export did not prepend the source ASN");
  require(route.attributes.nextHop == QStringLiteral("10.0.0.1"),
          "EBGP NEXT_HOP is wrong");

  engine.setLinkState(QStringLiteral("R1"), QStringLiteral("R2"), false);
  require(waitFor(
              [&] {
                return !engine.ribSnapshot(QStringLiteral("R2"))
                            .locRib.contains(QStringLiteral("203.0.113.0/24")) &&
                       engine.isConverged();
              },
              1000),
          "link down did not withdraw the learned route");
  engine.stopSimulation();
}

void routeReflectorPropagatesClientRoute() {
  Topology topology;
  topology.simulation.name = QStringLiteral("rr-test");
  topology.simulation.convergenceQuietMs = 20;
  for (int index = 1; index <= 3; ++index) {
    RouterConfig router;
    router.id = QStringLiteral("R%1").arg(index);
    router.routerId = QStringLiteral("10.0.0.%1").arg(index);
    router.clusterId = router.routerId;
    router.asn = 65000;
    router.position = QPointF(index * 150, 100);
    if (index == 2) {
      router.originatedPrefixes.append(QStringLiteral("198.51.100.0/24"));
    }
    topology.routers.insert(router.id, router);
  }
  topology.links.append(LinkConfig{.a = QStringLiteral("R1"),
                                   .b = QStringLiteral("R2"),
                                   .delayMs = 1,
                                   .rrClientFromA = true});
  topology.links.append(LinkConfig{.a = QStringLiteral("R1"),
                                   .b = QStringLiteral("R3"),
                                   .delayMs = 1,
                                   .rrClientFromA = true});
  SimulationEngine engine;
  engine.startSimulation(topology);
  require(waitFor(
              [&] {
                return engine.ribSnapshot(QStringLiteral("R3"))
                    .locRib.contains(QStringLiteral("198.51.100.0/24"));
              },
              1500),
          "route reflector did not propagate a client route");
  const auto route = engine.ribSnapshot(QStringLiteral("R3"))
                         .locRib.value(QStringLiteral("198.51.100.0/24"));
  require(route.learnedFrom == QStringLiteral("R1"),
          "reflected route came from the wrong peer");
  require(!route.attributes.originatorId.isEmpty(),
          "reflected route has no ORIGINATOR_ID");
  engine.stopSimulation();
}

void staleMraiAdvertisementDoesNotReappear() {
  auto topology = twoRouterTopology(180);
  topology.routers[QStringLiteral("R1")].originatedPrefixes.clear();
  SimulationEngine engine;
  engine.startSimulation(topology);
  require(waitFor([&] { return engine.isConverged(); }, 800),
          "empty topology did not converge");
  engine.originatePrefix(QStringLiteral("R1"),
                         QStringLiteral("192.0.2.0/24"));
  engine.withdrawPrefix(QStringLiteral("R1"),
                        QStringLiteral("192.0.2.0/24"));
  require(waitFor([&] { return engine.isConverged(); }, 1000),
          "MRAI cancellation did not converge");
  require(!engine.ribSnapshot(QStringLiteral("R2"))
               .locRib.contains(QStringLiteral("192.0.2.0/24")),
          "stale MRAI advertisement revived a withdrawn route");
  engine.stopSimulation();
}

void dynamicRouterPluginControlsRouteExport() {
  QString error;
  const auto pluginPath =
      QDir::fromNativeSeparators(QStringLiteral(BGPTESTER_TEST_PLUGIN_PATH));
  require(RouterPluginRegistry::instance().loadPlugin(pluginPath, &error),
          error.toUtf8().constData());
  require(RouterPluginRegistry::instance().contains(
              QStringLiteral("org.bgptester.example.configurable-export")),
          "dynamic router plugin was not registered");

  auto topology = twoRouterTopology();
  auto &customRouter = topology.routers[QStringLiteral("R1")];
  customRouter.pluginId =
      QStringLiteral("org.bgptester.example.configurable-export");
  customRouter.pluginSettings =
      QJsonObject{{QStringLiteral("export_routes"), false},
                  {QStringLiteral("local_preference"), 250}};

  SimulationEngine engine;
  engine.startSimulation(topology);
  require(waitFor([&] { return engine.isConverged(); }, 1500),
          "custom router topology did not converge");
  require(engine.ribSnapshot(QStringLiteral("R1"))
              .locRib.contains(QStringLiteral("203.0.113.0/24")),
          "custom router did not install its local route");
  require(!engine.ribSnapshot(QStringLiteral("R2"))
               .locRib.contains(QStringLiteral("203.0.113.0/24")),
          "custom router ignored export_routes=false");
  engine.stopSimulation();

  customRouter.pluginSettings[QStringLiteral("export_routes")] = true;
  engine.startSimulation(topology);
  require(waitFor(
              [&] {
                return engine.ribSnapshot(QStringLiteral("R2"))
                    .locRib.contains(QStringLiteral("203.0.113.0/24"));
              },
              1500),
          "custom router did not export routes when enabled");
  const auto route = engine.ribSnapshot(QStringLiteral("R2"))
                         .locRib.value(QStringLiteral("203.0.113.0/24"));
  require(route.attributes.localPref == 250,
          "custom router settings did not affect route attributes");
  engine.stopSimulation();

  QString startError;
  QObject::connect(&engine, &SimulationEngine::errorOccurred,
                   [&startError](const QString &message) {
                     startError = message;
                   });
  customRouter.pluginSettings[QStringLiteral("export_routes")] =
      QStringLiteral("not-a-boolean");
  engine.startSimulation(topology);
  require(!engine.isRunning() &&
              startError.contains(QStringLiteral("export_routes")),
          "invalid plugin configuration did not prevent simulation start");
}

void eventStoreWritesJsonAndSqlite() {
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("event-store-test-XXXXXX")));
  require(directory.isValid(), "temporary directory is unavailable");
  EventStore store;
  SimulationSettings settings;
  settings.name = QStringLiteral("event-test");
  settings.logDirectory = directory.path();
  QString error;
  if (!store.beginRun(settings, &error)) {
    throw std::runtime_error(
        QStringLiteral("event store failed to start: %1").arg(error).toStdString());
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
  store.appendEvent(event);
  store.flush();
  const auto databasePath = store.databasePath();
  const auto logPath = store.logFilePath();
  store.endRun();
  require(QFileInfo::exists(databasePath), "SQLite log file was not created");
  require(QFileInfo(logPath).size() > 0, "JSONL log file is empty");
  const auto history = EventStore::readDatabase(databasePath, 10, &error);
  require(error.isEmpty(), "SQLite history query failed");
  require(history.size() == 1 &&
              history.front().prefixes ==
                  QStringList{QStringLiteral("203.0.113.0/24")},
          "SQLite history did not preserve the event");
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    topologyRoundTripPreservesDirectionalFields();
    topologyValidationRejectsBrokenData();
    missingRouterPluginPreventsSimulationStart();
    ebgpRoutesPropagateAndWithdraw();
    routeReflectorPropagatesClientRoute();
    staleMraiAdvertisementDoesNotReappear();
    dynamicRouterPluginControlsRouteExport();
    eventStoreWritesJsonAndSqlite();
  } catch (const std::exception &error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All BgpTester core tests passed.\n";
  return 0;
}
