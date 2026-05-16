# TopoGenerator

`TopoGenerator` is an isolated PyQt6 topology editor that exports JSON for `TopoSimulator`.

## Run

```powershell
cd TopoGenerator
python -m pip install -r requirements.txt
python -m topogenerator.main
```

## Workflow

- Add routers directly on the canvas.
- Double-click an existing router to configure router id, ASN, cluster id and originated prefixes.
- New routers receive deterministic BGP router-ids from `10/8`: `R1` uses `10.0.0.1`, `R254` uses `10.0.0.254`, `R255` uses `10.0.1.1`, and so on. This keeps generated ids readable while staying valid for large topologies.
- Toggle `Link Mode (Q)`, then click two routers to create a link with `delay_ms` set to 0.
- Select an existing link and click `Edit` to configure state, link delay, A->B/B->A MRAI and route-reflector client direction.
- Right-drag the canvas to pan the view, and use the mouse wheel to zoom.
- Export JSON into the simulator's current working directory `topo/` folder, or pass it explicitly with `TopoSimulator --topology`.
- Loading a simulator JSON file restores directional MRAI and route-reflector client settings from each router's neighbor entries, so load/edit/export preserves those protocol fields.
- The editor remembers the last loaded or exported topology and automatically restores it on the next startup.

Routers in the same AS are automatically enclosed by a colored frame. The frame updates as routers are added, deleted, moved, or edited.

The editor stores visual node positions in each router's optional `position` field. The C++ simulator ignores that field.
