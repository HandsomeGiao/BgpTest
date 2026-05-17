# TopoSimulator

`TopoSimulator` is a C++20 BGP network simulation skeleton for large-scale convergence experiments. It reads a single JSON topology, starts all configured BGP speakers, exchanges simplified RFC4271-style OPEN, UPDATE and NOTIFICATION messages, writes BMP-like receive logs, opens a lightweight BMP observation window for interactive runs, waits for convergence, then accepts interactive topology changes.

Supported environment: Windows + MSVC + vcpkg only.

## Dependency Boundary

Protocol and route-decision code must stay pure C++20: `BgpRouter`, `ThreadPool`, `BgpTypes` and their state-machine logic may only use the C++ standard library plus project headers. Third-party libraries and Windows APIs are limited to peripheral facilities such as the CLI, colored console output, JSON topology loading, BMP log persistence and the BMP viewer. Core APIs should expose standard-library data structures; peripheral adapters convert them to JSON, text logs, SQLite rows or GUI tables.

## Architecture and Design

`TopoSimulator` is split into a small pure simulation core and thin peripheral adapters:

```text
TopoSimulator.exe
├─ CLI / Windows console / command completion
├─ BmpLogViewer: ImGui + Win32/DX11 live and historical BMP log browser
├─ toposim_io
│  └─ TopologyJson: JSON topology loading and JSON snapshot formatting
└─ toposim
   ├─ TopoManager: topology lifecycle, link state, node state, message delivery
   ├─ BgpRouter: BGP speaker state machine, RIBs, route selection, route export
   ├─ ThreadPool: asynchronous delivery workers and convergence idle detection
   ├─ BmpLogManager: BMP-like JSON Lines writer, SQLite history store and live buffer
   └─ BgpTypes: protocol messages, route attributes, config and snapshot structs
```

The intended dependency direction is one-way. Peripheral code may depend on the core, but the core does not depend on the CLI, JSON parser, console API or logging library. This keeps protocol experiments focused on BGP behavior instead of I/O details.

### Runtime Flow

1. The CLI finds a topology file from `--topology` or the executable directory's `topo/` folder.
2. `TopologyJson` parses the JSON into plain `TopologyConfig` structs.
3. `TopoManager` validates topology consistency, builds all `BgpRouter` instances, derives missing link-backed neighbors, creates the runtime link table, starts the thread pool and initializes the BMP JSONL/SQLite log manager.
4. Each router starts by sending OPEN messages to enabled neighbors. Receiving OPEN establishes the simulated session, and routers then exchange UPDATE messages. KEEPALIVE and hold-timer liveness are intentionally not simulated; link/node state changes drive neighbor availability.
5. `TopoManager::sendMessage` is the only message transport path. It checks router/link state, assigns a sequence number, applies MRAI/link delay through the worker pool, rechecks delivery state immediately before receipt, records the receive event, then delivers the message to the destination router.
6. Convergence is detected when the thread pool remains idle for a quiet window. The quiet window is `max(convergence_quiet_ms, ceil(1.5 * max_mrai_ms))`, so MRAI-delayed UPDATEs are included in the stability test.
7. In an interactive console, `TopoSimulator.exe` starts the ImGui BMP viewer in a separate thread so live convergence events can be filtered while the CLI remains usable.
8. After initial convergence, the CLI accepts runtime changes such as link up/down, node up/down, prefix advertise/withdraw and explicit convergence waits.

### Core Simulation Model

`TopoManager` owns topology-wide runtime state: routers, links, the worker pool, sequence numbers and BMP logging integration. It exposes high-level operations rather than letting routers mutate global topology directly. This gives all topology disturbances a single coordination point. A manager instance is a single-run object: after `stop()` releases the worker pool, create a new `TopoManager` for another run instead of calling `start()` again.

`BgpRouter` models one BGP speaker. It owns neighbor configuration, peer states, local originated routes, Adj-RIB-In, Loc-RIB and Adj-RIB-Out. The base implementation provides simplified RFC4271-style behavior: session establishment, UPDATE import, best-path selection, export policy, route reflection, EBGP AS_PATH prepending and MRAI scheduling.

Routers do not create real TCP sockets or bind real interface addresses. BGP sessions are simulated by delivering messages through `TopoManager` using router ids, neighbor ids and enabled links. The `router_id` field is the BGP router-id, not a transport socket address; it is also used as the simplified NEXT_HOP value in this model.

Route reflector behavior is derived from per-neighbor `rr_client`. A router with at least one RR client is treated as a reflector. Client-learned IBGP routes may be reflected to other peers; non-client-learned IBGP routes are reflected only to clients.

MRAI is tracked per neighbor and prefix inside each router. Advertisements may be delayed, while withdrawals are sent immediately. Delayed advertisements carry a per-prefix generation guard, so an older UPDATE is dropped if the prefix is withdrawn or superseded before the delay expires. This keeps the timing behavior close to the router that owns the export policy without allowing stale delayed advertisements to revive withdrawn routes.

