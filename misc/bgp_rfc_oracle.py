#!/usr/bin/env python3
"""Fast, independent BGP convergence oracle based on RFC 4271/4456.

This script intentionally does not reuse the BgpTester implementation.  It
models the conceptual Adj-RIB-In, Loc-RIB and Adj-RIB-Out processing described
by RFC 4271, especially Sections 5.1.2, 5.1.3, 5.1.5, 9.1 and 9.2.  Route
reflection, ORIGINATOR_ID and CLUSTER_LIST follow RFC 4456.

Only one current advertisement per (peer, prefix) and one selected route per
(router, prefix) are retained.  UPDATE timing, MRAI, link delay, transient path
exploration, logging, router plugins and TFP attributes are outside this
oracle.  Configured failed links are removed before the fixed point is found.

Examples:

    python misc/bgp_rfc_oracle.py BgpTester/topo/sample_topology.json \
        --router ISP

    python misc/bgp_rfc_oracle.py misc/provider-customer.json \
        --router R3 --prefix 1.0.0.0/8 --down-link R1 R3

    python misc/bgp_rfc_oracle.py misc/caida_topology.json \
        --router AS47458_C2 --show-candidates

Provider-customer/peer relationships are considered by default.  Use
``--policy rfc`` only when a neutral RFC baseline (equal preference for every
accepted external route and no commercial export filtering) is desired.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import sys
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


STANDARD_PLUGIN_ID = "org.bgptester.router.standard-bgp"
DEFAULT_LOCAL_PREF = 100
LOCAL_ORIGIN_PREF = (1 << 32) - 1
COMMERCIAL_LOCAL_PREF = {
    "customer": 200,
    "peer": 100,
    "provider": 50,
    "unspecified": DEFAULT_LOCAL_PREF,
}

SESSION_LOCAL = "local"
SESSION_IBGP = "ibgp"
SESSION_EBGP = "ebgp"

REL_UNSPECIFIED = "unspecified"
REL_PEER = "peer"
REL_PROVIDER = "provider"
REL_CUSTOMER = "customer"
VALID_RELATIONSHIPS = {
    REL_UNSPECIFIED,
    REL_PEER,
    "a_provider",
    "b_provider",
}

ORIGIN_IGP = 0
ORIGIN_NAMES = {0: "IGP", 1: "EGP", 2: "INCOMPLETE"}


class OracleError(Exception):
    """An input or convergence error suitable for displaying to the user."""


@dataclass(frozen=True)
class RouterConfig:
    id: str
    router_id: str
    asn: int
    cluster_id: str
    originated_prefixes: tuple[str, ...]
    plugin_id: str


@dataclass
class LinkConfig:
    a: str
    b: str
    enabled: bool = True
    relationship: str = REL_UNSPECIFIED
    rr_client_from_a: bool = False
    rr_client_from_b: bool = False
    explicit: bool = True
    enabled_explicit: bool = False

    @property
    def key(self) -> tuple[str, str]:
        return edge_key(self.a, self.b)


@dataclass(frozen=True)
class PeerConfig:
    id: str
    session: str
    relationship: str
    rr_client: bool


@dataclass(frozen=True)
class Advertisement:
    """The path information needed by this oracle's conceptual UPDATE."""

    as_path: tuple[int, ...]
    next_hop: str
    local_pref: int
    origin: int
    med: int
    source_kind: str
    originator_id: str | None
    cluster_list: tuple[str, ...]


@dataclass(frozen=True)
class Route:
    as_path: tuple[int, ...]
    next_hop: str
    local_pref: int
    origin: int
    med: int
    learned_from: str
    learned_session: str
    neighbor_as: int
    local_origin: bool
    source_kind: str
    originator_id: str | None
    cluster_list: tuple[str, ...]

    @property
    def as_path_length(self) -> int:
        # This oracle creates AS_SEQUENCE only.  RFC 4271 counts an AS_SET as
        # one, but the topology format has no way to originate an AS_SET.
        return len(self.as_path)


@dataclass
class RouterState:
    config: RouterConfig
    peers: dict[str, PeerConfig]
    local_routes: dict[str, Route] = field(default_factory=dict)
    adj_rib_in: dict[str, dict[str, Route]] = field(default_factory=dict)
    loc_rib: dict[str, Route] = field(default_factory=dict)


@dataclass
class Topology:
    routers: dict[str, RouterConfig]
    links: list[LinkConfig]
    convergence_quiet_ms: int
    rr_configured: bool
    plugin_ids: set[str]
    used_legacy_neighbors: bool

    def link_map(self) -> dict[tuple[str, str], LinkConfig]:
        return {link.key: link for link in self.links}


def edge_key(a: str, b: str) -> tuple[str, str]:
    return (a, b) if a < b else (b, a)


