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


# Canonical GALAHAD demo trace. The late-arriving `act` is the point:
# it *happened* at t0+20ms but was *recorded_at* t0+200ms, so an honest
# bitemporal query at as_of=t0+100ms has no way to know it happened.
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

print("GALAHAD populated with 4 events.")
print(f"Anchor time t0 = {t0.isoformat()}")
print("Note: 'act' occurred at t0+20ms but was recorded at t0+200ms.\n")

tools = adapter.get_tool_schemas()
print(f"Registered {len(tools)} GALAHAD tools with Claude.")

PROMPT = f"""I have a GALAHAD temporal reasoning engine populated with events from a
small agent that observed an open door and chose to close it. The anchor
time (t0) is {t0.isoformat()}.

Use the GALAHAD tools (get_ancestors, explain, get_event, query_range,
etc.) to answer two questions and show your reasoning through the tool
calls you make.

1. Why did the event "act" happen? Walk through the causal chain using
   `explain` and read back what you find.

2. CRITICAL BITEMPORAL QUESTION: What would the system have believed at
   exactly t0+100ms? Would it have known that "act" happened at that
   transaction time? Use `explain` a second time with the `as_of`
   parameter set to t0+100ms ({(t0 + ms(100)).isoformat()}) and compare
   the result to question 1. The difference between the two answers is
   the bitemporal distinction between "when something happened in the
   world" (valid time) and "when the system learned of it" (transaction
   time).

Call the tools step by step. Be concrete about which tool you chose and
what it returned. Then give a clear natural-language summary of what
you learned."""

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

    print()
    print("Dry run complete. GALAHAD + adapter are wired end-to-end.")
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
