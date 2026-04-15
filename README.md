# GALAHAD-TemporalSlicer

A unified temporal reasoning core for AI systems: bitemporal storage, causal DAG,
Allen interval algebra, branching projections, lifecycle, **and a first-class LLM
tool-call adapter**. C++20. In-memory. Research-grade.

GALAHAD exists because time is the weakest part of most AI systems. LLMs hallucinate
dates. Robots conflate "now" with "last observed." Planners treat the future as an
untyped blob. Bitemporal databases can tell you what was known when, but they cannot
reason about causation. Causal graph libraries can trace "why" but do not understand
intervals. Allen's interval algebra can say "during" but cannot branch into competing
futures. **There is no open-source project that unifies all four.** GALAHAD is that
unification.

Long-term target: the temporal substrate for serious AI — LLM tool-call surfaces,
robotics world models, simulation and planning systems, agent reasoning loops. Time
becomes a first-class capability, not an afterthought.

## The three questions GALAHAD answers

Every interesting temporal question an AI asks reduces to one of three shapes:

1. **"Why did that happen?"** — causal ancestry with Allen-relation constraints,
   filtered by what the system *could have known* at the time.
2. **"What did the system believe at that moment?"** — bitemporal as-of queries over
   events, separating valid time (when the event was true in the world) from
   transaction time (when the system learned of it).
3. **"What should happen next?"** — branching projections. Multiple competing futures
   coexist in the same store without contaminating ground truth, each with its own
   confidence, each promotable to reality when observation confirms it.

A single engine answers all three. The same event store holds ground truth and
hypotheticals. The same query path filters by as-of time, causal graph position, and
branch identity. This is the structural moat.

## Architecture

Three layers, bottom-up:

```
adapters/  LLMToolAdapter  — JSON tool-call surface for any LLM framework
engine/    TemporalEngine  — high-level reasoning: explain(), whatHappenedDuring(), knobs
core/      TemporalCore    — the substrate: bitemporal events, causal DAG, Allen algebra,
                             branching projections, lifecycle ops, auto-maintained indices
persistence/ (in design)   — on-disk format, in-progress next milestone
```

`TemporalCore` is a self-contained C++ library. `TemporalEngine` wraps it with
higher-level reasoning primitives and is the intended consumer API. `LLMToolAdapter`
exposes the reasoning surface as a vendor-neutral JSON tool-call API so any agent
framework (Anthropic, OpenAI, LangChain, custom loops) can reach for GALAHAD with
zero custom glue.

### What's in the core

- **Bitemporal event model.** Every event has `valid_from`/`valid_to` (world time) and
  `recorded_at` (transaction time). Queries can ask "what was true at time T" and
  "what did the system *know* at time T" independently.
- **Causal DAG.** Events cite their causes via `causal_links`. The core auto-maintains
  both the forward causal graph and its reverse, detects cycles, and exposes
  transitive closure (`getAncestors`, `getDescendants`) and direct-edge queries
  (`getCauses`, `getEffects`).
- **Allen's 13 interval relations** as first-class predicates. `getAllenRelation(a,b)`
  returns one of `Precedes`, `Meets`, `Overlaps`, `FinishedBy`, `Contains`, `Starts`,
  `Equals`, `StartedBy`, `During`, `Finishes`, `OverlappedBy`, `MetBy`, `PrecededBy`.
  `findRelated(id, relation)` returns every event standing in that relation to a
  source. Dispatches on the relation family to walk only the relevant slice of the
  time index.
- **Branching projections.** `addProjection(event)` stores a hypothetical on a named
  branch. Queries filter by branch: `nullopt` = all non-refuted branches, `"main"` =
  ground truth only, `"X"` = branch X only. Branches are fully isolated — two
  competing futures cannot see each other.
- **Branch lifecycle.** `promoteBranch(name)` appends main-branch copies of every
  event on that branch with a monotonic `recorded_at`, preserving the originals so
  as-of queries before the promotion still see the projection form. `refuteBranch`
  marks a branch as falsified without destroying its events (queryable by explicit
  name, hidden from default queries). `pruneBranch` destructively removes events on
  a branch. Main branch is protected from promote/refute/prune.
- **Monotonic transaction time.** `TemporalCore::now()` returns a strictly-increasing
  `TimePoint` even under burst calls. `computePromoteTime()` additionally guarantees
  the new timestamp exceeds every existing event's `recorded_at` so bitemporal
  ordering stays consistent through lifecycle operations.

### What's in the engine

- **`explain(id)`** — full causal ancestry of an event, returned as `vector<TemporalEvent>`
  sorted by `valid_from`. Includes a `require_completed_before` knob (default `true`)
  that excludes ancestors whose `valid_to` extends past the target's `valid_from`.
  This lets callers choose between "causal history" (completed causes) and "causal
  context" (ongoing conditions). Fully bitemporal: `as_of` parameter replays the
  system's past belief state.
