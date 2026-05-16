# TopoSimulator

`TopoSimulator` is a C++20 BGP network simulation skeleton for large-scale convergence experiments. It reads a single JSON topology, starts all configured BGP speakers, exchanges simplified RFC4271-style OPEN, KEEPALIVE, UPDATE and NOTIFICATION messages, writes BMP-like receive logs, waits for convergence, then accepts interactive topology changes.

Supported environment: Windows + MSVC + vcpkg only.

## Dependency Boundary

Core simulation code must stay pure C++20: `BgpRouter`, `TopoManager`, `ThreadPool`, `BgpTypes` and their protocol/state-machine logic may only use the C++ standard library plus project headers. Third-party libraries and Windows APIs are limited to peripheral facilities such as the CLI, colored console output, log I/O, JSON topology loading and JSON display adapters. Core APIs should expose standard-library data structures; peripheral adapters convert them to JSON, text logs or console output.

## Architecture and Design

`TopoSimulator` is split into a small pure simulation core and thin peripheral adapters:

```text
TopoSimulator.exe
├─ CLI / Windows console / command completion
├─ toposim_io
│  └─ TopologyJson: JSON topology loading and JSON snapshot formatting
└─ toposim
   ├─ TopoManager: topology lifecycle, link state, node state, message delivery
   ├─ BgpRouter: BGP speaker state machine, RIBs, route selection, route export
   ├─ ThreadPool: asynchronous delivery workers and convergence idle detection
   ├─ BmpCollector: BMP-like JSON Lines receive/event log writer
   └─ BgpTypes: protocol messages, route attributes, config and snapshot structs
```

The intended dependency direction is one-way. Peripheral code may depend on the core, but the core does not depend on the CLI, JSON parser, console API or logging library. This keeps protocol experiments focused on BGP behavior instead of I/O details.

### Runtime Flow

1. The CLI finds a topology file from `--topology` or the current directory's `topo/` folder.
2. `TopologyJson` parses the JSON into plain `TopologyConfig` structs.
3. `TopoManager` builds all `BgpRouter` instances, derives missing link-backed neighbors, creates the runtime link table, starts the thread pool and opens the BMP log.
4. Each router starts by sending OPEN messages to enabled neighbors. After sessions become established, routers exchange KEEPALIVE and UPDATE messages.
5. `TopoManager::sendMessage` is the only message transport path. It checks router/link state, assigns a sequence number, applies MRAI/link delay through the worker pool, records the receive event, then delivers the message to the destination router.
6. Convergence is detected when the thread pool remains idle for a quiet window. The quiet window is `max(convergence_quiet_ms, ceil(1.5 * max_mrai_ms))`, so MRAI-delayed UPDATEs are included in the stability test.
7. After initial convergence, the CLI accepts runtime changes such as link up/down, node up/down, prefix advertise/withdraw and explicit convergence waits.

### Core Simulation Model

`TopoManager` owns topology-wide runtime state: routers, links, the worker pool, sequence numbers and the BMP collector. It exposes high-level operations rather than letting routers mutate global topology directly. This gives all topology disturbances a single coordination point.

`BgpRouter` models one BGP speaker. It owns neighbor configuration, peer states, local originated routes, Adj-RIB-In, Loc-RIB and Adj-RIB-Out. The base implementation provides simplified RFC4271-style behavior: session establishment, UPDATE import, best-path selection, export policy, route reflection, EBGP AS_PATH prepending and MRAI scheduling.

Route reflector behavior is derived from per-neighbor `rr_client`. A router with at least one RR client is treated as a reflector. Client-learned IBGP routes may be reflected to other peers; non-client-learned IBGP routes are reflected only to clients.

MRAI is tracked per neighbor and prefix inside each router. Advertisements may be delayed, while withdrawals are sent immediately. This keeps the timing behavior close to the router that owns the export policy.

### Concurrency Model

The simulation uses asynchronous message delivery rather than one thread per router. Routers process messages synchronously when delivered, while `ThreadPool` handles delayed delivery tasks. Shared router and manager state is protected with mutexes. This keeps large topologies lightweight while still allowing many in-flight messages and delay timers to progress concurrently.

