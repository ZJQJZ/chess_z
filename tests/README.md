# Search sampling tests

The CMake build registers the following CTest cases when `BUILD_TESTING` is on:

- `search_sampling`: builtin/parallel paths, inherited helper prefixes, ordinary and
  quiescence recursion, physical-thread request uniqueness, waiting/idle filtering,
  truncation, cancellation, terminal roots and unchanged single-thread search results.
- `sampling_reports`: borrowed snapshot copying, iteration boundaries, duplicate
  requests and deep/truncated paths through the background collector.
- `sampling_report_format` (Python 3): JSON, per-thread totals, prefix-inclusive
  counts, folded weights and published file integrity.
- `sampling_allocation_failures`: inject failures into controller, collector and
  report allocations. Warnings are intentional; the observer must degrade safely.

For example:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
cd build
ctest --output-on-failure
```

The tests do not allocate the engine's large default transposition tables. Checks
remain active in Release builds. The report module also supports the compile-time
test override `CLI_SAMPLE_ITERATION_BYTES=4096` to exercise bounded statistics.

To inspect memory/thread safety, compile the native test drivers with
`-fsanitize=address,undefined` or `-fsanitize=thread` on a supported compiler.
Performance comparisons should use a separate Release build, fixed positions and
depths, matching TT state, and repeated paired baseline/current runs. Do not run
CPU-intensive tests concurrently with a performance comparison.

The offline search-sample viewer has a standalone Node.js test suite:

```sh
node tests/search_sample_viewer_tests.js
```

It checks report validation, path identity, FEN coordinates, captures, ordinary and
quiescence phases, 1024-move truncated paths, capacity-limited prefixes, thread
counts and browser/CommonJS loading without a DOM. Replay intentionally follows
`xq_position_make_move` rather than applying chess legality rules, because search
samples can include pseudo-legal positions. If `build/search_samples` exists, the
suite also loads its saved reports and replays every sampled endpoint. Those
local reports are optional and are not required for the synthetic tests.

The viewer's event handlers also have a standalone DOM smoke test:

```sh
node tests/search_sample_viewer_ui_tests.js
```

This uses a small simulated DOM, not a real browser, and does not check layout,
fonts, rendering or browser file permissions. It runs the shipped `viewer.js`
handlers to check demo loading, report import and iteration changes, move/phase
selection and board text, orientation, chart zoom, filtering, invalid reports,
zero samples, truncation warnings and stale asynchronous imports (including slow
directory drops). Existing local depth-10/11 reports are imported when available.