- **`whatHappenedDuring(window)`** — events overlapping a time window, branch-filtered,
  returned as full `TemporalEvent` objects.

### What's in the adapter

- **`LLMToolAdapter`** — a vendor-neutral JSON tool-call surface over the full
  reasoning API. Two entry points:
  - **`getToolSchemas()`** returns an Anthropic-style array of
    `{name, description, input_schema}` descriptors any LLM framework can register.
    Seventeen tools: `now`, `add_event`, `add_projection`, `get_event`, `query_range`,
    `explain`, `what_happened_during`, `get_ancestors`, `get_descendants`,
    `get_causes`, `get_effects`, `find_related`, `promote_branch`, `refute_branch`,
    `prune_branch`, `is_refuted`, and `list_tools`.
  - **`handleToolCall(name, args)`** dispatches and returns a structured envelope:
    `{"ok": true, "result": ...}` on success or `{"ok": false, "error": "..."}` on
    failure. Every exception path is caught and surfaced as JSON — the LLM never
    sees a crash.
- **Timestamp round-tripping.** Every result carries both ISO 8601 UTC strings
  (`"2026-04-14T12:00:00.000Z"`) and int64 nanoseconds since epoch. Inputs accept
  either. LLMs read ISO fluently; the int64 form guarantees exact round-trip for
  agent tool-chaining.
- **Allen relation names** are exposed as snake_case strings (`precedes`, `meets`,
  `during`, `met_by`, etc.) so a model reaches for them by their natural names.
- **Dependency:** `nlohmann/json v3.11.3`, pulled via CMake `FetchContent`. Zero
  manual setup.

## Quick start

```cpp
#include "temporal_core.h"
#include "temporal_engine.h"

using namespace galahad;
using namespace std::chrono;

int main() {
    TemporalCore core;
    TemporalEngine engine(core);
    const auto t0 = Clock::now();
    auto ms = [](int n) { return milliseconds(n); };

    // Ground truth: an agent perceives, infers, decides, acts.
    core.addEvent({"perceive", t0,        t0+ms(5),  t0,
                   "perception", {{"obs","door_open"}}, {}});
    core.addEvent({"infer",    t0+ms(5),  t0+ms(15), t0+ms(5),
                   "inference", {{"why","wind"}}, {"perceive"}});
    core.addEvent({"decide",   t0+ms(15), t0+ms(20), t0+ms(15),
                   "decision",  {{"choose","close"}}, {"infer"}});
    core.addEvent({"act",      t0+ms(20), t0+ms(30), t0+ms(200),
                   "action",    {{"do","close"}}, {"decide"}});

    // "Why did the agent act?"
    auto why = engine.explain("act");
    // why.causes == [perceive, infer, decide] in temporal order

    // "What did the system believe at t0+100ms?" The action was only
    // recorded at t0+200ms, so it is invisible to this past query.
    auto past = core.queryRange({t0, t0+ms(500)}, t0+ms(100));
    // past contains perceive, infer, decide — but not act

    // "What should happen next?" Two competing projected futures.
    TemporalEvent futClose;
    futClose.id = "fut_close";
    futClose.valid_from = t0+ms(30);
    futClose.valid_to   = t0+ms(40);
    futClose.recorded_at = t0+ms(20);
    futClose.type = "projected_action";
    futClose.causal_links = {"act"};
    futClose.branch_id = "close_door";
    futClose.confidence = 0.7;
    core.addProjection(futClose);

    TemporalEvent futIgnore = futClose;
    futIgnore.id = "fut_ignore";
    futIgnore.branch_id = "ignore";
    futIgnore.confidence = 0.3;
    core.addProjection(futIgnore);

    // Branch-scoped query: only the "close_door" future.
    auto close_only = core.queryRange(
        {t0, t0+ms(500)}, std::nullopt,
        std::optional<std::string>{"close_door"});

    // Observation confirms one future. Promote it to ground truth and
    // refute the alternative.
    core.promoteBranch("close_door");
    core.refuteBranch("ignore");

    // Bitemporal honesty preserved: as-of before the promotion, the
    // system's past belief still sees the projection form, not the
    // promoted main-branch copy.
}
```

## LLM integration

Any tool-calling LLM framework can drive GALAHAD with a few lines of glue. The
adapter speaks vendor-neutral JSON, so the same code wires up to Anthropic SDK
tool_use, OpenAI function calling, LangChain, or a custom agent loop.

