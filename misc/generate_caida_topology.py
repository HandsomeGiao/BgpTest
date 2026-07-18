#!/usr/bin/env python3
"""将 CAIDA AS Relationships 数据转换为 BgpTester 可加载的拓扑 JSON。

需要 Python 3.10 或更新版本，仅使用 Python 标准库。

CAIDA 数据只描述 AS 之间的关系，不包含 AS 内部拓扑。本脚本会为每个选中的
AS 稳定地随机选择一种合成 iBGP 模板，再用 CAIDA 的 peer/provider-customer
关系连接各 AS 的边界路由器。

默认使用 ``deep`` 抽样：从高层 provider 沿 provider→customer 关系做深度优先
遍历，并且只保留 valley-free 的 customer-cone 骨干树。这样 ``--max-ases``
增加时会真正增加 AS 路径深度，不会因为高连接 AS 和诱导图捷径退化成直径为
2 的星形图。旧的 BFS + 诱导子图行为可用 ``--sampling-mode dense`` 启用。

当前数据集全量展开会产生数十万台路由器和超过百万条链路，虽然 JSON 格式
有效，但并不适合直接交给 BgpTester 的 GUI 或仿真器。只有显式传入
``--all-ases`` 时才会生成全量拓扑。

示例：

    python misc/generate_caida_topology.py --max-ases 50 \
        --output misc/caida_topology.json
    python misc/generate_caida_topology.py --root-asn 3356 --seed 7 \
        --templates full_mesh,rr_star,dual_rr --force
"""

from __future__ import annotations

import argparse
import bz2
from collections import Counter, defaultdict, deque
from dataclasses import dataclass
import gzip
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import random
import sys
import tempfile
from typing import Callable, Iterator, Sequence, TextIO


if sys.version_info < (3, 10):
    print("错误：generate_caida_topology.py 需要 Python 3.10 或更新版本", file=sys.stderr)
    raise SystemExit(2)


UINT32_MAX = (1 << 32) - 1
INT32_MAX = (1 << 31) - 1
ROUTER_ID_CAPACITY = 256 * 256 * 254
BENCHMARK_PREFIX_BASE = int(ipaddress.IPv4Address("198.18.0.0"))
BENCHMARK_PREFIX_CAPACITY = 1 << 17  # 198.18.0.0/15 中的 /32 数量
STANDARD_PLUGIN_ID = "org.bgptester.router.standard-bgp"
DEFAULT_TEMPLATES = ("full_mesh", "rr_star", "dual_rr")
ALL_TEMPLATES = ("single", *DEFAULT_TEMPLATES)
DEEP_ROOT_CANDIDATE_COUNT = 128


class GenerationError(RuntimeError):
    """输入数据或生成参数不满足要求。"""


class DatasetFormatError(GenerationError):
    """CAIDA 关系文件中的某一行格式无效。"""


@dataclass(frozen=True, slots=True)
class ASRelationship:
    """规范化后的 AS 关系。

    provider-customer 关系始终保存为 a=provider、b=customer；peer 关系则按
    ASN 升序保存。这样生成 JSON 时方向不会依赖 ASN 大小或集合遍历顺序。
    """

    a: int
    b: int
    kind: str  # "peer" 或 "provider_customer"

    @property
    def key(self) -> tuple[int, int]:
        return (min(self.a, self.b), max(self.a, self.b))


@dataclass(slots=True)
class DatasetStats:
    relationship_count: int
    ases: set[int]
    invalid_count: int
    invalid_examples: list[str]


@dataclass(frozen=True, slots=True)
class TemplateNode:
    role: str
    x: float
    y: float


@dataclass(frozen=True, slots=True)
class InternalSession:
    a: str
    b: str
    rr_client_from_a: bool = False
    rr_client_from_b: bool = False


@dataclass(frozen=True, slots=True)
class InternalTemplate:
    name: str
    nodes: tuple[TemplateNode, ...]
    sessions: tuple[InternalSession, ...]
    border_roles: tuple[str, ...]
    origin_role: str


@dataclass(slots=True)
class ASInstance:
    asn: int
    template_name: str
    routers_by_role: dict[str, str]
    border_routers: list[str]
    border_cursor: int = 0

    def next_border_router(self) -> str:
        router = self.border_routers[self.border_cursor % len(self.border_routers)]
        self.border_cursor += 1
        return router


@dataclass(frozen=True, slots=True)
class BuildSummary:
    router_count: int
    internal_link_count: int
    peer_link_count: int
    provider_customer_link_count: int
    template_counts: Counter[str]


class DisjointSet:
    def __init__(self, values: Sequence[int]) -> None:
        self.parent = {value: value for value in values}
        self.rank = {value: 0 for value in values}

    def find(self, value: int) -> int:
        parent = self.parent[value]
        if parent != value:
            self.parent[value] = self.find(parent)
        return self.parent[value]

    def union(self, left: int, right: int) -> bool:
        left_root = self.find(left)
        right_root = self.find(right)
        if left_root == right_root:
            return False
        if self.rank[left_root] < self.rank[right_root]:
            left_root, right_root = right_root, left_root
        self.parent[right_root] = left_root
        if self.rank[left_root] == self.rank[right_root]:
            self.rank[left_root] += 1
        return True