### Concurrency Model

The simulation uses asynchronous message delivery rather than one thread per router. Routers process messages synchronously when delivered, while `ThreadPool` handles delayed delivery tasks. Shared router and manager state is protected with mutexes. Before a delayed task is delivered, `TopoManager` rechecks that the simulator is still running, the link is still enabled, and both endpoint routers are still active.

Convergence does not inspect routing-table equality directly. It observes the transport queue: when no delivery tasks are pending for the configured quiet period, the network is considered stable from the simulator's point of view.

Policy hooks such as import policy, export policy, route transformation and best-route selection are invoked without holding the router's internal mutex. This keeps custom `BgpRouter` subclasses from deadlocking when they query router state or call other safe extension helpers.

### Extension Philosophy

The base router is intentionally virtual at the protocol decision points. Custom protocol experiments should usually subclass `BgpRouter` and override import policy, export policy, route transformation, message handlers or best-route selection. `TopoManager` should remain responsible for topology lifecycle and message transport, so custom BGP behavior can evolve without duplicating simulation infrastructure.

New peripheral features should be added outside the core. For example, a different topology file format should become another adapter that produces `TopologyConfig`; a different UI should call `TopoManager`; a different snapshot output format should transform the standard snapshot structs.

## Build

```powershell
$env:VCPKG_ROOT = "<path-to-vcpkg>"
cmake -S TopoSimulator -B TopoSimulator/build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build TopoSimulator/build --config Release
```

Or use the helper script:

```powershell
.\TopoSimulator\build.ps1
```

`build.ps1` uses `VCPKG_ROOT`, a `-VcpkgRoot` argument, or a `vcpkg` executable found on `PATH`:

```powershell
.\TopoSimulator\build.ps1 -VcpkgRoot "<path-to-vcpkg>"
```

The script pauses before exit so double-click users can read the result. Use `-NoPause` for terminals, CI, or other automated runs.

Dependencies are declared in `vcpkg.json`:

- `nlohmann-json`
- `spdlog`
- `sqlite3`
- `imgui` with the Win32 and DX11 backends

## Test

The simulator has a small CTest target for core correctness checks:

```powershell
ctest --test-dir TopoSimulator\build -C Release --output-on-failure
```

The current tests cover topology validation and an MRAI regression where a delayed advertisement must not revive a prefix after it has been withdrawn.

## Run

```powershell
.\TopoSimulator\build\Release\TopoSimulator.exe --topology TopoSimulator\config\sample_topology.json
```

You can also double-click `TopoSimulator.exe` after building. Without `--topology`, it only looks in the `topo/` folder next to `TopoSimulator.exe`, lists the available `*.json` topology files, and lets you choose one. If no JSON topology exists there, it prints a message and exits.

The BMP viewer mode is controlled with `--bmp-viewer auto|on|off`:

- `auto` is the default. It opens the ImGui window only for an interactive console.
- `on` always opens the window.
- `off` disables the window for tests, CI and scripted runs.

If the viewer is closed during an interactive run, it can be opened again from
the CLI with `bmp viewer` or `bmp open`. `bmp close` closes it, and `bmp status`
prints whether the viewer thread is currently running.

The build copies `TopoSimulator/topo/` next to the executable, so double-clicking the built exe has a ready-to-run sample topology:

```text
TopoSimulator/build/Release/topo/sample_topology.json
```

Each run creates a directory below `tmp/` and writes both `bmp_collector.log` and a fresh `bmp_collector.sqlite`. BMP timestamps use readable China time in `YYYY-MM-DD HH:MM:SS.mmm` format. The JSON Lines file preserves the raw event stream. The SQLite database uses a single indexed `bmp_events` table with direct fields for prefixes, AS_PATH, source AS and destination AS so the viewer can query without reparsing the log. The viewer table has a `Columns` menu for choosing visible fields, and a `MessageFilter` popup for filtering by involved routers, source/destination routers, action, source AS and destination AS. Withdrawal-only UPDATE messages keep the raw BGP message type as `UPDATE`, but the viewer labels their action as `WITHDRAW`.

## JSON Topology

The simulator accepts:

- `simulation`: run name, log directory, worker count and convergence quiet period.
- `routers`: router id, BGP router-id, ASN, cluster id, originated prefixes and optional neighbors.
- `links`: physical/logical connectivity, state and delivery delay.

Neighbor entries may be explicit. If a link exists but one side omits the neighbor, `TopoManager` derives a symmetric neighbor using the routers' ASNs.

Topology validation rejects empty router ids, duplicate router ids, duplicate or invalid BGP router-ids, ASN 0, invalid originated IPv4 CIDR prefixes, self-neighbors, duplicate neighbors, explicit neighbors without a backing link, neighbor `remote_asn` or `session_type` mismatches, links with empty endpoints, self-links, duplicate links and links or neighbors that reference unknown routers. BGP router-ids must be dotted decimal `x.x.x.x` values with each octet in `0..255`, excluding `0.0.0.0`. These checks run before any routers, worker threads or logs are created.

