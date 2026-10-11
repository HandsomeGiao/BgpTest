#!/usr/bin/env python3
"""Regenerate the small, inspectable versioned-path experiment fixtures."""
import copy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PREFIX = "192.0.2.0/24"
OTHER = "198.51.100.0/24"
POLICY = "org.bgptester.router.versioned-path"


def topology(name, ids, origins=("O",), asns=None):
    asns = asns or {}
    return {
        "simulation": {"name": name, "log_dir": "tmp", "convergence_quiet_ms": 0,
                       "withdrawal_ignores_mrai": True},
        "routers": [
            {"id": node, "router_id": f"10.0.0.{i + 1}", "asn": asns.get(node, 65001 + i),
             "originated_prefixes": [PREFIX, OTHER] if node in origins else [],
             "plugin": {"id": POLICY, "settings": {"mode": "full"}},
             "position": {"x": 120 + (i % 4) * 190, "y": 100 + (i // 4) * 180}}
            for i, node in enumerate(ids)
        ], "links": []
    }


def link(topo, a, b, delay=1, relationship="a_provider"):
    topo["links"].append({
        "a": a, "b": b, "enabled": True, "delay_ms": delay,
        "relationship": relationship, "mrai_ms_from_a": 30000, "mrai_ms_from_b": 30000,
        "rr_client_from_a": False, "rr_client_from_b": False
    })


def action(kind, at=0, **kwargs):
    return {"kind": kind, "at_ms": at, **kwargs}


def phase(name, *actions, reachable=True):
    return {"name": name, "actions": list(actions), "observer_reachable": reachable}


def main():
    shared = topology("shared-dependency", ["O", "D", "A", "B", "X", "C", "E", "F"])
    for args in [("D", "O", 5), ("D", "A", 1), ("D", "B", 80),
                 ("A", "X", 1), ("B", "X", 1), ("C", "X", 1),
                 ("E", "C", 1), ("F", "E", 1), ("F", "O", 1)]:
        link(shared, *args)
    reroute = copy.deepcopy(shared)
    reroute["simulation"]["name"] = "reroute"
    reroute["routers"] = [r for r in reroute["routers"] if r["id"] != "F"]
    reroute["routers"].append({"id": "Z", "router_id": "10.0.0.9", "asn": 65009,
                              "originated_prefixes": [], "plugin": {"id": POLICY, "settings": {"mode": "full"}},
                              "position": {"x": 350, "y": 500}})
    reroute["links"] = [e for e in reroute["links"] if "F" not in (e["a"], e["b"])]
    link(reroute, "E", "O", 20)
    link(reroute, "D", "Z", 1)
    link(reroute, "Z", "O", 15)
    gap = copy.deepcopy(reroute)
    gap["simulation"]["name"] = "reroute-without-independent-backup"
    gap["routers"] = [r for r in gap["routers"] if r["id"] not in ("C", "E")]
    gap["links"] = [e for e in gap["links"] if not {"C", "E"} & {e["a"], e["b"]}]
    multi = copy.deepcopy(shared)
    multi["simulation"]["name"] = "multiple-origins"
    for r in multi["routers"]:
        if r["id"] == "F":
            r["originated_prefixes"] = [PREFIX]
    for e in multi["links"]:
        if (e["a"], e["b"]) == ("E", "C"):
            e["delay_ms"] = 20
    border = topology("ibgp-border", ["O", "BIN", "BOUT", "X"],
                      asns={"O": 65100, "BIN": 65200, "BOUT": 65200, "X": 65300})
    link(border, "BIN", "O", 5)
    link(border, "BIN", "BOUT", 10, "unspecified")
    link(border, "BOUT", "X", 5)
    internal = topology("ibgp-origin", ["BIN", "BOUT", "X"], origins=("BIN",),
                        asns={"BIN": 65200, "BOUT": 65200, "X": 65300})
    link(internal, "BIN", "BOUT", 10, "unspecified")
    link(internal, "BOUT", "X", 5)
    fanout = topology("eight-shared-branches", ["O", "D", *[f"R{i}" for i in range(8)], "X", "C", "E", "F"])
    # Match the framework's deterministic textual NEXT_HOP tie break to the
    # branch-delay ordering so the eight-branch exploration is observable.
    for i, router in enumerate(fanout["routers"]):
        router["router_id"] = (f"10.0.0.{int(router['id'][1:]) + 1}" if router["id"].startswith("R")
                               else f"10.0.1.{i + 1}")
    link(fanout, "D", "O", 5)
    for i in range(8):
        link(fanout, "D", f"R{i}", 1 + i * 10)
        link(fanout, f"R{i}", "X", 1)
    for a, b in [("C", "X"), ("E", "C"), ("F", "E"), ("F", "O")]:
        link(fanout, a, b)
    cascade = copy.deepcopy(shared)
    cascade["simulation"]["name"] = "mrai-exploration-cascade"
    cascade["routers"].append({"id": "Y", "router_id": "10.0.0.10", "asn": 65010,
                              "originated_prefixes": [], "plugin": {"id": POLICY, "settings": {"mode": "full"}},
                              "position": {"x": 880, "y": 460}})
    link(cascade, "X", "Y", 1)
    fixtures = {"mrai_cascade": cascade,"shared_dependency": shared, "reroute": reroute, "metadata_gap": gap, "multi_origin": multi,
                "ibgp_border": border, "ibgp_origin": internal, "fanout": fanout}
    target = ROOT / "topo" / "versioned"
    target.mkdir(parents=True, exist_ok=True)
    for name, fixture in fixtures.items():
        (target / f"{name}.json").write_text(json.dumps(fixture, ensure_ascii=False, indent=2) + "\n")

    down = lambda a, b, at=0: action("link", at, a=a, b=b, enabled=False)
    up = lambda a, b, at=0: action("link", at, a=a, b=b, enabled=True)
    withdraw = lambda router: action("withdraw", router=router, prefix=PREFIX)
    originate = lambda router: action("originate", router=router, prefix=PREFIX)
    scenarios = []
    def scenario(name, fixture, phases, observer="X"):
        scenarios.append({"name": name, "topology": f"../topo/versioned/{fixture}.json",
                          "observer": observer, "prefix": PREFIX, "unaffected_prefix": OTHER, "phases": phases})
    scenario("shared_loss", "shared_dependency", [phase("failure", down("D", "O")), phase("recovery", up("D", "O"))])
    scenario("local_link_loss", "shared_dependency", [phase("failure", down("D", "A")), phase("recovery", up("D", "A"))])
    scenario("export_only", "shared_dependency", [
        phase("filter", action("export", router="A", peer="X", enabled=False)),
        phase("restore", action("export", router="A", peer="X", enabled=True))])
    scenario("rapid_recovery", "shared_dependency", [phase("flap", down("D", "O"), up("D", "O", 3))])
    scenario("origin_withdraw", "shared_dependency", [phase("withdraw", withdraw("O"), reachable=False),
                                                       phase("restore", originate("O"))])
    scenario("reroute", "reroute", [phase("failure", down("D", "O")), phase("recovery", up("D", "O"))])
    scenario("metadata_gap", "metadata_gap", [phase("flap", down("D", "O"), up("D", "O", 3))])
    scenario("multi_origin", "multi_origin", [phase("withdraw_one_origin", withdraw("O")),
                                               phase("restore_origin", originate("O"))])
    scenario("ibgp_border", "ibgp_border", [phase("withdraw", withdraw("O"), reachable=False),
                                              phase("restore", originate("O"))])
    scenario("ibgp_origin", "ibgp_origin", [phase("withdraw", withdraw("BIN"), reachable=False),
                                              phase("restore", originate("BIN"))])
    scenario("fanout", "fanout", [phase("failure", down("D", "O")), phase("recovery", up("D", "O"))])
    scenario("mrai_cascade", "mrai_cascade", [phase("failure", down("D", "O")), phase("recovery", up("D", "O"))], observer="Y")
    (ROOT / "experiments" / "versioned_scenarios.json").write_text(
        json.dumps({"schema": "bgptester-versioned-scenarios-v1", "mrai_ms": [1000, 5000, 30000], "scenarios": scenarios},
                   ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