def open_relationship_file(path: Path) -> TextIO:
    suffix = path.suffix.lower()
    if suffix == ".gz":
        return gzip.open(path, "rt", encoding="utf-8", newline=None)
    if suffix == ".bz2":
        return bz2.open(path, "rt", encoding="utf-8", newline=None)
    return path.open("r", encoding="utf-8", newline=None)


def parse_relationship(path: Path, line_number: int, raw_line: str) -> ASRelationship | None:
    line = raw_line.strip()
    if not line or line.startswith("#"):
        return None

    fields = [field.strip() for field in line.split("|")]
    location = f"{path}:{line_number}"
    if len(fields) != 4:
        raise DatasetFormatError(f"{location}: 应为 4 列，实际为 {len(fields)} 列")

    try:
        first = int(fields[0], 10)
        second = int(fields[1], 10)
    except ValueError as exc:
        raise DatasetFormatError(f"{location}: ASN 必须是十进制整数") from exc

    for asn in (first, second):
        if not 1 <= asn <= UINT32_MAX:
            raise DatasetFormatError(f"{location}: ASN {asn} 不在 1..{UINT32_MAX} 范围内")
    if first == second:
        raise DatasetFormatError(f"{location}: 不允许 AS{first} 自环")
    if not fields[3]:
        raise DatasetFormatError(f"{location}: source 列不能为空")

    if fields[2] == "-1":
        # CAIDA serial-2: 第一列是 provider，第二列是 customer。
        return ASRelationship(first, second, "provider_customer")
    if fields[2] == "0":
        return ASRelationship(min(first, second), max(first, second), "peer")
    raise DatasetFormatError(f"{location}: 不支持的关系值 {fields[2]!r}，只接受 -1 或 0")


def iter_relationships(
    path: Path,
    *,
    skip_invalid: bool,
    on_invalid: Callable[[str], None] | None = None,
) -> Iterator[ASRelationship]:
    with open_relationship_file(path) as source:
        for line_number, raw_line in enumerate(source, start=1):
            try:
                relationship = parse_relationship(path, line_number, raw_line)
            except DatasetFormatError as exc:
                if not skip_invalid:
                    raise
                if on_invalid is not None:
                    on_invalid(str(exc))
                continue
            if relationship is not None:
                yield relationship


def scan_dataset(path: Path, *, skip_invalid: bool) -> DatasetStats:
    ases: set[int] = set()
    invalid_count = 0
    invalid_examples: list[str] = []

    def record_invalid(message: str) -> None:
        nonlocal invalid_count
        invalid_count += 1
        if len(invalid_examples) < 5:
            invalid_examples.append(message)

    relationship_count = 0
    for relationship in iter_relationships(
        path, skip_invalid=skip_invalid, on_invalid=record_invalid
    ):
        relationship_count += 1
        ases.add(relationship.a)
        ases.add(relationship.b)

    if relationship_count == 0:
        raise GenerationError(f"{path} 中没有可用的 AS 关系记录")
    return DatasetStats(relationship_count, ases, invalid_count, invalid_examples)


def stable_rng(seed: int, *parts: object) -> random.Random:
    """创建不依赖 Python hash 随机化的派生随机数生成器。"""

    material = "\0".join([str(seed), *(str(part) for part in parts)]).encode("utf-8")
    digest = hashlib.blake2b(material, digest_size=16).digest()
    return random.Random(int.from_bytes(digest, "big"))


def load_ordered_customers(
    path: Path,
    *,
    seed: int,
    skip_invalid: bool,
) -> dict[int, tuple[int, ...]]:
    """读取 provider→customer 邻接表，并为每个 provider 稳定地打乱客户顺序。"""

    customer_sets: dict[int, set[int]] = defaultdict(set)
    for relationship in iter_relationships(path, skip_invalid=skip_invalid):
        if relationship.kind == "provider_customer":
            customer_sets[relationship.a].add(relationship.b)

    ordered: dict[int, tuple[int, ...]] = {}
    for provider in sorted(customer_sets):
        customers = sorted(customer_sets[provider])
        stable_rng(seed, "deep-customer-order", provider).shuffle(customers)
        ordered[provider] = tuple(customers)
    return ordered


def build_customer_cone_tree(
    root_asn: int,
    max_ases: int,
    ordered_customers: dict[int, tuple[int, ...]],
) -> tuple[set[int], list[int], list[ASRelationship]]:
    """深度优先构造 provider→customer 树。

    每个节点只保留第一次到达它的 provider 边，因此结果无环。任意两节点间的
    唯一路径都是 customer→provider* 后接 provider→customer*，天然符合
    valley-free 出口策略。
    """

    selected = {root_asn}
    order = [root_asn]
    tree: list[ASRelationship] = []
    stack: list[tuple[int, Iterator[int]]] = [
        (root_asn, iter(ordered_customers.get(root_asn, ())))
    ]

    while stack and len(selected) < max_ases:
        provider, children = stack[-1]
        try:
            customer = next(children)
        except StopIteration:
            stack.pop()
            continue
        if customer in selected:
            continue
        selected.add(customer)
        order.append(customer)
        tree.append(ASRelationship(provider, customer, "provider_customer"))
        stack.append((customer, iter(ordered_customers.get(customer, ()))))

    return selected, order, tree


