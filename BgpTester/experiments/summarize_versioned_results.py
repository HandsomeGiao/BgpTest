#!/usr/bin/env python3
"""Export deterministic phase-level CSV and a compact, auditable summary."""
import argparse
import csv
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    raw = args.results.read_bytes()
    data = json.loads(raw)
    if data["errors"]:
        raise SystemExit(f"Refusing to summarize failed runs: {data['errors']}")
    destination = args.output_dir or args.results.parent
    destination.mkdir(parents=True, exist_ok=True)
    rows = []
    for run in data["runs"]:
        for phase in run["phases"]:
            row = {k: v for k, v in run.items() if k != "phases"}
            row.update({k: v for k, v in phase.items() if k not in ("observer_graph", "name")})
            row["phase"] = phase["name"]
            row["proxy_unreachable_ms"] = sum(phase[k] for k in ("proxy_no_route_ms", "proxy_broken_ms", "proxy_loop_ms"))
            for key, value in list(row.items()):
                if key.endswith("_ms") and isinstance(value, (int, float)):
                    row[key[:-3] + "_seconds"] = value / 1000
            rows.append(row)
    with (destination / "versioned_phases.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow({k: json.dumps(v, separators=(",", ":"), sort_keys=True) if isinstance(v, (list, dict)) else v
                             for k, v in row.items()})

    keys = ("scenario", "phase", "mrai_ms", "delay_scale", "withdrawal_ignores_mrai")
    standard = {tuple(row[k] for k in keys): row for row in rows if row["mode"] == "standard"}
    comparisons = {}
    for metric in ("observer_changes", "observer_last_change_ms", "proxy_unreachable_ms",
                   "proxy_no_route_ms", "convergence_ms", "updates_sent"):
        counts = {"lower": 0, "equal": 0, "higher": 0}
        for row in rows:
            if row["mode"] != "full":
                continue
            base = standard[tuple(row[k] for k in keys)][metric]
            counts["lower" if row[metric] < base else "higher" if row[metric] > base else "equal"] += 1
        comparisons[metric] = counts
    representative = [row for row in rows if row["delay_scale"] == 1 and (
        (row["scenario"] in ("shared_loss", "fanout", "mrai_cascade") and row["phase"] == "failure")
        or (row["scenario"] == "metadata_gap"))]
    summary = {
        "schema": "bgptester-versioned-summary-v1",
        "source_sha256": hashlib.sha256(raw).hexdigest(),
        "runs": len(data["runs"]), "phases": len(rows), "failures": len(data["errors"]),
        "scenarios": sorted({row["scenario"] for row in rows}),
        "modes": sorted({row["mode"] for row in rows}),
        "mrai_seconds": sorted({row["mrai_seconds"] for row in rows}),
        "withdrawal_ignores_mrai": sorted({row["withdrawal_ignores_mrai"] for row in rows}),
        "delay_scales": sorted({row["delay_scale"] for row in rows}),
        "full_vs_standard_phase_counts": comparisons,
        "representative_phases": representative,
        "measurement": {k: v for k, v in data.items() if k not in ("schema", "runs", "errors")},
        "comparison_note": "Counts describe this constructed suite only; phases are not statistical samples.",
    }
    (destination / "versioned_summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"{len(data['runs'])} runs, {len(rows)} phases -> {destination}")


if __name__ == "__main__":
    main()
