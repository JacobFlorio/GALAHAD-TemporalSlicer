#!/usr/bin/env python3
"""GALAHAD + Anthropic SDK demo.

An end-to-end demonstration of Claude reasoning over GALAHAD's
temporal substrate via JSON tool calls. Pre-populates a small agent
trace (perceive -> infer -> decide -> act) with a deliberately late-
arriving action so the bitemporal replay question is non-trivial,
then lets Claude inspect it through the LLM tool-call adapter.

This is the smallest working demo that exercises the full stack:

    TemporalCore -> TemporalEngine -> LLMToolAdapter
        -> Python bindings -> Anthropic SDK -> Claude

Requires the galahad Python module to be built:

    cmake -B build && cmake --build build -j

Run modes:

    # Dry run (no API call; verifies GALAHAD + adapter end-to-end):
    PYTHONPATH=build python3 examples/anthropic_demo.py

    # Real run:
    pip install anthropic
    export ANTHROPIC_API_KEY=sk-...
    PYTHONPATH=build python3 examples/anthropic_demo.py

Set GALAHAD_DEMO_DRY_RUN=1 to force dry-run even with an API key.
"""

import json
import os
import sys
from datetime import datetime, timedelta, timezone

# ---- Imports with helpful errors ----

try:
    import galahad
except ImportError:
    print("ERROR: galahad Python module not found on sys.path.",
          file=sys.stderr)
    print("Build it first:", file=sys.stderr)
    print("  cmake -B build && cmake --build build -j", file=sys.stderr)
    print("Then run:", file=sys.stderr)
    print("  PYTHONPATH=build python3 examples/anthropic_demo.py",
          file=sys.stderr)
    sys.exit(1)

try:
    from anthropic import Anthropic
    HAS_ANTHROPIC = True
except ImportError:
    HAS_ANTHROPIC = False

MODEL = "claude-opus-4-6"
MAX_TURNS = 12
API_KEY = os.getenv("ANTHROPIC_API_KEY")
DRY_RUN = bool(os.getenv("GALAHAD_DEMO_DRY_RUN")) or not (HAS_ANTHROPIC and API_KEY)

# ----------------------------------------------------------------------
# Populate a small agent trace
# ----------------------------------------------------------------------

core = galahad.TemporalCore()
engine = galahad.TemporalEngine(core)
adapter = galahad.LLMToolAdapter(core, engine)

t0 = datetime.now(timezone.utc)


def ms(n):
    return timedelta(milliseconds=n)


def mk(id_, start, end, recorded, type_, data=None, links=None, branch="main"):
    e = galahad.TemporalEvent()
    e.id = id_
    e.valid_from = start
    e.valid_to = end
    e.recorded_at = recorded
    e.type = type_
    e.data = data or {}
    e.causal_links = links or []
    e.branch_id = branch
    return e


# Canonical GALAHAD demo trace. Four ground-truth events on main plus
# two competing projection branches plus a refutation, so the scenario
# exercises all four question shapes we want Claude to walk through:
#
#   main timeline (ground truth):
#     perceive [t0, t0+5ms]                  obs=door_open
#     infer    [t0+5ms, t0+15ms]             why=wind
#     decide   [t0+15ms, t0+20ms]            choose=close
#     act      [t0+20ms, t0+30ms]            do=close
#         ^ late-arriving: recorded_at = t0+200ms.
#           A bitemporal query at as_of=t0+100ms won't see this.
#
#   projection branches (what the agent predicted before acting):
#     close_door / fut_close  — confidence 0.7, depends on decide
#     ignore     / fut_ignore — confidence 0.3, depends on decide  [REFUTED]
#
# The `ignore` branch is refuted to represent "the agent ruled out
# inaction." This makes fut_ignore the counterfactual target: it was
# once predicted, but the refutation means we're saying it didn't and
# won't happen. why_not("fut_ignore") returns the ignore branch + the
# would-have-been causal chain.
core.add_event(mk("perceive", t0,           t0 + ms(5),  t0,
                  "perception", {"obs": "door_open"}))