def tree_diameter(
    selected_ases: set[int],
    tree: Sequence[ASRelationship],
) -> tuple[int, tuple[int, int]]:
    if not selected_ases:
        return 0, (0, 0)
    if len(selected_ases) == 1:
        only = next(iter(selected_ases))
        return 0, (only, only)

    adjacency: dict[int, list[int]] = {asn: [] for asn in selected_ases}
    for relationship in tree:
        adjacency[relationship.a].append(relationship.b)
        adjacency[relationship.b].append(relationship.a)

    def farthest(start: int) -> tuple[int, int]:
        distances = {start: 0}
        pending = deque([start])
        while pending:
            current = pending.popleft()
            for neighbor in adjacency[current]:
                if neighbor not in distances:
                    distances[neighbor] = distances[current] + 1
                    pending.append(neighbor)
        if len(distances) != len(selected_ases):
            raise GenerationError("deep customer-cone 骨干树不连通")
        return max(distances, key=lambda asn: (distances[asn], -asn)), max(
            distances.values()
        )

    endpoint_a, _ = farthest(min(selected_ases))
    endpoint_b, diameter = farthest(endpoint_a)
    return diameter, (endpoint_a, endpoint_b)


def select_deep_customer_cone(
    *,
    all_ases: set[int],
    ordered_customers: dict[int, tuple[int, ...]],
    max_ases: int,
    requested_root_asn: int | None,
    seed: int,
) -> tuple[int, set[int], list[int], list[ASRelationship], int, tuple[int, int]]:
    """选择能容纳目标规模且骨干直径尽量大的 customer cone。"""

    if max_ases > len(all_ases):
        raise GenerationError(
            f"--max-ases={max_ases} 超过数据集中的 AS 数量 {len(all_ases)}"
        )
    if requested_root_asn is not None:
        if requested_root_asn not in all_ases:
            raise GenerationError(f"root ASN {requested_root_asn} 不存在于输入数据中")
        selected, order, tree = build_customer_cone_tree(
            requested_root_asn, max_ases, ordered_customers
        )
        if len(selected) < max_ases:
            raise GenerationError(
                f"AS{requested_root_asn} 的 provider→customer cone 只能选择 "
                f"{len(selected)} 个 AS，无法达到 {max_ases}；请更换 --root-asn，"
                "或使用 --sampling-mode dense"
            )
        diameter, endpoints = tree_diameter(selected, tree)
        return requested_root_asn, selected, order, tree, diameter, endpoints

    ranked_roots = sorted(
        ordered_customers,
        key=lambda asn: (-len(ordered_customers[asn]), asn),
    )[:DEEP_ROOT_CANDIDATE_COUNT]
    if not ranked_roots:
        raise GenerationError(
            "输入数据中没有 provider→customer 关系，deep 模式无法构造 "
            "customer cone；请使用 --sampling-mode dense"
        )
    best: tuple[
        tuple[int, int, float],
        int,
        set[int],
        list[int],
        list[ASRelationship],
        tuple[int, int],
    ] | None = None
    largest_cone = 0
    largest_root = 0

    for candidate in ranked_roots:
        selected, order, tree = build_customer_cone_tree(
            candidate, max_ases, ordered_customers
        )
        if len(selected) > largest_cone:
            largest_cone = len(selected)
            largest_root = candidate
        if len(selected) < max_ases:
            continue
        diameter, endpoints = tree_diameter(selected, tree)
        degrees: Counter[int] = Counter()
        for relationship in tree:
            degrees[relationship.a] += 1
            degrees[relationship.b] += 1
        max_degree = max(degrees.values(), default=0)
        tie_breaker = stable_rng(seed, "deep-root-tie", candidate).random()
        score = (diameter, -max_degree, tie_breaker)
        if best is None or score > best[0]:
            best = (score, candidate, selected, order, tree, endpoints)

    if best is None:
        raise GenerationError(
            f"前 {len(ranked_roots)} 个候选 provider 中最大的 customer cone "
            f"只有 {largest_cone} 个 AS（根 AS{largest_root}），无法达到 "
            f"{max_ases}；请指定其他 --root-asn 或使用 --sampling-mode dense"
        )
    score, root_asn, selected, order, tree, endpoints = best
    return root_asn, selected, order, tree, score[0], endpoints


def select_dense_connected_ases(
    path: Path,
    *,
    all_ases: set[int],
    max_ases: int,
    root_asn: int,
    seed: int,
    skip_invalid: bool,
) -> tuple[set[int], list[int]]:
    """用多遍流式 BFS 选择连通 AS 子图，避免保存全量邻接表。"""

    if root_asn not in all_ases:
        raise GenerationError(f"root ASN {root_asn} 不存在于输入数据中")
    if max_ases > len(all_ases):
        raise GenerationError(
            f"--max-ases={max_ases} 超过数据集中的 AS 数量 {len(all_ases)}"
        )

    selected = {root_asn}
    order = [root_asn]
    frontier = [root_asn]
    rng = stable_rng(seed, "as-selection", root_asn, max_ases)

    while len(selected) < max_ases:
        frontier_set = set(frontier)
        candidates: set[int] = set()
        for relationship in iter_relationships(path, skip_invalid=skip_invalid):
            if relationship.a in frontier_set and relationship.b not in selected:
                candidates.add(relationship.b)
            if relationship.b in frontier_set and relationship.a not in selected:
                candidates.add(relationship.a)

        if not candidates:
            raise GenerationError(
                f"AS{root_asn} 所在连通分量只有 {len(selected)} 个 AS，"
                f"无法选择 {max_ases} 个"
            )

        candidate_order = sorted(candidates)
        rng.shuffle(candidate_order)
        frontier = candidate_order[: max_ases - len(selected)]
        selected.update(frontier)
        order.extend(frontier)

    return selected, order


