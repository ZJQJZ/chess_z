"""Validate public report invariants using the native collector test driver."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def validate(directory):
    sessions = list(directory.glob("search-*/session.json"))
    assert len(sessions) == 1, sessions
    session = json.loads(sessions[0].read_text())
    assert session["finished"]
    assert session["valid_samples"] > 0
    assert session["duplicates"] >= 1
    assert session["path_limit"] == 1024
    reports = sorted(sessions[0].parent.glob("depth-*.json"))
    assert {json.loads(p.read_text())["iteration_depth"] for p in reports} == {1, 2}
    samples = 0
    found_quiescence = False
    for path in reports:
        report = json.loads(path.read_text())
        assert report["finished"]
        if session["iteration_memory_limit_bytes"]:
            assert report["statistics_bytes"] <= session["iteration_memory_limit_bytes"]
        assert report["thread_statistics_available"]
        assert sum(t["valid_samples"] for t in report["threads"]) == report["valid_samples"]
        nodes = report["nodes"]
        assert nodes[0]["parent"] is None
        assert nodes[0]["inclusive"] == report["valid_samples"]
        assert sum(n["self"] for n in nodes) == report["valid_samples"]
        children = [0] * len(nodes)
        for index, node in enumerate(nodes):
            assert node["id"] == index
            if node["parent"] is not None:
                assert 0 <= node["parent"] < index
                children[node["parent"]] += node["inclusive"]
            found_quiescence |= node["kind"] == "quiescence"
        for node, child_samples in zip(nodes, children):
            assert node["inclusive"] == node["self"] + child_samples
            assert node["self"] >= node["truncated_self"] + node["unrecorded_self"]
        folded = path.with_suffix(".folded").read_text().splitlines()
        assert sum(int(line.rsplit(" ", 1)[1]) for line in folded) == report["valid_samples"]
        samples += report["valid_samples"]
        if report["iteration_depth"] == 2:
            assert report["max_normal_ply"] == 1
            assert report["max_quiescence_ply"] == 1026
            assert report["truncated_samples"] or report["unrecorded_path_samples"]
            assert report["incomplete"]
    assert samples == session["valid_samples"]
    assert found_quiescence
    assert not list(sessions[0].parent.glob("*.tmp"))


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="xiangqi-sample-report-") as root:
        destination = Path(root)
        subprocess.run([sys.argv[1], str(destination)], check=True, timeout=15)
        validate(destination)
    print("sampling JSON/folded invariants passed")
