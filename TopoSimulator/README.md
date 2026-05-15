# TopoSimulator

`TopoSimulator` is a pure C++20 BGP network simulation skeleton for large-scale convergence experiments. It reads a single JSON topology, starts all configured BGP speakers, exchanges simplified RFC4271-style OPEN, KEEPALIVE, UPDATE and NOTIFICATION messages, writes BMP-like receive logs, waits for convergence, then accepts interactive topology changes.

Supported environment: Windows + MSVC + vcpkg only.

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
