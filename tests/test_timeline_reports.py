"""Validate timeline commit manifests, state carry, node references and failures."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def validate(executable, mode):
    with tempfile.TemporaryDirectory(prefix="xiangqi-timeline-report-") as root:
        subprocess.run([executable, root, mode], check=True, timeout=20)
        session_path, = Path(root).glob("search-*/session.json")
        session = json.loads(session_path.read_text())
        if mode in ("statistics_unlimited", "statistics_budget"):
            directory = session_path.parent
            report = json.loads((directory / "depth-1.json").read_text())
            limited = mode == "statistics_budget"
            assert session["finished"] and report["finished"]
            assert session["iteration_memory_limit_bytes"] == (4096 if limited else 0)
            if limited:
                assert report["statistics_bytes"] <= 4096
                assert report["unrecorded_path_samples"] > 0
                assert report["incomplete"] and session["incomplete"]
            else:
                assert report["statistics_bytes"] > 4096
                assert not report["unrecorded_path_samples"]
                assert not report["incomplete"] and not session["incomplete"]
            nodes = report["nodes"]
            assert session["valid_samples"] == report["valid_samples"] == (256 if limited else 0)
            assert sum(node["self"] for node in nodes) == report["valid_samples"]
            assert sum(node["unrecorded_self"] for node in nodes) == report["unrecorded_path_samples"]
            subtotals = [node["self"] for node in nodes]
            for index, node in enumerate(nodes):
                assert node["id"] == index
                if index:
                    assert 0 <= node["parent"] < index
                    subtotals[node["parent"]] += node["inclusive"]
            assert all(node["inclusive"] == total for node, total in zip(nodes, subtotals))
            assert sum(int(line.rsplit(" ", 1)[1]) for line in
                       (directory / "depth-1.folded").read_text().splitlines()) == report["valid_samples"]
            assert not list(directory.glob("*.tmp"))
            return
        assert session["finished"] and session["valid_samples"] == (4 if mode in ("boundaries", "sample_state") else 2)
        assert session["timeline_version"] == 1
        assert session["timeline_state_dropped"] == (6 if mode == "boundaries" else 4 if mode == "sample_state" else 2)
        directory = session_path.parent
        depths = [json.loads((directory / f"depth-{d}.json").read_text())
                  for d in (range(1, 8) if mode == "boundaries" else range(1, 5) if mode == "sample_state" else (1, 2))]
        first, second = depths[:2]
        assert first["valid_samples"] == 2 and second["valid_samples"] == 0
        for depth in depths:
            assert depth["finished"] and depth["timeline"]["finished"]
            assert depth["nodes"][0]["inclusive"] == depth["valid_samples"]
            assert sum(n["self"] for n in depth["nodes"]) == depth["valid_samples"]
            folded = directory / f'depth-{depth["iteration_depth"]}.folded'
            assert sum(int(line.rsplit(" ", 1)[1]) for line in folded.read_text().splitlines()) == depth["valid_samples"]
            assert len(depth["timeline"]["initial_states"]) == 3
        assert second["timeline"]["end_ms"] == 50
        assert second["timeline"]["start_ms"] == (40 if mode == "enable_allocation" else 50)
        initial = second["timeline"]["initial_states"]
        if mode != "enable_allocation":
            assert initial[1]["state"] == "idle" and initial[1]["time_ms"] == 30
        assert initial[2]["state"] == "unknown" and initial[2]["time_ms"] is None
        if mode in ("normal", "unlimited", "boundaries", "sample_state"):
            assert session["timeline_stop_reason"] is None
            if mode == "unlimited":
                assert session["timeline_byte_limit"] == 0 and session["timeline_bytes"] > 0
            assert session["timeline_incomplete"]  # Explicit gap and invalid clock.
            assert first["timeline"]["state_dropped"] == 2
            assert first["timeline"]["incomplete"]
            assert first["timeline"]["start_ms"] == 10
            assert first["timeline"]["end_ms"] == 40
            records = []
            for depth in depths:
                for info in depth["timeline"]["chunks"]:
                    block = json.loads((directory / info["file"]).read_text())
                    assert block["timeline_version"] == 1
                    assert block["iteration_depth"] == depth["iteration_depth"]
                    assert len(block["records"]) == info["records"]
                    assert block["start_ms"] == info["start_ms"] <= info["end_ms"] == block["end_ms"]
                    for event in block["records"]:
                        if event["type"] == "sample":
                            assert 0 <= event["node_id"] < len(depth["nodes"])
                    records.extend(block["records"])
            samples = [r for r in records if r["type"] == "sample"]
            assert len(samples) == (4 if mode in ("boundaries", "sample_state") else 2)
            assert samples[0]["time_ms"] == 20 and samples[0]["request_time_ms"] == 15
            assert samples[1]["request_time_ms"] is None
            assert samples[0]["sequence"] < samples[1]["sequence"]
            assert any(r["type"] == "gap" and r["lost_count"] == 2 for r in records)
            assert any(r["type"] == "state" and r["time_ms"] is None and r["state"] == "unknown" for r in records)
            assert first["timeline"]["cutoff_ms"] is None
            for known in depths[:2]:
                assert known["timeline"]["start_known"] and known["timeline"]["end_known"]
                assert not known["timeline"]["boundary_unknown"]
            if mode == "sample_state":
                third, fourth = [d["timeline"] for d in depths[2:]]
                assert third["start_known"] and third["end_known"]
                assert fourth["start_known"] and fourth["end_known"]
                assert fourth["initial_states"][1]["state"] == "unknown"
                assert samples[2]["time_ms"] == 70 and samples[2]["request_time_ms"] == 65
                assert samples[3]["time_ms"] is None and samples[3]["request_time_ms"] is None
                assert samples[3]["state"] == "unknown"
                assert fourth["incomplete"]
            if mode == "boundaries":
                missing, resumed, recovered, invalid_end, delayed_invalid_end = [d["timeline"] for d in depths[2:]]
                assert missing["start_ms"] == 60 and missing["end_ms"] == 80
                assert not missing["start_known"] and not missing["end_known"]
                assert missing["boundary_unknown"] and missing["incomplete"]
                assert missing["cutoff_ms"] == 70
                assert all(t["state"] == "unknown" for t in missing["initial_states"])
                assert resumed["start_ms"] == 60 and resumed["end_ms"] == 100
                assert not resumed["start_known"] and resumed["end_known"]
                assert resumed["boundary_unknown"] and resumed["incomplete"]
                assert all(t["state"] == "unknown" for t in resumed["initial_states"])
                assert resumed["cutoff_ms"] is None
                assert recovered["start_ms"] == 110 and recovered["end_ms"] == 120
                assert recovered["start_known"] and recovered["end_known"]
                assert not recovered["boundary_unknown"]
                assert invalid_end["start_ms"] == 130 and invalid_end["end_ms"] == 150
                assert invalid_end["start_known"] and not invalid_end["end_known"]
                assert invalid_end["boundary_unknown"] and invalid_end["cutoff_ms"] == 130
                assert delayed_invalid_end["start_ms"] == 170 and delayed_invalid_end["end_ms"] == 190
                assert delayed_invalid_end["start_known"] and not delayed_invalid_end["end_known"]
                assert delayed_invalid_end["boundary_unknown"] and delayed_invalid_end["incomplete"]
                assert delayed_invalid_end["cutoff_ms"] == 190
                assert not (directory / "depth-20.json").exists()
        else:
            expected = {"budget": "byte_limit", "write_failure": "write_failed",
                        "enable_allocation": "allocation", "iteration_allocation": "allocation", "iteration_struct_allocation": "allocation",
                        "iteration_nodes_allocation": "allocation"}[mode]
            assert session["timeline_stop_reason"] == expected
            assert session["timeline_incomplete"]
            assert first["timeline"]["stop_reason"] == expected
            assert first["timeline"]["incomplete"]
            assert first["timeline"]["cutoff_ms"] == 0
            assert not first["timeline"]["chunks"]
            assert not list(directory.glob("depth-*-timeline-*.json"))
        assert not [p for p in directory.glob("*.tmp") if p.is_file()]


if __name__ == "__main__":
    scenarios = sys.argv[2:] or ["normal", "unlimited", "statistics_unlimited", "statistics_budget",
                                "boundaries", "sample_state", "budget", "write_failure", "enable_allocation",
                                "iteration_allocation", "iteration_struct_allocation", "iteration_nodes_allocation"]
    for scenario in scenarios:
        validate(sys.argv[1], scenario)
    print(f"timeline JSON/manifest invariants passed ({len(scenarios)} scenarios)")
