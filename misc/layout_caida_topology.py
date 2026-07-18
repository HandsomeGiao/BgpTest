#!/usr/bin/env python3
"""Re-layout a BgpTester topology while preserving every non-position field.

The layout works at two levels:

1. Routers in the same AS form a rigid cluster.  Their existing relative
   geometry is preserved.
2. Provider-to-customer AS relationships form a directed hierarchy.  A
   variable-width tidy-tree pass determines a crossing-free forest order,
   then every rank is packed compactly.  A barycentric layered layout is
   used for a general acyclic multi-provider graph.

Screen y coordinates grow downwards, so every provider AS is assigned a
strictly smaller y band than each of its customers.  Cyclic commercial
relationships are rejected because no layout can satisfy that constraint.
"""

from __future__ import annotations

import argparse
import heapq
import json
import math
import os
import sys
import tempfile
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_INPUT = SCRIPT_DIR / "caida_topology.json"
DEFAULT_OUTPUT = SCRIPT_DIR / "caida_topology_layout.json"


class LayoutError(RuntimeError):
    """Raised when the topology cannot be laid out safely."""


@dataclass(frozen=True)
class Cluster:
    asn: int
    router_ids: tuple[str, ...]
    offsets: dict[str, tuple[float, float]]
    old_center_x: float
    old_center_y: float
    half_width: float
    top_extent: float
    bottom_extent: float


@dataclass(frozen=True)
class ProviderLink:
    provider_asn: int
    customer_asn: int
    provider_router: str
    customer_router: str


@dataclass
class AsGraph:
    children: dict[int, set[int]]
    parents: dict[int, set[int]]
    provider_links: list[ProviderLink]
    soft_neighbors: dict[int, set[int]]


@dataclass(frozen=True)
class LayoutResult:
    as_x: dict[int, float]
    ranks: dict[int, int]
    mode: str


def positive_float(raw: str) -> float:
    try:
        value = float(raw)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"必须是数字：{raw}") from exc
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("必须是大于 0 的有限数字")
    return value


def nonnegative_float(raw: str) -> float:
    try:
        value = float(raw)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"必须是数字：{raw}") from exc
    if not math.isfinite(value) or value < 0:
        raise argparse.ArgumentTypeError("必须是非负有限数字")
    return value


def positive_int(raw: str) -> int:
    try:
        value = int(raw)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"必须是整数：{raw}") from exc
    if value <= 0:
        raise argparse.ArgumentTypeError("必须是正整数")
    return value


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "按 provider→customer 层级重新排布 BgpTester JSON 拓扑；"
            "只修改 routers[].position。"
        )
    )
    parser.add_argument(
        "input",
        nargs="?",
        type=Path,
        default=DEFAULT_INPUT,
        help=f"输入拓扑（默认：{DEFAULT_INPUT}）",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="输出路径（默认：输入文件名加 _layout）",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="允许原子覆盖已有输出文件",
    )
    parser.add_argument(
        "--horizontal-gap",
        type=nonnegative_float,
        default=80.0,
        help="同层 AS 外框之间的最小水平空隙（默认：80）",
    )
    parser.add_argument(
        "--vertical-gap",
        type=positive_float,
        default=120.0,
        help="相邻层 AS 外框之间的垂直空隙（默认：120）",
    )
    parser.add_argument(
        "--component-gap",
        type=nonnegative_float,
        default=360.0,
        help="provider 森林中不同树之间的水平空隙（默认：360）",
    )
    parser.add_argument(
        "--margin",
        type=nonnegative_float,
        default=160.0,
        help="布局左侧和顶部留白（默认：160）",
    )
    parser.add_argument(
        "--as-padding-x",
        type=nonnegative_float,
        default=90.0,
        help="AS 内路由器中心包围盒的左右占位（默认：90）",
    )
    parser.add_argument(
        "--as-padding-top",
        type=nonnegative_float,
        default=80.0,
        help="AS 内路由器中心包围盒的顶部占位（默认：80）",
    )
    parser.add_argument(
        "--as-padding-bottom",
        type=nonnegative_float,
        default=70.0,
        help="AS 内路由器中心包围盒的底部占位（默认：70）",
    )
    parser.add_argument(
        "--sweeps",
        type=positive_int,
        default=12,
        help="一般 DAG 回退布局的重心扫描轮数（默认：12）",
    )
    parser.add_argument(
        "--compact",
        action="store_true",
        help="输出紧凑 JSON，而不是四空格缩进 JSON",
    )
    return parser.parse_args(argv)


