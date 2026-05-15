from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class RouterNode:
    id: str
    router_id: str
    asn: int
    cluster_id: str = ""
    originated_prefixes: list[str] = field(default_factory=list)
    x: float = 100.0
    y: float = 100.0

    @classmethod
    def from_json(cls, data: dict[str, Any], index: int) -> "RouterNode":
        position = data.get("position", {})
        return cls(
            id=str(data.get("id", f"R{index + 1}")),
            router_id=str(data.get("router_id", f"{index + 1}.{index + 1}.{index + 1}.{index + 1}")),
            asn=int(data.get("asn", 65000)),
            cluster_id=str(data.get("cluster_id", data.get("router_id", ""))),
            originated_prefixes=list(data.get("originated_prefixes", [])),
            x=float(position.get("x", 120 + index * 80)),
            y=float(position.get("y", 120 + index * 60)),
        )


@dataclass
class LinkEdge:
    a: str
    b: str
    enabled: bool = True
    delay_ms: int = 1
    rr_client_from_a: bool = False
    rr_client_from_b: bool = False

    @classmethod
    def from_json(cls, data: dict[str, Any]) -> "LinkEdge":
        return cls(
            a=str(data["a"]),
            b=str(data["b"]),
            enabled=bool(data.get("enabled", True)),
            delay_ms=int(data.get("delay_ms", 1)),
            rr_client_from_a=bool(data.get("rr_client_from_a", False)),
            rr_client_from_b=bool(data.get("rr_client_from_b", False)),
        )


@dataclass
class TopologyModel:
    simulation_name: str = "generated-topology"
    log_dir: str = "tmp"
    worker_threads: int = 0
    routers: dict[str, RouterNode] = field(default_factory=dict)
    links: list[LinkEdge] = field(default_factory=list)

    @classmethod
    def from_json(cls, data: dict[str, Any]) -> "TopologyModel":
        simulation = data.get("simulation", {})
        model = cls(
            simulation_name=str(simulation.get("name", "generated-topology")),
            log_dir=str(simulation.get("log_dir", "tmp")),
            worker_threads=int(simulation.get("worker_threads", 0)),
        )
        for index, router_data in enumerate(data.get("routers", [])):
            router = RouterNode.from_json(router_data, index)
            model.routers[router.id] = router
        model.links = [LinkEdge.from_json(link) for link in data.get("links", [])]
        return model

    def add_router(self, router: RouterNode) -> None:
        if not router.cluster_id:
            router.cluster_id = router.router_id
        self.routers[router.id] = router

    def remove_router(self, router_id: str) -> None:
        self.routers.pop(router_id, None)
        self.links = [link for link in self.links if link.a != router_id and link.b != router_id]

    def add_link(self, link: LinkEdge) -> None:
        if link.a == link.b:
            raise ValueError("A link must connect two different routers")
        if link.a not in self.routers or link.b not in self.routers:
            raise ValueError("Both link endpoints must exist")
        existing = {tuple(sorted((edge.a, edge.b))) for edge in self.links}
        if tuple(sorted((link.a, link.b))) in existing:
            raise ValueError("The link already exists")
        self.links.append(link)

    def remove_link(self, a: str, b: str) -> None:
        key = tuple(sorted((a, b)))
        self.links = [link for link in self.links if tuple(sorted((link.a, link.b))) != key]

    def to_json(self) -> dict[str, Any]:
        neighbor_map: dict[str, list[dict[str, Any]]] = {router_id: [] for router_id in self.routers}
        for link in self.links:
            router_a = self.routers[link.a]
            router_b = self.routers[link.b]
            session_type = "ibgp" if router_a.asn == router_b.asn else "ebgp"
            neighbor_map[link.a].append(
                {
                    "id": link.b,
                    "remote_asn": router_b.asn,
                    "session_type": session_type,
                    "rr_client": link.rr_client_from_a,
                    "enabled": link.enabled,
                }
            )
            neighbor_map[link.b].append(
                {
                    "id": link.a,
                    "remote_asn": router_a.asn,
                    "session_type": session_type,
                    "rr_client": link.rr_client_from_b,
                    "enabled": link.enabled,
                }
            )

        routers = []
        for router in self.routers.values():
            routers.append(
                {
                    "id": router.id,
                    "router_id": router.router_id,
                    "asn": router.asn,
                    "cluster_id": router.cluster_id or router.router_id,
                    "originated_prefixes": router.originated_prefixes,
                    "position": {"x": router.x, "y": router.y},
                    "neighbors": neighbor_map[router.id],
                }
            )

        return {
            "simulation": {
                "name": self.simulation_name,
                "log_dir": self.log_dir,
                "worker_threads": self.worker_threads,
                "convergence_quiet_ms": 300,
            },
            "routers": routers,
            "links": [
                {
                    "a": link.a,
                    "b": link.b,
                    "enabled": link.enabled,
                    "delay_ms": link.delay_ms,
                    "rr_client_from_a": link.rr_client_from_a,
                    "rr_client_from_b": link.rr_client_from_b,
                }
                for link in self.links
            ],
        }