def collect_induced_relationships(
    path: Path,
    selected_ases: set[int],
    *,
    skip_invalid: bool,
) -> list[ASRelationship]:
    relationships_by_pair: dict[tuple[int, int], ASRelationship] = {}
    for relationship in iter_relationships(path, skip_invalid=skip_invalid):
        if relationship.a not in selected_ases or relationship.b not in selected_ases:
            continue
        previous = relationships_by_pair.get(relationship.key)
        if previous is None:
            relationships_by_pair[relationship.key] = relationship
        elif previous != relationship:
            raise GenerationError(
                f"AS{relationship.key[0]} 与 AS{relationship.key[1]} 存在冲突关系："
                f"AS{previous.a}->AS{previous.b} ({previous.kind}) / "
                f"AS{relationship.a}->AS{relationship.b} ({relationship.kind})"
            )
    return sorted(
        relationships_by_pair.values(),
        key=lambda relationship: (*relationship.key, relationship.kind),
    )


def limit_relationships(
    relationships: list[ASRelationship],
    selected_ases: set[int],
    *,
    max_links: int,
    seed: int,
) -> list[ASRelationship]:
    """限制 AS 间边数，同时用随机生成树保持 AS 图连通。"""

    if max_links == 0 or len(relationships) <= max_links:
        return relationships
    minimum = max(0, len(selected_ases) - 1)
    if max_links < minimum:
        raise GenerationError(
            f"--max-inter-as-links 至少应为 {minimum}，否则无法保持 AS 图连通"
        )

    shuffled = list(relationships)
    stable_rng(seed, "link-limit", max_links).shuffle(shuffled)
    components = DisjointSet(sorted(selected_ases))
    tree: list[ASRelationship] = []
    extras: list[ASRelationship] = []
    for relationship in shuffled:
        if components.union(relationship.a, relationship.b):
            tree.append(relationship)
        else:
            extras.append(relationship)

    if len(tree) != minimum:
        raise GenerationError("选中的 AS 关系图不连通，无法在限制边数后保持连通")
    kept = tree + extras[: max_links - len(tree)]
    return sorted(kept, key=lambda relationship: (*relationship.key, relationship.kind))


def add_induced_extras(
    backbone: Sequence[ASRelationship],
    induced: Sequence[ASRelationship],
    *,
    requested_count: int,
    seed: int,
) -> tuple[list[ASRelationship], int]:
    backbone_keys = {relationship.key for relationship in backbone}
    candidates = [
        relationship
        for relationship in induced
        if relationship.key not in backbone_keys
    ]
    stable_rng(seed, "deep-extra-links", requested_count).shuffle(candidates)
    chosen = candidates[:requested_count]
    combined = [*backbone, *chosen]
    return (
        sorted(combined, key=lambda relationship: (*relationship.key, relationship.kind)),
        len(chosen),
    )


def circular_nodes(roles: Sequence[str], radius: float, phase: float = -math.pi / 2) -> tuple[TemplateNode, ...]:
    count = len(roles)
    return tuple(
        TemplateNode(
            role,
            radius * math.cos(phase + 2 * math.pi * index / count),
            radius * math.sin(phase + 2 * math.pi * index / count),
        )
        for index, role in enumerate(roles)
    )


def make_template(name: str, rng: random.Random) -> InternalTemplate:
    if name == "single":
        return InternalTemplate(
            name="single",
            nodes=(TemplateNode("R1", 0.0, 0.0),),
            sessions=(),
            border_roles=("R1",),
            origin_role="R1",
        )

    if name == "full_mesh":
        roles = tuple(f"R{index}" for index in range(1, rng.randint(2, 4) + 1))
        sessions = tuple(
            InternalSession(roles[left], roles[right])
            for left in range(len(roles))
            for right in range(left + 1, len(roles))
        )
        return InternalTemplate(
            name=name,
            nodes=circular_nodes(roles, 90.0),
            sessions=sessions,
            border_roles=roles,
            origin_role=roles[0],
        )

    if name == "rr_star":
        client_roles = tuple(f"C{index}" for index in range(1, rng.randint(2, 4) + 1))
        nodes = (TemplateNode("RR1", 0.0, 0.0),) + circular_nodes(client_roles, 105.0)
        sessions = tuple(
            InternalSession("RR1", client, rr_client_from_a=True)
            for client in client_roles
        )
        return InternalTemplate(name, nodes, sessions, client_roles, client_roles[0])

    if name == "dual_rr":
        client_roles = tuple(f"C{index}" for index in range(1, rng.randint(2, 4) + 1))
        client_nodes = tuple(
            TemplateNode(
                role,
                -105.0 + 210.0 * index / (len(client_roles) - 1),
                90.0,
            )
            for index, role in enumerate(client_roles)
        )
        nodes = (
            TemplateNode("RR1", -55.0, -35.0),
            TemplateNode("RR2", 55.0, -35.0),
            *client_nodes,
        )
        sessions: list[InternalSession] = [InternalSession("RR1", "RR2")]
        for client in client_roles:
            sessions.append(InternalSession("RR1", client, rr_client_from_a=True))
            sessions.append(InternalSession("RR2", client, rr_client_from_a=True))
        return InternalTemplate(name, nodes, tuple(sessions), client_roles, client_roles[0])

    raise GenerationError(f"未知 AS 内模板：{name}")


