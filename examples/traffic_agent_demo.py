#!/usr/bin/env python3
"""GALAHAD v0.2.0 — richer agent scenario (6-tick traffic light).

Pre-populates a full 6-tick pedestrian-at-crosswalk history in GALAHAD,
then hands it to Claude and asks six introspection questions that
exercise the entire reasoning surface: backward causal, bitemporal
replay, counterfactual (why_not), hypothetical (explain_with),
lifecycle audit, and full decision-tree reconstruction.

Unlike the shorter anthropic_demo.py, this scenario exercises the
full **projection lifecycle** — projections are created, some are
refuted when reality contradicts them, and survivors get **promoted
to ground truth**. By the end of the run, the agent's committed path
has been merged into main while the roads-not-taken remain queryable
on their original (refuted) branches. That's the first demo where a
real LLM is asked to introspect a decision tree that actually
happened inside GALAHAD's branching store.

Events and branches:

    main timeline (events added tick by tick):
      t1_perceive  RED          t0+0ms
      t1_infer     unsafe       t0+2ms
      t2_perceive  RED          t0+10ms
      t3_perceive  YELLOW       t0+20ms
      t3_infer     transition   t0+22ms
      t4_perceive  GREEN        t0+30ms
      t4_infer     safe         t0+32ms
      t5_perceive  stepping_off t0+40ms
      t5_act       walk         t0+42ms
      t6_perceive  complete     t0+70ms

    projection branches:
      go_now         (confidence 0.1)  refuted at tick 2
      wait_for_green (confidence 0.7)  promoted to main at tick 4
      jog_through    (confidence 0.2)  refuted at tick 2
      start_early    (confidence 0.4)  refuted at tick 4
      walk_across    (confidence 0.95) promoted to main at tick 5

Run modes (same as anthropic_demo.py):
    pip install galahad-temporal==0.2.0
    export ANTHROPIC_API_KEY=sk-...
    python3 examples/traffic_agent_demo.py
    # or: PYTHONPATH=build python3 examples/traffic_agent_demo.py

Set GALAHAD_DEMO_DRY_RUN=1 to force dry-run mode even with an API key.
"""

import json
import os
import sys
from datetime import datetime, timedelta, timezone

try:
    import galahad
except ImportError:
    print("ERROR: galahad Python module not found on sys.path.",
          file=sys.stderr)
    print("Install with: pip install galahad-temporal>=0.2.0",
          file=sys.stderr)
    sys.exit(1)

try:
    from anthropic import Anthropic
    HAS_ANTHROPIC = True
except ImportError:
    HAS_ANTHROPIC = False

MODEL = "claude-opus-4-6"
MAX_TURNS = 16
API_KEY = os.getenv("ANTHROPIC_API_KEY")
DRY_RUN = bool(os.getenv("GALAHAD_DEMO_DRY_RUN")) or not (HAS_ANTHROPIC and API_KEY)

# ----------------------------------------------------------------------
# Scenario pre-population
# ----------------------------------------------------------------------

core = galahad.TemporalCore()
engine = galahad.TemporalEngine(core)
adapter = galahad.LLMToolAdapter(core, engine)

t0 = datetime.now(timezone.utc)


def ms(n):
    return timedelta(milliseconds=n)


def mk(id_, start, end, recorded, type_, data=None,
       links=None, branch="main", confidence=1.0):
    e = galahad.TemporalEvent()
    e.id = id_
    e.valid_from = start
    e.valid_to = end
    e.recorded_at = recorded
    e.type = type_
    e.data = data or {}
    e.causal_links = links or []
    e.branch_id = branch
    e.confidence = confidence
    return e


# ---- Tick 1 (t0+0ms): Red light observed. Consider three options. ----
core.add_event(mk("t1_perceive", t0,         t0 + ms(2),  t0,
                  "perception", {"light": "RED"}))
core.add_event(mk("t1_infer",    t0 + ms(2), t0 + ms(4),  t0 + ms(2),
                  "inference",  {"assessment": "unsafe_to_cross"},
                  ["t1_perceive"]))

core.add_projection(mk(
    "fut_go_now", t0 + ms(5), t0 + ms(15), t0 + ms(4),
    "projected_action", {"action": "walk_now", "risk": "high"},
    ["t1_infer"], branch="go_now", confidence=0.1))

core.add_projection(mk(
    "fut_wait_for_green", t0 + ms(5), t0 + ms(50), t0 + ms(4),
    "projected_action", {"action": "wait", "safety": "high"},
    ["t1_infer"], branch="wait_for_green", confidence=0.7))