core.add_event(mk("infer",    t0 + ms(5),   t0 + ms(15), t0 + ms(5),
                  "inference",  {"why": "wind"},
                  ["perceive"]))
core.add_event(mk("decide",   t0 + ms(15),  t0 + ms(20), t0 + ms(15),
                  "decision",   {"choose": "close"},
                  ["infer"]))
core.add_event(mk("act",      t0 + ms(20),  t0 + ms(30), t0 + ms(200),
                  "action",     {"do": "close"},
                  ["decide"]))

# Two competing projections at t0+15ms (the moment of decision).
fut_close = mk("fut_close",
               t0 + ms(20), t0 + ms(30), t0 + ms(15),
               "projected_action", {"do": "close"},
               ["decide"], branch="close_door")
fut_close.confidence = 0.7
core.add_projection(fut_close)

fut_ignore = mk("fut_ignore",
                t0 + ms(20), t0 + ms(30), t0 + ms(15),
                "projected_action", {"do": "wait"},
                ["decide"], branch="ignore")
fut_ignore.confidence = 0.3
core.add_projection(fut_ignore)

# Observation confirms the agent closed the door, so the `ignore`
# projection is refuted. fut_ignore still exists in GALAHAD's memory,
# but it's marked as "this was once predicted and then ruled out" —
# which is exactly the counterfactual case why_not() is for.
core.refute_branch("ignore")

print("GALAHAD populated:")
print("  main      : 4 events (perceive -> infer -> decide -> act)")
print("  close_door: 1 projection (fut_close, confidence 0.7)")
print("  ignore    : 1 projection (fut_ignore, confidence 0.3) [REFUTED]")
print(f"Anchor time t0 = {t0.isoformat()}")
print("Note: 'act' occurred at t0+20ms but was recorded at t0+200ms.")
print("Note: 'ignore' branch is refuted — fut_ignore is the counterfactual.\n")

tools = adapter.get_tool_schemas()
print(f"Registered {len(tools)} GALAHAD tools with Claude.")

PROMPT = f"""I have a GALAHAD v0.2.0 temporal reasoning engine populated with events
from a small agent that observed an open door and chose to close it.
The anchor time (t0) is {t0.isoformat()}.

The scenario already contains ground-truth events and projection
branches — you do not need to add anything. Events on the `main` branch
are the actual timeline. Events on the `close_door` branch are a
projection that is still open. Events on the `ignore` branch are a
projection that has been refuted (the agent ruled out inaction).

Use the GALAHAD tools to walk through FOUR question shapes and show
your reasoning through the tool calls you make. Each question is
designed to reach for a different part of the surface. Pick the tool
you think fits best; do not just use the same tool four times.

1. BACKWARD CAUSAL — Why did the event "act" happen? Walk through the
   causal chain.

2. BITEMPORAL REPLAY — What would the system have believed at exactly
   t0+100ms ({(t0 + ms(100)).isoformat()})? Would it have known that
   "act" happened at that transaction time? Compare to question 1. The
   difference between the two answers is the bitemporal distinction
   between "when something happened in the world" (valid time) and
   "when the system learned of it" (transaction time).

3. COUNTERFACTUAL — What did the refuted `ignore` branch predict? The
   projected event on that branch is called "fut_ignore". Ask the
   engine what the refuted branch contained AND walk its would-have-
   been causal chain. (Hint: there's a tool called `why_not` that
   returns exactly this shape — the refuted-branch event + its causal
   ancestry walked across all branches.)

4. HYPOTHETICAL MUTATION — Suppose a different event "hyp_slam" had
   been added, one that cites "decide" as its cause, with valid_from
   around t0+25ms. What would its causal explanation look like if it
   had happened, without actually committing it to the store? (Hint:
   there's a tool called `explain_with` that clones the core, applies
   a mutation, and runs explain against the fork.)

After the four tool rounds, give a clear natural-language summary that
articulates the difference between each question's shape — backward,
bitemporal, counterfactual, hypothetical — in plain English. The
summary is the artifact we care about."""