def grid_centers(as_order: Sequence[int]) -> dict[int, tuple[float, float]]:
    """按 BFS 顺序放入近似 16:9 的蛇形网格。"""

    count = len(as_order)
    columns = max(1, math.ceil(math.sqrt(count * 16 / 9)))
    centers: dict[int, tuple[float, float]] = {}
    for index, asn in enumerate(as_order):
        row, column = divmod(index, columns)
        if row % 2:
            column = columns - 1 - column
        centers[asn] = (260.0 + column * 460.0, 240.0 + row * 390.0)
    return centers


def router_id_from_index(one_based_index: int) -> str:
    if not 1 <= one_based_index <= ROUTER_ID_CAPACITY:
        raise GenerationError(
            f"路由器数量超过可分配的 10.0.0.0/8 Router ID 容量 {ROUTER_ID_CAPACITY}"
        )
    zero_based = one_based_index - 1
    second = zero_based // (256 * 254)
    remainder = zero_based % (256 * 254)
    third = remainder // 254
    fourth = remainder % 254 + 1
    return f"10.{second}.{third}.{fourth}"


def synthetic_prefix(as_index: int) -> str:
    if not 0 <= as_index < BENCHMARK_PREFIX_CAPACITY:
        raise GenerationError(
            "选中的 AS 数量超过 198.18.0.0/15 可提供的唯一合成 /32 前缀数"
        )
    return f"{ipaddress.IPv4Address(BENCHMARK_PREFIX_BASE + as_index)}/32"


def make_link(
    a: str,
    b: str,
    *,
    delay_ms: int,
    mrai_ms: int,
    relationship: str,
    rr_client_from_a: bool = False,
    rr_client_from_b: bool = False,
) -> dict[str, object]:
    return {
        "a": a,
        "b": b,
        "enabled": True,
        "delay_ms": delay_ms,
        "rr_client_from_a": rr_client_from_a,
        "rr_client_from_b": rr_client_from_b,
        "mrai_ms_from_a": mrai_ms,
        "mrai_ms_from_b": mrai_ms,
        "relationship": relationship,
    }


def build_topology(
    as_order: Sequence[int],
    relationships: Sequence[ASRelationship],
    *,
    seed: int,
    template_names: Sequence[str],
    no_prefixes: bool,
    ibgp_delay_min_ms: int,
    ibgp_delay_max_ms: int,
    ebgp_delay_min_ms: int,
    ebgp_delay_max_ms: int,
    mrai_ms: int,
    simulation_name: str,
    log_dir: str,
    convergence_quiet_ms: int,
) -> tuple[dict[str, object], BuildSummary]:
    centers = grid_centers(as_order)
    routers: list[dict[str, object]] = []
    links: list[dict[str, object]] = []
    instances: dict[int, ASInstance] = {}
    template_counts: Counter[str] = Counter()
    router_index = 0
    internal_link_count = 0

    for as_index, asn in enumerate(as_order):
        as_rng = stable_rng(seed, "as-template", asn)
        template_name = as_rng.choice(tuple(template_names))
        template = make_template(template_name, as_rng)
        template_counts[template.name] += 1
        center_x, center_y = centers[asn]
        routers_by_role: dict[str, str] = {}

        for node in template.nodes:
            router_index += 1
            router_name = f"AS{asn}_{node.role}"
            bgp_router_id = router_id_from_index(router_index)
            routers_by_role[node.role] = router_name
            originated_prefixes = (
                [synthetic_prefix(as_index)]
                if not no_prefixes and node.role == template.origin_role
                else []
            )
            routers.append(
                {
                    "id": router_name,
                    "router_id": bgp_router_id,
                    "asn": asn,
                    "cluster_id": bgp_router_id,
                    "originated_prefixes": originated_prefixes,
                    "position": {
                        "x": round(center_x + node.x, 3),
                        "y": round(center_y + node.y, 3),
                    },
                    "plugin": {"id": STANDARD_PLUGIN_ID, "settings": {}},
                }
            )

        border_routers = [routers_by_role[role] for role in template.border_roles]
        stable_rng(seed, "border-order", asn).shuffle(border_routers)
        instances[asn] = ASInstance(
            asn=asn,
            template_name=template.name,
            routers_by_role=routers_by_role,
            border_routers=border_routers,
        )

        for session in template.sessions:
            delay_rng = stable_rng(seed, "ibgp-delay", asn, session.a, session.b)
            links.append(
                make_link(
                    routers_by_role[session.a],
                    routers_by_role[session.b],
                    delay_ms=delay_rng.randint(ibgp_delay_min_ms, ibgp_delay_max_ms),
                    mrai_ms=mrai_ms,
                    relationship="unspecified",
                    rr_client_from_a=session.rr_client_from_a,
                    rr_client_from_b=session.rr_client_from_b,
                )
            )
            internal_link_count += 1

    peer_link_count = 0
    provider_customer_link_count = 0
    for relationship in relationships:
        # provider-customer 已规范化为 a=provider；peer 的 a/b 按 ASN 升序。
        router_a = instances[relationship.a].next_border_router()
        router_b = instances[relationship.b].next_border_router()
        delay_rng = stable_rng(
            seed, "ebgp-delay", relationship.a, relationship.b, relationship.kind
        )
        if relationship.kind == "provider_customer":
            json_relationship = "a_provider"
            provider_customer_link_count += 1
        else:
            json_relationship = "peer"
            peer_link_count += 1
        links.append(
            make_link(
                router_a,
                router_b,
                delay_ms=delay_rng.randint(ebgp_delay_min_ms, ebgp_delay_max_ms),
                mrai_ms=mrai_ms,
                relationship=json_relationship,
            )
        )

    topology: dict[str, object] = {
        "simulation": {
            "name": simulation_name,
            "log_dir": log_dir,
            "worker_threads": 0,
            "convergence_quiet_ms": convergence_quiet_ms,
            "router_class": "BgpRouter",
        },
        "routers": routers,
        "links": links,
    }
    summary = BuildSummary(
        router_count=len(routers),
        internal_link_count=internal_link_count,
        peer_link_count=peer_link_count,
        provider_customer_link_count=provider_customer_link_count,
        template_counts=template_counts,
    )
    return topology, summary


