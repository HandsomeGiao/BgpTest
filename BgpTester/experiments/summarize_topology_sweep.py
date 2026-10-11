#!/usr/bin/env python3
"""Validate and summarize the fixed-5s sweep; optionally render publication figures."""
import argparse
import csv
import gzip
import hashlib
import itertools
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
METRICS = ("network_intermediate_paths", "network_invalid_as_path_selections", "observer_intermediate_paths",
           "observer_path_changes", "observer_last_path_change_ms", "convergence_ms", "updates_sent",
           "announcements_sent", "withdrawals_sent", "control_updates_sent", "proxy_unreachable_ms",
           "proxy_no_route_ms", "proxy_unreachable_while_origin_connected_ms", "estimated_extension_bytes")
MAIN = ["chain_withdraw", "star_withdraw", "tree_withdraw", "independent_failure", "shared_loss", "fanout",
        "fanout32", "ring_withdraw", "ring_failure", "grid_withdraw", "grid_failure", "clique6_withdraw",
        "clique8_withdraw", "clique_cut", "tiered_failure", "layered_failure", "mrai_cascade", "multi_origin",
        "ibgp_border", "ibgp_origin", "metadata_gap"]


def write_csv(path, rows):
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow({k: json.dumps(v, ensure_ascii=False, separators=(",", ":")) if isinstance(v, (list, dict)) else v
                             for k, v in row.items()})


def compare_counts(pairs):
    return {metric: {key: sum((row[f"full_{metric}"] < row[f"standard_{metric}"] if key == "lower" else
                              row[f"full_{metric}"] > row[f"standard_{metric}"] if key == "higher" else
                              row[f"full_{metric}"] == row[f"standard_{metric}"]) for row in pairs)
                     for key in ("lower", "equal", "higher")} for metric in METRICS}


def load_and_validate(raw, suite, suite_dir):
    if raw["errors"]:
        raise ValueError(f"Failed runs must be resolved or reported explicitly: {raw['errors']}")
    expected = set(itertools.product((s["name"] for s in suite["scenarios"]), suite["modes"], [5000],
                                     suite["delay_scales"], suite["withdrawal_ignores_mrai"]))
    observed = [(r["scenario"], r["mode"], r["mrai_ms"], r["delay_scale"], r["withdrawal_ignores_mrai"]) for r in raw["runs"]]
    if len(observed) != len(set(observed)) or set(observed) != expected:
        raise ValueError("Missing, duplicate or unexpected matrix cells")
    scenarios = {s["name"]: s for s in suite["scenarios"]}
    topologies = {s["name"]: json.loads((suite_dir / s["topology"]).read_text()) for s in suite["scenarios"]}
    for topo in topologies.values():
        assert all(e["mrai_ms_from_a"] == e["mrai_ms_from_b"] == 5000 for e in topo["links"])
    rows = []
    for run in raw["runs"]:
        scenario = scenarios[run["scenario"]]
        assert [p["name"] for p in run["phases"]] == [p["name"] for p in scenario["phases"]]
        for index, phase in enumerate(run["phases"]):
            assert phase["updates_sent"] == sum(phase[k] for k in ("announcements_sent", "withdrawals_sent", "control_updates_sent"))
            for node, sequence in phase["network_path_sequences"].items():
                times = [e["at_ms"] for e in sequence]
                assert times == sorted(times) and min(times) >= 0 and max(times) <= phase["convergence_ms"]
                assert sequence[-1]["path"] == phase["final_paths"][node]
            row = {k: v for k, v in run.items() if k != "phases"}
            row.update(family=scenario["family"], observer=scenario["observer"], phase=phase["name"],
                       phase_index=index, router_count=len(topologies[run["scenario"]]["routers"]),
                       link_count=len(topologies[run["scenario"]]["links"]))
            row.update({k: v for k, v in phase.items() if not isinstance(v, (list, dict)) and k != "name"})
            row["proxy_unreachable_ms"] = sum(phase[k] for k in ("proxy_no_route_ms", "proxy_broken_ms", "proxy_loop_ms"))
            row["observer_path_sequence"] = phase["network_path_sequences"].get(scenario["observer"], [])
            rows.append(row)
    keys = ("scenario", "phase", "mrai_ms", "delay_scale", "withdrawal_ignores_mrai")
    baseline = {tuple(r[k] for k in keys): r for r in rows if r["mode"] == "standard"}
    runs = {(r["scenario"], r["mode"], r["delay_scale"], r["withdrawal_ignores_mrai"]): r for r in raw["runs"]}
    pairs = []
    for row in rows:
        if row["mode"] != "full":
            continue
        base = baseline[tuple(row[k] for k in keys)]
        phases = [runs[(row["scenario"], mode, row["delay_scale"], row["withdrawal_ignores_mrai"])]["phases"][row["phase_index"]]
                  for mode in ("standard", "full")]
        observer = scenarios[row["scenario"]]["observer"]
        pair = {k: row[k] for k in (*keys, "phase_index", "family", "observer", "router_count", "link_count")}
        pair["equal_initial_observer_path"] = phases[0]["initial_paths"][observer] == phases[1]["initial_paths"][observer]
        pair["equal_initial_network_paths"] = phases[0]["initial_paths"] == phases[1]["initial_paths"]
        pair["equal_final_observer_path"] = phases[0]["final_paths"][observer] == phases[1]["final_paths"][observer]
        for metric in METRICS:
            pair[f"standard_{metric}"] = base[metric]
            pair[f"full_{metric}"] = row[metric]
        denominator = base["network_intermediate_paths"]
        pair["network_intermediate_reduction_pct"] = 100 * (denominator - row["network_intermediate_paths"]) / denominator if denominator else None
        pairs.append(pair)
    return rows, pairs, scenarios, topologies


