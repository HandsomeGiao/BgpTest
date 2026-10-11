#!/usr/bin/env python3
"""Generate the fixed-5-second topology comparison, without changing old fixtures."""
import copy
import json
import math
import random
from pathlib import Path

from generate_versioned_scenarios import OTHER, PREFIX, ROOT, action, link, phase, topology

DEST = ROOT / "topo" / "versioned_mrai5"


def down(a, b, at=0):
    return action("link", at, a=a, b=b, enabled=False)


def up(a, b, at=0):
    return action("link", at, a=a, b=b, enabled=True)


def withdrawal(origin="O"):
    return [phase("withdraw", action("withdraw", router=origin, prefix=PREFIX), reachable=False),
            phase("restore", action("originate", router=origin, prefix=PREFIX))]


def failure(a, b):
    return [phase("failure", down(a, b)), phase("recovery", up(a, b))]


def place(topo, positions):
    for router in topo["routers"]:
        x, y = positions[router["id"]]
        router["position"] = {"x": round(x, 2), "y": round(y, 2)}


def circle(topo):
    n = len(topo["routers"])
    place(topo, {r["id"]: (420 + 310 * math.cos(2 * math.pi * i / n),
                           420 + 310 * math.sin(2 * math.pi * i / n))
                 for i, r in enumerate(topo["routers"])})


def fanout(n, profile="ordered", seed=None):
    branches = [f"R{i:02}" for i in range(n)]
    topo = topology(f"fanout-{n}-{profile}", ["O", "D", *branches, "X", "C", "E", "F"])
    # Textual router-ID ordering agrees with branch ranking even above 9 nodes.
    for i, router in enumerate(topo["routers"]):
        router["router_id"] = f"10.1.0.{100 + i}"
    delays = [1 + 10 * i for i in range(n)]
    if profile == "reverse":
        delays.reverse()
    elif profile == "equal":
        delays = [10] * n
    elif profile == "shuffled":
        random.Random(seed).shuffle(delays)
    link(topo, "D", "O", 5)
    for branch, delay in zip(branches, delays):
        link(topo, "D", branch, delay)
        link(topo, branch, "X", 1)
    for a, b in [("C", "X"), ("E", "C"), ("F", "E"), ("F", "O")]:
        link(topo, a, b)
    positions = {"O": (50, 250), "D": (200, 250), "X": (640, 250),
                 "F": (180, 750), "E": (330, 750), "C": (490, 750)}
    positions.update({branch: (420, 30 + i * 650 / max(1, n - 1)) for i, branch in enumerate(branches)})
    place(topo, positions)
    return topo


