"""Run in temporary game directories; never overwrite a user's logs/cache."""
import pathlib
import resource
import signal
import subprocess
import sys
import tempfile
import unittest

CLI = str(pathlib.Path(sys.argv.pop(1)).resolve())
FEN = "3k5/9/9/9/9/9/9/4r4/9/4K4 r - -"


class ParallelCliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cwd = pathlib.Path(self.temp.name)

    def run_cli(self, *args, input="quit\n", success=True):
        result = subprocess.run([CLI, *args], input=input, text=True, capture_output=True,
                                cwd=self.cwd, timeout=20)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_search_and_logs(self):
        result = self.run_cli("--search", "parallel", "--threads", "4", "--depth", "2",
                              "--split-percent", "50", "--fen", FEN, "--engine", "red",
                              "--search-detail", "--parallel-trace", "trace.txt")
        self.assertIn("red engine plays: e0f0", result.stdout)
        summary = (self.cwd / "build/search_detail.txt").read_text()
        self.assertIn("search_mode: parallel", summary)
        self.assertIn("depth: 2", summary)
        self.assertIn("threads: 4", summary)
        self.assertIn("bound_updates:", summary)
        self.assertNotIn("iteration_progress", summary)
        self.assertNotIn("cache_hits", summary)
        events = [dict(item.split("=", 1) for item in line.split())
                  for line in (self.cwd / "trace.txt").read_text().splitlines()
                  if line.startswith("search=")]
        self.assertEqual([int(e["event"]) for e in events], list(range(1, len(events) + 1)))
        self.assertTrue(any(e["kind"] == "dispatch" for e in events))
        self.assertTrue(any(e["kind"] == "refresh" and e["beta_source"] != "0" for e in events))
        self.assertTrue(all(int(e["lower"]) <= int(e["upper"]) for e in events))

    def test_conflicting_and_invalid_arguments(self):
        for args in (("--time-ms", "10"), ("--tt-file", "missing"),
                     ("--threads", "0"), ("--threads", "x"),
                     ("--split-percent", "101"), ("--split-percent", "-1"),
                     ("--depth", "0"), ("--parallel-trace",)):
            self.run_cli("--search", "parallel", *args, success=False)
        self.run_cli("--threads", "2", success=False)
        self.run_cli("--parallel-trace", "trace.txt", success=False)
        self.run_cli("--search", "unknown", success=False)

    def test_time_rejection_does_not_play_move(self):
        result = self.run_cli("--search", "parallel", "--fen", FEN,
                              input="fen\ne0f0 200\nfen\nsave-tt cache.tt\nquit\n")
        self.assertEqual(result.stdout.count(FEN), 2)
        self.assertIn("no moves played", result.stdout)
        self.assertIn("no transposition table", result.stdout)
        self.assertNotIn("engine plays:", result.stdout)
        self.assertFalse((self.cwd / "cache.tt").exists())

    def test_default_depth_and_manual_reply_undo(self):
        result = self.run_cli("--search", "parallel", "--threads", "1", "--split-percent", "0",
                              "--fen", FEN, input="e0f0 d9d8\nundo\nflip\nfen\nquit\n")
        self.assertIn("fixed depth 3", result.stdout)
        self.assertIn("specified by player", result.stdout)
        self.assertIn("Undid 2 move(s)", result.stdout)
        self.assertIn(FEN, result.stdout)
        self.assertIn("Board view:", result.stdout)

    def test_trace_open_failure_does_not_stop_search(self):
        result = self.run_cli("--search", "parallel", "--depth", "1", "--fen", FEN,
                              "--engine", "red", "--parallel-trace", "missing/trace.txt")
        self.assertIn("tracing disabled", result.stderr)
        self.assertIn("red engine plays: e0f0", result.stdout)

    def test_trace_write_failure_does_not_stop_search(self):
        def limit_trace_file():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (64, 64))

        result = subprocess.run(
            [CLI, "--search", "parallel", "--depth", "1", "--engine", "red",
             "--parallel-trace", "trace.txt"], input="quit\n", text=True,
            capture_output=True, cwd=self.cwd, timeout=20, preexec_fn=limit_trace_file)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("tracing disabled", result.stderr)
        self.assertIn("red engine plays:", result.stdout)

    def test_trace_appends_sessions(self):
        for _ in range(2):
            self.run_cli("--search", "parallel", "--depth", "1", "--fen", FEN,
                         "--engine", "red", "--parallel-trace", "trace.txt")
        trace = (self.cwd / "trace.txt").read_text()
        self.assertEqual(trace.count("=== parallel trace session ==="), 2)
        self.assertEqual(trace.count("search=1 event=1 kind=enter"), 2)

    def test_builtin_still_works(self):
        result = self.run_cli("--search", "builtin", "--depth", "1", "--time-ms", "10",
                              "--fen", FEN, "--engine", "red", "--search-detail")
        self.assertIn("red engine plays: e0f0", result.stdout)
        summary = (self.cwd / "build/search_detail.txt").read_text()
        self.assertIn("iteration_progress:", summary)
        self.assertNotIn("search_mode: parallel", summary)


if __name__ == "__main__":
    unittest.main()