core.add_projection(mk(
    "fut_jog_through", t0 + ms(5), t0 + ms(8), t0 + ms(4),
    "projected_action", {"action": "jog_across", "risk": "moderate"},
    ["t1_infer"], branch="jog_through", confidence=0.2))

# ---- Tick 2 (t0+10ms): Still red. Rule out the two risky options. ----
core.add_event(mk("t2_perceive", t0 + ms(10), t0 + ms(12), t0 + ms(10),
                  "perception", {"light": "RED"}))

# Reality held long enough to confirm both risky plans were wrong.
core.refute_branch("go_now")
core.refute_branch("jog_through")

# ---- Tick 3 (t0+20ms): Yellow. New premature-start projection. ----
core.add_event(mk("t3_perceive", t0 + ms(20), t0 + ms(22), t0 + ms(20),
                  "perception", {"light": "YELLOW"}))
core.add_event(mk("t3_infer",    t0 + ms(22), t0 + ms(24), t0 + ms(22),
                  "inference",  {"assessment": "transition_starting"},
                  ["t3_perceive"]))

core.add_projection(mk(
    "fut_start_walking", t0 + ms(25), t0 + ms(45), t0 + ms(24),
    "projected_action", {"action": "start_walk_early"},
    ["t3_infer"], branch="start_early", confidence=0.4))

# ---- Tick 4 (t0+30ms): Green. Promote wait_for_green. Refute start_early. ----
core.add_event(mk("t4_perceive", t0 + ms(30), t0 + ms(32), t0 + ms(30),
                  "perception", {"light": "GREEN"}))
core.add_event(mk("t4_infer",    t0 + ms(32), t0 + ms(34), t0 + ms(32),
                  "inference",  {"assessment": "safe_to_cross"},
                  ["t4_perceive"]))

# The wait paid off — promote to ground truth.
core.promote_branch("wait_for_green")
# start_early was premature — refute.
core.refute_branch("start_early")

# Project the actual cross on a new branch.
core.add_projection(mk(
    "fut_walk_across", t0 + ms(35), t0 + ms(80), t0 + ms(34),
    "projected_action", {"action": "cross_street"},
    ["t4_infer"], branch="walk_across", confidence=0.95))

# ---- Tick 5 (t0+40ms): Begin walking. Promote walk_across. ----
core.add_event(mk("t5_perceive", t0 + ms(40), t0 + ms(42), t0 + ms(40),
                  "perception", {"status": "stepping_off_curb"}))
core.add_event(mk("t5_act",      t0 + ms(42), t0 + ms(70), t0 + ms(42),
                  "action",     {"performed": "walk"},
                  ["t5_perceive", "t4_infer"]))

core.promote_branch("walk_across")

# ---- Tick 6 (t0+70ms): Cross complete. ----
core.add_event(mk("t6_perceive", t0 + ms(70), t0 + ms(72), t0 + ms(70),
                  "perception", {"status": "crossing_complete"},
                  ["t5_act"]))

# ----------------------------------------------------------------------
# Summary of what got populated
# ----------------------------------------------------------------------

main_count = len(core.query_range(
    galahad.TimeWindow(t0, t0 + ms(100)), None, "main"))
projections_now = core.get_projections()
refuted_now = core.get_refuted_branches()

print("GALAHAD populated (6-tick traffic light scenario):")
print(f"  anchor t0 = {t0.isoformat()}")
print(f"  main events: {main_count}")
print(f"  non-refuted projections: {len(projections_now)}")
print(f"  refuted branches: {sorted(refuted_now)}")
print()

tools = adapter.get_tool_schemas()
print(f"Registered {len(tools)} GALAHAD tools with Claude.")

# ----------------------------------------------------------------------
# Prompt — six introspection questions, no spoon-fed event ids
# ----------------------------------------------------------------------

