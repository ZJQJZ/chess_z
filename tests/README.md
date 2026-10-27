# Search tests

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
- `timeline_report_format` (Python 3): timestamped path/state records, atomic chunk
  publication, dictionary references, incomplete clocks and timeline storage limits;
  scenarios also check unlimited timeline storage, paged statistics growth,
  explicit statistics budgets, cross-iteration lost boundaries and conservative unknown
  states after a gap, including a null response time with a formerly plausible request.
- `timeline_collector` and `timeline_collector_format` (Python 3): actual collector
  runs for builtin and both parallel strategies with 1/2/16 threads; every iteration's
  caller boundaries, initial/never-tasked helper states, node/folded totals, ordinary
  and quiescence samples, request uniqueness, stale sequences and invalid clocks.
- `timeline_collector_overflow` and `timeline_collector_overflow_format` (Python 3):
  compile the collector with a two-entry unified queue and one-entry batches;
  a short mixed sequence checks that backpressure retains all states and paths,
  copied paths survive immediate source-buffer reuse, and shutdown drains the tail.
  No overflow gap or overwritten sample is expected. The normal
  and overflow targets use separate report directories; CTest fixtures generate
  fresh reports before running either Python validator.
- `parallel_similarity`: white-box coverage of sibling enumeration, shared query
  budgets, invalid/duplicate positions, read-only hint collection, unified
  winning/quiet-move ordering and read-only TT
  snapshots, plus serial and parallel search, sampling and cancellation checks.
- `parallel_similarity_disabled`: the same driver with a query limit of `0` to check
  disabled behavior. The enabled driver uses a limit of `2`; both drivers use a
  test-only TT with 64 buckets. CMake generates separate implementation copies in
  the build directory, changing only the two enum initializers; production sources
  and constants are unaffected. Editing the source regenerates these copies.

To configure production similarity lookups, edit the
`PARALLEL_MAX_SIMILAR_POSITION_QUERIES` enum value in `src/parallel_search.c`;
`0` disables them. These constants have no `-D` overrides.

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
Similarity-query tests count misses, busy buckets and unusable entries against the
budget, and check that neighborhood reads neither refresh generations nor change
the ordinary TT statistics. Ordinary similar-position hints remain quiet moves below
PV/cache/capture priority. A matching `XQ_SEARCH_SCORE_EXACT` or
`XQ_SEARCH_SCORE_LOWER_BOUND` entry with `score == WIN` (`30000`, from the candidate's
side-to-move perspective) can instead promote a capture or quiet move below PV but
ahead of same-position cache moves. Winning hints do not end enumeration: later
winning and ordinary hints are collected until the query budget or sources are
exhausted, or the node stops. Distinct winning hints retain their discovery order.
`parallel_collect_similar_hints()` returns complete current-move records without
reordering the list. `parallel_apply_move_hints()` applies all collected, cache and PV
hints in one stage, after stable capture ordering and before task splitting.
Tests check collection leaves move order unchanged, the full
`PV > WIN > TT > captures > similar quiet > other quiet` precedence, continuation
after wins, post-win misses, busy buckets and unusable hits against the budget,
duplicate hints, source exhaustion, exact `from`/`to`/`piece` matching, and
current-move metadata. They also check PV suffix propagation for valid matching or
conflicting PV first moves and suffix clearing for absent, empty or invalid PV hints.
Similar scores determine ordering priority only and cannot directly cause a search cutoff.

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
counts, incremental model construction and browser/CommonJS loading without a DOM. Replay intentionally follows
`xq_position_make_move` rather than applying chess legality rules, because search
samples can include pseudo-legal positions. If `build/search_samples` exists, the
suite also loads saved reports up to 64 MiB and replays every sampled endpoint. Those
local reports are optional and are not required for the synthetic tests.

The large-report reader has bounded-chunk JSON tests:

```sh
node tests/search_sample_report_reader_tests.js
```

These cover UTF-8 and JSON boundaries, streamed node delivery, malformed or
truncated input, cancellation and reading without a whole-file string. The UI
suite also checks importing and switching large reports, cancellation during a
read, and retaining the current report when a large file is damaged.

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

The timeline model and Canvas event handlers have separate tests:

```sh
node tests/search_sample_timeline_tests.js
node tests/search_sample_timeline_ui_tests.js
```

They cover chunk validation and lazy loading, state intervals, missing/corrupt
chunks, unknown gaps, relative timestamps, thread/time/iteration selection,
sample stepping, zoom/pan hit testing and board clearing for non-path states.
Canvas draw-operation checks verify connected frames for matching path prefixes,
single labels on continuous runs, nearest-sample prefix replay within a frame,
breaks at waits and long sampling gaps, visible move text at 20ms spacing, fixed
headers during deep-path scrolling, waiting backgrounds confined to the status
band, accurate prefix clicks, readable-window controls, and a separate collapsed
thread overview. Gesture checks cover horizontal trackpad panning, Shift-wheel
and Shift-drag panning, time boundaries, pixel/line/page wheel units, gradual
pointer-anchored zoom and small reciprocal zoom-button steps. Vertical scrolling
is distinguished from horizontal panning and modified-wheel zoom.
Overlapping chunk timestamps and invalid-clock events use per-thread sequence
history, so older events cannot recover unknown states or hide a later wait.
The UI tests use a simulated Canvas/DOM; actual Chrome checks are still required
for pixels, layout, zoom gestures and browser file access. All five viewer test
suites are registered with CTest when Node.js is available.

The collector validator can also inspect the native driver's most recent run:

```sh
build/xiangqi_timeline_collector_tests build/timeline_validation/integration
python3 tests/test_timeline_collector.py build/timeline_validation/integration
build/xiangqi_timeline_collector_overflow_tests build/timeline_validation/overflow overflow
python3 tests/test_timeline_collector.py build/timeline_validation/overflow
```

The driver has the form `[report_directory] [overflow]`. Without `overflow` it runs
the seven real-search combinations plus a synthetic clock/ordering scenario; with
`overflow` it runs only the short queue/backpressure scenario. The validator uses `collector-cases.json`
to select reports from that exact process, so older reports do not affect it.
