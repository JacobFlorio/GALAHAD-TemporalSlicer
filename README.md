# GALAHAD-TemporalSlicer

A unified temporal reasoning core for AI systems. C++20. In-memory. Research-grade.

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
engine/    TemporalEngine  — high-level reasoning: explain(), whatHappenedDuring(), knobs
core/      TemporalCore    — the substrate: bitemporal events, causal DAG, Allen algebra,
                             branching projections, lifecycle ops, auto-maintained indices
adapters/  (planned)       — bindings to LLMs, agents, robotics, persistence
```

`TemporalCore` is a self-contained C++ library. `TemporalEngine` wraps it with
higher-level reasoning primitives and is the intended consumer API. Adapters will
translate between GALAHAD's model and whatever the external system speaks.

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
- Two test binaries: correctness stress at 10k events and 100-branch isolation

**Not yet:**
- Persistence (on-disk, mmap, serialization)
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
./build/bench_temporal
```

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
- Real benchmark harness with workloads: 1M-event insert/query, `explain` at depth 1000,
  mixed Allen+causal queries at 100k events, 100-branch projection scaling
- Topological sort in `explain` for general DAGs
- First adapter: thin wrapper exposing `explain` and `whatHappenedDuring` to an LLM
  tool-call surface

**Medium term**
- Persistence layer (mmap-friendly serialization of the event deque + pools)
- Counterfactual queries (`whyNot`, hypothetical mutation)
- Confidence propagation through causal chains
- Python bindings

**Longer term**
- Concurrent readers / writer-exclusion
- Interval-tree time index for true logarithmic range queries on long-lived events
- Hierarchical branches ("close_door.option_A" nested under "close_door")
- First robotics integration — plug into an active agent loop as the belief-and-plan
  substrate

## License

TBD — this is a research project in active development.

## Project scope

This is a one-author research project. Contributions, discussion, and use cases are
welcome but there is no formal contribution process yet. If you are building a system
that needs temporal reasoning and this is interesting to you, open an issue describing
the use case — the API is young enough that concrete workloads can still shape it.