# ----------------------------------------------------------------------
# Dry run: verify GALAHAD + adapter work without calling the API
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
    print(f"  First tool name:          {tools[0]['name']}")
    print(f"  Prompt length:            {len(PROMPT)} characters")
    print()
    print("Smoke-testing the GALAHAD tool dispatch path directly:")

    r1 = adapter.handle_tool_call("explain", {"id": "act"})
    assert r1["ok"] is True, r1
    assert len(r1["result"]["causes"]) == 3
    print(f"  explain('act')                  -> "
          f"{len(r1['result']['causes'])} causes "
          f"({', '.join(c['id'] for c in r1['result']['causes'])})")

    as_of_iso = (t0 + ms(100)).isoformat()
    r2 = adapter.handle_tool_call("explain",
                                  {"id": "act", "as_of": as_of_iso})
    assert r2["ok"] is True, r2
    assert len(r2["result"]["causes"]) == 0
    print(f"  explain('act', as_of=t0+100ms)  -> "
          f"{len(r2['result']['causes'])} causes "
          f"(empty — the action was not recorded yet)")

    r3 = adapter.handle_tool_call("find_related",
                                  {"id": "perceive", "relation": "meets"})
    assert r3["ok"] is True, r3
    print(f"  find_related('perceive', meets) -> {r3['result']}")

    # Counterfactual smoke tests (new in v0.2.0)
    r4 = adapter.handle_tool_call("why_not", {"id": "fut_ignore"})
    assert r4["ok"] is True, r4
    assert len(r4["result"]) == 1
    assert r4["result"][0]["branch"] == "ignore"
    wh_causes = [c["id"] for c in r4["result"][0]["would_have_been_causes"]]
    print(f"  why_not('fut_ignore')           -> branch={r4['result'][0]['branch']}, "
          f"causes={wh_causes}")

    r5 = adapter.handle_tool_call("why_not", {"id": "act"})
    assert r5["ok"] is True, r5
    assert len(r5["result"]) == 0
    print(f"  why_not('act')                  -> [] (actually happened)")

    r6 = adapter.handle_tool_call("explain_with", {
        "target_id": "hyp_slam",
        "mutation": {
            "id": "hyp_slam",
            "valid_from": (t0 + ms(25)).isoformat(),
            "valid_to":   (t0 + ms(35)).isoformat(),
            "recorded_at": (t0 + ms(25)).isoformat(),
            "type": "hypothetical_action",
            "data": {"do": "slam"},
            "causal_links": ["decide"],
        },
    })
    assert r6["ok"] is True, r6
    hyp_causes = [c["id"] for c in r6["result"]["causes"]]
    print(f"  explain_with(hyp_slam)          -> causes={hyp_causes}")
    # Real core must not contain the hypothetical event
    r7 = adapter.handle_tool_call("get_event", {"id": "hyp_slam"})
    assert r7["ok"] is True and r7["result"] is None, \
        "hyp_slam must not exist in the real core after explain_with"

    print()
    print("Dry run complete. GALAHAD + adapter + counterfactuals wired end-to-end.")
    print("Set ANTHROPIC_API_KEY and re-run to see Claude reason over it.")
    sys.exit(0)

# ----------------------------------------------------------------------
# Real agent loop
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
        max_tokens=2048,
        tools=tools,
        messages=messages,
    )

    for block in response.content:
        if block.type == "text" and block.text.strip():
            print(f"[Claude]\n{block.text}")
        elif block.type == "tool_use":
            args_str = json.dumps(block.input)
            if len(args_str) > 140:
                args_str = args_str[:140] + "..."
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
        preview = payload[:180] + ("..." if len(payload) > 180 else "")
        print(f"[tool_result {block.name}] {preview}")
        tool_results.append({
            "type": "tool_result",
            "tool_use_id": block.id,
            "content": payload,
        })

    messages.append({"role": "user", "content": tool_results})

print()
print("=" * 72)
print("Demo complete. Claude reasoned over GALAHAD's temporal substrate.")
print("=" * 72)
