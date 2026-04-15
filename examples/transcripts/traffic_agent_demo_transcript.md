# Traffic Agent 6-Tick Demo — live transcript (v0.2.1)

- **Date:** 2026-04-15
- **Model:** `claude-opus-4-6`
- **Demo script:** [`examples/traffic_agent_demo.py`](../traffic_agent_demo.py)
- **GALAHAD version:** 0.2.1 (adds `get_projections`, `get_refuted_branches`,
  `get_all_events` introspection tools alongside the v0.2.0 counterfactual
  primitives)
- **Tool count registered:** 22
- **Turns to completion:** 5 tool-call turns + 1 synthesis turn
- **Parallel tool calls:** 21 across 5 turns, avg 4.2 per turn
- **What it demonstrates:** full projection-lifecycle exercise under a
  live LLM — multiple projections created, refuted, and promoted over
  time; Claude invents a **discovery-then-query** agent pattern, computes
  a new metric called **promotion latency**, produces an ASCII decision
  tree, and surfaces a latent duplicate-id bug in `getEffects` (fixed in
  0.2.2).

## Scenario

A pedestrian at a crosswalk runs a 6-tick perceive/decide/act loop:

```
  Tick 1 (t0+0ms)   RED observed         → infer unsafe
                                           projections: go_now(0.1),
                                                        wait_for_green(0.7),
                                                        jog_through(0.2)
  Tick 2 (t0+10ms)  RED still observed   → refute go_now + jog_through
  Tick 3 (t0+20ms)  YELLOW observed      → infer transition
                                           projection: start_early(0.4)
  Tick 4 (t0+30ms)  GREEN observed       → infer safe
                                           promote wait_for_green → main
                                           refute start_early
                                           projection: walk_across(0.95)
  Tick 5 (t0+40ms)  stepping_off_curb    → t5_act fires (walk)
                                           promote walk_across → main
  Tick 6 (t0+70ms)  crossing_complete
```

Resulting store: **12 main events** (including two promoted
projection copies), **5 projection branches** (3 refuted, 2 with their
original pre-promotion copies still alive on their branches).

The prompt asks six introspection questions — one per canonical
reasoning shape plus a full decision-tree reconstruction synthesis
question. The prompt deliberately **does not spoon-feed event ids or
branch names** and tells Claude to discover the structure via tools.

## Transcript (verbatim)

