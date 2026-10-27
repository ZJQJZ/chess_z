#!/usr/bin/env python3
"""Validate reports produced by timeline_collector_tests (normal and lossless tiny queue)."""
import json
from pathlib import Path
import re
import sys


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def validate_case(directory, case):
    session = load(directory / "session.json")
    assert session["finished"] and session["search_id"] == case["search_id"]
    assert session["thread_capacity"] == case["threads"]
    depth_files = sorted((p for p in directory.glob("depth-*.json")
                          if re.fullmatch(r"depth-\d+\.json", p.name)),
                         key=lambda p: int(p.stem.split("-")[1]))
    assert depth_files, (case, directory)
    real = case["name"].startswith("real-")
    if real:
        assert [int(p.stem.split("-")[1]) for p in depth_files] == list(range(1, 7))
    all_records, total_samples, sample_threads = [], 0, set()
    previous_end = 0
    for path in depth_files:
        data = load(path)
        depth = data["iteration_depth"]
        timeline = data["timeline"]
        assert data["timeline_version"] == 1 and data["finished"] and timeline["finished"]
        assert timeline["start_ms"] <= timeline["end_ms"]
        if real:
            assert previous_end <= timeline["start_ms"], (case, depth, previous_end, timeline)
            assert timeline["stop_reason"] is None
        previous_end = timeline["end_ms"]
        initials = {s["thread_id"]: s for s in timeline["initial_states"]}
        assert set(initials) == set(range(case["threads"]))
        if real:
            for thread in range(case["threads"]):
                assert initials[thread]["state"] == "idle", (case, depth, initials)
                assert initials[thread]["time_ms"] <= timeline["start_ms"]
        elif case["name"] == "synthetic":
            assert initials[2]["state"] == "unknown" and initials[2]["sequence"] == 0
        nodes = {node["id"]: node for node in data["nodes"]}
        assert len(nodes) == len(data["nodes"]) and nodes[0]["kind"] == "root"
        assert sum(node["self"] for node in nodes.values()) == data["valid_samples"]
        for node in nodes.values():
            assert node["parent"] is None or node["parent"] in nodes
            children = sum(n["inclusive"] for n in nodes.values() if n["parent"] == node["id"])
            assert node["inclusive"] == node["self"] + children
        folded = path.with_suffix(".folded").read_text(encoding="utf-8").splitlines()
        assert sum(int(line.rsplit(" ", 1)[1]) for line in folded if line) == data["valid_samples"]
        records = []
        for chunk in timeline["chunks"]:
            assert "/" not in chunk["file"] and "\\" not in chunk["file"]
            block = load(directory / chunk["file"])
            assert block["timeline_version"] == 1
            assert block["search_id"] == session["search_id"] and block["iteration_depth"] == depth
            assert len(block["records"]) == chunk["records"]
            assert block["start_ms"] == chunk["start_ms"] and block["end_ms"] == chunk["end_ms"]
            records += block["records"]
        sequences, requests = {}, {}
        path_count = 0
        for record in records:
            thread = record["thread_id"]
            assert 0 <= thread < case["threads"]
            assert record["sequence"] > sequences.get(thread, 0), (case, depth, record, sequences)
            sequences[thread] = record["sequence"]
            if record["time_ms"] is not None:
                assert timeline["start_ms"] <= record["time_ms"] <= timeline["end_ms"]
            if record["type"] == "sample":
                path_count += 1
                sample_threads.add(thread)
                assert record["node_id"] in nodes
                assert record["request_id"] > requests.get(thread, 0)
                requests[thread] = record["request_id"]
                if record["request_time_ms"] is not None:
                    assert record["request_time_ms"] <= record["time_ms"]
                assert record["normal_ply"] <= depth
            else:
                assert "node_id" not in record and "request_id" not in record
        assert path_count == data["valid_samples"], (case, depth, path_count, data["valid_samples"])
        all_records += records
        total_samples += path_count
        caller_states = [r for r in records if r["thread_id"] == 0 and r["type"] == "state"]
        if real or case["name"] == "overflow":
            assert caller_states[0]["state"] == "search", (case, depth, caller_states)
            assert caller_states[-1]["state"] == "idle", (case, depth, caller_states)
        if real and depth < 5:
            assert all(r["thread_id"] == 0 for r in records), (case, depth, records)
    assert session["valid_samples"] == total_samples
    if real:
        assert total_samples > 0
        assert all(r["state"] in {"search", "quiescence"} for r in all_records if r["type"] == "sample")
        if case["threads"] > 1:
            assert sample_threads - {0}, (case, sample_threads)
    elif case["name"] == "synthetic":
        assert total_samples == 3, (case, total_samples, all_records)
        samples = [r for r in all_records if r["type"] == "sample"]
        assert any(r["node_id"] == 0 and r["state"] == "search" for r in samples)
        assert any(r["state"] == "quiescence" and r["quiescence_ply"] == 1 for r in samples)
        assert any(r["time_ms"] is None and r["state"] == "unknown" for r in samples)
        assert all(r["request_time_ms"] is None for r in samples)
        assert any(r["type"] == "state" and r["state"] == "wait_helpers" and r["thread_id"] == 1
                   for r in all_records)
        assert any(r["type"] == "state" and r["time_ms"] is None for r in all_records)
        assert session["duplicates"] >= 1 and session["dropped"] >= 1
        assert load(depth_files[0])["timeline"]["incomplete"]
    elif case["name"] == "overflow":
        assert len(depth_files) == 1
        data = load(depth_files[0])
        gaps = [r for r in all_records if r["type"] == "gap"]
        assert not gaps and data["timeline"]["state_dropped"] == 0
        assert session["timeline_state_dropped"] == 0
        assert session["slot_overwritten"] == session["boundaries_dropped"] == 0
        assert session["dropped"] == session["duplicates"] == session["allocation_dropped"] == 0
        assert not session["incomplete"] and not data["incomplete"] and not data["timeline"]["incomplete"]
        assert data["timeline"]["start_known"] and data["timeline"]["end_known"]
        samples = [r for r in all_records if r["type"] == "sample"]
        assert total_samples == len(samples) == 3, (case, total_samples, samples)
        assert [r["request_id"] for r in samples] == [100000001, 100000002, 100000003]
        assert [nodes[r["node_id"]]["move"] for r in samples] == ["e0d0", "e0f0", "e0d0"]
        assert all(r["thread_id"] == 0 and r["state"] == "search" and
                   r["normal_ply"] == 1 and r["quiescence_ply"] == 0 and
                   nodes[r["node_id"]]["parent"] == 0 and
                   nodes[r["node_id"]]["kind"] == "normal_move" for r in samples)
        helper_states = [r for r in all_records if r["thread_id"] == 1 and r["type"] == "state"]
        expected = [(sequence, "wait_helpers" if sequence % 2 == 0 else "search")
                    for sequence in range(2, 66)] + [(66, "idle")]
        assert [(r["sequence"], r["state"]) for r in helper_states] == expected
    return total_samples, len(all_records)


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "build/timeline_validation/integration")
    metadata = load(root / "collector-cases.json")
    directories = {}
    for path in root.glob(f"search-*-{metadata['pid']}-*-*/session.json"):
        session = load(path)
        directories[session["search_id"]] = path.parent
    sample_count = record_count = 0
    for case in metadata["cases"]:
        assert case["search_id"] in directories, case
        samples, records = validate_case(directories[case["search_id"]], case)
        sample_count += samples
        record_count += records
    print(f"timeline collector format passed: {len(metadata['cases'])} searches, "
          f"{sample_count} path samples, {record_count} records")


if __name__ == "__main__":
    main()