```cpp
#include "llm_tool_adapter.h"
using namespace galahad;
using nlohmann::json;

TemporalCore core;
TemporalEngine engine(core);
LLMToolAdapter adapter(core, engine);

// 1. Register tools with your LLM framework.
json schemas = adapter.getToolSchemas();
// schemas is an array of 17 {name, description, input_schema} objects.
// Hand it to your framework's tool registry.

// 2. Dispatch the LLM's tool calls.
//    When the model picks a tool, forward the JSON args:
json result = adapter.handleToolCall(
    "explain",
    json{{"id", "act"}}
);
// result == {"ok": true, "result": {"causes": [...], "completed_before_target": true}}
```

An agent loop using GALAHAD walks the full perceive → project → observe → reconcile
cycle through tool calls alone:

1. `now` — get a monotonic transaction-time anchor
2. `add_event` — record a perception with causal links and metadata
3. `explain` — ask the core *why* a downstream event happened, read the chain back
4. `add_projection` — stash a hypothetical future on a named branch with confidence
5. `query_range` with `branch: "<name>"` — inspect a specific projected future
6. `promote_branch` / `refute_branch` — reconcile projection with observation
7. Ask `explain` again with `as_of: <earlier time>` — replay the system's past
   belief state, honestly

No custom C++ integration, no bespoke serialization, no timestamp hallucination.
That full loop did not exist in any open-source project before GALAHAD.

## API surface

### Mutation

```cpp
void addEvent(TemporalEvent e);                  // ground truth
void addProjection(TemporalEvent e);             // hypothetical, non-main branch
void promoteBranch(const std::string& branch);   // projection -> ground truth
void refuteBranch(const std::string& branch);    // mark as falsified (non-destructive)
void pruneBranch(const std::string& branch);     // destructive removal
```

### Query — bitemporal

```cpp
std::optional<TemporalEvent> get(const std::string& id,
                                 std::optional<TimePoint> as_of = {},
                                 std::optional<std::string> branch = {}) const;

std::vector<TemporalEvent> queryRange(TimeWindow window,
                                      std::optional<TimePoint> as_of = {},
                                      std::optional<std::string> branch = {}) const;
```

### Query — causal

```cpp
std::vector<std::string> getCauses(const std::string& id, ...);      // direct parents
std::vector<std::string> getEffects(const std::string& id, ...);     // direct children
std::vector<std::string> getAncestors(const std::string& id, ...);   // transitive
std::vector<std::string> getDescendants(const std::string& id, ...); // transitive
bool hasCycle() const;
```

### Query — Allen

```cpp
AllenRelation getAllenRelation(const TemporalEvent& a, const TemporalEvent& b) const;
bool holds(AllenRelation r, const TemporalEvent& a, const TemporalEvent& b) const;
std::vector<std::string> findRelated(const std::string& id,
                                     AllenRelation r,
                                     std::optional<TimePoint> as_of = {},
                                     std::optional<std::string> branch = {}) const;
```

### Engine

```cpp
struct Explanation {
    std::vector<TemporalEvent> causes;     // sorted by valid_from
    bool completed_before_target = true;
};

Explanation explain(const std::string& id,
                    std::optional<TimePoint> as_of = {},
                    bool require_completed_before = true,
                    std::optional<std::string> branch = {}) const;

std::vector<TemporalEvent> whatHappenedDuring(TimeWindow window,
                                              std::optional<TimePoint> as_of = {},
                                              std::optional<std::string> branch = {}) const;
```

## Current status

**This is a v0.1 research core.** It is not a database, not persistent, not yet
concurrent, not yet benchmarked against production systems. It is a complete, correct,
internally-optimized skeleton that answers every question in the unified model.

**Done:**
- Bitemporal event model with valid + transaction time
- Causal DAG with cycle detection, transitive closure, auto-maintained forward +
  reverse indices
- Allen's 13 interval relations + `findRelated` dispatched per relation family
- Branching projections with full isolation
- Branch lifecycle: promote (bitemporally honest), refute (non-destructive), prune
  (destructive). Main branch protected.
- Monotonic transaction time clock
- Engine layer with `explain`, `whatHappenedDuring`, bitemporal honesty, causal
  ordering, `completed_before` knob
- uint32-interned event and branch ids — all hot-path comparisons are integer-only
- Per-branch time indices for fast range and Allen queries on any single branch
- Flat, sorted-vector event data field — one allocation per event's metadata
- Lazy-rebuilt indices behind dirty flags; no stale-graph footguns
- **LLM tool-call adapter: 17 JSON tools, getToolSchemas + handleToolCall, ISO-8601
  and int64 timestamp round-trip, Allen relations as snake_case strings, structured
  error envelopes. Vendor-neutral.**
- Three test binaries: correctness stress at 10k events, 100-branch isolation,
  full adapter round-trip

**In progress:**
- Persistence (design starting; initial skeleton in `persistence/`)