Convergence does not inspect routing-table equality directly. It observes the transport queue: when no delivery tasks are pending for the configured quiet period, the network is considered stable from the simulator's point of view.

### Extension Philosophy

The base router is intentionally virtual at the protocol decision points. Custom protocol experiments should usually subclass `BgpRouter` and override import policy, export policy, route transformation, message handlers or best-route selection. `TopoManager` should remain responsible for topology lifecycle and message transport, so custom BGP behavior can evolve without duplicating simulation infrastructure.

New peripheral features should be added outside the core. For example, a different topology file format should become another adapter that produces `TopologyConfig`; a different UI should call `TopoManager`; a different snapshot output format should transform the standard snapshot structs.

## Build

```powershell
$env:VCPKG_ROOT = "C:\Users\giaogiao\AllMyLibFiles\vcpkg"
cmake -S TopoSimulator -B TopoSimulator/build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build TopoSimulator/build --config Release
```

Or use the helper script:

```powershell
.\TopoSimulator\build.ps1
```

If `VCPKG_ROOT` is not set, `build.ps1` will try `C:\Users\giaogiao\AllMyLibFiles\vcpkg`, which is the vcpkg location detected on this machine. You can also pass an explicit path:

```powershell
.\TopoSimulator\build.ps1 -VcpkgRoot "D:\path\to\vcpkg"
```

Dependencies are declared in `vcpkg.json`:

- `nlohmann-json`
- `spdlog`

## Run

```powershell
.\TopoSimulator\build\Release\TopoSimulator.exe --topology TopoSimulator\config\sample_topology.json
```

You can also double-click `TopoSimulator.exe` after building. Without `--topology`, it only looks in the current working directory's `topo/` folder, lists the available `*.json` topology files, and lets you choose one. If no JSON topology exists there, it prints a message and exits.

The build copies `TopoSimulator/topo/` next to the executable, so double-clicking the built exe has a ready-to-run sample topology:

```text
TopoSimulator/build/Release/topo/sample_topology.json
```

Each run creates a directory below `tmp/` and writes `bmp_collector.log` as JSON lines. Every received BGP message includes timestamp, receiver, sender, sequence, message type and full message payload.

## JSON Topology

The simulator accepts:

- `simulation`: run name, log directory, worker count and convergence quiet period.
- `routers`: router id, BGP router-id, ASN, cluster id, originated prefixes and optional neighbors.
- `links`: physical/logical connectivity, state and delivery delay.

Neighbor entries may be explicit. If a link exists but one side omits the neighbor, `TopoManager` derives a symmetric neighbor using the routers' ASNs.

Neighbor entries can include `"mrai_ms"` to enforce a per-neighbor MRAI for UPDATE advertisements. `mrai_ms=0` disables MRAI. Withdrawals are sent immediately.

Convergence waits for the worker queue to stay idle for at least `max(convergence_quiet_ms, ceil(1.5 * max_mrai_ms))`, so configured MRAI timers are included in the stability window.

Route reflector support is modeled through:

- Per-neighbor flag on the reflector: `"rr_client": true`

There is no separate behavioral switch for route reflectors. If a router has at least one neighbor marked with `"rr_client": true`, it is treated as a route reflector. IBGP routes are not re-advertised to other IBGP peers unless this derived route-reflector role applies. Client-learned routes are reflected to other peers except the origin peer; non-client-learned routes are reflected only to clients.

## Extension Points

Subclass `toposim::BgpRouter` and override:

- `onMessageReceived`
- `onOpenMessage`
- `onKeepaliveMessage`
- `onUpdateMessage`
- `onNotificationMessage`
- `importRouteAllowed`
- `exportRouteAllowed`
- `transformRouteForPeer`
- `selectBestRoute`

These hooks are intentionally virtual so custom BGP protocol experiments can replace policy, decision process, message handling, and attribute transforms without rewriting the topology manager.

## Interactive Commands

Press `Tab` in the interactive prompt to complete command keywords and router ids.
The prompt, help text, completion candidates, status messages and errors use Windows console colors plus ASCII markers such as `[OK]`, `[!]` and `[ERR]`.

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
quit
```
