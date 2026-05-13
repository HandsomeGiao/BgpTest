# TopoGenerator

`TopoGenerator` is an isolated PyQt6 topology editor that exports JSON for `TopoSimulator`.

## Run

```powershell
cd TopoGenerator
python -m pip install -r requirements.txt
python -m topogenerator.main
```

## Workflow

- Add routers, configure router id, ASN, route-reflector flag, cluster id and originated prefixes.
- Add links between routers, configure state, delay and route-reflector client direction.
- Export JSON and pass it to `TopoSimulator --topology`.

The editor stores visual node positions in each router's optional `position` field. The C++ simulator ignores that field.

