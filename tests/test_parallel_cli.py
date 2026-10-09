"""Run in temporary game directories; never overwrite a user's logs/cache."""
import pathlib
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
                              "--search-detail")
        self.assertIn("red engine plays: e0f0", result.stdout)
        summary = (self.cwd / "build/search_detail.txt").read_text()
        self.assertIn("search_mode: parallel", summary)
        self.assertIn("depth: 2", summary)
        self.assertIn("threads: 4", summary)
        self.assertNotIn("bound_updates:", summary)
        self.assertNotIn("nodes:", summary)
        self.assertNotIn("elapsed_ms:", summary)
        self.assertNotIn("iteration_progress", summary)
        self.assertNotIn("cache_hits", summary)

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

    def test_removed_trace_option_is_rejected(self):
        self.run_cli("--search", "parallel", "--parallel-trace", "trace.txt", success=False)
        self.assertFalse((self.cwd / "trace.txt").exists())

    def test_builtin_still_works(self):
        result = self.run_cli("--search", "builtin", "--depth", "1", "--time-ms", "10",
                              "--fen", FEN, "--engine", "red", "--search-detail")
        self.assertIn("red engine plays: e0f0", result.stdout)
        summary = (self.cwd / "build/search_detail.txt").read_text()
        self.assertIn("iteration_progress:", summary)
        self.assertNotIn("search_mode: parallel", summary)


if __name__ == "__main__":
    unittest.main()