**Not yet:**
- Concurrency (single-threaded)
- Real benchmark harness with published numbers
- Topological sort in `explain` (currently sorts by `valid_from`; equivalent for
  DAGs that respect causal-temporal ordering, not general)
- Counterfactual queries (`whyNot`, hypothetical mutation)
- Confidence propagation (stored, not composed)
- Interval-tree time index (current sorted-vector is fine until bench says otherwise)
- Nested/hierarchical branches
- Language bindings (Python, FFI)

**Benchmarks:** a placeholder harness exists at `bench/bench_temporal`. It does not
yet publish numbers. Real benchmarks will land after workload design; meanwhile the
correctness tests stress the time index at 10k events and the branch indices at
100 branches × 100 events.

## Build

```bash
cmake -B build
cmake --build build -j
./build/test_temporal
./build/test_engine
./build/test_adapter
./build/bench_temporal
```

The first `cmake -B build` pulls `nlohmann/json` via `FetchContent` (pinned, shallow).
Subsequent configures are cache hits.

Requires CMake 3.20+ and a C++20 compiler.

## Design notes

- **Bitemporal first.** Every event carries both world-time (`valid_from`/`valid_to`)
  and transaction-time (`recorded_at`) from day one. This is the single most important
  modeling choice in the project. Without it, causal reasoning leaks foreknowledge
  from the future and projections cannot be replayed honestly.
- **Unified query path.** `as_of`, branch, and Allen relations all compose. You can
  ask "events during window W that are ancestors of X, on branch Y, as the system knew
  them at time T." No special-case code paths — every query method takes the same
  filters.
- **No auto-magic.** Lifecycle operations are explicit: promoting one branch does not
  auto-refute its siblings. If the caller wants both, they make two calls. Predictable
  beats convenient.
- **Branch isolation by construction.** Branch filters gate both traversal and
  inclusion in causal queries, and the per-branch time indices make branch-scoped
  reads structurally fast, not just filtered-fast. Two projected futures referencing
  the same main-branch cause cannot see each other, even though they share that cause.
- **Path B interning.** Public API uses human-readable strings. Internal storage uses
  uint32 handles with string-pool round-trip on read. The public ergonomics are
  preserved, the hot path is pointer-equality fast, and future bindings (Python,
  FFI) will not need to re-invent the translation layer.
- **Lazy index materialization.** Time indices, branch time indices, and causal graphs
  are all rebuilt on first read after mutation. Bulk inserts amortize to one rebuild
  across the batch. Callers never have to call `buildCausalGraph()` explicitly; it
  still exists for backward compatibility but is a no-op in the steady state.

## What makes GALAHAD unique

There are excellent projects in each of the adjacent categories:

- **Bitemporal DBs** (XTDB, SirixDB, BarbelHisto) — great at as-of queries, but have no
  notion of causation or interval algebra, and no branching projections.
- **Causal / temporal reasoning libraries** (PyReason, ETIA, tgLib) — great at graphs
  and relations, but are not bitemporal and treat the future as uniform with the past.
- **Allen interval algebra libraries** — standalone implementations of the 13
  relations, but without an event store or causal model.
- **Planning and simulation engines** — handle branching futures, but as a separate
  concern from observation and belief update.

GALAHAD's goal is to be the **one substrate** where all of these compose. A query
like "find every event that is a causal ancestor of X, happened *during* Y, was known
at time T, and lives on branch B" is one line of code here and is not directly
expressible in any single library in the categories above.

## Roadmap

**Near term**
- Persistence: append-only log + mmap sidecar, crash-safe replay on startup
- Real benchmark harness with workloads: 1M-event insert/query, `explain` at depth 1000,
  mixed Allen+causal queries at 100k events, 100-branch projection scaling
- Topological sort in `explain` for general DAGs

**Medium term**
- Counterfactual queries (`whyNot`, hypothetical mutation)
- Confidence propagation through causal chains
- Python bindings (pybind11) on top of the existing adapter layer
- Framework-specific integrations (`galahad-anthropic-agent`, `galahad-langchain`)

**Longer term**
- Concurrent readers / writer-exclusion
- Interval-tree time index for true logarithmic range queries on long-lived events
- Hierarchical branches ("close_door.option_A" nested under "close_door")
- First robotics integration — plug into an active agent loop as the belief-and-plan
  substrate

## License

MIT. See [LICENSE](LICENSE). Copyright (c) 2026 Jacob T. Florio.

Third-party dependency: [nlohmann/json](https://github.com/nlohmann/json) v3.11.3,
pulled via CMake `FetchContent`, also MIT-licensed.

## Project scope

This is a one-author research project. Contributions, discussion, and use cases are
welcome but there is no formal contribution process yet. If you are building a system
that needs temporal reasoning and this is interesting to you, open an issue describing
the use case — the API is young enough that concrete workloads can still shape it.