def atomic_write_json(
    output_path: Path,
    topology: dict[str, object],
    *,
    compact: bool,
    force: bool,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists() and output_path.is_dir():
        raise GenerationError(f"输出路径是目录：{output_path}")
    if output_path.exists() and not force:
        raise GenerationError(f"输出文件已存在：{output_path}；使用 --force 才会覆盖")

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
                )
            else:
                json.dump(topology, temporary, ensure_ascii=False, indent=2)
            temporary.write("\n")
            temporary.flush()
            os.fsync(temporary.fileno())
        if force:
            os.replace(temporary_path, output_path)
        else:
            # 同目录硬链接使用 O_EXCL 语义：若检查后有其他进程创建了目标，
            # 这里会安全失败，不会像 os.replace 那样覆盖新文件。
            try:
                os.link(temporary_path, output_path)
            except FileExistsError as exc:
                raise GenerationError(
                    f"输出文件已存在：{output_path}；使用 --force 才会覆盖"
                ) from exc
            temporary_path.unlink()
    finally:
        if temporary_path is not None and temporary_path.exists():
            temporary_path.unlink()


def non_negative_int(value: str) -> int:
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("必须是整数") from exc
    if not 0 <= number <= INT32_MAX:
        raise argparse.ArgumentTypeError(f"必须在 0..{INT32_MAX} 范围内")
    return number


def positive_int(value: str) -> int:
    number = non_negative_int(value)
    if number == 0:
        raise argparse.ArgumentTypeError("必须大于 0")
    return number


def asn_value(value: str) -> int:
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("ASN 必须是十进制整数") from exc
    if not 1 <= number <= UINT32_MAX:
        raise argparse.ArgumentTypeError(f"ASN 必须在 1..{UINT32_MAX} 范围内")
    return number


def parse_template_names(raw_value: str) -> tuple[str, ...]:
    names: list[str] = []
    for raw_name in raw_value.split(","):
        name = raw_name.strip().lower().replace("-", "_")
        if not name:
            continue
        if name not in ALL_TEMPLATES:
            choices = ", ".join(ALL_TEMPLATES)
            raise GenerationError(f"未知模板 {raw_name!r}；可用模板：{choices}")
        if name not in names:
            names.append(name)
    if not names:
        raise GenerationError("--templates 至少应包含一个模板")
    return tuple(names)


def discover_default_input(script_directory: Path) -> Path:
    candidates: list[Path] = []
    for pattern in ("*.as-rel2.txt", "*.as-rel2.txt.gz", "*.as-rel2.txt.bz2"):
        candidates.extend(script_directory.glob(pattern))
    if not candidates:
        raise GenerationError(
            f"{script_directory} 中没有找到 *.as-rel2.txt[.gz|.bz2]；请使用 --input 指定"
        )
    return sorted(candidates, key=lambda path: path.name)[-1]


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="从 CAIDA AS Relationships 数据生成 BgpTester 拓扑 JSON",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--input",
        type=Path,
        help="CAIDA as-rel2 输入；省略时自动选择脚本目录中名称最新的数据文件",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="输出 JSON；省略时写入脚本目录下的 caida_topology.json",
    )
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument(
        "--max-ases",
        type=positive_int,
        default=50,
        help="连通样本包含的 AS 数量",
    )
    selection.add_argument(
        "--all-ases",
        action="store_true",
        help="显式生成全量 AS（输出非常大，不推荐直接用于 GUI/仿真）",
    )
    parser.add_argument(
        "--sampling-mode",
        choices=("deep", "dense"),
        default="deep",
        help=(
            "deep 构造较长的 provider→customer DFS 骨干；dense 保留旧版 "
            "BFS + 诱导子图行为"
        ),
    )
    parser.add_argument(
        "--root-asn",
        type=asn_value,
        help="deep 模式的 customer-cone 根 provider，或 dense 模式的 BFS 根 ASN",
    )
    parser.add_argument("--seed", type=int, default=20260701, help="可复现随机种子")
    parser.add_argument(
        "--templates",
        default=",".join(DEFAULT_TEMPLATES),
        help="逗号分隔的 AS 内模板；可选 single/full_mesh/rr_star/dual_rr",
    )
    parser.add_argument(
        "--max-inter-as-links",
        type=non_negative_int,
        default=0,
        help=(
            "dense/--all-ases 模式的 AS 间链路上限；0 保留全部关系；"
            "裁边只保证无向连通"
        ),
    )
    parser.add_argument(
        "--extra-inter-as-links",
        type=non_negative_int,
        default=0,
        help=(
            "deep 模式在 customer-cone 树之外加入的诱导边数量；"
            "额外边可能显著缩短 AS_PATH"
        ),
    )
    parser.add_argument("--no-prefixes", action="store_true", help="不生成每 AS 一个合成 /32 前缀")
    parser.add_argument("--ibgp-delay-min-ms", type=non_negative_int, default=1)
    parser.add_argument("--ibgp-delay-max-ms", type=non_negative_int, default=10)
    parser.add_argument("--ebgp-delay-min-ms", type=non_negative_int, default=10)
    parser.add_argument("--ebgp-delay-max-ms", type=non_negative_int, default=80)
    parser.add_argument("--mrai-ms", type=non_negative_int, default=0)
    parser.add_argument("--simulation-name", default="caida-as-topology")
    parser.add_argument("--log-dir", default="tmp")
    parser.add_argument("--convergence-quiet-ms", type=non_negative_int, default=1000)
    parser.add_argument(
        "--skip-invalid-lines",
        action="store_true",
        help="跳过并报告非法数据行；默认遇到第一条非法记录即失败",
    )
    parser.add_argument("--compact", action="store_true", help="输出无缩进的紧凑 JSON")
    parser.add_argument("--dry-run", action="store_true", help="完成解析和构造但不写文件")
    parser.add_argument("--force", action="store_true", help="原子覆盖已存在的输出文件")
    return parser