```
jacob@DESKTOP-4HG0HUR:~/GALAHAD-TemporalSlicer$ python3 examples/traffic_agent_demo.py
GALAHAD populated (6-tick traffic light scenario):
  anchor t0 = 2026-04-15T05:34:52.456815+00:00
  main events: 12
  non-refuted projections: 2
  refuted branches: ['go_now', 'jog_through', 'start_early']

Registered 22 GALAHAD tools with Claude.

========================================================================
Running agent loop against claude-opus-4-6 (up to 16 turns)
========================================================================

--- Turn 1 ---
[Claude]


I'll systematically work through this scenario using the GALAHAD tools. Let me start by gathering the foundational data with several independent calls.

## Phase 1: Discovery — Get the full event store, refuted branches, and open projections
[tool_use] get_all_events({})
[tool_use] get_refuted_branches({})
[tool_use] get_projections({})
[tool_use] now({})
[tool_result get_all_events] {"ok": true, "result": [{"branch_id": "main", "causal_links": [], "confidence": 1.0, "data": {"light": "RED"}, "id": "t1_perceive", "recorded_at": "2026-04-15T05:34:52.456Z", "recorded_at_ns": 1776231...
[tool_result get_refuted_branches] {"ok": true, "result": ["start_early", "jog_through", "go_now"]}
[tool_result get_projections] {"ok": true, "result": [{"branch_id": "wait_for_green", "causal_links": ["t1_infer"], "confidence": 0.7, "data": {"action": "wait", "safety": "high"}, "id": "fut_wait_for_green", "recorded_at": "2026-...
[tool_result now] {"ok": true, "result": {"iso": "2026-04-15T05:34:56.534Z", "ns": 1776231296534573118}}

--- Turn 2 ---
[Claude]
Excellent! I now have the full picture. Let me proceed with the targeted queries for each of the six questions.

## Phase 2: Questions 1–5 — Targeted tool calls
[tool_use] explain({"id": "t5_act"})
[tool_use] what_happened_during({"start": "2026-04-15T05:34:52.456815+00:00", "end": "2026-04-15T05:34:52.470815+00:00", "as_of": "2026-04-15T05:34:52.468815+00:00"})
[tool_use] why_not({"id": "fut_go_now"})
[tool_use] why_not({"id": "fut_jog_through"})
[tool_use] why_not({"id": "fut_start_walking"})
[tool_use] explain_with({"target_id": "hyp_jogged", "mutation": {"id": "hyp_jogged", "valid_from": "2026-04-15T05:34:52.461815+00:00", "valid_to": "2026-04-15T05:34:52.464815+00:00", "...)
[tool_result explain] {"ok": true, "result": {"causes": [{"branch_id": "main", "causal_links": [], "confidence": 1.0, "data": {"light": "GREEN"}, "id": "t4_perceive", "recorded_at": "2026-04-15T05:34:52.486Z", "recorded_at...
[tool_result what_happened_during] {"ok": true, "result": [{"branch_id": "main", "causal_links": [], "confidence": 1.0, "data": {"light": "RED"}, "id": "t1_perceive", "recorded_at": "2026-04-15T05:34:52.456Z", "recorded_at_ns": 1776231...
[tool_result why_not] {"ok": true, "result": [{"branch": "go_now", "hypothetical_event": {"branch_id": "go_now", "causal_links": ["t1_infer"], "confidence": 0.1, "data": {"action": "walk_now", "risk": "high"}, "id": "fut_g...
[tool_result why_not] {"ok": true, "result": [{"branch": "jog_through", "hypothetical_event": {"branch_id": "jog_through", "causal_links": ["t1_infer"], "confidence": 0.2, "data": {"action": "jog_across", "risk": "moderate...
[tool_result why_not] {"ok": true, "result": [{"branch": "start_early", "hypothetical_event": {"branch_id": "start_early", "causal_links": ["t3_infer"], "confidence": 0.4, "data": {"action": "start_walk_early"}, "id": "fut...
[tool_result explain_with] {"ok": true, "result": {"causes": [{"branch_id": "main", "causal_links": [], "confidence": 1.0, "data": {"light": "RED"}, "id": "t1_perceive", "recorded_at": "2026-04-15T05:34:52.456Z", "recorded_at_n...

--- Turn 3 ---
[Claude]
All data is in. Now let me get a couple more details to fully cross-reference the lifecycle audit for Question 5.
[tool_use] is_refuted({"branch": "wait_for_green"})
[tool_use] is_refuted({"branch": "walk_across"})
[tool_use] get_event({"id": "fut_wait_for_green", "branch": "main"})
[tool_use] get_event({"id": "fut_walk_across", "branch": "main"})
[tool_result is_refuted] {"ok": true, "result": false}
[tool_result is_refuted] {"ok": true, "result": false}
[tool_result get_event] {"ok": true, "result": {"branch_id": "main", "causal_links": ["t1_infer"], "confidence": 1.0, "data": {"action": "wait", "safety": "high"}, "id": "fut_wait_for_green", "recorded_at": "2026-04-15T05:34...
[tool_result get_event] {"ok": true, "result": {"branch_id": "main", "causal_links": ["t4_infer"], "confidence": 1.0, "data": {"action": "cross_street"}, "id": "fut_walk_across", "recorded_at": "2026-04-15T05:34:52.498Z", "r...

--- Turn 4 ---
[Claude]
Let me also verify the causal descendants from the initial decision point for the decision-tree reconstruction.
[tool_use] get_effects({"id": "t1_infer"})
[tool_use] get_effects({"id": "t3_infer"})
[tool_use] get_effects({"id": "t4_infer"})
[tool_use] get_descendants({"id": "t1_perceive"})
[tool_result get_effects] {"ok": true, "result": ["fut_wait_for_green", "fut_wait_for_green"]}
[tool_result get_effects] {"ok": true, "result": []}
[tool_result get_effects] {"ok": true, "result": ["fut_walk_across", "t5_act", "fut_walk_across"]}
[tool_result get_descendants] {"ok": true, "result": ["t1_infer", "fut_wait_for_green"]}

--- Turn 5 ---
[Claude]
Now let me check the refuted branches' events explicitly to ensure I have their full detail:
[tool_use] get_projections({"branch": "go_now"})
[tool_use] get_projections({"branch": "jog_through"})
[tool_use] get_projections({"branch": "start_early"})
[tool_result get_projections] {"ok": true, "result": [{"branch_id": "go_now", "causal_links": ["t1_infer"], "confidence": 0.1, "data": {"action": "walk_now", "risk": "high"}, "id": "fut_go_now", "recorded_at": "2026-04-15T05:34:52...
[tool_result get_projections] {"ok": true, "result": [{"branch_id": "jog_through", "causal_links": ["t1_infer"], "confidence": 0.2, "data": {"action": "jog_across", "risk": "moderate"}, "id": "fut_jog_through", "recorded_at": "202...
[tool_result get_projections] {"ok": true, "result": [{"branch_id": "start_early", "causal_links": ["t3_infer"], "confidence": 0.4, "data": {"action": "start_walk_early"}, "id": "fut_start_walking", "recorded_at": "2026-04-15T05:3...

--- Turn 6 ---
[Claude]
I now have all the data needed to answer all six questions comprehensively. Here is the full analysis:

---

# Complete Analysis of the Pedestrian-at-Crosswalk Scenario

[... per-question analysis tables and narratives, 5000+ tokens of structured
 output including a full-scale ASCII decision tree, lifecycle audit tables,
 and three unprompted observations called out under "Notable moments" below ...]

### Complete Decision Tree (Visual)

     TICK 1          TICK 2       TICK 3          TICK 4        TICK 5         TICK 6
     ┌─perceive RED  │perceive    ┌─perceive YEL  ┌─perceive    ┌─perceive     ┌─perceive
     │  ↓            │ RED        │  ↓             │  GREEN      │ stepping     │ crossing
     │ infer         │ (confirm)  │ infer          │  ↓          │ off curb     │ complete
     │ UNSAFE        │            │ TRANSITION     │ infer       │  ↓           │
     │  ↓            │            │  ↓             │ SAFE        │ t5_act       │
     │  ├──[go_now]──┤──✗ REFUTED │  ├──[start    │  ↓          │ (WALK!)      │
     │  │  (10%)     │            │  │  early]     │ PROMOTE     │  ↓           │
     │  ├──[jog]─────┤──✗ REFUTED │  │  (40%)     │ wait_for    │ PROMOTE      │
     │  │  (20%)     │            │  │   ↓         │ green       │ walk_across  │
     │  └──[wait]────┤────────────┤──┤──✗ REFUTED  │ (→ main)   │ (→ main)     │
     │     (70%)     │            │  │             │             │              │
     └───────────────┘            │  └──[walk     │             │              │
                                  │   across]─────┤─────────────┤──────────────┘
                                  │   (95%)       │             │
                                  └───────────────┘             │
                                                                └──► SAFE CROSSING COMPLETE


[The run hit max_tokens=4096 partway through a tick-by-tick narrative
 summary restating information already produced above. All six
 question answers, the ASCII decision tree, and the lifecycle audit
 tables completed inside the original budget. The demo script was
 subsequently patched to max_tokens=8192 so future runs have more
 headroom for the synthesis turn.]

========================================================================
6-tick traffic light demo complete.
========================================================================
```

