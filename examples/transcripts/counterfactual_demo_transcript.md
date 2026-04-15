# Counterfactual Demo — live transcript (v0.2.0)

- **Date:** 2026-04-15
- **Model:** `claude-opus-4-6`
- **Demo script:** [`examples/anthropic_demo.py`](../anthropic_demo.py)
- **GALAHAD version:** 0.2.0 (first release with `why_not` and `explain_with`)
- **Tool count registered:** 19
- **Turns to completion:** 4
- **What it demonstrates:** first live LLM use of the counterfactual
  primitives shipped in v0.2.0; parallel tool calls across independent
  questions; Claude articulating the bitemporal distinction and the
  "paths not taken" framing in plain English.

## Scenario

A four-event perceive → infer → decide → act chain on the main branch,
with the action deliberately **late-arriving** (valid at t0+20ms,
recorded at t0+200ms) so bitemporal replay is non-trivial. Two competing
projection branches — `close_door` (confidence 0.7) and `ignore`
(confidence 0.3) — both depend on `decide`. The `ignore` branch is
refuted so `fut_ignore` becomes the counterfactual target.

The prompt asks four question shapes, one for each part of the
reasoning surface:

1. **Backward causal** — why did `act` happen?
2. **Bitemporal replay** — what would the system have believed at `t0+100ms`?
3. **Counterfactual** — what did the refuted `ignore` branch predict?
4. **Hypothetical mutation** — if a `hyp_slam` event had been added citing `decide`, what would its explanation look like?

## Transcript (verbatim)

