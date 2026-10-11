#!/usr/bin/env python3
"""Reproduce the 5s experiment matrix and record the exact inputs and executable."""
import argparse
import hashlib
import json
import platform
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="Path to the Qt-plugin BgpTesterExperiments executable")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "experiments/results/topology_mrai5")
    parser.add_argument("--plots", action="store_true", help="Also generate PNG/PDF figures (requires matplotlib)")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / "experiments/generate_topology_sweep.py")], check=True)
    suite = ROOT / "experiments/topology_sweep_mrai5.json"
    result = output / "raw_results.json"
    inputs = {ROOT / "CMakeLists.txt", suite, *ROOT.glob("topo/versioned_mrai5/*.json"),
              *ROOT.glob("experiments/*.py"), ROOT / "experiments/VersionedExperiments.cpp"}
    for folder in ("engine", "model", "plugin", "router_plugins", "persistence"):
        inputs.update(p for p in (ROOT / "src" / folder).rglob("*") if p.suffix in (".hpp", ".cpp"))
    source_hashes = {str(p.relative_to(ROOT)): sha256(p) for p in sorted(inputs)}
    started = datetime.now(timezone.utc).isoformat()
    command = [str(binary), str(suite), str(result)]
    before = time.monotonic()
    subprocess.run(command, check=True, cwd=ROOT)
    elapsed = time.monotonic() - before
    summarize = [sys.executable, str(ROOT / "experiments/summarize_topology_sweep.py"), str(result), "--suite", str(suite)]
    if args.plots:
        summarize.append("--plots")
    subprocess.run(summarize, check=True)
    if any(sha256(ROOT / p) != digest for p, digest in source_hashes.items()):
        raise RuntimeError("An experiment input changed during execution")
    receipt = {"started_utc": started, "runner_wall_seconds": elapsed, "platform": platform.platform(),
               "python_version": platform.python_version(), "executable": str(binary), "executable_sha256": sha256(binary),
               "git_head": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
               "command": command, "input_sha256": source_hashes, "raw_sha256": sha256(result),
               "note": "Source hashes identify the working files, including uncommitted changes. Wall time is diagnostic, not simulated convergence."}
    (output / "provenance.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"Results and provenance: {output}")


if __name__ == "__main__":
    main()