def validate_arguments(args: argparse.Namespace) -> None:
    if args.root_asn is not None and args.all_ases:
        raise GenerationError("--root-asn 只用于有限连通抽样，不能与 --all-ases 同时使用")
    if args.all_ases and args.extra_inter_as_links:
        raise GenerationError("--extra-inter-as-links 不能与 --all-ases 同时使用")
    if not args.all_ases and args.sampling_mode == "deep" and args.max_inter_as_links:
        raise GenerationError(
            "--max-inter-as-links 只用于 dense/--all-ases 模式；deep 模式请使用 "
            "--extra-inter-as-links"
        )
    if args.sampling_mode == "dense" and args.extra_inter_as_links:
        raise GenerationError("--extra-inter-as-links 只用于 deep 模式")
    for minimum_name, maximum_name in (
        ("ibgp_delay_min_ms", "ibgp_delay_max_ms"),
        ("ebgp_delay_min_ms", "ebgp_delay_max_ms"),
    ):
        if getattr(args, minimum_name) > getattr(args, maximum_name):
            raise GenerationError(
                f"--{minimum_name.replace('_', '-')} 不能大于 --{maximum_name.replace('_', '-')}"
            )
    if not args.simulation_name.strip():
        raise GenerationError("--simulation-name 不能为空")
    if not args.log_dir.strip():
        raise GenerationError("--log-dir 不能为空")


