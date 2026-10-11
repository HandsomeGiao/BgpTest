#include "engine/SimulationEngine.hpp"
#include "router_plugins/VersionedPathRouterPlugin.hpp"
#include <QCoreApplication>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace bgptester;
using Json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
QString qs(const std::string& value) { return QString::fromStdString(value); }
QString qs(const Json& value) { return qs(value.get<std::string>()); }
Json jsonValue(const QString& value) { return value.toStdString(); }
template<class T> Json jsonValue(const QList<T>& values)
{
    Json result = Json::array();
    for (const auto& value : values)
        if constexpr(std::is_same_v<T, QString>) result.push_back(value.toStdString());
        else result.push_back(value);
    return result;
}
Json jsonValue(const QMap<QString, QString>& values)
{
    Json result = Json::object();
    for (auto i = values.cbegin(); i != values.cend(); ++i) result[i.key().toStdString()] = i.value().toStdString();
    return result;
}

void require(bool condition, const std::string& reason)
{
    if (!condition) throw std::runtime_error(reason);
}

// This follows the simulator's learnedFrom chain, NOT a packet-level data
// plane. For iBGP it is explicitly only a control-plane forwarding proxy.
std::string forwardingProxy(const SimulationEngine& engine, const Topology& topology,
                            const std::string& observer, const std::string& prefix)
{
    std::set<std::string> seen;
    auto current = observer;
    for (;;)
    {
        if (!seen.insert(current).second) return "loop";
        const auto rib = engine.ribSnapshot(qs(current));
        const auto route = rib.locRib.constFind(qs(prefix));
        if (route == rib.locRib.cend()) return current == observer ? "no_route" : "broken";
        if (route.value().localOrigin) return "reachable";
        const auto& next = route.value().learnedFrom;
        const auto link = topology.findLink(qs(current), next);
        if (!link || !link->enabled) return "broken";
        current = next.toStdString();
    }
}

bool originConnected(const SimulationEngine& engine, const Topology& topology,
                     const std::string& observer, const std::string& prefix)
{
    std::set<std::string> seen{observer};
    std::vector<std::string> pending{observer};
    while (!pending.empty())
    {
        const auto node = pending.back(); pending.pop_back();
        if (engine.ribSnapshot(qs(node)).localRoutes.contains(qs(prefix))) return true;
        for (const auto& edge : topology.links)
        {
            if (!edge.enabled || (edge.a != qs(node) && edge.b != qs(node))) continue;
            const auto& next = edge.a == qs(node) ? edge.b : edge.a;
            if (seen.insert(next.toStdString()).second) pending.push_back(next.toStdString());
        }
    }
    return false;
}

struct PhaseMetrics
{
    bool active = false;
    bool previousOriginConnected = true;
    std::int64_t start = 0, lastAt = 0, lastChoiceAt = 0;
    std::string previousStatus, previousPeer;
    std::map<std::string, std::int64_t> durations;
    std::uint64_t sent = 0, received = 0, announces = 0, withdrawals = 0, controls = 0, extensionBytes = 0;
    std::uint64_t choices = 0, unusableChoices = 0, metadataSelections = 0;
    Json sequence = Json::array();
};

void validateForest(const Json& graph)
{
    std::map<std::string, std::optional<std::string>> successor;
    std::size_t edges = 0;
    for (const auto& node : graph.at("nodes"))
    {
        require(node.at("references").get<std::size_t>() > 0, "unreferenced active node");
        const auto key = node.at("state").dump();
        const auto next = node.at("successor").is_null() ? std::nullopt : std::optional(node.at("successor").dump());
        require(successor.emplace(key, next).second, "duplicate state node");
        if (next) ++edges;
    }
    std::set<std::string> roots;
    for (const auto& [node, next] : successor)
    {
        (void)next;
        std::set<std::string> visited;
        auto current = node;
        for (;;)
        {
            require(visited.insert(current).second, "dependency cycle");
            require(successor.contains(current), "dangling dependency");
            if (!successor.at(current)) { roots.insert(current); break; }
            current = *successor.at(current);
        }
    }
    require(successor.size() == graph.at("active_nodes").get<std::size_t>() &&
            edges == graph.at("active_edges").get<std::size_t>() &&
            roots.size() == graph.at("components").get<std::size_t>(), "incorrect forest statistics");
    for (const auto& candidate : graph.at("candidates"))
        require(successor.contains(candidate.at("root").dump()), "candidate root is missing");
}