def _require_object(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise OracleError(f"{where} 必须是 JSON 对象")
    return value


def _json_int(value: Any, where: str, *, minimum: int = 0, maximum: int = (1 << 32) - 1) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise OracleError(f"{where} 必须是整数")
    number = int(value)
    if number != value or number < minimum or number > maximum:
        raise OracleError(f"{where} 必须是 {minimum}..{maximum} 的整数")
    return number


def _json_bool(value: Any, where: str) -> bool:
    if not isinstance(value, bool):
        raise OracleError(f"{where} 必须是布尔值")
    return value


def _validate_router_id(value: str, where: str) -> str:
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as error:
        raise OracleError(f"{where} 不是有效 IPv4 地址：{value}") from error
    if int(address) == 0:
        raise OracleError(f"{where} 不能为 0.0.0.0")
    return value


def _validate_cluster_id(value: str, where: str) -> str:
    try:
        ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as error:
        raise OracleError(f"{where} 不是有效 IPv4 地址：{value}") from error
    return value


def _validate_prefix(value: str, where: str) -> str:
    slash = value.rfind("/")
    if slash <= 0 or slash == len(value) - 1:
        raise OracleError(f"{where} 不是有效 IPv4 前缀：{value}")
    try:
        ipaddress.IPv4Address(value[:slash])
        length = int(value[slash + 1 :])
    except (ipaddress.AddressValueError, ValueError) as error:
        raise OracleError(f"{where} 不是有效 IPv4 前缀：{value}") from error
    if not 0 <= length <= 32:
        raise OracleError(f"{where} 的前缀长度必须是 0..32：{value}")
    return value


def _unique_trimmed_strings(value: Any, where: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if not isinstance(value, list):
        raise OracleError(f"{where} 必须是字符串数组")
    result: list[str] = []
    seen: set[str] = set()
    for index, item in enumerate(value):
        if not isinstance(item, str):
            raise OracleError(f"{where}[{index}] 必须是字符串")
        item = item.strip()
        if item and item not in seen:
            seen.add(item)
            result.append(item)
    return tuple(result)


def _default_router_id(one_based_index: int) -> str:
    zero_based = max(0, one_based_index - 1)
    second = zero_based // (256 * 254)
    remainder = zero_based % (256 * 254)
    third = remainder // 254
    fourth = remainder % 254 + 1
    return f"10.{second}.{third}.{fourth}"


def _plugin_id(entry: dict[str, Any]) -> str:
    plugin = entry.get("plugin")
    fallback = entry.get("plugin_id", STANDARD_PLUGIN_ID)
    if not isinstance(fallback, str):
        fallback = STANDARD_PLUGIN_ID
    if plugin is None:
        result = fallback
    elif isinstance(plugin, str):
        result = plugin
    elif isinstance(plugin, dict):
        result = plugin.get("id", fallback)
        if not isinstance(result, str):
            result = fallback
    else:
        raise OracleError("router.plugin 必须是字符串、对象或 null")
    result = result.strip()
    if not result:
        raise OracleError("router.plugin.id 不能为空")
    return result


def _link_relationship(value: Any, where: str) -> str:
    if value is None:
        return REL_UNSPECIFIED
    if not isinstance(value, str):
        raise OracleError(f"{where} 必须是字符串")
    value = value.strip().lower()
    if value not in VALID_RELATIONSHIPS:
        allowed = ", ".join(sorted(VALID_RELATIONSHIPS))
        raise OracleError(f"{where} 无效：{value}；可用值为 {allowed}")
    return value


def _legacy_relationship(value: Any, local_is_a: bool, where: str) -> str:
    if value is None:
        return REL_UNSPECIFIED
    if not isinstance(value, str):
        raise OracleError(f"{where} 必须是字符串")
    relation = value.strip().lower()
    if relation == REL_UNSPECIFIED:
        return REL_UNSPECIFIED
    if relation == REL_PEER:
        return REL_PEER
    if relation == REL_PROVIDER:
        return "b_provider" if local_is_a else "a_provider"
    if relation == REL_CUSTOMER:
        return "a_provider" if local_is_a else "b_provider"
    raise OracleError(f"{where} 无效：{value}")


def load_topology(path: Path) -> Topology:
    try:
        with path.open("r", encoding="utf-8-sig") as handle:
            root = json.load(handle)
    except OSError as error:
        raise OracleError(f"无法读取拓扑文件 {path}：{error}") from error
    except json.JSONDecodeError as error:
        raise OracleError(
            f"拓扑 JSON 语法错误：第 {error.lineno} 行，第 {error.colno} 列：{error.msg}"
        ) from error

    root = _require_object(root, "拓扑顶层")
    raw_routers = root.get("routers")
    raw_links = root.get("links", [])
    if not isinstance(raw_routers, list) or not raw_routers:
        raise OracleError("routers 必须是至少含一项的数组")
    if not isinstance(raw_links, list):
        raise OracleError("links 必须是数组")

    routers: dict[str, RouterConfig] = {}
    router_entries: dict[str, dict[str, Any]] = {}
    plugin_ids: set[str] = set()
    for index, raw in enumerate(raw_routers, start=1):
        entry = _require_object(raw, f"routers[{index - 1}]")
        router_id_text = entry.get("id", f"R{index}")
        if not isinstance(router_id_text, str) or not router_id_text.strip():
            raise OracleError(f"routers[{index - 1}].id 必须是非空字符串")
        node_id = router_id_text.strip()
        if node_id in routers:
            raise OracleError(f"路由器 ID 重复：{node_id}")

        bgp_id = entry.get("router_id", _default_router_id(index))
        if not isinstance(bgp_id, str):
            raise OracleError(f"路由器 {node_id} 的 router_id 必须是字符串")
        bgp_id = _validate_router_id(bgp_id.strip(), f"路由器 {node_id} 的 router_id")
        cluster_id = entry.get("cluster_id", bgp_id)
        if not isinstance(cluster_id, str):
            raise OracleError(f"路由器 {node_id} 的 cluster_id 必须是字符串")
        cluster_id = _validate_cluster_id(
            cluster_id.strip() or bgp_id, f"路由器 {node_id} 的 cluster_id"
        )
        asn = _json_int(entry.get("asn", 65000), f"路由器 {node_id} 的 asn", minimum=1)
        prefixes = _unique_trimmed_strings(
            entry.get("originated_prefixes", []), f"路由器 {node_id} 的 originated_prefixes"
        )
        for prefix in prefixes:
            _validate_prefix(prefix, f"路由器 {node_id} 的 originated_prefixes")
        plugin_id = _plugin_id(entry)
        plugin_ids.add(plugin_id)
        routers[node_id] = RouterConfig(
            id=node_id,
            router_id=bgp_id,
            asn=asn,
            cluster_id=cluster_id,
            originated_prefixes=prefixes,
            plugin_id=plugin_id,
        )
        router_entries[node_id] = entry

    bgp_ids: dict[str, str] = {}
    for router in routers.values():
        previous = bgp_ids.get(router.router_id)
        if previous is not None:
            raise OracleError(
                f"BGP Router ID 重复：{router.router_id}（{previous} 与 {router.id}）"
            )
        bgp_ids[router.router_id] = router.id

    links: list[LinkConfig] = []
    by_edge: dict[tuple[str, str], LinkConfig] = {}
    rr_configured = False
    for index, raw in enumerate(raw_links):
        entry = _require_object(raw, f"links[{index}]")
        a = entry.get("a")
        b = entry.get("b")
        if not isinstance(a, str) or not isinstance(b, str):
            raise OracleError(f"links[{index}].a 和 .b 必须是字符串")
        a, b = a.strip(), b.strip()
        if a not in routers or b not in routers:
            raise OracleError(f"链路端点不存在：{a} - {b}")
        if a == b:
            raise OracleError(f"链路不能连接路由器自身：{a}")
        key = edge_key(a, b)
        if key in by_edge:
            raise OracleError(f"链路重复：{a} - {b}")
        enabled = entry.get("enabled", True)
        if "enabled" in entry:
            enabled = _json_bool(enabled, f"links[{index}].enabled")
        rr_a = entry.get("rr_client_from_a", False)
        rr_b = entry.get("rr_client_from_b", False)
        if "rr_client_from_a" in entry:
            rr_a = _json_bool(rr_a, f"links[{index}].rr_client_from_a")
        if "rr_client_from_b" in entry:
            rr_b = _json_bool(rr_b, f"links[{index}].rr_client_from_b")
        relationship = _link_relationship(entry.get("relationship"), f"links[{index}].relationship")
        if routers[a].asn == routers[b].asn and relationship != REL_UNSPECIFIED:
            raise OracleError(f"同一 AS 内链路不能设置商业关系：{a} - {b}")
        if routers[a].asn != routers[b].asn and (rr_a or rr_b):
            raise OracleError(f"EBGP 链路不能配置 RR Client：{a} - {b}")
        link = LinkConfig(
            a=a,
            b=b,
            enabled=enabled,
            relationship=relationship,
            rr_client_from_a=rr_a,
            rr_client_from_b=rr_b,
            explicit=True,
            enabled_explicit="enabled" in entry,
        )
        links.append(link)
        by_edge[key] = link
        rr_configured = rr_configured or rr_a or rr_b

    # Older files may contain the session graph only in routers[].neighbors.
    # Explicit links remain authoritative; legacy entries only create missing
    # edges and fill the two directions of those generated edges.
    used_legacy_neighbors = False
    legacy_relationships: dict[tuple[str, str], str] = {}
    for local_id, router_entry in router_entries.items():
        neighbors = router_entry.get("neighbors", [])
        if not isinstance(neighbors, list):
            raise OracleError(f"路由器 {local_id} 的 neighbors 必须是数组")
        for index, raw_neighbor in enumerate(neighbors):
            if not isinstance(raw_neighbor, dict):
                continue
            peer_id = raw_neighbor.get("id")
            if not isinstance(peer_id, str):
                continue
            peer_id = peer_id.strip()
            if peer_id not in routers or peer_id == local_id:
                continue
            key = edge_key(local_id, peer_id)
            link = by_edge.get(key)
            if link is not None and link.explicit:
                continue
            used_legacy_neighbors = True
            if link is None:
                link = LinkConfig(a=local_id, b=peer_id, explicit=False)
                links.append(link)
                by_edge[key] = link
            local_is_a = link.a == local_id
            if not link.enabled_explicit and "enabled" in raw_neighbor:
                link.enabled = link.enabled and _json_bool(
                    raw_neighbor["enabled"],
                    f"路由器 {local_id} 的 neighbors[{index}].enabled",
                )
            rr_value = raw_neighbor.get("rr_client", False)
            if "rr_client" in raw_neighbor:
                rr_value = _json_bool(
                    rr_value, f"路由器 {local_id} 的 neighbors[{index}].rr_client"
                )
            if local_is_a:
                link.rr_client_from_a = rr_value
            else:
                link.rr_client_from_b = rr_value
            rr_configured = rr_configured or rr_value
            relation = _legacy_relationship(
                raw_neighbor.get("relationship"),
                local_is_a,
                f"路由器 {local_id} 的 neighbors[{index}].relationship",
            )
            if relation != REL_UNSPECIFIED:
                previous = legacy_relationships.get(key)
                if previous is not None and previous != relation:
                    raise OracleError(f"链路 {link.a} - {link.b} 的双向 relationship 不一致")
                legacy_relationships[key] = relation
                link.relationship = relation

    for link in links:
        if (
            routers[link.a].asn != routers[link.b].asn
            and (link.rr_client_from_a or link.rr_client_from_b)
        ):
            raise OracleError(f"EBGP 链路不能配置 RR Client：{link.a} - {link.b}")

    simulation = root.get("simulation", {})
    if simulation is None:
        simulation = {}
    simulation = _require_object(simulation, "simulation")
    quiet = simulation.get("convergence_quiet_ms", 1000)
    if isinstance(quiet, bool) or not isinstance(quiet, (int, float)) or int(quiet) != quiet or quiet < 0:
        raise OracleError("simulation.convergence_quiet_ms 必须是非负整数")

    return Topology(
        routers=routers,
        links=links,
        convergence_quiet_ms=int(quiet),
        rr_configured=rr_configured,
        plugin_ids=plugin_ids,
        used_legacy_neighbors=used_legacy_neighbors,
    )


def relationship_for(link: LinkConfig, local_id: str) -> str:
    local_is_a = link.a == local_id
    if link.relationship == REL_UNSPECIFIED:
        return REL_UNSPECIFIED
    if link.relationship == REL_PEER:
        return REL_PEER
    if link.relationship == "a_provider":
        return REL_CUSTOMER if local_is_a else REL_PROVIDER
    if link.relationship == "b_provider":
        return REL_PROVIDER if local_is_a else REL_CUSTOMER
    raise AssertionError(f"unexpected relationship: {link.relationship}")


def _router_id_number(value: str) -> int:
    return int(ipaddress.IPv4Address(value))


class Rfc4271Oracle:
    """A coalescing path-vector fixed-point evaluator."""

    def __init__(
        self,
        topology: Topology,
        prefixes: Iterable[str],
        *,
        policy: str,
        max_updates: int,
    ) -> None:
        self.topology = topology
        self.prefixes = set(prefixes)
        self.policy = policy
        self.max_updates = max_updates
        self.states: dict[str, RouterState] = {}
        self.adj_rib_out: dict[tuple[str, str, str], Advertisement] = {}
        self.pending: dict[tuple[str, str, str], Advertisement | None] = {}
        self.queue: deque[tuple[str, str, str]] = deque()
        self.updates_processed = 0
        self.best_route_changes = 0

        peer_maps: dict[str, dict[str, PeerConfig]] = {
            router_id: {} for router_id in topology.routers
        }
        for link in topology.links:
            if not link.enabled:
                continue
            a = topology.routers[link.a]
            b = topology.routers[link.b]
            session = SESSION_IBGP if a.asn == b.asn else SESSION_EBGP
            peer_maps[a.id][b.id] = PeerConfig(
                id=b.id,
                session=session,
                relationship=relationship_for(link, a.id),
                rr_client=link.rr_client_from_a,
            )
            peer_maps[b.id][a.id] = PeerConfig(
                id=a.id,
                session=session,
                relationship=relationship_for(link, b.id),
                rr_client=link.rr_client_from_b,
            )

        for router_id in sorted(topology.routers):
            config = topology.routers[router_id]
            state = RouterState(config=config, peers=peer_maps[router_id])
            for prefix in config.originated_prefixes:
                if prefix not in self.prefixes:
                    continue
                state.local_routes[prefix] = Route(
                    as_path=(),
                    next_hop=config.router_id,
                    local_pref=LOCAL_ORIGIN_PREF,
                    origin=ORIGIN_IGP,
                    med=0,
                    learned_from=config.id,
                    learned_session=SESSION_LOCAL,
                    neighbor_as=config.asn,
                    local_origin=True,
                    source_kind="local",
                    originator_id=None,
                    cluster_list=(),
                )
            self.states[router_id] = state

    def run(self) -> None:
        # Inject local routes through the same selection/export pipeline used
        # after every later Adj-RIB-In change.
        for router_id in sorted(self.states):
            for prefix in sorted(self.states[router_id].local_routes):
                self._recompute(router_id, prefix)

        while self.queue:
            if self.updates_processed >= self.max_updates:
                raise OracleError(
                    "在达到 --max-updates 上限后仍未收敛；拓扑/策略可能发生路径振荡，"
                    "也可以提高该上限后重试"
                )
            key = self.queue.popleft()
            advertisement = self.pending.pop(key)
            sender, receiver, prefix = key
            self.updates_processed += 1
            self._receive(sender, receiver, prefix, advertisement)

    def _enqueue(
        self,
        sender: str,
        receiver: str,
        prefix: str,
        advertisement: Advertisement | None,
    ) -> None:
        key = (sender, receiver, prefix)
        if key not in self.pending:
            self.queue.append(key)
        # Coalesce path exploration: a newer replacement supersedes an UPDATE
        # that has not yet been consumed by the conceptual peer.
        self.pending[key] = advertisement

    def _receive(
        self,
        sender: str,
        receiver: str,
        prefix: str,
        advertisement: Advertisement | None,
    ) -> None:
        state = self.states[receiver]
        peer = state.peers.get(sender)
        if peer is None:
            return
        peer_routes = state.adj_rib_in.setdefault(prefix, {})
        old_route = peer_routes.get(sender)
        new_route = (
            None
            if advertisement is None
            else self._import_route(state, peer, advertisement)
        )
        if new_route is None:
            peer_routes.pop(sender, None)
            if not peer_routes:
                state.adj_rib_in.pop(prefix, None)
        else:
            peer_routes[sender] = new_route
        if old_route != new_route:
            self._recompute(receiver, prefix)

    def _import_route(
        self,
        state: RouterState,
        peer: PeerConfig,
        advertisement: Advertisement,
    ) -> Route | None:
        # RFC 4271 Section 9.1.2: exclude an AS loop.  NEXT_HOP
        # resolvability is otherwise assumed because the JSON has no IGP RIB.
        # RFC 4456 Section 8 adds the two route-reflection loop checks.
        if advertisement.originator_id == state.config.router_id:
            return None
        if state.config.cluster_id in advertisement.cluster_list:
            return None
        if state.config.asn in advertisement.as_path:
            return None
        if advertisement.next_hop == state.config.router_id:
            return None

        if peer.session == SESSION_IBGP:
            local_pref = advertisement.local_pref
            source_kind = advertisement.source_kind
        elif self.policy == "commercial":
            local_pref = COMMERCIAL_LOCAL_PREF[peer.relationship]
            source_kind = peer.relationship
        else:
            # RFC 4271 leaves the EBGP degree-of-preference function to the
            # local PIB.  The neutral reference policy assigns every accepted
            # external route the same value.
            local_pref = DEFAULT_LOCAL_PREF
            source_kind = REL_UNSPECIFIED

        neighbor_as = (
            advertisement.as_path[0]
            if advertisement.as_path
            else state.config.asn
        )
        return Route(
            as_path=advertisement.as_path,
            next_hop=advertisement.next_hop,
            local_pref=local_pref,
            origin=advertisement.origin,
            med=advertisement.med,
            learned_from=peer.id,
            learned_session=peer.session,
            neighbor_as=neighbor_as,
            local_origin=False,
            source_kind=source_kind,
            originator_id=advertisement.originator_id,
            cluster_list=advertisement.cluster_list,
        )

    def _recompute(self, router_id: str, prefix: str) -> None:
        state = self.states[router_id]
        candidates: list[Route] = []
        local = state.local_routes.get(prefix)
        if local is not None:
            candidates.append(local)
        candidates.extend(state.adj_rib_in.get(prefix, {}).values())
        selected = self._select_best(state, candidates)
        previous = state.loc_rib.get(prefix)
        if selected is None:
            state.loc_rib.pop(prefix, None)
        else:
            state.loc_rib[prefix] = selected
        if previous != selected:
            self.best_route_changes += 1
            self._refresh_exports(state, prefix, selected)

    def _select_best(self, state: RouterState, candidates: list[Route]) -> Route | None:
        if not candidates:
            return None

        # RFC 4271 9.1.1: highest degree of preference.
        best_pref = max(route.local_pref for route in candidates)
        remaining = [route for route in candidates if route.local_pref == best_pref]

        # RFC 4271 9.1.2.2(a): shortest AS_PATH.  Only AS_SEQUENCE can be
        # constructed from this topology, so len(as_path) is the RFC count.
        shortest = min(route.as_path_length for route in remaining)
        remaining = [route for route in remaining if route.as_path_length == shortest]

        # RFC 4271 9.1.2.2(b): lowest ORIGIN number.
        lowest_origin = min(route.origin for route in remaining)
        remaining = [route for route in remaining if route.origin == lowest_origin]

        # RFC 4271 9.1.2.2(c): compare MED only within the same neighboring AS.
        remaining = [
            route
            for route in remaining
            if not any(
                other.neighbor_as == route.neighbor_as and other.med < route.med
                for other in remaining
            )
        ]

        # RFC 4271 9.1.2.2(d): EBGP over IBGP.  A local route receives the
        # maximum degree of preference above and therefore will not normally
        # reach this ambiguous originated-route case.
        if any(route.learned_session == SESSION_EBGP for route in remaining):
            remaining = [
                route for route in remaining if route.learned_session == SESSION_EBGP
            ]

        # Step (e), IGP cost to NEXT_HOP, is skipped: the topology does not
        # contain an IGP RIB or IGP metrics.  RFC 4271 explicitly permits all
        # costs to be treated as equal when no cost can be determined.

        # RFC 4271 step (f), as amended by RFC 4456 Section 9: use
        # ORIGINATOR_ID as the advertising BGP Identifier when present.
        lowest_identifier = min(
            _router_id_number(
                route.originator_id
                or self.topology.routers[route.learned_from].router_id
            )
            for route in remaining
        )
        remaining = [
            route
            for route in remaining
            if _router_id_number(
                route.originator_id
                or self.topology.routers[route.learned_from].router_id
            )
            == lowest_identifier
        ]

        # RFC 4456 inserts the shortest CLUSTER_LIST rule between RFC 4271
        # steps (f) and (g).
        shortest_cluster_list = min(len(route.cluster_list) for route in remaining)
        remaining = [
            route
            for route in remaining
            if len(route.cluster_list) == shortest_cluster_list
        ]

        # RFC 4271 step (g): lowest peer address.  Session interface addresses
        # are absent from the topology, so peer router_id is the surrogate.
        lowest_peer_address = min(
            _router_id_number(self.topology.routers[route.learned_from].router_id)
            for route in remaining
        )
        remaining = [
            route
            for route in remaining
            if _router_id_number(
                self.topology.routers[route.learned_from].router_id
            )
            == lowest_peer_address
        ]

        return min(
            remaining,
            key=lambda route: (
                route.learned_from,
                route.next_hop,
                route.as_path,
            ),
        )

    def _refresh_exports(
        self,
        state: RouterState,
        prefix: str,
        selected: Route | None,
    ) -> None:
        for peer_id in sorted(state.peers):
            peer = state.peers[peer_id]
            desired = (
                None
                if selected is None
                else self._export_route(state, peer, selected)
            )
            key = (state.config.id, peer_id, prefix)
            previous = self.adj_rib_out.get(key)
            if desired is None:
                if key in self.adj_rib_out:
                    self.adj_rib_out.pop(key, None)
                    self._enqueue(state.config.id, peer_id, prefix, None)
            elif previous != desired:
                self.adj_rib_out[key] = desired
                self._enqueue(state.config.id, peer_id, prefix, desired)

    def _export_route(
        self,
        state: RouterState,
        peer: PeerConfig,
        route: Route,
    ) -> Advertisement | None:
        if route.learned_from == peer.id:
            return None

        reflecting = False
        # RFC 4271 9.2 normally forbids IBGP-to-IBGP redistribution.  RFC 4456
        # Section 6 permits an RR to reflect a non-client route to clients, or
        # a client route to both client and non-client peers.
        if peer.session == SESSION_IBGP and route.learned_session == SESSION_IBGP:
            incoming_peer = state.peers.get(route.learned_from)
            learned_from_client = bool(
                incoming_peer is not None
                and incoming_peer.session == SESSION_IBGP
                and incoming_peer.rr_client
            )
            is_route_reflector = any(
                candidate.session == SESSION_IBGP and candidate.rr_client
                for candidate in state.peers.values()
            )
            if not is_route_reflector or not (learned_from_client or peer.rr_client):
                return None
            reflecting = True

        if self.policy == "commercial" and peer.session == SESSION_EBGP:
            if (
                peer.relationship in {REL_PEER, REL_PROVIDER}
                and route.source_kind in {REL_PEER, REL_PROVIDER}
            ):
                return None

        if peer.session == SESSION_EBGP:
            as_path = (state.config.asn,) + route.as_path
            next_hop = state.config.router_id
            # LOCAL_PREF is not sent to an external peer (RFC 4271 5.1.5).
            # Its value here is an internal placeholder ignored on EBGP import.
            local_pref = DEFAULT_LOCAL_PREF
            source_kind = REL_UNSPECIFIED
            # These RFC 4456 attributes are optional non-transitive and stay
            # inside the local AS.
            originator_id = None
            cluster_list: tuple[str, ...] = ()
            # The input has no locally configured MED.  A received MED is not
            # propagated from one neighboring AS to another (Section 5.1.4).
            med = 0
        else:
            as_path = route.as_path
            next_hop = route.next_hop
            local_pref = route.local_pref
            source_kind = route.source_kind
            med = route.med
            originator_id = route.originator_id
            cluster_list = route.cluster_list
            if reflecting:
                if originator_id is None:
                    originator_id = self.topology.routers[
                        route.learned_from
                    ].router_id
                cluster_list = (state.config.cluster_id,) + cluster_list

        return Advertisement(
            as_path=as_path,
            next_hop=next_hop,
            local_pref=local_pref,
            origin=route.origin,
            med=med,
            source_kind=source_kind,
            originator_id=originator_id,
            cluster_list=cluster_list,
        )

    def control_plane_path(self, router_id: str, prefix: str) -> list[str]:
        route = self.states[router_id].loc_rib.get(prefix)
        if route is None:
            return [router_id]
        return self.control_plane_path_for_route(router_id, prefix, route)

    def control_plane_path_for_route(
        self, router_id: str, prefix: str, route: Route
    ) -> list[str]:
        path = [router_id]
        seen = {router_id}
        current_route: Route | None = route
        while current_route is not None:
            if (
                current_route.local_origin
                or not current_route.learned_from
                or current_route.learned_from in seen
                or current_route.learned_from not in self.states
            ):
                break
            current = current_route.learned_from
            seen.add(current)
            path.append(current)
            current_route = self.states[current].loc_rib.get(prefix)
        return path


def _route_as_json(
    oracle: Rfc4271Oracle,
    router_id: str,
    prefix: str,
    route: Route,
) -> dict[str, Any]:
    return {
        "prefix": prefix,
        "local_origin": route.local_origin,
        "learned_from": route.learned_from,
        "learned_session": route.learned_session,
        "next_hop": route.next_hop,
        "local_pref": route.local_pref,
        "as_path": list(route.as_path),
        "as_path_length": route.as_path_length,
        "origin": ORIGIN_NAMES.get(route.origin, str(route.origin)),
        "med": route.med,
        "neighbor_as": route.neighbor_as,
        "source_kind": route.source_kind,
        "originator_id": route.originator_id,
        "cluster_list": list(route.cluster_list),
        "control_plane_path": oracle.control_plane_path_for_route(
            router_id, prefix, route
        ),
    }


def _candidate_rows(
    oracle: Rfc4271Oracle,
    router_id: str,
    requested_prefix: str | None,
) -> list[dict[str, Any]]:
    state = oracle.states[router_id]
    if requested_prefix is not None:
        prefixes = [requested_prefix]
    else:
        prefixes = sorted(set(state.local_routes) | set(state.adj_rib_in))

    rows: list[dict[str, Any]] = []
    for prefix in prefixes:
        selected = state.loc_rib.get(prefix)
        local = state.local_routes.get(prefix)
        if local is not None:
            row = _route_as_json(oracle, router_id, prefix, local)
            row["candidate_rib"] = "local"
            row["selected"] = local == selected
            rows.append(row)
        for peer_id, route in sorted(state.adj_rib_in.get(prefix, {}).items()):
            row = _route_as_json(oracle, router_id, prefix, route)
            row["candidate_rib"] = "adj-rib-in"
            row["selected"] = route == selected
            rows.append(row)
    return rows


def _format_as_path(route: Route) -> str:
    return " ".join(str(asn) for asn in route.as_path) if route.as_path else "-"


def _print_table(rows: list[dict[str, Any]]) -> None:
    headers = [
        "PREFIX",
        "FROM",
        "SESSION",
        "NEXT_HOP",
        "LOCAL_PREF",
        "AS_PATH_LEN",
        "AS_PATH",
    ]
    values = [
        [
            row["prefix"],
            row["learned_from"],
            row["learned_session"],
            row["next_hop"],
            str(row["local_pref"]),
            str(row["as_path_length"]),
            " ".join(str(asn) for asn in row["as_path"]) or "-",
        ]
        for row in rows
    ]
    widths = [len(header) for header in headers]
    for row in values:
        for index, value in enumerate(row):
            widths[index] = max(widths[index], len(value))
    print("  ".join(header.ljust(widths[index]) for index, header in enumerate(headers)))
    print("  ".join("-" * width for width in widths))
    for row in values:
        print("  ".join(value.ljust(widths[index]) for index, value in enumerate(row)))


def _print_candidate_table(rows: list[dict[str, Any]]) -> None:
    headers = [
        "BEST",
        "PREFIX",
        "RIB",
        "FROM",
        "SESSION",
        "NEXT_HOP",
        "LOCAL_PREF",
        "AS_PATH_LEN",
        "AS_PATH",
    ]
    values = [
        [
            "*" if row["selected"] else "",
            row["prefix"],
            row["candidate_rib"],
            row["learned_from"],
            row["learned_session"],
            row["next_hop"],
            str(row["local_pref"]),
            str(row["as_path_length"]),
            " ".join(str(asn) for asn in row["as_path"]) or "-",
        ]
        for row in rows
    ]
    widths = [len(header) for header in headers]
    for row in values:
        for index, value in enumerate(row):
            widths[index] = max(widths[index], len(value))
    print("  ".join(header.ljust(widths[index]) for index, header in enumerate(headers)))
    print("  ".join("-" * width for width in widths))
    for row in values:
        print("  ".join(value.ljust(widths[index]) for index, value in enumerate(row)))


def _build_assumptions(topology: Topology, policy: str) -> list[str]:
    assumptions = [
        "依据 RFC 4271 的概念性决策过程及 RFC 4456 Route Reflection；只保留每邻居当前通告和每前缀最佳路由",
        "所有已启用链路视为会话已建立；断链在计算开始前移除",
        f"本地起源路由使用本地策略 degree of preference={LOCAL_ORIGIN_PREF}，以优先于外部路由",
        "NEXT_HOP 均视为可解析；缺少 IGP RIB/代价，因此 IGP cost tie-break 视为相等",
        "忽略链路时延、MRAI、消息时序、瞬态 path exploration、聚合和衰减",
        "忽略路由器插件、TFP 与插件私有设置",
    ]
    if policy == "rfc":
        assumptions.append(
            "RFC 未规定 EBGP 本地策略；参考策略为所有外部路由 degree of preference=100"
        )
    else:
        assumptions.append(
            "额外本地策略（非 RFC 强制）：customer(200) > peer(100) > provider(50)，并执行 valley-free 出口过滤"
        )
    if topology.rr_configured:
        assumptions.append(
            "方向性 RR Client、ORIGINATOR_ID、CLUSTER_LIST 与反射选路修正规则按 RFC 4456 处理"
        )
    nonstandard_plugins = sorted(topology.plugin_ids - {STANDARD_PLUGIN_ID})
    if nonstandard_plugins:
        assumptions.append("已忽略非 RFC 插件：" + ", ".join(nonstandard_plugins))
    if topology.used_legacy_neighbors:
        assumptions.append("拓扑中缺失的链路已由旧式 routers[].neighbors 补建")
    return assumptions


def _positive_int(value: str) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("必须是正整数") from error
    if number <= 0:
        raise argparse.ArgumentTypeError("必须是正整数")
    return number


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "依据 RFC 4271/4456 快速计算断链后某台路由器的 Loc-RIB，或某前缀的最佳路由。"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "示例：\n"
            "  python misc/bgp_rfc_oracle.py misc/caida_topology.json --router R20\n"
            "  python misc/bgp_rfc_oracle.py misc/provider-customer.json --router R3 "
            "--prefix 1.0.0.0/8 --down-link R1 R3\n"
            "  python misc/bgp_rfc_oracle.py topo.json --router R9 --json\n"
            "  python misc/bgp_rfc_oracle.py topo.json --router R9 --policy rfc\n"
            "  python misc/bgp_rfc_oracle.py misc/caida_topology.json "
            "--router AS47458_C2 --show-candidates"
        ),
    )
    parser.add_argument("topology", type=Path, help="拓扑 JSON 文件")
    parser.add_argument("-r", "--router", required=True, help="要查询的路由器 ID（区分大小写）")
    parser.add_argument(
        "-p",
        "--prefix",
        help="只计算并查询这个精确前缀；省略时输出该路由器的完整 Loc-RIB",
    )
    parser.add_argument(
        "--down-link",
        "--down",
        action="append",
        nargs=2,
        default=[],
        metavar=("ROUTER_A", "ROUTER_B"),
        help="计算前断开一条链路；可重复指定",
    )
    parser.add_argument(
        "--policy",
        choices=("rfc", "commercial"),
        default="commercial",
        help="EBGP 本地策略：commercial=使用商业关系偏好和 valley-free 出口（默认）；rfc=中性参考策略",
    )
    parser.add_argument(
        "--max-updates",
        type=_positive_int,
        default=5_000_000,
        help="判定路径振荡前最多处理的合并 UPDATE 数（默认 5000000）",
    )
    parser.add_argument(
        "--show-candidates",
        action="store_true",
        help="在 Loc-RIB 之外输出本地路由及每邻居当前的 Adj-RIB-In 候选，并用 * 标出最佳路由",
    )
    parser.add_argument("--json", action="store_true", help="以 JSON 输出，便于和框架结果自动比较")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        topology = load_topology(args.topology)
        if args.router not in topology.routers:
            available = ", ".join(sorted(topology.routers)[:20])
            suffix = " ..." if len(topology.routers) > 20 else ""
            raise OracleError(
                f"路由器不存在：{args.router}；可用路由器：{available}{suffix}"
            )

        if args.prefix is not None:
            args.prefix = _validate_prefix(args.prefix.strip(), "--prefix")

        links = topology.link_map()
        requested_down: list[tuple[str, str]] = []
        seen_down: set[tuple[str, str]] = set()
        for a, b in args.down_link:
            a, b = a.strip(), b.strip()
            key = edge_key(a, b)
            link = links.get(key)
            if link is None:
                raise OracleError(f"--down-link 指定的链路不存在：{a} - {b}")
            if key in seen_down:
                raise OracleError(f"--down-link 重复指定：{a} - {b}")
            seen_down.add(key)
            requested_down.append(key)
            link.enabled = False

        all_prefixes = sorted(
            {
                prefix
                for router in topology.routers.values()
                for prefix in router.originated_prefixes
            }
        )
        selected_prefixes = [args.prefix] if args.prefix is not None else all_prefixes
        oracle = Rfc4271Oracle(
            topology,
            selected_prefixes,
            policy=args.policy,
            max_updates=args.max_updates,
        )
        oracle.run()

        state = oracle.states[args.router]
        if args.prefix is not None:
            route_items = (
                [(args.prefix, state.loc_rib[args.prefix])]
                if args.prefix in state.loc_rib
                else []
            )
        else:
            route_items = sorted(state.loc_rib.items())
        rows = [
            _route_as_json(oracle, args.router, prefix, route)
            for prefix, route in route_items
        ]
        candidate_rows = (
            _candidate_rows(oracle, args.router, args.prefix)
            if args.show_candidates
            else []
        )
        assumptions = _build_assumptions(topology, args.policy)
        effective_down = sorted(link.key for link in topology.links if not link.enabled)
        result = {
            "model": "RFC 4271 + RFC 4456 fast best-path oracle",
            "topology": str(args.topology.resolve()),
            "policy": args.policy,
            "router": {
                "id": state.config.id,
                "router_id": state.config.router_id,
                "asn": state.config.asn,
                "cluster_id": state.config.cluster_id,
            },
            "requested_prefix": args.prefix,
            "requested_down_links": [list(link) for link in requested_down],
            "effective_down_links": [list(link) for link in effective_down],
            "updates_processed": oracle.updates_processed,
            "best_route_changes": oracle.best_route_changes,
            "route_count": len(rows),
            "routes": rows,
            "assumptions": assumptions,
        }
        if args.show_candidates:
            result["candidate_count"] = len(candidate_rows)
            result["candidates"] = candidate_rows

        if args.json:
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0

        print(
            f"路由器: {state.config.id}  "
            f"Router-ID={state.config.router_id}  AS={state.config.asn}"
        )
        print(f"策略: {args.policy}  已处理合并 UPDATE: {oracle.updates_processed}")
        if effective_down:
            print(
                "断开链路: "
                + ", ".join(f"{a} <-> {b}" for a, b in effective_down)
            )
        else:
            print("断开链路: 无")

        if args.prefix is not None:
            print(f"查询前缀: {args.prefix}")
            if not rows:
                print("最佳路由: 无")
            else:
                row = rows[0]
                route = route_items[0][1]
                print("最佳路由:")
                print(f"  来源邻居: {row['learned_from']} ({row['learned_session']})")
                print(f"  NEXT_HOP: {row['next_hop']}")
                print(f"  LOCAL_PREF / degree: {row['local_pref']}")
                print(f"  AS_PATH: {_format_as_path(route)}")
                print(f"  AS_PATH 长度: {row['as_path_length']}")
                print(f"  ORIGIN: {row['origin']}  MED: {row['med']}")
                if row["originator_id"] is not None:
                    print(f"  ORIGINATOR_ID: {row['originator_id']}")
                if row["cluster_list"]:
                    print("  CLUSTER_LIST: " + " ".join(row["cluster_list"]))
                print("  控制面路径: " + " -> ".join(row["control_plane_path"]))
        else:
            print(f"Loc-RIB 路由数: {len(rows)}")
            if rows:
                _print_table(rows)
            else:
                print("Loc-RIB 为空")

        if args.show_candidates:
            print(f"当前候选路由数（本地 + Adj-RIB-In）: {len(candidate_rows)}")
            if candidate_rows:
                print("* 表示该前缀最终选入 Loc-RIB 的最佳路由")
                _print_candidate_table(candidate_rows)
            else:
                print("当前候选路由为空")

        print("注意事项:")
        for assumption in assumptions:
            print(f"  - {assumption}")
        return 0
    except OracleError as error:
        print(f"错误: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