## Notable moments

### Discovery-then-query pattern, invented on the spot

Turn 1 header:

> *"## Phase 1: Discovery — Get the full event store, refuted branches, and open projections"*

Turn 2 header:

> *"## Phase 2: Questions 1–5 — Targeted tool calls"*

Claude explicitly labeled its own turns as phases and structured the
agent loop as a discovery pass followed by targeted queries. This
pattern was not prompted for. It emerged because the v0.2.1
introspection tools (`get_all_events`, `get_refuted_branches`,
`get_projections`) made it natural to first ask "what's in the store?"
before asking specific questions about specific events.

### Parallel tool calls at scale

| Turn | Calls | Pattern |
|---|---|---|
| 1 | 4 in parallel | Discovery — every introspection tool simultaneously |
| 2 | 6 in parallel | Q1 + Q2 + three `why_not` for Q3 + Q4 `explain_with` at once |
| 3 | 4 in parallel | Lifecycle cross-reference |
| 4 | 4 in parallel | Decision-tree edge walk |
| 5 | 3 in parallel | Refuted-branch audit |
| 6 | 0 | Synthesis turn |

**21 tool calls across 5 turns, averaging 4.2 parallel calls per turn.**

### "Promotion latency" — a metric Claude invented unprompted

> *"The `wait_for_green` branch was created at .460 (tick 1) but promoted
> at .488 (tick 4) — the agent believed in this plan for 28ms of
> transaction time before reality confirmed it. The `walk_across` branch
> was created at .490 (tick 4) and promoted just 8ms later at .498
> (tick 5) — a much faster confirmation cycle."*