```
jacob@DESKTOP-4HG0HUR:~/GALAHAD-TemporalSlicer$ python3 -c "import galahad; print(galahad.__version__)"
0.2.0
jacob@DESKTOP-4HG0HUR:~/GALAHAD-TemporalSlicer$ python3 examples/anthropic_demo.py
GALAHAD populated with 4 events.
Anchor time t0 = 2026-04-15T05:04:44.548715+00:00
Note: 'act' occurred at t0+20ms but was recorded at t0+200ms.
Note: 'ignore' branch is refuted — fut_ignore is the counterfactual.

Registered 19 GALAHAD tools with Claude.

========================================================================
Running agent loop against claude-opus-4-6 (up to 12 turns)
========================================================================

--- Turn 1 ---
[Claude]


I'll systematically work through all four question shapes. Let me start by gathering the baseline information and then tackle each question.

## Question 1: BACKWARD CAUSAL — Why did "act" happen?
## Question 3: COUNTERFACTUAL — What did the refuted `ignore` branch predict?

These two are independent, so I'll fire them in parallel:
[tool_use] explain({"id": "act"})
[tool_use] why_not({"id": "fut_ignore"})
[tool_use] now({})
[tool_result explain] {"ok": true, "result": {"causes": [{"branch_id": "main", "causal_links": [], "confidence": 1.0, "data": {"obs": "door_open"}, "id": "perceive", "recorded_at": "2026-04-15T05:04:44....
[tool_result why_not] {"ok": true, "result": [{"branch": "ignore", "hypothetical_event": {"branch_id": "ignore", "causal_links": ["decide"], "confidence": 0.3, "data": {"do": "wait"}, "id": "fut_ignore"...
[tool_result now] {"ok": true, "result": {"iso": "2026-04-15T05:04:48.646Z", "ns": 1776229488646335169}}

--- Turn 2 ---
[Claude]
Excellent — Questions 1 and 3 are answered. Now let me tackle the remaining two in parallel:

## Question 2: BITEMPORAL REPLAY — What did the system believe at t0+100ms?
## Question 4: HYPOTHETICAL MUTATION — What if "hyp_slam" had happened?
[tool_use] what_happened_during({"start": "2026-04-15T05:04:44.548715+00:00", "end": "2026-04-15T05:04:44.648715+00:00", "as_of": "2026-04-15T05:04:44.648715+00:00"})
[tool_use] explain_with({"target_id": "hyp_slam", "mutation": {"id": "hyp_slam", "type": "action", "valid_from": "2026-04-15T05:04:44.573715+00:00", "valid_to": "20...)
[tool_result what_happened_during] {"ok": true, "result": [{"branch_id": "main", "causal_links": [], "confidence": 1.0, "data": {"obs": "door_open"}, "id": "perceive", "recorded_at": "2026-04-15T05:04:44.548Z", "rec...
[tool_result explain_with] {"ok": true, "result": {"causes": [{"branch_id": "main", "causal_links": [], "confidence": 1.0, "data": {"obs": "door_open"}, "id": "perceive", "recorded_at": "2026-04-15T05:04:44....

--- Turn 3 ---
[Claude]
Now let me verify the bitemporal distinction by checking what happens *without* the `as_of` ceiling — i.e., asking the same question from the system's *current* vantage point:
[tool_use] what_happened_during({"start": "2026-04-15T05:04:44.548715+00:00", "end": "2026-04-15T05:04:44.648715+00:00"})
[tool_use] get_event({"id": "act"})
[tool_result what_happened_during] {"ok": true, "result": [{"branch_id": "main", "causal_links": [], "confidence": 1.0, "data": {"obs": "door_open"}, "id": "perceive", "recorded_at": "2026-04-15T05:04:44.548Z", "rec...
[tool_result get_event] {"ok": true, "result": {"branch_id": "main", "causal_links": ["decide"], "confidence": 1.0, "data": {"do": "close"}, "id": "act", "recorded_at": "2026-04-15T05:04:44.748Z", "record...

--- Turn 4 ---
[Claude]
All four questions are now fully answered. Here is the complete analysis:

---

# Four Shapes of Temporal Reasoning — Full Walkthrough

## 1. BACKWARD CAUSAL — "Why did `act` happen?"

**Tool used:** `explain` — walks the transitive causal ancestry of an event.

The engine returned a clean three-step causal chain, all on `main`:

| Step | Event | Type | Data | Valid interval (offset from t0) |
|------|-------|------|------|----|
| 1 | `perceive` | perception | `obs: door_open` | t0 → t0+5ms |
| 2 | `infer` | inference | `why: wind` | t0+5ms → t0+15ms |
| 3 | `decide` | decision | `choose: close` | t0+15ms → t0+20ms |
| **→** | **`act`** | **action** | **`do: close`** | **t0+20ms → t0+30ms** |

**Reading:** The agent perceived the door was open, inferred the wind caused it, decided to close it, and then acted. Each event's `valid_to` lines up with the next event's `valid_from` — a tight, completed causal chain.

---

## 2. BITEMPORAL REPLAY — "What did the system believe at t0+100ms?"

**Tool used:** `what_happened_during` with an `as_of` ceiling of t0+100ms, compared against the same query with no ceiling (current knowledge).

### At t0+100ms (`as_of` = 2026-04-15T05:04:44.648715Z):
The system saw **4 events**: `perceive`, `infer`, `decide`, and the **projection** `fut_close` (on the `close_door` branch). **`act` was absent.** The system knew the agent *intended* to close the door (the projection existed with confidence 0.7) but had not yet *recorded* that the action actually happened.

### Now (no `as_of` ceiling):
The system sees **5 events**: the same four, plus `act` on `main`. The `act` event has `recorded_at` = t0+200ms — the system only *learned* about the actual action 200ms after t0, even though the action's **valid time** was t0+20ms → t0+30ms.

### The bitemporal distinction:
- **Valid time** answers *"when did this happen in the world?"* — `act` happened at t0+20ms.
- **Transaction time** answers *"when did the system learn of it?"* — `act` was recorded at t0+200ms.
- At t0+100ms the action had *already happened* in the world but the system *didn't know yet*. All it had was the `close_door` projection (a confident guess). This is exactly the gap bitemporality is designed to preserve.

---

## 3. COUNTERFACTUAL — "What did the refuted `ignore` branch predict?"

**Tool used:** `why_not` — returns every refuted branch that contained a prediction for a given event, plus the would-have-been causal chain.

The engine found **one refuted branch** (`ignore`) containing:

| Field | Value |
|-------|-------|
| Event | `fut_ignore` |
| Type | `projected_action` |
| Data | `do: wait` |
| Confidence | **0.3** |
| Causal link | `decide` |

Its **would-have-been causal chain** walks back through `main`:
> `perceive` → `infer` → `decide` → *(then, hypothetically)* → `fut_ignore`

**Reading:** The agent considered doing nothing — just waiting. That possibility branched off from the same `decide` event but was assigned only 30% confidence (vs. 70% for the close-door projection). The branch was subsequently refuted, meaning the agent ruled out inaction. The data isn't deleted; it's simply marked so default queries skip it, but explicit counterfactual queries like `why_not` can still retrieve the full hypothetical ancestry. This lets us answer *"what almost happened, and why didn't it?"*

---

## 4. HYPOTHETICAL MUTATION — "What if `hyp_slam` had happened instead?"

**Tool used:** `explain_with` — clones the core, injects a hypothetical event into the fork, runs `explain` on the fork, and discards it. The real store is untouched.

I injected:
```
hyp_slam: action, do=slam_door, valid t0+25ms → t0+35ms, caused by "decide"
```

The engine returned the same clean causal chain:

> `perceive` → `infer` → `decide` → **`hyp_slam`**

**Reading:** If the agent had slammed the door instead of gently closing it, the causal explanation would be identical up to the decision point — the same perception, same inference, same decision. Only the final action differs. Critically, **no event was committed to the store**. This is a pure sandbox: we tested a "what-if" without polluting the ground-truth timeline or any existing branch.

---

# Summary: Four Question Shapes in Plain English

| Shape | Question it answers | Key mechanism | What makes it distinct |
|-------|-------------------|---------------|----------------------|
| **Backward Causal** | *"Why did this happen?"* | Walk the `causal_links` graph backward from an event to its roots. | Traces **actual causation** through the ground-truth timeline. Every link is something that really happened. |
| **Bitemporal Replay** | *"What did we know, and when did we know it?"* | Query with an `as_of` transaction-time ceiling to see the system's past belief state. | Separates **world time** (when things happened) from **knowledge time** (when the system recorded them). At t0+100ms the action had already occurred in the world but the system still only had a projection — it hadn't recorded the ground truth yet. |
| **Counterfactual** | *"What almost happened, and why didn't it?"* | Query refuted projection branches to recover discarded futures and their hypothetical causal chains. | Looks at **paths not taken**. The `ignore` branch was a real deliberation artifact — the agent considered waiting — but was ruled out. The data survives refutation so we can audit the decision. |
| **Hypothetical Mutation** | *"What *would* the causal picture look like if X had happened?"* | Clone the store, inject an invented event, run explain, discard the clone. | A pure **sandbox**. No branch is created, no event is committed. It lets you test arbitrary "what-if" scenarios against the existing causal graph without side effects — ideal for planning, debugging, or post-hoc analysis. |

Together, these four shapes give a temporal reasoning engine its full expressive range: you can look **backward** (what caused what), **sideways in time** (what did we know when), **at roads not taken** (what was considered and rejected), and **into pure imagination** (what would happen if we injected something new) — all without ever corrupting the ground-truth record.

========================================================================
Demo complete. Claude reasoned over GALAHAD's temporal substrate.
========================================================================
```