Json run(const Json& scenario, const fs::path& suiteDirectory, const std::string& mode, int mrai, int delayScale, bool withdrawalBypass,
         const fs::path& traceDirectory)
{
    QString error;
    auto loaded = Topology::load(qs((suiteDirectory / scenario.at("topology").get<std::string>()).string()), &error);
    require(loaded.has_value(), error.toStdString());
    auto topology = std::move(*loaded);
    for (auto& router : topology.routers)
    {
        router.pluginId = mode == "standard" ? StandardRouterPluginId : VersionedPathRouterPluginId;
        router.pluginSettings = mode == "standard" ? QJsonObject{} : QJsonObject{{"mode", qs(mode)}};
    }
    for (auto& link : topology.links)
    {
        link.mraiMsFromA = mrai; link.mraiMsFromB = mrai;
        link.delayMs *= delayScale;
    }
    topology.simulation.convergenceQuietMs = 0;
    topology.simulation.withdrawalIgnoresMrai = withdrawalBypass;
    const std::string observer = scenario.at("observer"), prefix = scenario.at("prefix");
    SimulationEngine engine(nullptr, 1, 100000, true);
    PhaseMetrics metrics;
    std::ofstream trace;
    if (!traceDirectory.empty())
    {
        fs::create_directories(traceDirectory);
        trace.open(traceDirectory / (scenario.at("name").get<std::string>() + "_" + mode + "_m" +
                    std::to_string(mrai) + "_d" + std::to_string(delayScale) + (withdrawalBypass ? "_bypass" : "_limited") + ".jsonl"));
        require(trace.good(), "cannot open trace");
    }
    const auto sample = [&]
    {
        if (!metrics.active) return;
        const auto now = engine.statsSnapshot().elapsedMs;
        require(now >= metrics.lastAt, "virtual time moved backwards");
        metrics.durations[metrics.previousStatus] += now - metrics.lastAt;
        if (metrics.previousOriginConnected && metrics.previousStatus != "reachable")
            metrics.durations["connected_but_unreachable"] += now - metrics.lastAt;
        metrics.lastAt = now;
        metrics.previousStatus = forwardingProxy(engine, topology, observer, prefix);
        metrics.previousOriginConnected = originConnected(engine, topology, observer, prefix);
    };
    QObject::connect(&engine, &SimulationEngine::eventsGenerated, [&](const auto& events)
    {
        sample();
        for (const auto& event : events)
        {
            if (trace.is_open())
                trace << Json{{"time_ms", event.timestamp.toMSecsSinceEpoch() - SimulationEpochMilliseconds}, {"event", jsonValue(event.event)},
                              {"router", jsonValue(event.router)}, {"from", jsonValue(event.from)}, {"to", jsonValue(event.to)},
                              {"sequence", event.sequence}, {"action", jsonValue(event.action)}, {"nlri", jsonValue(event.prefixes)},
                              {"withdrawn", jsonValue(event.withdrawn)}, {"details", jsonValue(event.details)}}.dump() << '\n';
            if (!metrics.active) continue;
            if (event.event == "message_sent" && event.messageType == "UPDATE")
            {
                ++metrics.sent;
                if (event.action == "UPDATE") ++metrics.announces;
                if (event.action == "WITHDRAW") ++metrics.withdrawals;
                if (event.action == "STATE_RECORDS") ++metrics.controls;
                if (event.details.contains("estimated_extension_bytes"))
                    metrics.extensionBytes += event.details.value("estimated_extension_bytes").toULongLong();
            }
            if (event.event == "message_received" && event.messageType == "UPDATE") ++metrics.received;
            require(event.event != "versioned_withdrawal_rejected", "withdrawal binding mismatch in valid traffic");
            if (event.event == "route_selected" && event.router == qs(observer) && event.details.value("prefix") == qs(prefix))
            {
                ++metrics.metadataSelections;
                const auto selected = event.details.value("learned_from").toStdString();
                if (selected == metrics.previousPeer) continue;
                ++metrics.choices;
                metrics.lastChoiceAt = event.timestamp.toMSecsSinceEpoch() - SimulationEpochMilliseconds;
                if (!selected.empty() && metrics.previousStatus != "reachable") ++metrics.unusableChoices;
                if (metrics.sequence.size() < 256)
                    metrics.sequence.push_back({{"at_ms", metrics.lastChoiceAt - metrics.start}, {"peer", selected},
                                                {"as_path", Json::parse(event.details.value("as_path").toStdString())},
                                                {"forwarding_proxy", metrics.previousStatus}});
                metrics.previousPeer = selected;
            }
        }
    });
    engine.startSimulation(topology);
    require(engine.isRunning() && engine.runUntilConverged(), "bootstrap: " + engine.lastError().toStdString());
    require(forwardingProxy(engine, topology, observer, prefix) == "reachable", "initial route absent");
    Json phases = Json::array();
    // Independent experiments start after all MRAI timers have expired. Starting
    // each scheme's next fault immediately at its own queue-empty time would
    // introduce different residual timers and confound recovery comparisons.
    int maximumDelay = 0;
    for (const auto& link : topology.links) maximumDelay = std::max(maximumDelay, link.delayMs);
    const auto idleMs = static_cast<std::int64_t>(mrai) + 2LL * maximumDelay + 1;
    for (const auto& phase : scenario.at("phases"))
    {
        require(engine.advanceTime(idleMs), "inter-phase idle");
        metrics = {};
        metrics.active = true;
        metrics.start = metrics.lastAt = metrics.lastChoiceAt = engine.statsSnapshot().elapsedMs;
        metrics.previousStatus = forwardingProxy(engine, topology, observer, prefix);
        metrics.previousOriginConnected = originConnected(engine, topology, observer, prefix);
        const auto initialRib = engine.ribSnapshot(qs(observer));
        if (initialRib.locRib.contains(qs(prefix))) metrics.previousPeer = initialRib.locRib.value(qs(prefix)).learnedFrom.toStdString();
        const auto initialPeer = metrics.previousPeer;
        for (const auto& action : phase.at("actions"))
        {
            const auto at = metrics.start + action.at("at_ms").get<std::int64_t>();
            require(at >= engine.statsSnapshot().elapsedMs, "unsorted actions");
            require(engine.advanceTime(at - engine.statsSnapshot().elapsedMs), "advance: " + engine.lastError().toStdString());
            sample();
            const std::string kind = action.at("kind");
            if (kind == "link")
            {
                const std::string a = action.at("a"), b = action.at("b");
                auto* link = topology.findLink(qs(a), qs(b));
                require(link != nullptr, "missing scenario link");
                link->enabled = action.at("enabled").get<bool>();
                engine.setLinkState(qs(a), qs(b), link->enabled);
            }
            else if (kind == "export")
                engine.setPeerExportEnabled(qs(action.at("router")), qs(action.at("peer")), action.at("enabled"));
            else if (kind == "withdraw")
                engine.withdrawPrefix(qs(action.at("router")), qs(action.at("prefix")));
            else if (kind == "originate")
                engine.originatePrefix(qs(action.at("router")), qs(action.at("prefix")));
            else throw std::runtime_error("unknown action: " + kind);
            sample();
        }
        const bool converged = engine.runUntilConverged();
        sample();
        require(converged, "phase " + phase.at("name").get<std::string>() + ": " + engine.lastError().toStdString());
        const auto status = forwardingProxy(engine, topology, observer, prefix);
        require((status == "reachable") == phase.at("observer_reachable").get<bool>(), "unexpected final reachability");
        require(forwardingProxy(engine, topology, observer, scenario.at("unaffected_prefix")) == "reachable",
                "unrelated prefix lost");
        Json graph = Json::object();
        std::uint64_t historicalNodes = 0, watermarks = 0;
        if (mode != "standard")
        {
            for (const auto& summary : engine.routerSnapshots())
            {
                const auto diagnostics = engine.ribSnapshot(summary.id).policyDiagnostics;
                require(diagnostics.at("rejected_attributes") == 0, "invalid version attributes on a valid topology");
                for (const auto& [p, g] : diagnostics.at("prefixes").items())
                {
                    (void)p;
                    validateForest(g);
                    historicalNodes += g.at("historical_nodes").get<std::uint64_t>();
                    watermarks += g.at("highest_versions").size();
                }
            }
            graph = engine.ribSnapshot(qs(observer)).policyDiagnostics.at("prefixes").at(prefix);
        }
        phases.push_back({
            {"name", phase.at("name")}, {"convergence_ms", engine.statsSnapshot().elapsedMs - metrics.start},
            {"observer_last_change_ms", metrics.lastChoiceAt - metrics.start},
            {"initial_peer", initialPeer}, {"final_peer", metrics.previousPeer}, {"final_status", status},
            {"observer_changes", metrics.choices}, {"observer_metadata_selections", metrics.metadataSelections},
            {"observer_unusable_selections", metrics.unusableChoices},
            {"proxy_no_route_ms", metrics.durations["no_route"]}, {"proxy_broken_ms", metrics.durations["broken"]},
            {"proxy_loop_ms", metrics.durations["loop"]},
            {"proxy_unreachable_while_origin_connected_ms", metrics.durations["connected_but_unreachable"]},
            {"updates_sent", metrics.sent}, {"updates_received", metrics.received}, {"announcements_sent", metrics.announces},
            {"withdrawals_sent", metrics.withdrawals}, {"control_updates_sent", metrics.controls},
            {"estimated_extension_bytes", metrics.extensionBytes}, {"observer_choices", metrics.sequence},
            {"historical_nodes_all_prefixes", historicalNodes}, {"watermarks_all_prefixes", watermarks},
            {"observer_graph", graph}});
        metrics.active = false;
    }
    engine.stopSimulation();
    if (trace.is_open()) { trace.flush(); require(trace.good(), "trace write failed"); }
    return {{"scenario", scenario.at("name")}, {"mode", mode}, {"mrai_ms", mrai}, {"delay_scale", delayScale},
            {"withdrawal_ignores_mrai", withdrawalBypass}, {"inter_phase_idle_ms", idleMs}, {"phases", phases}};
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try
    {
        require(argc >= 3, "usage: BgpTesterExperiments <suite.json> <results.json> [--quick] [--trace-dir DIR]");
        const fs::path suitePath = fs::absolute(argv[1]), outputPath = argv[2];
        bool quick = false;
        fs::path traces;
        for (int i=3; i<argc; ++i)
        {
            if (std::string(argv[i]) == "--quick") quick = true;
            else if (std::string(argv[i]) == "--trace-dir" && i+1<argc) traces = argv[++i];
            else throw std::runtime_error("unknown argument");
        }
        std::ifstream input(suitePath);
        require(input.good(), "cannot open suite");
        Json suite; input >> suite;
        Json results = Json::array(), errors = Json::array();
        for (const auto& scenario : suite.at("scenarios"))
            for (const auto& mode : {"standard", "path_only", "local", "full"})
                for (int mrai : quick ? std::vector<int>{1000} : suite.value("mrai_ms", std::vector<int>{1000,5000,30000}))
                    for (int scale : quick ? std::vector<int>{1} : std::vector<int>{1,3})
                    for (bool bypass : {true, false})
                    {
                        try { results.push_back(run(scenario, suitePath.parent_path(), mode, mrai, scale, bypass, traces)); }
                        catch (const std::exception& error)
                        {
                            errors.push_back({{"scenario",scenario.at("name")},{"mode",mode},{"mrai_ms",mrai},
                                              {"delay_scale",scale},{"withdrawal_ignores_mrai",bypass},{"error",error.what()}});
                            std::cerr << errors.back().dump() << '\n';
                        }
                    }
        if (!outputPath.parent_path().empty()) fs::create_directories(outputPath.parent_path());
        std::ofstream output(outputPath);
        output << Json{{"schema","bgptester-versioned-results-v1"}, {"runs",results}, {"errors",errors}, {"engine","Qt RouterNodePlugin"},
                       {"forwarding_metric","learnedFrom control-plane proxy; no packet/IGP/FIB simulation"},
                       {"byte_metric","estimated extension payload, not serialized BGP/TCP wire bytes"},
                       {"control_scheduling","retirement records bypass MRAI, route UPDATEs retain configured MRAI"},
                       {"transport","generation guards before transmission; in-flight messages remain ordered"}}.dump(2) << '\n';
        output.flush();
        require(output.good(), "cannot write results");
        std::cout << results.size() << " runs, " << errors.size() << " failures\n";
        return errors.empty() ? 0 : 1;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