Promotion latency is not a metric defined anywhere in GALAHAD's code,
API, README, or prompt. Claude computed the delta between a projection's
original `recorded_at` and its promoted main-branch copy's `recorded_at`,
compared the two values across branches, and interpreted the gap as a
meaningful psychological quantity. The 28ms vs 8ms contrast was framed
as evidence that **the agent's confidence in waiting grew slowly (it
had to be patient) but its confidence in crossing grew fast (once the
light turned green, it committed quickly)**. That is exactly the kind of
temporal-reasoning insight GALAHAD was built to make legible.

### Third-level bitemporal distinction

> *"The three projection branches (go_now, jog_through, wait_for_green)
> were all created at recorded_at .460 (t0+4ms), so by .468 the agent
> had already entertained them. However, the go_now and jog_through
> branches were not yet refuted — their refutation happened later."*

Claude distinguished two distinct transaction times: the time at which
a deliberation's branches were **created**, and the time at which the
deliberation's **conclusion** (refutation) was recorded. These are
different events on the transaction-time axis. The prompt did not set
up this distinction; Claude derived it from the raw event log.

### Structural contrast of hypothetical vs real causation

> *"There is no intervening 'safe_to_cross' inference that would justify
> the action. The forked world makes the recklessness structurally
> visible — `hyp_jogged` is causally grounded in an unsafe assessment,
> unlike the actual t5_act which was grounded in t4_infer's
> safe_to_cross assessment."*

The sentence *"The forked world makes the recklessness structurally
visible"* is the clearest articulation of `explain_with`'s purpose that
exists anywhere in the project. The comparison is ontologically correct:
the difference between a rational action and a reckless one is the
shape of the causal graph beneath them, not a value judgment. Claude
derived this from comparing two `explain` results.

### Latent bug surfaced: duplicate ids in `getEffects` after promotion

Turn 4 tool results:

```
get_effects('t1_infer') -> ['fut_wait_for_green', 'fut_wait_for_green']
get_effects('t4_infer') -> ['fut_walk_across', 't5_act', 'fut_walk_across']
```

Both returns contain the same projection id twice. Root cause: after
`promoteBranch` appends a main-branch copy of each projection on the
promoted branch, `rebuildCausalIndices` sees two stored events with the
same id and pushes into `reverseCausal[parent]` twice. Claude did not
flag this as a bug — it just worked around the duplication in its
narrative — but the duplicate is visible in the raw tool output.

**Fixed in v0.2.2** by deduplicating both `causalGraph` and
`reverseCausal` in place at the end of `rebuildCausalIndices`, with a
regression test in `tests/test_engine.cpp` that specifically checks
`getEffects` and `getCauses` after a promotion returns unique ids.

### The max-tokens cutoff at the end

The run hit `max_tokens=4096` during a tick-by-tick narrative wrap-up
that largely restated information already produced above. All six
question answers, the lifecycle audit tables, the ASCII decision tree,
and the three unprompted observations above completed inside the
original budget. The demo script was subsequently bumped to
`max_tokens=8192` so future runs have headroom for the synthesis turn.

## The larger claim this transcript supports

This is currently the strongest existing evidence that GALAHAD's full
forward-looking half — projection creation, refutation, promotion, and
counterfactual introspection over the resulting branched store — works
under a real LLM driving the tool surface. It is also the first
recorded case of an LLM agent **inventing a metric** (promotion
latency) that GALAHAD's bitemporal structure makes computable, and
**inventing a design pattern** (discovery-then-query) that matches how
the introspection surface is naturally used.