def print_summary(
    *,
    input_path: Path,
    output_path: Path,
    stats: DatasetStats,
    selected_count: int,
    relationship_count: int,
    root_asn: int | None,
    sampling_mode: str,
    backbone_diameter: int | None,
    diameter_endpoints: tuple[int, int] | None,
    extra_link_count: int,
    summary: BuildSummary,
    dry_run: bool,
) -> None:
    print("CAIDA → BgpTester 拓扑生成完成" if not dry_run else "CAIDA → BgpTester 拓扑预演完成")
    print(f"  输入: {input_path}")
    print(f"  数据集: {len(stats.ases)} 个 AS, {stats.relationship_count} 条关系")
    if stats.invalid_count:
        print(f"  已跳过非法记录: {stats.invalid_count}")
        for example in stats.invalid_examples:
            print(f"    - {example}")
    print(f"  选中: {selected_count} 个 AS, {relationship_count} 条 AS 间关系")
    print(f"  抽样模式: {sampling_mode}")
    if root_asn is not None:
        print(f"  抽样根: AS{root_asn}")
    if backbone_diameter is not None and diameter_endpoints is not None:
        path_note = "；无额外边时对应最长预期 AS_PATH" if not extra_link_count else ""
        print(
            f"  customer-cone 骨干直径: {backbone_diameter} AS hops "
            f"(AS{diameter_endpoints[0]} ↔ AS{diameter_endpoints[1]}){path_note}"
        )
    if extra_link_count:
        print(f"  额外诱导边: {extra_link_count}（可能缩短实际 AS_PATH）")
    print(f"  路由器: {summary.router_count}")
    print(
        "  链路: "
        f"{summary.internal_link_count} 条 iBGP, "
        f"{summary.peer_link_count} 条 peer eBGP, "
        f"{summary.provider_customer_link_count} 条 provider-customer eBGP"
    )
    template_text = ", ".join(
        f"{name}={count}" for name, count in sorted(summary.template_counts.items())
    )
    print(f"  AS 内模板: {template_text}")
    if dry_run:
        print("  输出: 未写入（--dry-run）")
    else:
        print(f"  输出: {output_path}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)

    try:
        validate_arguments(args)
        template_names = parse_template_names(args.templates)
        script_directory = Path(__file__).resolve().parent
        input_path = (
            args.input.expanduser()
            if args.input is not None
            else discover_default_input(script_directory)
        )
        output_path = (
            args.output.expanduser()
            if args.output is not None
            else script_directory / "caida_topology.json"
        )
        if not input_path.is_file():
            raise GenerationError(f"输入文件不存在或不是普通文件：{input_path}")
        if input_path.resolve() == output_path.resolve():
            raise GenerationError("输入文件和输出文件不能相同")
        if not args.dry_run:
            if output_path.exists() and output_path.is_dir():
                raise GenerationError(f"输出路径是目录：{output_path}")
            if output_path.exists() and not args.force:
                raise GenerationError(
                    f"输出文件已存在：{output_path}；使用 --force 才会覆盖"
                )

        stats = scan_dataset(input_path, skip_invalid=args.skip_invalid_lines)
        backbone_diameter: int | None = None
        diameter_endpoints: tuple[int, int] | None = None
        extra_link_count = 0
        if args.all_ases:
            print(
                "警告：正在显式生成全量拓扑；该结果通常不适合直接用于 BgpTester GUI/仿真。",
                file=sys.stderr,
            )
            selected_ases = set(stats.ases)
            as_order = sorted(selected_ases)
            root_asn: int | None = None
            sampling_mode = "all"
            relationships = collect_induced_relationships(
                input_path, selected_ases, skip_invalid=args.skip_invalid_lines
            )
            induced_relationship_count = len(relationships)
            relationships = limit_relationships(
                relationships,
                selected_ases,
                max_links=args.max_inter_as_links,
                seed=args.seed,
            )
            if len(relationships) < induced_relationship_count:
                print(
                    "警告：AS 间裁边仅保持无向图连通；商业关系策略下的路由"
                    "可达性可能低于未裁边拓扑。",
                    file=sys.stderr,
                )
        elif args.sampling_mode == "deep":
            sampling_mode = "deep"
            ordered_customers = load_ordered_customers(
                input_path,
                seed=args.seed,
                skip_invalid=args.skip_invalid_lines,
            )
            (
                root_asn,
                selected_ases,
                as_order,
                backbone,
                backbone_diameter,
                diameter_endpoints,
            ) = select_deep_customer_cone(
                all_ases=stats.ases,
                ordered_customers=ordered_customers,
                max_ases=args.max_ases,
                requested_root_asn=args.root_asn,
                seed=args.seed,
            )
            relationships = sorted(
                backbone,
                key=lambda relationship: (*relationship.key, relationship.kind),
            )
            if args.extra_inter_as_links:
                induced = collect_induced_relationships(
                    input_path, selected_ases, skip_invalid=args.skip_invalid_lines
                )
                relationships, extra_link_count = add_induced_extras(
                    backbone,
                    induced,
                    requested_count=args.extra_inter_as_links,
                    seed=args.seed,
                )
                if extra_link_count < args.extra_inter_as_links:
                    print(
                        f"警告：选中的 AS 中只有 {extra_link_count} 条可加入的额外"
                        "诱导边。",
                        file=sys.stderr,
                    )
                if extra_link_count:
                    print(
                        "警告：额外诱导边会产生捷径，实际最长 AS_PATH 可能小于"
                        "报告的 customer-cone 骨干直径。",
                        file=sys.stderr,
                    )
        else:
            sampling_mode = "dense"
            root_asn = args.root_asn
            if root_asn is None:
                root_rng = stable_rng(args.seed, "root-asn", len(stats.ases))
                root_asn = root_rng.choice(sorted(stats.ases))
            selected_ases, as_order = select_dense_connected_ases(
                input_path,
                all_ases=stats.ases,
                max_ases=args.max_ases,
                root_asn=root_asn,
                seed=args.seed,
                skip_invalid=args.skip_invalid_lines,
            )
            relationships = collect_induced_relationships(
                input_path, selected_ases, skip_invalid=args.skip_invalid_lines
            )
            induced_relationship_count = len(relationships)
            relationships = limit_relationships(
                relationships,
                selected_ases,
                max_links=args.max_inter_as_links,
                seed=args.seed,
            )
            if len(relationships) < induced_relationship_count:
                print(
                    "警告：AS 间裁边仅保持无向图连通；商业关系策略下的路由"
                    "可达性可能低于未裁边拓扑。",
                    file=sys.stderr,
                )
        topology, summary = build_topology(
            as_order,
            relationships,
            seed=args.seed,
            template_names=template_names,
            no_prefixes=args.no_prefixes,
            ibgp_delay_min_ms=args.ibgp_delay_min_ms,
            ibgp_delay_max_ms=args.ibgp_delay_max_ms,
            ebgp_delay_min_ms=args.ebgp_delay_min_ms,
            ebgp_delay_max_ms=args.ebgp_delay_max_ms,
            mrai_ms=args.mrai_ms,
            simulation_name=args.simulation_name.strip(),
            log_dir=args.log_dir.strip(),
            convergence_quiet_ms=args.convergence_quiet_ms,
        )
        if not args.dry_run:
            atomic_write_json(
                output_path,
                topology,
                compact=args.compact,
                force=args.force,
            )
        print_summary(
            input_path=input_path,
            output_path=output_path,
            stats=stats,
            selected_count=len(selected_ases),
            relationship_count=len(relationships),
            root_asn=root_asn,
            sampling_mode=sampling_mode,
            backbone_diameter=backbone_diameter,
            diameter_endpoints=diameter_endpoints,
            extra_link_count=extra_link_count,
            summary=summary,
            dry_run=args.dry_run,
        )
        return 0
    except (GenerationError, OSError, UnicodeError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