PROMPT = f"""I have a GALAHAD v0.2.0 temporal reasoning engine pre-populated with a
6-tick pedestrian-at-crosswalk scenario. The anchor time t0 is
{t0.isoformat()}. Each tick is roughly 10ms apart in valid time. The
scenario is already in memory — DO NOT add events.

Scenario summary: an agent at a crosswalk observed a red light,
considered three competing plans (go now, wait for green, jog
through), waited while ruling out the risky plans, observed the
light turn yellow and then green, committed to its wait-then-cross
plan, and walked safely across.

Events on the main timeline are named by tick (t1_perceive,
t1_infer, t2_perceive, t3_perceive, t3_infer, t4_perceive, t4_infer,
t5_perceive, t5_act, t6_perceive). Some projection branches were
**promoted** to ground truth when reality confirmed them, and others
were **refuted** when reality contradicted them. You do not know the
exact branch names — discover them via GALAHAD tools.

Answer these six questions by calling GALAHAD tools. Pick the tool
that best fits each question; do not use the same tool for all six.

1. **BACKWARD CAUSAL.** Why did the agent walk? The agent's actual
   walk action is on the main timeline as `t5_act`. Explain its
   full causal chain.

2. **BITEMPORAL REPLAY.** What did the agent believe at tick 2,
   specifically at t0+12ms ({(t0 + ms(12)).isoformat()})? Had it
   ruled out any of the risky options yet by that transaction time?

3. **COUNTERFACTUAL ENUMERATION.** What alternatives were considered
   and refuted across the entire run? There are multiple refuted
   branches. For each refuted branch, surface the hypothetical event
   it predicted and its would-have-been causal chain. (Hint:
   `get_refuted_branches` lists refuted branches by name;
   `query_range` with an explicit branch filter can fetch the events
   on a specific branch even if it is refuted; `why_not` with the
   predicted event's id returns the full counterfactual record.)

4. **HYPOTHETICAL MUTATION.** Suppose the agent had committed to
   jogging through on tick 1 — inject a hypothetical event
   `hyp_jogged` citing the decision step as its cause, around
   t0+5ms. What would its causal explanation look like in that
   forked world? Use the tool that probes mutations without
   committing them.

5. **LIFECYCLE AUDIT.** Which projection branches were promoted to
   ground truth, and which were refuted? Cross-reference
   `get_refuted_branches` with `get_projections` to identify both
   sets cleanly.

6. **DECISION-TREE RECONSTRUCTION.** Given everything you learned
   above, reconstruct the full decision tree the agent actually
   explored during the 6 ticks. Identify (a) the branching points
   where multiple futures were projected, (b) the moments of
   commitment where reality confirmed one path and the agent
   promoted it, and (c) the abandoned branches with their reasons.

After the tool calls, produce a clear natural-language summary that
walks through the agent's decision-making story in order: what it
perceived each tick, what it considered and why, what it rejected
and why, what it committed to and why, and what the bitemporal
record tells us about the gap between world-time events and the
agent's belief state at each moment.

Be precise about *when* (tick / valid time) versus *when known*
(transaction time), and be explicit about which reasoning shape each
question required you to use."""

# ----------------------------------------------------------------------
# Dry run
# ----------------------------------------------------------------------