## Notable moments

### Parallel tool calls across independent questions (Turn 1 and Turn 2)

Claude recognized that Q1 (backward causal) and Q3 (counterfactual) are
logically independent and fired them in parallel — `explain`, `why_not`,
and `now` all in Turn 1. Then Q2 (bitemporal) and Q4 (hypothetical) fired
in parallel in Turn 2. The four-question analysis completed in three tool
rounds plus a synthesis turn. This behavior was not prompted for.

### Unprompted two-level bitemporal observation (Turn 4)

> *"The system saw 4 events: perceive, infer, decide, and the projection
> fut_close (on the close_door branch). `act` was absent. The system knew
> the agent intended to close the door (the projection existed with
> confidence 0.7) but had not yet recorded that the action actually
> happened."*

This is a subtle observation that falls out of the data only if the
reader notices that the `close_door` projection's `recorded_at` was
earlier than the action's `recorded_at`. The prompt did not set this up
explicitly; Claude derived it from the tool results alone and
articulated the distinction between "the system knew the agent intended"
and "the system had recorded the action" in one sentence.

### The project's positioning statement, produced organically

> *"you can look **backward** (what caused what), **sideways in time**
> (what did we know when), **at roads not taken** (what was considered
> and rejected), and **into pure imagination** (what would happen if we
> injected something new) — all without ever corrupting the
> ground-truth record."*

This is essentially the project's elevator pitch. Four question shapes
with four spatial metaphors, each matching a distinct GALAHAD primitive,
ending on the integrity constraint. Produced as a demo artifact, not
requested as marketing copy.

### Pre-history note

An earlier run of this same demo script against **v0.1.3** (before
`why_not` and `explain_with` shipped) forced Claude to **improvise
replacements** using `add_projection` + `explain` + `prune_branch` as a
manual equivalent of `explain_with`. That pre-v0.2.0 run took **11
turns** because Claude had to work around the missing tools. The
v0.2.0 run above takes **4 turns** because the counterfactual primitives
are directly available. The same workflow, with the right tools
shipped, uses roughly **36%** of the turn count.