def plot(destination, rows, pairs, scenarios, topologies):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_pdf import PdfPages
    plt.rcParams.update({"font.size": 9, "axes.spines.top": False, "axes.spines.right": False, "pdf.fonttype": 42})
    colors = {"standard": "#557795", "full": "#d3733d"}
    primary = {r["scenario"]: r for r in pairs if r["phase_index"] == 0 and r["delay_scale"] == 1 and r["withdrawal_ignores_mrai"]}

    fig, axes = plt.subplots(1, 3, figsize=(15, 9), sharey=True, layout="constrained")
    for ax, metric, title in zip(axes, ("network_intermediate_paths", "convergence_ms", "updates_sent"),
                                  ("Intermediate path selections (network)", "Queue drained (seconds)", "UPDATE messages (network)")):
        for mode, offset in (("standard", -.19), ("full", .19)):
            values = [primary[name][f"{mode}_{metric}"] / (1000 if metric.endswith("_ms") else 1) for name in MAIN]
            ax.barh([i + offset for i in range(len(MAIN))], values, height=.36, label=mode, color=colors[mode])
        ax.set_yticks(range(len(MAIN)), MAIN)
        ax.set_title(title, fontsize=10)
        ax.grid(axis="x", alpha=.2)
        ax.set_axisbelow(True)
        ax.set_xscale("symlog", linthresh=.1 if metric.endswith("_ms") else 1)
        ax.set_xlabel("Symmetric log scale (includes zero)")
    axes[0].invert_yaxis()
    axes[0].legend()
    fig.suptitle("BGP topology comparison | MRAI 5 s | delay scale 1 | withdrawals bypass MRAI")
    for extension in ("png", "pdf"):
        fig.savefig(destination / f"comparison.{extension}", dpi=180)
    plt.close(fig)

    fig, axes = plt.subplots(1, 3, figsize=(13, 3.8), layout="constrained")
    names = ["fanout2", "fanout4", "fanout", "fanout16", "fanout32"]
    for ax, metric, title in zip(axes, ("observer_intermediate_paths", "observer_last_path_change_ms", "updates_sent"),
                                  ("Observer intermediate paths", "Observer path stable (seconds)", "Network UPDATE messages")):
        for mode in colors:
            ax.plot([2, 4, 8, 16, 32], [primary[n][f"{mode}_{metric}"] / (1000 if metric.endswith("_ms") else 1) for n in names],
                    marker="o", label=mode, color=colors[mode])
        ax.set_title(title, fontsize=10)
        ax.set_xlabel("Shared branches")
        ax.set_xticks([2, 4, 8, 16, 32])
        ax.grid(alpha=.2)
    axes[0].legend()
    fig.suptitle("Shared-dependency fanout | MRAI 5 s | ordered link delays")
    for extension in ("png", "pdf"):
        fig.savefig(destination / f"fanout_scaling.{extension}", dpi=180)
    plt.close(fig)

    atlas_names = ["chain_withdraw", "star_withdraw", "tree_withdraw", "independent_failure", "shared_loss", "ring_withdraw",
                   "grid_withdraw", "clique8_withdraw", "tiered_failure", "layered_failure", "mrai_cascade", "ibgp_border"]

    def draw_topology(ax, name):
        topo, scenario = topologies[name], scenarios[name]
        positions = {r["id"]: (r["position"]["x"], -r["position"]["y"]) for r in topo["routers"]}
        failures = {frozenset((a["a"], a["b"])) for a in scenario["phases"][0]["actions"] if a["kind"] == "link"}
        for edge in topo["links"]:
            x1, y1 = positions[edge["a"]]
            x2, y2 = positions[edge["b"]]
            ax.plot([x1, x2], [y1, y2], color="#c45045" if frozenset((edge["a"], edge["b"])) in failures else "#bdc6cf",
                    linewidth=1.4, zorder=1)
        for router in topo["routers"]:
            x, y = positions[router["id"]]
            origin = bool(router["originated_prefixes"])
            observer = router["id"] == scenario["observer"]
            ax.scatter(x, y, s=150, color="#d3733d" if origin else "#557795" if observer else "#e5ebf1",
                       marker="s" if origin else "o", edgecolors="#557795", linewidths=.5, zorder=2)
            ax.annotate(router["id"], (x, y), xytext=(0, 10), textcoords="offset points", ha="center", fontsize=7)
        ax.set_title(f"{name} ({len(topo['routers'])} nodes, {len(topo['links'])} links)", fontsize=9)
        ax.margins(.18)
        ax.set_aspect("equal", adjustable="datalim")
        ax.set_axis_off()

    fig, axes = plt.subplots(4, 3, figsize=(14, 13), layout="constrained")
    for ax, name in zip(axes.flat, atlas_names):
        draw_topology(ax, name)
    fig.suptitle("Topology examples | orange square: origin | blue circle: observer | red edge: fault")
    fig.savefig(destination / "topologies.png", dpi=180)
    plt.close(fig)
    unique = list({scenarios[n]["topology"]: n for n in scenarios}.values())
    with PdfPages(destination / "topology_atlas.pdf") as pdf:
        for start in range(0, len(unique), 4):
            fig, axes = plt.subplots(2, 2, figsize=(12, 9), layout="constrained")
            for ax in axes.flat:
                ax.set_axis_off()
            for ax, name in zip(axes.flat, unique[start:start+4]):
                draw_topology(ax, name)
            pdf.savefig(fig)
            plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--suite", type=Path, default=ROOT / "experiments/topology_sweep_mrai5.json")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--plots", action="store_true", help="Requires matplotlib; tables use only the Python standard library")
    args = parser.parse_args()
    raw_bytes = args.results.read_bytes()
    if args.results.suffix == ".gz":
        raw_bytes = gzip.decompress(raw_bytes)
    raw, suite = json.loads(raw_bytes), json.loads(args.suite.read_text())
    rows, pairs, scenarios, topologies = load_and_validate(raw, suite, args.suite.parent)
    dest = args.output_dir or args.results.parent
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "raw_results.json.gz").write_bytes(gzip.compress(raw_bytes, mtime=0))
    write_csv(dest / "phases.csv", rows)
    write_csv(dest / "comparisons.csv", pairs)
    disruptions = [p for p in pairs if p["phase_index"] == 0]
    summary = {"raw_sha256": hashlib.sha256(raw_bytes).hexdigest(), "runs": len(raw["runs"]), "phases": len(rows),
               "topologies": len({s["topology"] for s in scenarios.values()}), "scenarios": len(scenarios),
               "mrai_ms": suite["mrai_ms"], "failures": len(raw["errors"]),
               "all_phase_pairs": len(pairs), "disruption_pairs": len(disruptions),
               "all_phase_comparisons": compare_counts(pairs), "disruption_comparisons": compare_counts(disruptions),
               "initial_observer_path_mismatches": sum(not p["equal_initial_observer_path"] for p in disruptions),
               "initial_network_path_mismatches": sum(not p["equal_initial_network_paths"] for p in disruptions),
               "same_initial_network_disruption_comparisons": compare_counts([p for p in disruptions if p["equal_initial_network_paths"]]),
               "note": "Constructed mechanism tests and fixed delay permutations; not independent statistical samples or Internet averages."}
    (dest / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    primary = {p["scenario"]: p for p in disruptions if p["delay_scale"] == 1 and p["withdrawal_ignores_mrai"]}
    lines = ["# MRAI 5 秒拓扑实验数据表", "", "本表由原始结果自动生成。主表采用延迟倍率 1、撤销绕过 MRAI；箭头表示标准 BGP → full。",
             "", "## 各场景的首个扰动阶段", "", "| 场景 | 节点/边 | 观察点中间路径 | 全网中间路径 | 全网失效 AS 路径选择 | 队列清空 s | UPDATE 数 | 有物理通路但代理不可用 s |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for name, p in primary.items():
        def arrow(metric, seconds=False):
            a, b = p[f"standard_{metric}"], p[f"full_{metric}"]
            return f"{a/1000:.3f} → {b/1000:.3f}" if seconds else f"{a} → {b}"
        oracle = "—" if scenarios[name]["family"] == "ibgp" else arrow("network_invalid_as_path_selections")
        lines.append(f"| {name} | {p['router_count']}/{p['link_count']} | {arrow('observer_intermediate_paths')} | "
                     f"{arrow('network_intermediate_paths')} | {oracle} | {arrow('convergence_ms', True)} | "
                     f"{arrow('updates_sent')} | {arrow('proxy_unreachable_while_origin_connected_ms', True)} |")
    lines += ["", "## 机制消融", "", "延迟倍率 1、撤销绕过 MRAI；取首个扰动阶段。", "",
              "| 场景 | 机制 | 全网中间路径 | 全网失效 AS 路径选择 | 队列清空 s | UPDATE 数 | 独立状态 UPDATE 数 |", "|---|---|---:|---:|---:|---:|---:|"]
    for name in ("shared_loss", "fanout32", "clique8_withdraw", "layered_failure", "mrai_cascade", "metadata_gap"):
        for r in rows:
            if r["scenario"] == name and r["phase_index"] == 0 and r["delay_scale"] == 1 and r["withdrawal_ignores_mrai"]:
                lines.append(f"| {name} | {r['mode']} | {r['network_intermediate_paths']} | {r['network_invalid_as_path_selections']} | "
                             f"{r['convergence_ms']/1000:.3f} | {r['updates_sent']} | {r['control_updates_sent']} |")
    lines += ["", "## 全部成对比较", "", "lower/equal/higher 分别表示 full 相对标准 BGP 更小、相同、更大。",
              "", "| 指标 | 首次扰动 lower/equal/higher | 含恢复阶段 lower/equal/higher |", "|---|---:|---:|"]
    for metric in METRICS:
        counts = [" / ".join(str(summary[group][metric][k]) for k in ("lower", "equal", "higher"))
                  for group in ("disruption_comparisons", "all_phase_comparisons")]
        lines.append(f"| {metric} | {counts[0]} | {counts[1]} |")
    lines += ["", "## 拓扑清单", "", "| 场景 | 类型 | 观察点 | 构造和扰动 | 拓扑 |", "|---|---|---|---|---|"]
    for name, s in scenarios.items():
        topo = "../../../topo/versioned_mrai5/" + Path(s["topology"]).name
        lines.append(f"| {name} | {s['family']} | {s['observer']} | {s['description']} | [JSON]({topo}) |")
    lines += ["", "原始完整序列见 [raw_results.json.gz](raw_results.json.gz)；CSV 保留全部延迟倍率、撤销模式和恢复阶段。",
              "唯一源永久撤销时，后续无路由是预期状态；proxy_no_route_ms 和 proxy_unreachable_ms 的积分终点为各自队列清空时刻，不能跨机制解释为业务中断差异。",
              "主表改用尚有物理路径通向有效起源时的代理不可用时长；该值依然不是实际丢包测量。", ""]
    (dest / "TABLES.md").write_text("\n".join(lines), encoding="utf-8")
    if args.plots:
        plot(dest, rows, pairs, scenarios, topologies)
    print(json.dumps({k: summary[k] for k in ("topologies", "scenarios", "runs", "phases", "failures", "initial_network_path_mismatches")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