if DRY_RUN:
    print()
    print("=" * 72)
    print("DRY RUN — not calling the Anthropic API.")
    print("=" * 72)
    if not HAS_ANTHROPIC:
        print("  Reason: `anthropic` package not installed.")
        print("  Fix:    pip install anthropic")
    if not API_KEY:
        print("  Reason: ANTHROPIC_API_KEY not set in the environment.")
        print("  Fix:    export ANTHROPIC_API_KEY=sk-...")
    if os.getenv("GALAHAD_DEMO_DRY_RUN"):
        print("  Reason: GALAHAD_DEMO_DRY_RUN is set.")
    print()
    print(f"  Model that would be used: {MODEL}")
    print(f"  Tools registered:         {len(tools)}")
    print(f"  Prompt length:            {len(PROMPT)} characters")
    print()
    print("Smoke tests against the populated scenario:")

    # 1. Backward causal — walk action's ancestry
    r1 = adapter.handle_tool_call("explain", {"id": "t5_act"})
    assert r1["ok"], r1
    causes = [c["id"] for c in r1["result"]["causes"]]
    print(f"  explain('t5_act')               -> {len(causes)} causes: {causes}")
    # The walk pulls in t5_perceive + t4_infer + t4_perceive (at least).
    assert "t4_perceive" in causes, f"expected t4_perceive in causes, got {causes}"

    # 2. Bitemporal — what was known at tick 2
    as_of_iso = (t0 + ms(12)).isoformat()
    r2 = adapter.handle_tool_call("explain",
                                  {"id": "t5_act", "as_of": as_of_iso})
    assert r2["ok"], r2
    past_causes = len(r2["result"]["causes"])
    print(f"  explain('t5_act', as_of=t0+12ms) -> {past_causes} causes "
          f"(should be 0 — t5_act was not recorded yet)")
    assert past_causes == 0

    # 3. Counterfactual — refuted branches
    r3 = adapter.handle_tool_call("get_refuted_branches", {})
    assert r3["ok"], r3
    refuted = sorted(r3["result"])
    print(f"  get_refuted_branches            -> {refuted}")
    assert set(refuted) == {"go_now", "jog_through", "start_early"}

    # 3b. why_not on each refuted branch's predicted event
    for event_id in ("fut_go_now", "fut_jog_through", "fut_start_walking"):
        r = adapter.handle_tool_call("why_not", {"id": event_id})
        assert r["ok"] and len(r["result"]) == 1, (event_id, r)
        branch = r["result"][0]["branch"]
        n_causes = len(r["result"][0]["would_have_been_causes"])
        print(f"  why_not('{event_id}')"
              f"{' ' * max(0, 25 - len(event_id))}-> branch={branch}, "
              f"would_have_been_causes={n_causes}")

    # 4. Hypothetical — explain_with
    r4 = adapter.handle_tool_call("explain_with", {
        "target_id": "hyp_jogged",
        "mutation": {
            "id": "hyp_jogged",
            "valid_from": (t0 + ms(5)).isoformat(),
            "valid_to":   (t0 + ms(8)).isoformat(),
            "recorded_at": (t0 + ms(5)).isoformat(),
            "type": "hypothetical_action",
            "data": {"action": "jog_through"},
            "causal_links": ["t1_infer"],
        },
    })
    assert r4["ok"], r4
    hyp_causes = [c["id"] for c in r4["result"]["causes"]]
    print(f"  explain_with('hyp_jogged')      -> causes: {hyp_causes}")
    # Real core unaffected
    r4_check = adapter.handle_tool_call("get_event", {"id": "hyp_jogged"})
    assert r4_check["ok"] and r4_check["result"] is None
    print(f"  get_event('hyp_jogged') after   -> null (fork did not mutate)")

    # 5. Lifecycle audit — projections still visible
    r5 = adapter.handle_tool_call("get_projections", {})
    assert r5["ok"], r5
    # Non-refuted projections: after promotion, the originals still live
    # on their branches — wait_for_green and walk_across still have their
    # own-branch copies. Promotion appends to main; it does not delete.
    visible_projs = sorted(e["branch_id"] for e in r5["result"])
    print(f"  get_projections (non-refuted)   -> {visible_projs}")

    print()
    print("Dry run complete. Scenario + adapter wired end-to-end.")
    print("Set ANTHROPIC_API_KEY and re-run to see Claude reason over it.")
    sys.exit(0)

# ----------------------------------------------------------------------
# Real agent loop (same shape as anthropic_demo.py)
# ----------------------------------------------------------------------

client = Anthropic()
messages = [{"role": "user", "content": PROMPT}]

print()
print("=" * 72)
print(f"Running agent loop against {MODEL} (up to {MAX_TURNS} turns)")
print("=" * 72)

for turn in range(MAX_TURNS):
    print(f"\n--- Turn {turn + 1} ---")
    response = client.messages.create(
        model=MODEL,
        max_tokens=4096,
        tools=tools,
        messages=messages,
    )

    for block in response.content:
        if block.type == "text" and block.text.strip():
            print(f"[Claude]\n{block.text}")
        elif block.type == "tool_use":
            args_str = json.dumps(block.input)
            if len(args_str) > 160:
                args_str = args_str[:160] + "..."
            print(f"[tool_use] {block.name}({args_str})")

    if response.stop_reason == "end_turn":
        break
    if response.stop_reason != "tool_use":
        print(f"[stopped unexpectedly: {response.stop_reason}]")
        break

    messages.append({"role": "assistant", "content": response.content})

    tool_results = []
    for block in response.content:
        if block.type != "tool_use":
            continue
        try:
            result = adapter.handle_tool_call(block.name, block.input)
        except Exception as exc:  # noqa: BLE001
            result = {"ok": False, "error": f"adapter raised: {exc}"}
        payload = json.dumps(result)
        preview = payload[:200] + ("..." if len(payload) > 200 else "")
        print(f"[tool_result {block.name}] {preview}")
        tool_results.append({
            "type": "tool_result",
            "tool_use_id": block.id,
            "content": payload,
        })

    messages.append({"role": "user", "content": tool_results})

print()
print("=" * 72)
print("6-tick traffic light demo complete.")
print("=" * 72)