def main():
    DEST.mkdir(parents=True, exist_ok=True)
    fixtures, scenarios = {}, []

    def add(name, topo, phases, observer, family, description, seed=None):
        fixtures.setdefault(topo["simulation"]["name"], topo)
        scenarios.append({"name": name, "topology": f"../topo/versioned_mrai5/{topo['simulation']['name']}.json",
                          "family": family, "description": description, "delay_seed": seed,
                          "observer": observer, "prefix": PREFIX, "unaffected_prefix": OTHER, "phases": phases})

    old = json.loads((ROOT / "experiments/versioned_scenarios.json").read_text())
    descriptions = {
        "shared_loss": ("shared_dependency", "共同依赖失效，有独立备用"),
        "local_link_loss": ("shared_dependency", "单条分支链路失效，另一分支仍可用"),
        "export_only": ("shared_dependency", "仅撤销 A 对 X 的导出，保留共享状态"),
        "rapid_recovery": ("shared_dependency", "共享依赖断开后 3 ms 恢复"),
        "origin_withdraw": ("shared_dependency", "唯一源撤销目标前缀，另一前缀继续发布"),
        "reroute": ("reroute", "共同依赖节点自身绕行"),
        "metadata_gap": ("metadata_gap", "退役事实先到，新正向声明等待 MRAI"),
        "multi_origin": ("multi_origin", "撤销双起源中的一个"),
        "ibgp_border": ("ibgp", "同 AS 多边界，经普通 iBGP 传播"),
        "ibgp_origin": ("ibgp", "空 AS_PATH 的 iBGP 本地起源"),
        "fanout": ("fanout", "8 分支引用相同旧依赖"),
        "mrai_cascade": ("cascade", "中间路径发布触发下游一轮 MRAI"),
    }
    for scenario in old["scenarios"]:
        topo = json.loads((ROOT / "experiments" / scenario["topology"]).read_text())
        family, description = descriptions[scenario["name"]]
        if family not in ("fanout", "ibgp"):
            positions = {"O": (50, 220), "D": (220, 220), "A": (410, 110), "B": (410, 340),
                         "X": (620, 220), "Y": (800, 220), "F": (200, 590), "E": (350, 590),
                         "C": (500, 590), "Z": (140, 390)}
            place(topo, positions)
        elif family == "fanout":
            place(topo, {"O": (50, 250), "D": (200, 250), "X": (640, 250),
                         "F": (180, 750), "E": (330, 750), "C": (490, 750),
                         **{f"R{i}": (420, 30 + i * 650 / 7) for i in range(8)}})
        add(scenario["name"], topo, scenario["phases"], scenario["observer"], family, description)

    chain = topology("chain-8", ["O", *[f"R{i}" for i in range(1, 8)]])
    ids = [r["id"] for r in chain["routers"]]
    for a, b in zip(ids, ids[1:]):
        link(chain, a, b, 10, "unspecified")
    place(chain, {node: (70 + i * 130, 200) for i, node in enumerate(ids)})
    add("chain_withdraw", chain, withdrawal(), "R7", "chain", "8 节点链，唯一传播路径")

    star = topology("star-9", ["O", *[f"R{i}" for i in range(1, 9)]])
    for i in range(1, 9):
        link(star, "O", f"R{i}", 10, "unspecified")
    circle(star)
    star["routers"][0]["position"] = {"x": 420, "y": 420}
    add("star_withdraw", star, withdrawal(), "R8", "star", "单中心 8 叶节点，无替代路径")

    tree = topology("tree-15", ["O", *[f"R{i:02}" for i in range(1, 15)]])
    ids = [r["id"] for r in tree["routers"]]
    for i in range(1, 15):
        link(tree, ids[(i - 1) // 2], ids[i], 10, "unspecified")
    positions = {}
    for i, node in enumerate(ids):
        depth = int(math.log2(i + 1))
        positions[node] = ((i - (2**depth - 1) + .5) * 1000 / 2**depth, 70 + 170 * depth)
    place(tree, positions)
    add("tree_withdraw", tree, withdrawal(), "R14", "tree", "15 节点二叉树，分叉但不重汇合")

    diamond = topology("independent-diamond", ["O", "A", "B", "X"])
    for a, b, delay in [("A", "O", 1), ("B", "O", 20), ("A", "X", 1), ("B", "X", 1)]:
        link(diamond, a, b, delay)
    place(diamond, {"O": (70, 220), "A": (270, 100), "B": (270, 340), "X": (500, 220)})
    add("independent_failure", diamond, failure("A", "O"), "X", "independent", "首选分支断开，独立备用有效")

    ring = topology("ring-8", ["O", *[f"R{i}" for i in range(1, 8)]])
    ids = [r["id"] for r in ring["routers"]]
    for i, a in enumerate(ids):
        link(ring, a, ids[(i + 1) % len(ids)], 5 + 3 * i, "unspecified")
    circle(ring)
    add("ring_withdraw", ring, withdrawal(), "R3", "ring", "8 节点环，唯一源撤销")
    add("ring_failure", ring, failure("O", "R1"), "R3", "ring", "8 节点环单边断开，可反向绕行")

    grid_ids = [["O" if (x, y) == (0, 0) else f"G{x}{y}" for x in range(3)] for y in range(3)]
    grid = topology("grid-3x3", [node for row in grid_ids for node in row])
    for y in range(3):
        for x in range(3):
            if x < 2:
                link(grid, grid_ids[y][x], grid_ids[y][x+1], 4 + x + 3*y, "unspecified")
            if y < 2:
                link(grid, grid_ids[y][x], grid_ids[y+1][x], 8 + 2*x + y, "unspecified")
    place(grid, {grid_ids[y][x]: (80 + x * 230, 80 + y * 230) for x in range(3) for y in range(3)})
    add("grid_withdraw", grid, withdrawal(), "G22", "grid", "3×3 网格，多个重叠替代路径")
    add("grid_failure", grid, failure("O", "G10"), "G20", "grid", "3×3 网格，起源的一条邻边失效")

    for n in (6, 8):
        mesh = topology(f"clique-{n}", ["O", *[f"R{i}" for i in range(1, n)]])
        ids = [r["id"] for r in mesh["routers"]]
        for i in range(n):
            for j in range(i+1, n):
                link(mesh, ids[i], ids[j], 1 + (i*7 + j*11) % 40, "unspecified")
        circle(mesh)
        add(f"clique{n}_withdraw", mesh, withdrawal(), f"R{n-1}", "clique", f"{n} 节点全互联，唯一源撤销")

    neck = topology("clique-shared-neck", ["O", "D", *[f"R{i}" for i in range(1, 6)]])
    ids = ["D", *[f"R{i}" for i in range(1, 6)]]
    link(neck, "D", "O", 1, "unspecified")
    for i in range(len(ids)):
        for j in range(i+1, len(ids)):
            link(neck, ids[i], ids[j], 1 + (i*7 + j*11) % 40, "unspecified")
    circle(neck)
    phases = failure("D", "O")
    phases[0].update(observer_reachable=False, unaffected_prefix_reachable=False)
    add("clique_cut", neck, phases, "R5", "clique", "6 节点全互联经单一出口接起源，出口断开")

    tier = topology("tiered-policy", ["O", "P1", "P2", "C1", "C2", "X"])
    for a, b, delay in [("P1", "O", 1), ("P2", "O", 30), ("P1", "C1", 1),
                        ("P1", "C2", 30), ("P2", "C1", 25), ("P2", "C2", 1),
                        ("C1", "X", 1), ("C2", "X", 1)]:
        link(tier, a, b, delay)
    link(tier, "P1", "P2", 5, "peer")
    place(tier, {"O": (350, 60), "P1": (160, 220), "P2": (530, 220),
                 "C1": (160, 430), "C2": (530, 430), "X": (350, 610)})
    add("tiered_failure", tier, failure("P1", "O"), "X", "tiered", "双上游加 peer，执行商业偏好与 valley-free")

    layered = topology("layered-3x3", ["O", "D", "A1", "A2", "A3", "B1", "B2", "B3", "X", "C", "E", "F", "H"])
    link(layered, "D", "O", 1)
    for i in range(1, 4):
        link(layered, "D", f"A{i}", 1 + 30*(i-1))
        for j in range(1, 4):
            link(layered, f"A{i}", f"B{j}", 1 + j)
        link(layered, f"B{i}", "X", 1)
    for a, b in [("C", "X"), ("E", "C"), ("F", "E"), ("H", "F"), ("H", "O")]:
        link(layered, a, b)
    place(layered, {"O": (50, 240), "D": (200, 240), "X": (810, 240),
                    **{f"A{i}": (400, 80 + 170*(i-1)) for i in range(1, 4)},
                    **{f"B{i}": (610, 80 + 170*(i-1)) for i in range(1, 4)},
                    "H": (200, 630), "F": (350, 630), "E": (500, 630), "C": (650, 630)})
    add("layered_failure", layered, failure("D", "O"), "X", "layered", "两层 3×3 分支反复合流，共享出口断开")

    for n in (2, 4, 16, 32):
        add(f"fanout{n}", fanout(n), failure("D", "O"), "X", "fanout", f"{n} 分支，退役按偏好先后到达")
    for profile, seed in [("reverse", None), ("equal", None), ("shuffled", 7), ("shuffled", 19), ("shuffled", 42)]:
        topo = fanout(8, profile, seed)
        if seed is not None:
            topo["simulation"]["name"] += f"-{seed}"
        add(f"fanout8_{profile}" + (f"_{seed}" if seed is not None else ""), topo, failure("D", "O"),
            "X", "fanout_order", f"8 分支延迟排列 {profile}" + (f"，seed={seed}" if seed is not None else ""), seed)

    for name, topo in sorted(fixtures.items()):
        topo = copy.deepcopy(topo)
        for edge in topo["links"]:
            edge["mrai_ms_from_a"] = edge["mrai_ms_from_b"] = 5000
        (DEST / f"{name}.json").write_text(json.dumps(topo, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    suite = {"schema": "bgptester-versioned-scenarios-v1", "mrai_ms": [5000],
             "modes": ["standard", "path_only", "local", "full"], "delay_scales": [1, 3],
             "withdrawal_ignores_mrai": [True, False], "scenarios": scenarios}
    (ROOT / "experiments/topology_sweep_mrai5.json").write_text(json.dumps(suite, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{len(fixtures)} topologies, {len(scenarios)} scenarios, MRAI=5000 ms")


if __name__ == "__main__":
    main()