def default_output_path(input_path: Path) -> Path:
    try:
        if input_path.resolve() == DEFAULT_INPUT:
            return DEFAULT_OUTPUT
    except OSError:
        pass
    suffix = input_path.suffix or ".json"
    return input_path.with_name(f"{input_path.stem}_layout{suffix}")


def load_topology(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8-sig") as source:
            topology = json.load(source)
    except FileNotFoundError as exc:
        raise LayoutError(f"输入文件不存在：{path}") from exc
    except json.JSONDecodeError as exc:
        raise LayoutError(
            f"JSON 无效：{path}:{exc.lineno}:{exc.colno}: {exc.msg}"
        ) from exc

    if not isinstance(topology, dict):
        raise LayoutError("顶层 JSON 必须是对象")
    if not isinstance(topology.get("routers"), list):
        raise LayoutError("routers 必须是数组")
    if not isinstance(topology.get("links"), list):
        raise LayoutError("links 必须是数组")
    if not topology["routers"]:
        raise LayoutError("routers 不能为空")
    return topology


def finite_number(value: Any, where: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise LayoutError(f"{where} 必须是有限数字")
    result = float(value)
    if not math.isfinite(result):
        raise LayoutError(f"{where} 必须是有限数字")
    return result


def build_clusters(
    routers: list[Any],
    *,
    padding_x: float,
    padding_top: float,
    padding_bottom: float,
) -> tuple[dict[int, Cluster], dict[str, int]]:
    grouped: dict[int, list[tuple[str, float, float]]] = defaultdict(list)
    router_to_asn: dict[str, int] = {}

    for index, router in enumerate(routers):
        where = f"routers[{index}]"
        if not isinstance(router, dict):
            raise LayoutError(f"{where} 必须是对象")
        router_id = router.get("id")
        if not isinstance(router_id, str) or not router_id:
            raise LayoutError(f"{where}.id 必须是非空字符串")
        if router_id in router_to_asn:
            raise LayoutError(f"路由器 ID 重复：{router_id}")
        asn = router.get("asn")
        if isinstance(asn, bool) or not isinstance(asn, int) or asn < 0:
            raise LayoutError(f"{where}.asn 必须是非负整数")
        position = router.get("position")
        if not isinstance(position, dict):
            raise LayoutError(f"{where}.position 必须是对象")
        x = finite_number(position.get("x"), f"{where}.position.x")
        y = finite_number(position.get("y"), f"{where}.position.y")
        router_to_asn[router_id] = asn
        grouped[asn].append((router_id, x, y))

    clusters: dict[int, Cluster] = {}
    for asn, entries in grouped.items():
        min_x = min(entry[1] for entry in entries)
        max_x = max(entry[1] for entry in entries)
        min_y = min(entry[2] for entry in entries)
        max_y = max(entry[2] for entry in entries)
        span_x = max_x - min_x
        span_y = max_y - min_y
        if not math.isfinite(span_x) or not math.isfinite(span_y):
            raise LayoutError(f"AS{asn} 的原始坐标跨度过大")
        center_x = min_x / 2.0 + max_x / 2.0
        center_y = min_y / 2.0 + max_y / 2.0
        offsets = {
            router_id: (x - center_x, y - center_y)
            for router_id, x, y in entries
        }
        clusters[asn] = Cluster(
            asn=asn,
            router_ids=tuple(entry[0] for entry in entries),
            offsets=offsets,
            old_center_x=center_x,
            old_center_y=center_y,
            half_width=span_x / 2.0 + padding_x,
            top_extent=span_y / 2.0 + padding_top,
            bottom_extent=span_y / 2.0 + padding_bottom,
        )
    return clusters, router_to_asn


def build_as_graph(
    ases: Iterable[int],
    links: list[Any],
    router_to_asn: dict[str, int],
) -> AsGraph:
    children = {asn: set() for asn in ases}
    parents = {asn: set() for asn in ases}
    soft_neighbors = {asn: set() for asn in ases}
    provider_links: list[ProviderLink] = []
    valid_relationships = {"unspecified", "peer", "a_provider", "b_provider"}

    for index, link in enumerate(links):
        where = f"links[{index}]"
        if not isinstance(link, dict):
            raise LayoutError(f"{where} 必须是对象")
        a = link.get("a")
        b = link.get("b")
        if not isinstance(a, str) or not isinstance(b, str):
            raise LayoutError(f"{where}.a 和 {where}.b 必须是路由器 ID 字符串")
        if a not in router_to_asn:
            raise LayoutError(f"{where}.a 引用了未知路由器：{a}")
        if b not in router_to_asn:
            raise LayoutError(f"{where}.b 引用了未知路由器：{b}")
        relationship_value = link.get("relationship")
        if relationship_value is None:
            relationship = "unspecified"
        elif isinstance(relationship_value, str):
            relationship = relationship_value.strip().lower()
        else:
            raise LayoutError(f"{where}.relationship 必须是字符串或 null")
        if relationship not in valid_relationships:
            raise LayoutError(f"{where}.relationship 无效：{relationship_value!r}")

        asn_a = router_to_asn[a]
        asn_b = router_to_asn[b]
        if relationship == "a_provider":
            provider_asn, customer_asn = asn_a, asn_b
            provider_router, customer_router = a, b
        elif relationship == "b_provider":
            provider_asn, customer_asn = asn_b, asn_a
            provider_router, customer_router = b, a
        else:
            if asn_a != asn_b:
                soft_neighbors[asn_a].add(asn_b)
                soft_neighbors[asn_b].add(asn_a)
            continue

        if provider_asn == customer_asn:
            raise LayoutError(
                f"{where} 在同一 AS{provider_asn} 内声明了 provider/customer 关系"
            )
        children[provider_asn].add(customer_asn)
        parents[customer_asn].add(provider_asn)
        provider_links.append(
            ProviderLink(
                provider_asn=provider_asn,
                customer_asn=customer_asn,
                provider_router=provider_router,
                customer_router=customer_router,
            )
        )

    return AsGraph(children, parents, provider_links, soft_neighbors)


def topological_ranks(
    ases: Iterable[int],
    children: dict[int, set[int]],
    parents: dict[int, set[int]],
) -> tuple[list[int], dict[int, int]]:
    as_list = sorted(ases)
    indegree = {asn: len(parents[asn]) for asn in as_list}
    queue = [asn for asn in as_list if indegree[asn] == 0]
    heapq.heapify(queue)
    order: list[int] = []
    ranks = {asn: 0 for asn in as_list}

    while queue:
        provider = heapq.heappop(queue)
        order.append(provider)
        for customer in sorted(children[provider]):
            ranks[customer] = max(ranks[customer], ranks[provider] + 1)
            indegree[customer] -= 1
            if indegree[customer] == 0:
                heapq.heappush(queue, customer)

    if len(order) != len(as_list):
        cyclic = [asn for asn in as_list if indegree[asn] > 0]
        preview = ", ".join(f"AS{asn}" for asn in cyclic[:12])
        if len(cyclic) > 12:
            preview += ", ..."
        raise LayoutError(
            "provider/customer 关系含有有向环，严格的 provider-above-customer "
            f"布局无解；环内节点包括：{preview}"
        )
    return order, ranks


def tidy_forest_x(
    clusters: dict[int, Cluster],
    graph: AsGraph,
    topological_order: Sequence[int],
    *,
    horizontal_gap: float,
    component_gap: float,
    margin: float,
) -> dict[int, float]:
    """Lay out a provider forest using variable-width depth contours."""

    edge_anchor_values: dict[tuple[int, int], list[float]] = defaultdict(list)
    for link in graph.provider_links:
        provider_cluster = clusters[link.provider_asn]
        edge_anchor_values[(link.provider_asn, link.customer_asn)].append(
            provider_cluster.offsets[link.provider_router][0]
        )
    edge_anchor = {
        edge: sum(values) / len(values)
        for edge, values in edge_anchor_values.items()
    }
    ordered_children = {
        asn: sorted(
            graph.children[asn],
            key=lambda child: (
                edge_anchor.get((asn, child), 0.0),
                clusters[child].old_center_x,
                child,
            ),
        )
        for asn in clusters
    }
    left_contour: dict[int, list[float]] = {}
    right_contour: dict[int, list[float]] = {}
    child_offsets: dict[int, dict[int, float]] = {}

    for asn in reversed(topological_order):
        children = ordered_children[asn]
        if not children:
            left_contour[asn] = [-clusters[asn].half_width]
            right_contour[asn] = [clusters[asn].half_width]
            child_offsets[asn] = {}
            continue

        forest_left: list[float] = []
        forest_right: list[float] = []
        shifts: list[float] = []
        for child in children:
            child_left = left_contour[child]
            child_right = right_contour[child]
            if not forest_left:
                shift = 0.0
            else:
                shared_depth = min(len(forest_right), len(child_left))
                shift = max(
                    forest_right[depth]
                    + horizontal_gap
                    - child_left[depth]
                    for depth in range(shared_depth)
                )
            shifts.append(shift)

            new_depth = max(len(forest_left), len(child_left))
            merged_left: list[float] = []
            merged_right: list[float] = []
            for depth in range(new_depth):
                candidates_left: list[float] = []
                candidates_right: list[float] = []
                if depth < len(forest_left):
                    candidates_left.append(forest_left[depth])
                    candidates_right.append(forest_right[depth])
                if depth < len(child_left):
                    candidates_left.append(child_left[depth] + shift)
                    candidates_right.append(child_right[depth] + shift)
                merged_left.append(min(candidates_left))
                merged_right.append(max(candidates_right))
            forest_left, forest_right = merged_left, merged_right

        parent_center = (shifts[0] + shifts[-1]) / 2.0
        child_offsets[asn] = {
            child: shift - parent_center
            for child, shift in zip(children, shifts, strict=True)
        }
        left_contour[asn] = [-clusters[asn].half_width] + [
            value - parent_center for value in forest_left
        ]
        right_contour[asn] = [clusters[asn].half_width] + [
            value - parent_center for value in forest_right
        ]

    roots = sorted(
        (asn for asn in clusters if not graph.parents[asn]),
        key=lambda asn: (clusters[asn].old_center_x, asn),
    )
    as_x: dict[int, float] = {}
    cursor = margin
    for root in roots:
        relative_x = {root: 0.0}
        stack = [root]
        while stack:
            parent = stack.pop()
            for child in reversed(ordered_children[parent]):
                relative_x[child] = relative_x[parent] + child_offsets[parent][child]
                stack.append(child)

        component_left = min(
            relative_x[asn] - clusters[asn].half_width for asn in relative_x
        )
        component_right = max(
            relative_x[asn] + clusters[asn].half_width for asn in relative_x
        )
        shift = cursor - component_left
        for asn, x in relative_x.items():
            as_x[asn] = x + shift
        cursor += component_right - component_left + component_gap

    if len(as_x) != len(clusters):
        missing = sorted(set(clusters) - set(as_x))
        raise LayoutError(f"内部错误：未布局 AS：{missing[:12]}")
    return as_x


def compact_forest_layers(
    clusters: dict[int, Cluster],
    graph: AsGraph,
    ranks: dict[int, int],
    order_hint: dict[int, float],
    *,
    horizontal_gap: float,
    component_gap: float,
    margin: float,
) -> dict[int, float]:
    """Pack each tree depth to the minimum width without changing tree order."""

    component_root: dict[int, int] = {}

    def find_root(asn: int) -> int:
        trail: list[int] = []
        current = asn
        while current not in component_root and graph.parents[current]:
            trail.append(current)
            current = next(iter(graph.parents[current]))
        root = component_root.get(current, current)
        component_root[current] = root
        for member in trail:
            component_root[member] = root
        return root

    for asn in clusters:
        find_root(asn)

    layers: dict[int, list[int]] = defaultdict(list)
    for asn, rank in ranks.items():
        layers[rank].append(asn)
    for nodes in layers.values():
        nodes.sort(key=lambda asn: (order_hint[asn], asn))

    local_x: dict[int, float] = {}
    layer_width: dict[int, float] = {}
    for rank in range(max(layers) + 1):
        nodes = layers[rank]
        for index, asn in enumerate(nodes):
            if index == 0:
                center = clusters[asn].half_width
            else:
                previous = nodes[index - 1]
                gap = (
                    horizontal_gap
                    if component_root[previous] == component_root[asn]
                    else component_gap
                )
                center = (
                    local_x[previous]
                    + clusters[previous].half_width
                    + gap
                    + clusters[asn].half_width
                )
            local_x[asn] = center
        last = nodes[-1]
        layer_width[rank] = local_x[last] + clusters[last].half_width

    widest = max(layer_width.values())
    for rank, nodes in layers.items():
        shift = margin + (widest - layer_width[rank]) / 2.0
        for asn in nodes:
            local_x[asn] += shift
    return local_x


def normalized_positions(layers: dict[int, list[int]]) -> dict[int, float]:
    result: dict[int, float] = {}
    for nodes in layers.values():
        denominator = max(1, len(nodes) - 1)
        for index, asn in enumerate(nodes):
            result[asn] = index / denominator
    return result


def reorder_layer(
    nodes: list[int],
    references: dict[int, set[int]],
    soft_neighbors: dict[int, set[int]],
    positions: dict[int, float],
) -> list[int]:
    current = {asn: index / max(1, len(nodes) - 1) for index, asn in enumerate(nodes)}

    def score(asn: int) -> tuple[float, float, int]:
        neighbors = references[asn]
        soft = soft_neighbors[asn]
        total_weight = len(neighbors) + 0.2 * len(soft)
        if total_weight:
            barycenter = (
                sum(positions[neighbor] for neighbor in neighbors)
                + 0.2 * sum(positions[neighbor] for neighbor in soft)
            ) / total_weight
        else:
            barycenter = current[asn]
        return barycenter, current[asn], asn

    return sorted(nodes, key=score)


def layered_dag_x(
    clusters: dict[int, Cluster],
    graph: AsGraph,
    ranks: dict[int, int],
    *,
    horizontal_gap: float,
    margin: float,
    sweeps: int,
) -> dict[int, float]:
    """Sugiyama-style crossing reduction for acyclic multi-provider graphs."""

    layers: dict[int, list[int]] = defaultdict(list)
    for asn, rank in ranks.items():
        layers[rank].append(asn)
    for nodes in layers.values():
        nodes.sort(key=lambda asn: (clusters[asn].old_center_x, asn))
    max_rank = max(layers)

    for _ in range(sweeps):
        positions = normalized_positions(layers)
        for rank in range(1, max_rank + 1):
            layers[rank] = reorder_layer(
                layers[rank], graph.parents, graph.soft_neighbors, positions
            )
            positions = normalized_positions(layers)
        for rank in range(max_rank - 1, -1, -1):
            layers[rank] = reorder_layer(
                layers[rank], graph.children, graph.soft_neighbors, positions
            )
            positions = normalized_positions(layers)

    local_x: dict[int, float] = {}
    layer_bounds: dict[int, tuple[float, float]] = {}
    for rank in range(max_rank + 1):
        nodes = layers[rank]
        cursor = 0.0
        for index, asn in enumerate(nodes):
            if index == 0:
                center = clusters[asn].half_width
            else:
                previous = nodes[index - 1]
                center = (
                    local_x[previous]
                    + clusters[previous].half_width
                    + horizontal_gap
                    + clusters[asn].half_width
                )
            local_x[asn] = center
            cursor = center + clusters[asn].half_width
        layer_bounds[rank] = (0.0, cursor)

    widest = max(right - left for left, right in layer_bounds.values())
    for rank, nodes in layers.items():
        left, right = layer_bounds[rank]
        shift = margin + (widest - (right - left)) / 2.0
        for asn in nodes:
            local_x[asn] += shift
    return local_x


def horizontal_layout(
    clusters: dict[int, Cluster],
    graph: AsGraph,
    topological_order: Sequence[int],
    ranks: dict[int, int],
    *,
    horizontal_gap: float,
    component_gap: float,
    margin: float,
    sweeps: int,
) -> LayoutResult:
    is_forest = all(len(graph.parents[asn]) <= 1 for asn in clusters)
    if is_forest:
        order_hint = tidy_forest_x(
            clusters,
            graph,
            topological_order,
            horizontal_gap=horizontal_gap,
            component_gap=component_gap,
            margin=0.0,
        )
        as_x = compact_forest_layers(
            clusters,
            graph,
            ranks,
            order_hint,
            horizontal_gap=horizontal_gap,
            component_gap=component_gap,
            margin=margin,
        )
        mode = "轮廓排序的紧凑分层树"
    else:
        as_x = layered_dag_x(
            clusters,
            graph,
            ranks,
            horizontal_gap=horizontal_gap,
            margin=margin,
            sweeps=sweeps,
        )
        mode = "多 provider 分层 DAG"
    return LayoutResult(as_x=as_x, ranks=ranks, mode=mode)


def vertical_layout(
    clusters: dict[int, Cluster],
    ranks: dict[int, int],
    *,
    vertical_gap: float,
    margin: float,
) -> dict[int, float]:
    by_rank: dict[int, list[int]] = defaultdict(list)
    for asn, rank in ranks.items():
        by_rank[rank].append(asn)
    max_rank = max(by_rank)
    layer_y: dict[int, float] = {}
    previous_bottom = 0.0
    for rank in range(max_rank + 1):
        nodes = by_rank[rank]
        top_extent = max(clusters[asn].top_extent for asn in nodes)
        bottom_extent = max(clusters[asn].bottom_extent for asn in nodes)
        if rank == 0:
            center_y = margin + top_extent
        else:
            center_y = previous_bottom + vertical_gap + top_extent
        layer_y[rank] = center_y
        previous_bottom = center_y + bottom_extent
    return {asn: layer_y[rank] for asn, rank in ranks.items()}


def rounded_coordinate(value: float) -> int | float:
    if not math.isfinite(value):
        raise LayoutError("布局计算产生了非有限坐标；请缩小输入坐标或间距")
    rounded = round(value, 3)
    if abs(rounded) < 0.0005:
        rounded = 0.0
    if float(rounded).is_integer():
        return int(rounded)
    return rounded


def apply_positions(
    routers: list[Any],
    clusters: dict[int, Cluster],
    as_x: dict[int, float],
    as_y: dict[int, float],
) -> None:
    for router in routers:
        router_id = router["id"]
        asn = router["asn"]
        offset_x, offset_y = clusters[asn].offsets[router_id]
        position = router["position"]
        position["x"] = rounded_coordinate(as_x[asn] + offset_x)
        position["y"] = rounded_coordinate(as_y[asn] + offset_y)


def validate_layout(
    routers: list[Any],
    clusters: dict[int, Cluster],
    graph: AsGraph,
    as_x: dict[int, float],
    ranks: dict[int, int],
) -> tuple[float, float, float, float]:
    bounds: dict[int, list[float]] = {}
    all_x: list[float] = []
    all_y: list[float] = []
    for router in routers:
        asn = router["asn"]
        x = float(router["position"]["x"])
        y = float(router["position"]["y"])
        all_x.append(x)
        all_y.append(y)
        if asn not in bounds:
            bounds[asn] = [x, x, y, y]
        else:
            bounds[asn][0] = min(bounds[asn][0], x)
            bounds[asn][1] = max(bounds[asn][1], x)
            bounds[asn][2] = min(bounds[asn][2], y)
            bounds[asn][3] = max(bounds[asn][3], y)

    failures: list[str] = []
    for link in graph.provider_links:
        provider_bottom = bounds[link.provider_asn][3]
        customer_top = bounds[link.customer_asn][2]
        if not provider_bottom < customer_top:
            failures.append(
                f"AS{link.provider_asn}({provider_bottom:g}) → "
                f"AS{link.customer_asn}({customer_top:g})"
            )
    if failures:
        raise LayoutError(
            "内部错误：以下 provider/customer 上下约束未满足："
            + "; ".join(failures[:8])
        )

    by_rank: dict[int, list[int]] = defaultdict(list)
    for asn, rank in ranks.items():
        by_rank[rank].append(asn)
    for rank, nodes in by_rank.items():
        intervals = sorted(
            (
                as_x[asn] - clusters[asn].half_width,
                as_x[asn] + clusters[asn].half_width,
                asn,
            )
            for asn in nodes
        )
        for previous, current in zip(intervals, intervals[1:]):
            if previous[1] > current[0] + 1e-6:
                raise LayoutError(
                    "内部错误：同层 AS 外框重叠："
                    f"第 {rank} 层 AS{previous[2]} 与 AS{current[2]}"
                )
    return min(all_x), max(all_x), min(all_y), max(all_y)


def atomic_write_json(
    output_path: Path,
    topology: dict[str, Any],
    *,
    compact: bool,
    force: bool,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists() and output_path.is_dir():
        raise LayoutError(f"输出路径是目录：{output_path}")
    if output_path.exists() and not force:
        raise LayoutError(f"输出文件已存在：{output_path}；使用 --force 才会覆盖")

    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "w",
            encoding="utf-8",
            newline="\n",
            dir=output_path.parent,
            prefix=f".{output_path.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_path = Path(temporary.name)
            if compact:
                json.dump(
                    topology,
                    temporary,
                    ensure_ascii=False,
                    separators=(",", ":"),
                    allow_nan=False,
                )
            else:
                json.dump(
                    topology,
                    temporary,
                    ensure_ascii=False,
                    indent=4,
                    allow_nan=False,
                )
            temporary.write("\n")
            temporary.flush()
            os.fsync(temporary.fileno())

        if force:
            os.replace(temporary_path, output_path)
            temporary_path = None
        else:
            try:
                os.link(temporary_path, output_path)
            except FileExistsError as exc:
                raise LayoutError(
                    f"输出文件刚刚被其他进程创建：{output_path}；"
                    "使用 --force 才会覆盖"
                ) from exc
            temporary_path.unlink()
            temporary_path = None
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass


def run(args: argparse.Namespace) -> None:
    input_path = args.input.resolve()
    output_path = (
        args.output.resolve()
        if args.output is not None
        else default_output_path(input_path).resolve()
    )
    topology = load_topology(input_path)
    routers = topology["routers"]
    links = topology["links"]
    clusters, router_to_asn = build_clusters(
        routers,
        padding_x=args.as_padding_x,
        padding_top=args.as_padding_top,
        padding_bottom=args.as_padding_bottom,
    )
    graph = build_as_graph(clusters, links, router_to_asn)
    topological_order, ranks = topological_ranks(
        clusters, graph.children, graph.parents
    )
    layout = horizontal_layout(
        clusters,
        graph,
        topological_order,
        ranks,
        horizontal_gap=args.horizontal_gap,
        component_gap=args.component_gap,
        margin=args.margin,
        sweeps=args.sweeps,
    )
    as_y = vertical_layout(
        clusters,
        layout.ranks,
        vertical_gap=args.vertical_gap,
        margin=args.margin,
    )
    apply_positions(routers, clusters, layout.as_x, as_y)
    min_x, max_x, min_y, max_y = validate_layout(
        routers, clusters, graph, layout.as_x, layout.ranks
    )
    atomic_write_json(
        output_path,
        topology,
        compact=args.compact,
        force=args.force,
    )

    unique_provider_edges = {
        (link.provider_asn, link.customer_asn) for link in graph.provider_links
    }
    print("BGP 拓扑布局完成")
    print(f"  输入: {input_path}")
    print(f"  输出: {output_path}")
    print(
        f"  规模: {len(clusters)} 个 AS, {len(routers)} 台路由器, "
        f"{len(links)} 条链路"
    )
    print(
        f"  层级: {max(layout.ranks.values()) + 1} 层, "
        f"{len(unique_provider_edges)} 条 provider→customer AS 关系"
    )
    print(f"  算法: {layout.mode}")
    print(
        f"  路由器坐标范围: x=[{min_x:g}, {max_x:g}], "
        f"y=[{min_y:g}, {max_y:g}]"
    )
    print(
        f"  约束校验: {len(graph.provider_links)}/{len(graph.provider_links)} "
        "条 provider 链路严格位于 customer 上方"
    )


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        run(args)
    except (LayoutError, OSError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