Neighbor entries can include `"mrai_ms"` to enforce a per-neighbor MRAI for UPDATE advertisements. `mrai_ms=0` disables MRAI. Withdrawals are sent immediately.

If an MRAI-delayed advertisement becomes stale because a newer UPDATE or withdraw supersedes it before delivery, the delayed task is dropped instead of being logged or delivered.

Convergence waits for the worker queue to stay idle for at least `max(convergence_quiet_ms, ceil(1.5 * max_mrai_ms))`, so configured MRAI timers are included in the stability window.

Route reflector support is modeled through:

- Per-neighbor flag on the reflector: `"rr_client": true`

There is no separate behavioral switch for route reflectors. If a router has at least one neighbor marked with `"rr_client": true`, it is treated as a route reflector. IBGP routes are not re-advertised to other IBGP peers unless this derived route-reflector role applies. Client-learned routes are reflected to other peers except the origin peer; non-client-learned routes are reflected only to clients.

## Custom BGP Router Behavior

Custom protocol experiments should be implemented by subclassing `toposim::BgpRouter`. Keep `TopoManager` as the owner of topology lifecycle, link/node state and message transport; customize only the router behavior you want to study.

Useful override points:

- `importRouteAllowed`: reject or accept routes before they enter Adj-RIB-In.
- `exportRouteAllowed`: decide whether the current best route may be advertised to a neighbor.
- `transformRouteForPeer`: edit attributes before advertisement, such as MED, communities, NEXT_HOP or AS_PATH policy.
- `selectBestRoute`: replace the best-path decision rule.
- `onMessageReceived`, `onOpenMessage`, `onUpdateMessage`, `onNotificationMessage`: customize protocol handling. Call the base implementation if you still want the default state-machine and RIB side effects.

Minimal example:

```cpp
#pragma once

#include <algorithm>

#include "toposim/BgpRouter.hpp"

namespace toposim {

class LabRouter final : public BgpRouter {
public:
  using BgpRouter::BgpRouter;

protected:
  bool importRouteAllowed(const RouteEntry &route,
                          const NeighborConfig &from_peer) const override {
    if (route.attributes.communities.contains("drop")) {
      return false;
    }
    return BgpRouter::importRouteAllowed(route, from_peer);
  }

  RouteEntry transformRouteForPeer(const RouteEntry &route,
                                   const NeighborConfig &to_peer) const override {
    auto transformed = BgpRouter::transformRouteForPeer(route, to_peer);
    if (to_peer.session_type == SessionType::Ebgp) {
      transformed.attributes.med = 50;
    }
    return transformed;
  }

  std::optional<RouteEntry>
  selectBestRoute(const std::string &prefix,
                  const std::vector<RouteEntry> &candidates) const override {
    const auto preferred =
        std::find_if(candidates.begin(), candidates.end(), [](const auto &route) {
          return route.learned_from == "R2";
        });
    if (preferred != candidates.end()) {
      return *preferred;
    }
    return BgpRouter::selectBestRoute(prefix, candidates);
  }
};

} // namespace toposim
```

To use the custom router, add the header/source under `include/toposim/` and `src/`. If you add a `.cpp` file, list it in the `toposim` target in `CMakeLists.txt`. Then include the custom router in `src/TopoManager.cpp` and choose it in `TopoManager::buildRouters`:

```cpp
#include "toposim/LabRouter.hpp"

void TopoManager::buildRouters() {
  for (const auto &router_config : config_.routers) {
    std::shared_ptr<BgpRouter> router;
    if (router_config.id.starts_with("LAB")) {
      router = std::make_shared<LabRouter>(router_config);
    } else {
      router = std::make_shared<BgpRouter>(router_config);
    }
    router->attachManager(this);
    routers_[router_config.id] = std::move(router);
  }
}
```

For one experiment, branching by router id, ASN or cluster id is usually enough. If many router families are needed, add an explicit field to `RouterConfig` and the topology JSON, then centralize construction in a small router factory.

The override hooks are called outside the router's internal mutex. Avoid storing references to route candidates after the function returns, and prefer returning modified copies. Use the protected `sendMessage` helper only when implementing new message behavior that should still go through `TopoManager`, BMP logging and delivery checks.

## Interactive Commands

Press `Tab` in the interactive prompt to complete command keywords and router ids.
The prompt, help text, completion candidates, status messages and errors use Windows console colors plus ASCII markers such as `[OK]`, `[!]` and `[ERR]`.
Commands that change the network (`link`, `node`, `advertise`, `withdraw`) are accepted only when the current topology is already converged. If the network is still changing, the simulator rejects the command and asks you to run `converge [timeout_ms]` first. `show` and `converge` remain available while the network is not converged.

```text
show routers
show peers <router>
show rib <router>
link down <a> <b>
link up <a> <b>
node down <router>
node up <router>
advertise <router> <prefix>
withdraw <router> <prefix>
converge [timeout_ms]
bmp viewer
bmp close
bmp status
quit
```
