#include "engine/SimulationEngine.hpp"
#include "router_plugins/VersionedPathRouterPlugin.hpp"
#include "router_plugins/TfpVersionRouterPlugin.hpp"
#include "plugin/RouterPluginRegistry.hpp"
#include <QCoreApplication>

#include <functional>
#include <iostream>
#include <stdexcept>

using namespace bgptester;
using Json = nlohmann::json;

namespace
{
void require(bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(reason);
}
void require(bool condition, const QString& reason) { require(condition, reason.toStdString().c_str()); }
VersionedState state(std::uint32_t asn, std::uint64_t version)
{
    return {{asn, "10.0.0." + std::to_string(asn)}, version};
}

Topology line(bool ibgp = false)
{
    Topology topology;
    topology.simulation.convergenceQuietMs = 0;
    for (const auto& [id, asn, rid] :
         std::vector<std::tuple<std::string, std::uint32_t, std::string>>{
             {"O", 1, "10.0.0.1"}, {"X", ibgp ? 1U : 2U, "10.0.0.2"}})
    {
        RouterConfig router;
        router.id = QString::fromStdString(id); router.asn = asn; router.routerId = QString::fromStdString(rid);
        router.pluginId = VersionedPathRouterPluginId;
        if (id == "O") router.originatedPrefixes = {"192.0.2.0/24", "198.51.100.0/24"};
        topology.routers.insert(QString::fromStdString(id), router);
    }
    LinkConfig link;
    link.a = "O"; link.b = "X"; link.delayMs = 100;
    topology.links.push_back(link);
    return topology;
}

void settle(SimulationEngine& engine)
{
    require(engine.isRunning(), engine.lastError());
    require(engine.runUntilConverged(), engine.lastError());
}

std::unique_ptr<RouterNode> observerPolicy()
{
    QMap<QString, RouterConfig> routers;
    for (const auto& [id, asn] : std::map<std::string, std::uint32_t>{{"A",2},{"B",1},{"O",3},{"X",4}})
    {
        RouterConfig router;
        router.id=QString::fromStdString(id); router.asn=asn; router.routerId=QString::fromStdString(state(asn,1).creator.routerId);
        router.pluginId=VersionedPathRouterPluginId;
        routers.insert(QString::fromStdString(id),router);
    }
    return std::unique_ptr<RouterNode>(RouterPluginRegistry::instance().createRouterNode({routers.value("X"),routers,{}},nullptr));
}

RouteEntry advertised(const QString& peer, std::uint32_t asn, std::uint64_t version)
{
    RouteEntry route;
    route.learnedFrom=peer;
    route.attributes.asPath={asn,3};
    route.attributes.nextHop=QString::fromStdString(state(asn,version).creator.routerId);
    route.attributes.versionedPath=VersionedPath{state(asn,version),{state(asn,version),state(3,1)}};
    return route;
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc,argv);
    int failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& run)
    {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    };
    test("source plugin is registered through the existing factory", []
    {
        const auto& registry=RouterPluginRegistry::instance();
        require(registry.contains(VersionedPathRouterPluginId),"missing plugin registration");
        auto topology=line();
        QString error;
        std::unique_ptr<RouterNode> node(registry.createRouterNode(topology.routers.value("O"),topology,nullptr,&error));
        require(node && dynamic_cast<VersionedPathRouterNode*>(node.get()),"factory did not create the source plugin");
        require(node->validateConfiguration().isEmpty(),"valid plugin rejected");
    });
    test("shared dependency closure and independent suffix", []
    {
        VersionedDependencyGraph graph;
        const auto a=state(1,11), b=state(2,20), d=state(4,7), o=state(9,3);
        const auto c=state(3,6), e=state(5,8);
        require(graph.observe({a,d,o}) && graph.observe({b,d,o}) && graph.observe({c,e,o}), "valid forest");
        graph.setCandidate("A",{a,d,o}); graph.setCandidate("B",{b,d,o}); graph.setCandidate("C",{c,e,o});
        require(graph.advance(d.creator,8), "new watermark");
        require(graph.invalid(a) && graph.invalid(b) && !graph.invalid(c) && !graph.invalid(o), "wrong closure direction");
        const auto snapshot=graph.snapshot();
        require(snapshot["active_nodes"] == 6 && snapshot["active_edges"] == 5, "shared suffix not merged");
        graph.removeCandidate("A");
        require(graph.snapshot()["active_nodes"] == 5, "shared suffix deleted with one candidate");
    });
    test("immutable suffix validation is atomic", []
    {
        VersionedDependencyGraph graph;
        require(graph.observe({state(1,1),state(2,1)}), "initial");
        require(!graph.observe({state(3,100),state(1,1),state(4,1)}), "conflicting suffix accepted");
        require(!graph.highestVersions().contains(state(3,100).creator), "invalid attribute poisoned watermark");
        require(!graph.observe({state(5,1),state(5,1)}), "cycle accepted");
    });
    test("tombstone and late paths cannot resurrect old instances", []
    {
        VersionedDependencyGraph graph;
        graph.advance(state(4,8).creator,8);
        require(graph.observe({state(1,1),state(4,7),state(9,1)}), "late declaration structural validity");
        require(graph.invalid(state(1,1)), "late dependent accepted");
        require(graph.observe({state(4,9),state(9,1)}), "recovery");
        require(!graph.advance(state(4,8).creator,8), "watermark moved backwards");
        graph.revoke(state(4,7));
        require(!graph.invalid(state(4,9)), "old revoke erased recovery");
        graph.retainCandidates({});
        require(graph.invalid(state(4,7)), "GC forgot negative knowledge");
    });
    test("prefix and creator namespaces", []
    {
        VersionedDependencyGraph first, second;
        const auto a=state(1,1);
        const VersionedState b{{2,a.creator.routerId},1};
        require(first.observe({a}) && first.observe({b}) && second.observe({a}), "initial");
        first.advance(a.creator,2);
        require(!first.invalid(b) && !second.invalid(a), "namespace leak");
    });
    test("dependency-only change advances local version without disturbing BGP tie stability", []
    {
        auto policy=observerPolicy();
        const NeighborConfig a{.id="A",.remoteAsn=2,.sessionType=SessionType::Ebgp};
        const NeighborConfig b{.id="B",.remoteAsn=1,.sessionType=SessionType::Ebgp};
        const QString prefix="192.0.2.0/24";
        const auto firstA=policy->importAdvertisedRoute(prefix,advertised("A",2,1),a);
        auto best=policy->selectBestRoute(prefix,{*firstA},std::nullopt);
        const auto firstExport=policy->exportRouteForPrefix(prefix,*best,b);
        policy->decisionCompleted(prefix);
        const auto firstB=policy->importAdvertisedRoute(prefix,advertised("B",1,1),b);
        best=policy->selectBestRoute(prefix,{*firstA,*firstB},best);
        require(best->learnedFrom=="A","old path preference changed");
        const auto refreshedA=policy->importAdvertisedRoute(prefix,advertised("A",2,2),a);
        best=policy->selectBestRoute(prefix,{*refreshedA,*firstB},best);
        require(best->learnedFrom=="A","version refresh reset route age and picked a different equal-cost peer");
        const auto secondExport=policy->exportRouteForPrefix(prefix,*best,b);
        require(firstExport->attributes.asPath==secondExport->attributes.asPath,"AS_PATH changed");
        require(firstExport->attributes.versionedPath->advertiser.version==1 &&
                secondExport->attributes.versionedPath->advertiser.version==2,"dependency-only change reused local version");
        require(secondExport->attributes.versionedPath->path[1].version==2,"lost dependency revision");
    });
    test("explicit records propagate even when their version was already learned from a path", []
    {
        auto policy=observerPolicy();
        const NeighborConfig a{.id="A",.remoteAsn=2,.sessionType=SessionType::Ebgp};
        const NeighborConfig b{.id="B",.remoteAsn=1,.sessionType=SessionType::Ebgp};
        const QString prefix="192.0.2.0/24";
        require(policy->importAdvertisedRoute(prefix,advertised("A",2,2),a).has_value(),"initial path");
        const StateRecord record{1,1,prefix.toStdString(),StateRecordKind::RetireBefore,state(2,2)};
        require(policy->observeStateRecords({record},a).contains(prefix),"explicit evidence confused with watermark");
        require(policy->takeStateRecords(b)==std::vector<StateRecord>{record},"known-version event not forwarded");
        require(policy->observeStateRecords({record},a).empty() && policy->takeStateRecords(b).empty(),"duplicate event circulated");
        policy->peerStateChanged(b,PeerState::Idle);
        require(policy->takeStateRecords(b)==std::vector<StateRecord>{record},"reconnect failed to resynchronize");
    });
    test("AS prepend alignment and malformed attribute rejection", []
    {
        auto policy=observerPolicy();
        const NeighborConfig a{.id="A",.remoteAsn=2,.sessionType=SessionType::Ebgp};
        auto route=advertised("A",2,1);
        route.attributes.asPath={2,2,3};
        const auto imported=policy->importAdvertisedRoute("192.0.2.0/24",route,a);
        require(imported && imported->attributes.asPath.size()==3,"AS prepend normalized away for BGP selection");
        route.attributes.asPath={2};
        route.attributes.versionedPath->advertiser.version=100;
        route.attributes.versionedPath->path.front().version=100;
        require(!policy->importAdvertisedRoute("192.0.2.0/24",route,a),"misaligned attribute accepted");
        const auto graph=policy->diagnostics()["prefixes"]["192.0.2.0/24"];
        for(const auto& highest:graph["highest_versions"])
            if(highest["asn"]==2) require(highest["version"]==1,"malformed path changed watermark");
        const StateRecord wrongFamily{2,1,"192.0.2.0/24",StateRecordKind::RetireBefore,state(2,200)};
        require(policy->observeStateRecords({wrongFamily},a).empty(),"unsupported address family leaked into IPv4");
    });
    test("pure withdrawal binds old version and publishes no-route version", []
    {
        SimulationEngine engine(nullptr,1,100000);
        std::vector<SimulationEvent> events;
        QObject::connect(&engine,&SimulationEngine::eventsGenerated,[&](const auto& batch){events.insert(events.end(),batch.begin(),batch.end());});
        engine.startSimulation(line()); settle(engine); events.clear();
        engine.withdrawPrefix("O","192.0.2.0/24"); settle(engine);
        bool found=false;
        for(const auto& event:events)
        {
            if(event.event!="message_received" || event.from!="O" || event.to!="X" || event.action!="WITHDRAW") continue;
            require(event.details.contains("mp_unreach_nlri") && event.details.contains("versioned_withdraw"),"missing withdrawal attribute");
            const auto binding=Json::parse(event.details.value("versioned_withdraw").toStdString());
            require(binding.at("192.0.2.0/24").at("advertiser").at("version")==1,"wrong withdrawn version");
            require(event.details.contains("state_records"),"missing no-route retirement");
            found=true;
        }
        require(found,"no pure WITHDRAW delivered");
        auto rib=engine.ribSnapshot("X");
        require(!rib.locRib.contains("192.0.2.0/24") && rib.locRib.contains("198.51.100.0/24"),"prefix isolation");
        require(engine.ribSnapshot("O").policyDiagnostics["prefixes"]["192.0.2.0/24"]["local_version"]==2,"tombstone version");
    });
    test("sent UPDATEs remain in flight through a rapid recovery", []
    {
        SimulationEngine engine(nullptr,1,100000);
        std::vector<SimulationEvent> events;
        QObject::connect(&engine,&SimulationEngine::eventsGenerated,[&](const auto& batch){events.insert(events.end(),batch.begin(),batch.end());});
        engine.startSimulation(line()); settle(engine); events.clear();
        engine.withdrawPrefix("O","192.0.2.0/24");
        require(engine.advanceTime(1),"advance to flush");
        engine.originatePrefix("O","192.0.2.0/24"); settle(engine);
        std::size_t withdrawals=0;
        for(const auto& event:events)
            if(event.event=="message_received" && event.from=="O" && event.action=="WITHDRAW") ++withdrawals;
        require(withdrawals==1,"in-flight withdrawal was cancelled by a newer generation");
        const auto rib=engine.ribSnapshot("X");
        require(rib.locRib.contains("192.0.2.0/24"),"recovery lost");
        require(rib.locRib.value("192.0.2.0/24").attributes.versionedPath->advertiser.version==3,"version reused after tombstone");
        for(const auto& event:events) require(event.event!="versioned_withdrawal_rejected","valid ordered withdrawal rejected");
    });
    test("standard and TFP transport preserve already-sent withdrawals", []
    {
        for (const auto policy : {StandardRouterPluginId,TfpVersionRouterPluginId})
        {
            auto topology=line();
            for(auto& router:topology.routers)
            {
                router.pluginId=policy;
                router.pluginSettings=policy==TfpVersionRouterPluginId
                    ? QJsonObject{{"initial_version","0"},{"entity_id",""}} : QJsonObject{};
            }
            SimulationEngine engine(nullptr,1,100000);
            std::size_t received=0;
            QObject::connect(&engine,&SimulationEngine::eventsGenerated,[&](const auto& events)
            {
                for(const auto& event:events)
                    if(event.event=="message_received" && event.from=="O" && event.action=="WITHDRAW") ++received;
            });
            engine.startSimulation(topology); settle(engine);
            engine.withdrawPrefix("O","192.0.2.0/24");
            require(engine.advanceTime(1),"flush");
            engine.originatePrefix("O","192.0.2.0/24"); settle(engine);
            require(received==1 && engine.ribSnapshot("X").locRib.contains("192.0.2.0/24"),"existing policy transport regression");
        }
    });
    test("empty iBGP AS_PATH still has an exact withdrawal binding", []
    {
        SimulationEngine engine(nullptr,1,100000);
        std::vector<SimulationEvent> events;
        QObject::connect(&engine,&SimulationEngine::eventsGenerated,[&](const auto& batch){events.insert(events.end(),batch.begin(),batch.end());});
        engine.startSimulation(line(true)); settle(engine);
        const auto before=engine.ribSnapshot("X").locRib.value("192.0.2.0/24");
        require(before.attributes.asPath.empty() && before.attributes.versionedPath->path.empty(),"iBGP altered AS_PATH");
        engine.withdrawPrefix("O","192.0.2.0/24"); settle(engine);
        bool found=false;
        for(const auto& event:events)
            if(event.event=="message_received" && event.action=="WITHDRAW" && event.from=="O")
            {
                const auto binding=Json::parse(event.details.value("versioned_withdraw").toStdString()).at("192.0.2.0/24");
                require(binding["path_head"].is_null(),"invented AS path element");
                require(binding["advertiser"]["version"]==1,"lost exporter identity");
                found=true;
            }
        require(found && !engine.ribSnapshot("X").locRib.contains("192.0.2.0/24"),"iBGP withdrawal failed");
    });
    test("router restart preserves counters and resynchronizes peer facts", []
    {
        SimulationEngine engine(nullptr,1,100000);
        engine.startSimulation(line()); settle(engine);
        engine.setRouterState("O",false); settle(engine);
        engine.setRouterState("O",true); settle(engine);
        require(engine.ribSnapshot("X").locRib.value("192.0.2.0/24").attributes.versionedPath->advertiser.version==3,"restart reused identity");
        require(engine.ribSnapshot("X").policyDiagnostics["rejected_attributes"]==0,"bad state resynchronization");
    });
    test("relation-only withdrawal permits re-exporting the same live version", []
    {
        SimulationEngine engine(nullptr,1,100000);
        std::vector<SimulationEvent> events;
        QObject::connect(&engine,&SimulationEngine::eventsGenerated,[&](const auto& batch){events.insert(events.end(),batch.begin(),batch.end());});
        engine.startSimulation(line()); settle(engine); events.clear();
        engine.setPeerExportEnabled("O","X",false); settle(engine);
        require(!engine.ribSnapshot("X").locRib.contains("192.0.2.0/24"),"export withdrawal missing");
        require(engine.ribSnapshot("O").policyDiagnostics["prefixes"]["192.0.2.0/24"]["local_version"]==1,
                "relation withdrawal retired an unchanged state");
        for (const auto& event : events)
            if (event.event=="message_received" && event.from=="O" && event.action=="WITHDRAW")
                require(!event.details.contains("state_records"),"fabricated retirement on export-only withdrawal");
        engine.setPeerExportEnabled("O","X",true); settle(engine);
        require(engine.ribSnapshot("X").locRib.value("192.0.2.0/24").attributes.versionedPath->advertiser.version==1,
                "unchanged re-export acquired a new state version");
    });
    test("partial deployment is rejected explicitly", []
    {
        auto topology=line(); topology.routers["O"].pluginId=StandardRouterPluginId;
        SimulationEngine engine;
        engine.startSimulation(topology);
        require(!engine.isRunning() && !engine.lastError().isEmpty(),"silently accepted partial deployment");
    });
    return failed ? 1 : 0;
}
