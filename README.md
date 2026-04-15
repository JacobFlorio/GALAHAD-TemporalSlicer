# GALAHAD-TemporalSlicer

[![CI](https://github.com/JacobFlorio/GALAHAD-TemporalSlicer/actions/workflows/ci.yml/badge.svg)](https://github.com/JacobFlorio/GALAHAD-TemporalSlicer/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/galahad-temporal.svg)](https://pypi.org/project/galahad-temporal/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A unified temporal reasoning core for AI systems: bitemporal storage, causal DAG,
Allen interval algebra, branching projections, lifecycle, a first-class LLM
tool-call adapter, binary persistence, **and Python bindings**. C++20 core,
usable from C++ or Python. Research-grade.

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
python/      galahad             — pybind11 bindings: full C++ surface from Python
adapters/    LLMToolAdapter      — JSON tool-call surface for any LLM framework
persistence/ TemporalPersistence — binary save/load with full round-trip
engine/      TemporalEngine      — high-level reasoning: explain(), whatHappenedDuring(), knobs
core/        TemporalCore        — the substrate: bitemporal events, causal DAG, Allen algebra,
                                    branching projections, lifecycle ops, auto-maintained indices
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
- **`explainWith(target, mutation)`** — hypothetical explain: clones the core,
  applies a mutation, then runs `explain` on the fork without touching the
  original. Ask "if this event had happened, what would its causal
  explanation look like?" (see Counterfactual queries below).

### Counterfactual queries (new in 0.2.0)

GALAHAD is the only OSS temporal engine with first-class counterfactual
primitives because it already has what they require: an event-level
refutation set and bitemporal semantics. Two new operations expose this:

- **`TemporalCore::whyNot(id)`** — "why did this event NOT happen?"
  Returns one entry per refuted projection branch that contained a
  prediction of the given event id. Each entry includes the refuted
  branch name, the predicted event (with its metadata and confidence),
  and the would-have-been causal ancestry walked across all branches so
  you see the full hypothetical chain — not just the subset that
  survived refutation. Empty if the event actually happened on main,
  if no prediction of it was ever made, or if every predicting branch
  was promoted rather than refuted.

- **`TemporalEngine::explainWith(target, mutation)`** — hypothetical
  explain. Clone the core, apply the mutation as a fresh `addEvent`,
  run `explain(target)` on the fork. Original core untouched. Use this
  to probe causal hypotheses without committing them — "if I had added
  this event, what would its ancestry have been?"

- **`TemporalCore::clone()`** — the enabling primitive. Deep copies the
  full core state (events, indices, pools, refutation set, clock)
  into an independent `TemporalCore`. All members are value types, so
  the implicit copy constructor does a true deep copy with zero shared
  state. `explainWith` is a thin wrapper on top of this.

The LLM adapter exposes two new JSON tools for counterfactuals:
**`why_not`** and **`explain_with`**, bringing the adapter's vendor-
neutral tool count to 19. An agent can now ask "what refuted branches
predicted this event?" and "what would the causal chain look like if
this mutation had happened?" with one tool call each, in addition to
all the existing forward queries.

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

## Python

`pybind11` bindings expose the full C++ surface — `TemporalCore`,
`TemporalEngine`, `LLMToolAdapter`, `TemporalPersistence`, plus the value
types `TemporalEvent`, `TimeWindow`, `AllenRelation`, `Explanation` — with
idiomatic Python ergonomics (snake_case methods, Python `datetime` for time
points, `dict` for event data, `None` for optional arguments, `list[dict]`
returned from the adapter).

**Install from PyPI:**

```bash
pip install galahad-temporal                  # or: pip install "galahad-temporal[anthropic]"
python -c "import galahad; print(galahad.__version__)"
```

Live at https://pypi.org/project/galahad-temporal/ . Current release is
**`0.2.2`**, and `pip install galahad-temporal` fetches a **prebuilt
wheel** (no compile step) on Linux x86_64 `manylinux_2_28` and macOS
arm64, for CPython 3.8 through 3.12. Other platforms fall through to
the sdist and compile from source (about a minute). Or install straight
from the repo:

```bash
git clone https://github.com/JacobFlorio/GALAHAD-TemporalSlicer
cd GALAHAD-TemporalSlicer
pip install .                     # or: pip install .[anthropic]
python -c "import galahad; print(galahad.__version__)"
```

`pip install .` uses [`scikit-build-core`](https://scikit-build-core.readthedocs.io/)
with `pybind11` as a build-system dependency. It compiles a single extension
module (`galahad.*.so`) and installs it directly — no `PYTHONPATH` dance, no
test binaries pulled into the wheel, no bench harness compiled. The install
step takes roughly a minute on a commodity machine (FetchContent pulls
`nlohmann/json` once; `pybind11` comes from the build env).

The optional `[anthropic]` extra installs the Anthropic SDK so
`examples/anthropic_demo.py` runs end-to-end.

You can still build everything (C++ tests, bench, the Python module,
adapter/persistence) from the repo without pip:

```bash
cmake -B build && cmake --build build -j
PYTHONPATH=build python3 python/test_galahad.py
```

Quick-start mirror of the C++ example above:

```python
from datetime import datetime, timedelta, timezone
import galahad

core = galahad.TemporalCore()
engine = galahad.TemporalEngine(core)
t0 = datetime.now(timezone.utc)

def mk(id_, start, end, recorded, type_, data=None, links=None, branch="main"):
    e = galahad.TemporalEvent()
    e.id, e.valid_from, e.valid_to, e.recorded_at = id_, start, end, recorded
    e.type, e.data, e.causal_links, e.branch_id = type_, data or {}, links or [], branch
    return e

ms = lambda n: timedelta(milliseconds=n)
core.add_event(mk("perceive", t0,         t0+ms(5),  t0,        "perception", {"obs": "door_open"}))
core.add_event(mk("infer",    t0+ms(5),   t0+ms(15), t0+ms(5),  "inference",  {"why": "wind"}, ["perceive"]))
core.add_event(mk("decide",   t0+ms(15),  t0+ms(20), t0+ms(15), "decision",   {"choose": "close"}, ["infer"]))
core.add_event(mk("act",      t0+ms(20),  t0+ms(30), t0+ms(200),"action",     {"do": "close"}, ["decide"]))

# Why did the agent act?
why = engine.explain("act")
print([c.id for c in why.causes])   # ['perceive', 'infer', 'decide']

# What did the system believe at t0+100ms?
past = engine.explain("act", t0 + ms(100))
assert len(past.causes) == 0  # the action had not been recorded yet

# Drive the full agent surface via the LLM adapter from Python:
adapter = galahad.LLMToolAdapter(core, engine)
schemas = adapter.get_tool_schemas()          # list of dicts: register with any LLM framework
result = adapter.handle_tool_call("explain", {"id": "act"})
assert result["ok"] and len(result["result"]["causes"]) == 3

# Persist state across runs:
galahad.TemporalPersistence(core).save("state.gtp")
restored = galahad.TemporalCore()
galahad.TemporalPersistence(restored).load("state.gtp")
```

Lifetime is handled automatically — `TemporalEngine`, `LLMToolAdapter`, and
`TemporalPersistence` each pin their backing `TemporalCore` (and each other
where relevant) via `py::keep_alive`, so the Python garbage collector will
not reclaim a core out from under a derived object. Pass a `TemporalCore`
to a dozen wrappers and they will all keep it alive until the last one is
dropped.

`python/test_galahad.py` exercises the full surface end-to-end: causal
chain, bitemporal as-of replay, Allen relations, branching projections,
refute + explicit-name override, LLM tool-call round-trip (including the
`nlohmann::json` ↔ Python `dict` caster), and persistence save/load with
state preservation. Run with
`PYTHONPATH=build python3 python/test_galahad.py`.

### Examples: Claude reasoning through GALAHAD

Two end-to-end demo scripts wire the adapter straight into the
Anthropic SDK. Both support a dry-run mode that exercises the full
GALAHAD surface without calling the API — set `GALAHAD_DEMO_DRY_RUN=1`
or simply omit the `ANTHROPIC_API_KEY` environment variable to use it.

**`examples/anthropic_demo.py`** — the counterfactual demo. Pre-populates
a perceive → infer → decide → act chain with a late-arriving action
plus two competing projection branches (`close_door` confidence 0.7,
`ignore` confidence 0.3, the latter refuted). Asks Claude four question
shapes in one prompt: backward causal (`explain`), bitemporal replay
(`explain` with `as_of`), counterfactual (`why_not`), and hypothetical
mutation (`explain_with`). Claude handles the four questions in four
turns by firing tool calls in parallel across independent questions.

**`examples/traffic_agent_demo.py`** — the richer 6-tick pedestrian-at-
crosswalk scenario. Pre-populates a full projection lifecycle across
6 ticks: three competing plans at tick 1, refutation of the risky ones
at tick 2, a new premature-start projection at tick 3, promotion of
`wait_for_green` + refutation of `start_early` at tick 4, promotion of
`walk_across` at tick 5, crossing complete at tick 6. Asks six
introspection questions that span the entire adapter surface including
`get_refuted_branches`, `get_projections`, and the full counterfactual
path.

Claude is handed all GALAHAD tools via `get_tool_schemas()` and runs
an agent loop: for each `tool_use` in the response, the script calls
`adapter.handle_tool_call(name, args)`, serializes the JSON result
back as a `tool_result` block, and continues until Claude emits
`end_turn`.

```bash
# Dry run (no API key needed): exercises the adapter + Python path
# end-to-end with smoke assertions on explain, as_of replay,
# find_related, why_not, explain_with, and the introspection surface.
python3 examples/anthropic_demo.py           # counterfactual demo
python3 examples/traffic_agent_demo.py       # 6-tick traffic agent demo

# Real run:
pip install anthropic
export ANTHROPIC_API_KEY=sk-...
PYTHONPATH=build python3 examples/anthropic_demo.py
```

The dry-run mode is the important part for CI and for anyone who
wants to verify the demo works before spending tokens on it — no API
key needed, prints the smoke assertions, exits cleanly.

**Verbatim transcripts** of both demos running live against real
PyPI releases are committed under
[`examples/transcripts/`](examples/transcripts/). They are the
single most concrete evidence that GALAHAD's reasoning surface works
with a production LLM on the other end: Claude Opus 4.6 actually
walked the backward causal, bitemporal replay, counterfactual, and
hypothetical question shapes in one session, invented a
discovery-then-query design pattern on the spot, computed a new
metric called **promotion latency** (the delay between a projection
being recorded and being promoted to ground truth), and surfaced a
latent duplicate-id bug in `getEffects` that was subsequently fixed
in v0.2.2. Start with
[`counterfactual_demo_transcript.md`](examples/transcripts/counterfactual_demo_transcript.md)
for the short 4-turn run, then
[`traffic_agent_demo_transcript.md`](examples/transcripts/traffic_agent_demo_transcript.md)
for the longer 6-question decision-tree reconstruction with an ASCII
diagram.

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

Explanation explainWith(const std::string& target_id,
                        const TemporalEvent& mutation,
                        std::optional<TimePoint> as_of = {},
                        bool require_completed_before = true,
                        std::optional<std::string> branch = {}) const;
```

### Counterfactuals

```cpp
struct CounterfactualExplanation {
    std::string branch;
    TemporalEvent hypothetical_event;
    std::vector<TemporalEvent> would_have_been_causes;
};

TemporalCore TemporalCore::clone() const;
std::vector<CounterfactualExplanation>
    TemporalCore::whyNot(const std::string& id) const;
```

## Current status

**v0.2.2 — research core, feature-complete for the four canonical
question shapes (backward causal, bitemporal replay, counterfactual,
hypothetical mutation), live on PyPI with prebuilt wheels, validated
in two separate live-LLM runs committed as transcripts.** Not yet a
database, not persistent across process lifetimes with an append-only
log (only full snapshots today), not concurrent, not ported to
Windows. The forward path is documented in the roadmap below.

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
- **LLM tool-call adapter: 22 vendor-neutral JSON tools
  (`getToolSchemas` + `handleToolCall`), ISO-8601 and int64 timestamp
  round-trip, Allen relations as snake_case strings, structured
  error envelopes. Every tool is documented with an Anthropic-style
  `input_schema` so the LLM picks the right tool from the description
  alone.**
- **Counterfactual query primitives: `whyNot(id)` returns refuted
  projection branches with their would-have-been causal chains;
  `explainWith(target, mutation)` clones the core, applies a
  hypothetical, and explains the target in the fork without touching
  the real store. Exposed as both C++ + Python methods and as the
  `why_not` / `explain_with` JSON tools.**
- **Persistence: single-file binary format, full save/load round-trip.
  Events, causal links, branches, refutations, and bitemporal ordering
  all survive the round-trip losslessly. Load bumps the monotonic clock
  so subsequent writes can't regress before loaded events.**
- **Python bindings via pybind11: the full C++ surface exposed as
  idiomatic Python (snake_case methods, `datetime` time points, `dict`
  event data, automatic lifetime management via `keep_alive`). Custom
  timezone-aware time_point caster rejects naive datetimes and
  round-trips UTC via `.timestamp()`. Inline `nlohmann::json` ↔ Python
  dict/list type caster so the LLM adapter round-trips JSON naturally.**
- **PyPI distribution: `pip install galahad-temporal` fetches a
  prebuilt wheel on Linux x86_64 and macOS arm64 for CPython 3.8–3.12.
  Fully automated release pipeline via GitHub Actions + cibuildwheel +
  trusted publishing on every `v*` tag push.**
- **Live LLM transcripts in
  [`examples/transcripts/`](examples/transcripts/) — verbatim captures
  of Claude Opus 4.6 running the demos against live PyPI releases.**
- Four C++ test binaries plus one Python test: 10k-event correctness
  stress, 100-branch isolation, counterfactual + promotion-dedupe
  regression, adapter JSON round-trip, persistence round-trip, and the
  full Python surface end-to-end.

**Not yet:**
- Windows wheels (the adapter uses POSIX `strptime`/`gmtime_r`/`timegm`;
  next focused session after the current stopping point)
- Incremental/append-only persistence (current v1 format writes full
  snapshots)
- Concurrency (single-threaded)
- Topological sort in `explain` for general DAGs (current sort by
  `valid_from` is equivalent only for DAGs that respect causal-temporal
  ordering)
- Confidence propagation (stored, not composed)
- Interval-tree time index (current sorted-vector is fine until bench
  says otherwise)
- Nested/hierarchical branches
- Non-Python language bindings (direct C API, Rust, etc.)

**Benchmarks:** `bench/bench_temporal` runs seven workloads end-to-end.
See the Performance section below. Numbers are honest — single-threaded,
single machine, Release build, sanity-checked on every run.

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

Requires CMake 3.20+ and a C++20 compiler. The project defaults to Release
when you don't pick a build type; Debug still works with
`cmake -B build -DCMAKE_BUILD_TYPE=Debug`.

## Performance

Numbers from `./build/bench_temporal` on a single-threaded commodity machine,
Release build (`-O3`). The harness warms every index before measuring and
sanity-checks every result. Reproduce with:

```bash
cmake -B build && cmake --build build -j && ./build/bench_temporal
```

| Workload                               |     p50 |      p99 | Notes                                       |
| -------------------------------------- | ------: | -------: | ------------------------------------------- |
| `addEvent` bulk (1M events)            |  749 ns |        — | ~1.33M events/sec                           |
| `get()` over 100k events               |  640 ns |   1.2 µs |                                             |
| `queryRange` 100 µs window / 100k      |   92 µs |  300 µs  | ~100 events materialized per call           |
| `queryRange` all 100 branches (100k)   | 12.4 ms |  21.1 ms | full scan + materialize                     |
| `queryRange` one branch (1k of 100k)   |   33 µs |   66 µs  | **~380× faster — per-branch time index**    |
| `explain()` at causal depth 1000       |  135 µs |  176 µs  | 1000-step ancestry with full reconstruction |
| `findRelated(Overlaps)` / 100k         |   89 µs |  379 µs  | family dispatch, walks only overlapping slice |
| `findRelated(Meets)` / 100k            |  119 µs |  459 µs  |                                             |
| `findRelated(Precedes)` / 100k         |  435 µs |  647 µs  | ~50k results, cost linear in result size    |
| `findRelated(PrecededBy)` / 100k       |  437 µs |  861 µs  |                                             |
| `save` 100k events (binary v1)         |   25 ms |        — | ~4M events/sec, 75 bytes/event              |
| `load` 100k events (binary v1)         |   48 ms |        — | ~2M events/sec, full reindex on load        |

**What these numbers mean:**

- **Branch isolation is not a filter, it is a structural skip.** Asking
  "what happened on branch X" takes ~33 µs because the per-branch time
  index iterates only that branch's 1000 events. The same window across all
  100 branches takes 12.4 ms because it materializes all 100k. This is the
  difference between "branch filter applied at query time" and
  "branch-scoped index consulted by name."
- **`explain()` at depth 1000 is 135 µs** — ~135 ns per ancestor for full
  causal traversal plus materialization. An agent can answer "why did this
  happen" over a thousand-step chain faster than a network round-trip.
- **`findRelated` algorithmic cost scales with the result set, not the
  corpus.** Selective relations (`Meets`, `Overlaps`, a handful of results)
  are an order of magnitude faster than scan-everything relations
  (`Precedes`, `PrecededBy`, tens of thousands of results). The time index
  dispatch walks only the relevant slice; final cost is dominated by
  `resolveEventId` per result.
- **Persistence v1 is full-snapshot only.** Save is pure serialization
  (4M events/s); load is intern + reindex (2M events/s) because it runs
  every event through the normal `addEvent` path. An append-only incremental
  format is on the near-term roadmap.

The bench harness is single-threaded and the core is single-threaded;
concurrent readers and a real interval-tree time index are not v0.1 goals.
Nothing in this section is compared against other systems yet — see the
"What makes GALAHAD unique" section for why comparative benchmarks against
XTDB / PyReason / etc. are the wrong frame. Numbers here are
ourselves-against-ourselves, and they will move as we land incremental
persistence, per-call materialization opt-outs, and a better time index.

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
- **Windows support (next focused session):** port the adapter's POSIX
  time APIs (`strptime`, `gmtime_r`, `timegm`) to MSVC equivalents so
  `cibuildwheel` can add Windows wheels to the release matrix.
- Incremental persistence: append-only log alongside the snapshot format,
  so long-lived agents don't have to rewrite the full state on every
  checkpoint.
- Topological sort in `explain` for general DAGs.
- Formalize **promotion latency** as a documented concept. Claude
  computed this metric unprompted during the traffic-light demo run
  (see `examples/transcripts/traffic_agent_demo_transcript.md`): it's
  the delay between a projection being recorded and being promoted to
  ground truth — effectively "how long the agent held a prediction
  before reality confirmed it." Worth a convenience method on
  `TemporalCore` once the concept is written down.

**Medium term**
- Confidence propagation through causal chains (the `confidence` field
  on `TemporalEvent` is stored and round-trips through every layer,
  but no engine operation currently composes or reasons about
  probability — this is where GALAHAD would start answering
  probability-weighted "how likely was this path?" questions).
- More framework-specific demo scripts alongside
  `examples/anthropic_demo.py` and `examples/traffic_agent_demo.py`
  (LangChain, OpenAI function-calling, local-model tool-use harness).
- Multi-agent scenarios — two agents sharing a GALAHAD store with
  cross-agent counterfactual reasoning.

**Longer term**
- Concurrent readers / writer-exclusion
- Interval-tree time index for true logarithmic range queries on long-lived events
- Hierarchical branches ("close_door.option_A" nested under "close_door")
- First robotics integration — plug into an active agent loop as the belief-and-plan
  substrate

## Release process

CI and wheel publishing are driven by two GitHub Actions workflows in
`.github/workflows/`.

### Continuous integration

`ci.yml` runs on every push to `main` and every pull request. It builds
the full C++ tree (core, engine, adapter, persistence, bench, all four
test binaries), runs the four C++ test binaries end-to-end, runs the
bench as a smoke check, installs the package with `pip install .`, and
runs the Python test suite against the *installed* module from a neutral
working directory so `import galahad` hits site-packages and not the
build tree. Matrix covers Python 3.10 / 3.11 / 3.12 on Ubuntu plus one
run on macOS. Windows is intentionally skipped until the adapter's
POSIX time APIs (`strptime`, `gmtime_r`, `timegm`) are ported to the
MSVC CRT equivalents.

### Cutting a release

1. Bump the version in both `pyproject.toml` and
   `python/galahad.cpp` (the `m.attr("__version__")` line).
2. Commit with a descriptive message (don't skip the `Co-Authored-By`
   trailer — it matches the rest of the project's history).
3. Tag and push:
   ```bash
   git tag -a v0.1.2 -m "release v0.1.2"
   git push origin v0.1.2
   ```

### Automated wheel building

The `v*` tag triggers `wheels.yml`, which:

- Uses [cibuildwheel](https://cibuildwheel.readthedocs.io/) to build
  CPython 3.8–3.12 wheels on two native runners:
  Linux `manylinux_2_28` x86_64 (via `gcc-toolset-14`, new enough for
  C++20) and macOS arm64 (`macos-14`, Apple Silicon). Intel macOS is
  source-install only for now — the GitHub Actions `macos-13` runner
  label was retired during v0.1.2, and Intel macOS users can still
  `pip install galahad-temporal` to compile from the sdist locally.
  When a verified Intel macOS runner label lands, it will be added
  back to the matrix.
- Runs `python/test_galahad.py` against every built wheel via
  `CIBW_TEST_COMMAND` before the wheel leaves the runner. The timezone
  regression that cost us 0.1.1 would have been caught here.
- Builds the sdist via `python -m build --sdist` on a fourth runner so
  `pip install galahad-temporal` from source still works on any
  platform including Windows (via user-side compilation).
- Publishes every wheel plus the sdist to PyPI via
  [trusted publishing](https://docs.pypi.org/trusted-publishers/) —
  no API token stored in GitHub secrets.

### One-time PyPI trusted-publishing setup

Before the first automated release, you need to tell PyPI to trust this
workflow. This is a one-time click-through:

1. Go to https://pypi.org/manage/project/galahad-temporal/settings/publishing/
2. Add a new "Trusted publisher" with:
   - **Owner:** `JacobFlorio`
   - **Repository name:** `GALAHAD-TemporalSlicer`
   - **Workflow name:** `wheels.yml`
   - **Environment name:** `pypi`
3. Save.

After that, every `git push origin v*` tag triggers a full wheel build
and publish without any manual intervention. Future releases become
`git tag -a v0.1.2 -m … && git push --tags`.

Until the trusted-publisher is configured, the `publish_to_pypi` job
will fail, but the `build_wheels` and `build_sdist` jobs still run and
upload their artifacts to the GitHub Actions run page. You can download
those manually and `twine upload` them if needed.

## License

MIT. See [LICENSE](LICENSE). Copyright (c) 2026 Jacob T. Florio.

Third-party dependency: [nlohmann/json](https://github.com/nlohmann/json) v3.11.3,
pulled via CMake `FetchContent`, also MIT-licensed.

## Project scope

This is a one-author research project. Contributions, discussion, and use cases are
welcome but there is no formal contribution process yet. If you are building a system
that needs temporal reasoning and this is interesting to you, open an issue describing
the use case — the API is young enough that concrete workloads can still shape it.
