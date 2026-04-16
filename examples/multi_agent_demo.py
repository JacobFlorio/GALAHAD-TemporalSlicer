#!/usr/bin/env python3
"""GALAHAD v0.3.0 Multi-Agent Scenario.

Two agents (Network Ops + Security) share a GALAHAD temporal store.
Each perceives distinct events and projects competing futures. The
scenario forces cross-agent projection interaction, shared causal
reasoning, and counterfactual queries across agent boundaries.

This is the first demo to exercise the v0.3.0 anomaly detection
(ConsciousMem2 confidence decay + co-occurrence) alongside the full
existing surface (bitemporal replay, Allen relations, branching
projections, counterfactuals).

Run modes:

    # Dry run (no API call; exercises full stack end-to-end):
    PYTHONPATH=build python3 examples/multi_agent_demo.py

    # Real run:
    pip install anthropic
    export ANTHROPIC_API_KEY=sk-...
    PYTHONPATH=build python3 examples/multi_agent_demo.py

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
    print("  PYTHONPATH=build python3 examples/multi_agent_demo.py",
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
# Populate a multi-agent scenario
# ----------------------------------------------------------------------

core = galahad.TemporalCore()
engine = galahad.TemporalEngine(core)
adapter = galahad.LLMToolAdapter(core, engine)
detector = galahad.AnomalyDetector(core)

utc = timezone.utc
t0 = datetime(2026, 4, 15, 3, 0, 0, tzinfo=utc)


def ms(n):
    return timedelta(milliseconds=n)


def sec(n):
    return timedelta(seconds=n)


def mk(id_, start, end, recorded, type_, data=None, links=None,
       branch="main", confidence=1.0):
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


# --- Agent A: Network Ops ---
# Observes traffic spike, then DNS anomaly, then projects scale-up.

core.add_event(mk("net_spike", t0, t0 + sec(10), t0,
                   "perceive", {"agent": "network", "metric": "traffic_spike",
                                "magnitude": "3.2x_baseline"}))

core.add_event(mk("net_dns_flood", t0 + sec(5), t0 + sec(15), t0 + sec(5),
                   "perceive", {"agent": "network", "metric": "dns_query_flood",
                                "qps": "12000"},
                   ["net_spike"]))

core.add_event(mk("net_infer_ddos", t0 + sec(15), t0 + sec(20), t0 + sec(15),
                   "inference", {"agent": "network", "hypothesis": "possible_ddos",
                                 "confidence": "0.6"},
                   ["net_spike", "net_dns_flood"]))

# Network agent projects two futures:
#   scale: auto-scale to absorb the traffic
#   ratelimit: rate-limit at the edge
fut_scale = mk("fut_scale_up", t0 + sec(30), t0 + sec(60), t0 + sec(20),
               "projection", {"agent": "network", "action": "auto_scale",
                              "cost": "high"},
               ["net_infer_ddos"], branch="scale", confidence=0.7)
core.add_projection(fut_scale)

fut_ratelimit = mk("fut_ratelimit", t0 + sec(25), t0 + sec(55), t0 + sec(20),
                    "projection", {"agent": "network", "action": "edge_ratelimit",
                                   "cost": "low"},
                    ["net_infer_ddos"], branch="ratelimit", confidence=0.5)
core.add_projection(fut_ratelimit)

# --- Agent B: Security ---
# Sees suspicious login overlapping with the traffic spike, infers
# possible breach, projects quarantine.

core.add_event(mk("sec_login", t0 + sec(3), t0 + sec(8), t0 + sec(3),
                   "perceive", {"agent": "security", "alert": "suspicious_login",
                                "source_ip": "198.51.100.42",
                                "target": "admin_panel"}))

core.add_event(mk("sec_lateral", t0 + sec(12), t0 + sec(18), t0 + sec(12),
                   "perceive", {"agent": "security", "alert": "lateral_movement",
                                "from": "webserver_01", "to": "db_primary"},
                   ["sec_login"]))

core.add_event(mk("sec_infer_breach", t0 + sec(18), t0 + sec(22), t0 + sec(18),
                   "inference", {"agent": "security", "hypothesis": "active_breach",
                                 "confidence": "0.8"},
                   ["sec_login", "sec_lateral"]))

# Security agent projects quarantine of compromised hosts.
fut_quarantine = mk("fut_quarantine", t0 + sec(25), t0 + sec(55), t0 + sec(22),
                     "projection", {"agent": "security", "action": "quarantine_hosts",
                                    "targets": "webserver_01,db_primary"},
                     ["sec_infer_breach"], branch="quarantine", confidence=0.8)
core.add_projection(fut_quarantine)

# --- Cross-agent causal link ---
# Security agent also links to the network spike as corroborating evidence.
core.add_event(mk("cross_correlation", t0 + sec(20), t0 + sec(25), t0 + sec(20),
                   "inference", {"agent": "joint", "finding": "spike_and_breach_correlated",
                                 "implication": "ddos_may_be_cover_for_breach"},
                   ["net_infer_ddos", "sec_infer_breach"]))

# --- Decisions ---
# After cross-correlation, the ratelimit projection is refuted (too weak
# against an active breach). Scale-up is also questionable if the traffic
# is attacker-generated. Quarantine is the favored response.
core.refute_branch("ratelimit")

print("GALAHAD v0.3.0 Multi-Agent Scenario populated:")
print("  Agent A (Network Ops):")
print("    main: net_spike -> net_dns_flood -> net_infer_ddos")
print("    branch 'scale':     fut_scale_up     (confidence 0.7)")
print("    branch 'ratelimit': fut_ratelimit    (confidence 0.5) [REFUTED]")
print("  Agent B (Security):")
print("    main: sec_login -> sec_lateral -> sec_infer_breach")
print("    branch 'quarantine': fut_quarantine  (confidence 0.8)")
print("  Cross-agent:")
print("    main: cross_correlation (cites both net_infer_ddos + sec_infer_breach)")
print(f"  Anchor time t0 = {t0.isoformat()}")
print()

tools = adapter.get_tool_schemas()
print(f"Registered {len(tools)} GALAHAD tools with Claude.")

PROMPT = f"""I have a GALAHAD v0.3.0 temporal reasoning engine populated with a
multi-agent incident response scenario. Two agents share the same
temporal store:

**Agent A (Network Ops)** observed a traffic spike and DNS flood,
inferred a possible DDoS, and projected two futures: auto-scale
(branch "scale", confidence 0.7) and edge rate-limiting (branch
"ratelimit", confidence 0.5, now REFUTED).

**Agent B (Security)** observed a suspicious admin login and lateral
movement, inferred an active breach, and projected quarantine of
compromised hosts (branch "quarantine", confidence 0.8).

**Cross-agent link**: A joint correlation event on main links both
agents' inferences, concluding the DDoS may be cover for the breach.

The ratelimit branch has been refuted (too weak against active breach).

Anchor time t0 = {t0.isoformat()}.

Use GALAHAD tools to investigate this scenario. Walk through these
question shapes:

1. **SHARED CAUSAL PICTURE** — What is the full causal graph? Start
   from cross_correlation and walk backward through both agents' chains.
   Show how two independent perception streams converge.

2. **PROJECTION INTERACTION** — List all active projections. Which
   agent's projection conflicts with which? If quarantine is promoted,
   does scale-up still make sense? Use explain_with to test.

3. **COUNTERFACTUAL** — What did the refuted ratelimit branch predict?
   Use why_not to see what would have happened. Walk the would-have-been
   causal chain.

4. **BITEMPORAL REPLAY** — Query at as_of = {(t0 + sec(10)).isoformat()}.
   At that moment, which agent knew what? Had the cross-correlation been
   recorded yet? Show how the picture differs from the current state.

5. **ALLEN INTERVAL ANALYSIS** — Which events temporally overlap? The
   traffic spike and suspicious login happen in overlapping intervals.
   Find them using Allen relations. What does this co-occurrence mean
   for the incident timeline?

After the tool rounds, synthesize a natural-language incident report
that a NOC manager could act on. The report should name the moment of
commitment (when the agents' independent streams converged into a
joint conclusion) and recommend whether to promote the quarantine
branch or the scale branch — with reasoning grounded in the causal
and counterfactual evidence you gathered."""

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
    print("Smoke-testing the GALAHAD tool dispatch path directly:")

    # 1. Shared causal picture: explain cross_correlation
    r1 = adapter.handle_tool_call("explain", {"id": "cross_correlation"})
    assert r1["ok"] is True, r1
    cause_ids = [c["id"] for c in r1["result"]["causes"]]
    print(f"  explain('cross_correlation')    -> {len(cause_ids)} causes: {cause_ids}")
    # Should trace back through both agents' chains.
    assert "net_infer_ddos" in cause_ids or "sec_infer_breach" in cause_ids

    # 2. Projections: list active projections
    r2 = adapter.handle_tool_call("get_projections", {})
    assert r2["ok"] is True, r2
    proj_ids = [p["id"] for p in r2["result"]]
    print(f"  get_projections()               -> {proj_ids}")
    assert "fut_ratelimit" not in proj_ids  # refuted branch filtered out

    # 3. Counterfactual: why_not on the refuted ratelimit projection
    r3 = adapter.handle_tool_call("why_not", {"id": "fut_ratelimit"})
    assert r3["ok"] is True, r3
    assert len(r3["result"]) == 1
    assert r3["result"][0]["branch"] == "ratelimit"
    wh_causes = [c["id"] for c in r3["result"][0]["would_have_been_causes"]]
    print(f"  why_not('fut_ratelimit')        -> branch=ratelimit, "
          f"causes={wh_causes}")

    # 4. Bitemporal replay at t0+10s
    as_of_iso = (t0 + sec(10)).isoformat()
    r4 = adapter.handle_tool_call("explain", {"id": "cross_correlation",
                                              "as_of": as_of_iso})
    assert r4["ok"] is True, r4
    past_causes = [c["id"] for c in r4["result"]["causes"]]
    print(f"  explain('cross_correlation', as_of=t0+10s) -> {len(past_causes)} causes: {past_causes}")
    # cross_correlation was recorded at t0+20s, so at t0+10s it shouldn't exist yet.
    assert len(past_causes) == 0

    # 5. Allen: find events overlapping with net_spike
    r5 = adapter.handle_tool_call("find_related",
                                  {"id": "net_spike", "relation": "overlapped_by"})
    assert r5["ok"] is True, r5
    print(f"  find_related('net_spike', overlapped_by) -> {r5['result']}")

    # Also test overlaps direction
    r5b = adapter.handle_tool_call("find_related",
                                   {"id": "net_spike", "relation": "overlaps"})
    print(f"  find_related('net_spike', overlaps)      -> {r5b['result']}")

    # 6. Anomaly detection: co-occurrence break smoke test
    baseline = galahad.TimeWindow(t0, t0 + sec(20))
    current = galahad.TimeWindow(t0 + sec(50), t0 + sec(80))
    co_results = detector.detect_co_occurrence_break(baseline, current, 1)
    print(f"  detect_co_occurrence_break()    -> {len(co_results)} breaks")
    for cr in co_results[:3]:
        print(f"    {cr.description[:80]}...")

    # 7. Confidence decay
    decay_cfg = galahad.network_decay()
    decay_results = detector.detect_confidence_decay(
        galahad.TimeWindow(t0, t0 + sec(60)),
        t0 + timedelta(days=3),  # 3 days later
        0.5,
        decay_cfg)
    print(f"  detect_confidence_decay(3d)     -> {len(decay_results)} decayed events")

    print()
    print("Dry run complete. Full multi-agent stack wired end-to-end.")
    print("Set ANTHROPIC_API_KEY and re-run to see Claude reason over it.")
    sys.exit(0)

# ----------------------------------------------------------------------
# Real agent loop
# ----------------------------------------------------------------------

client = Anthropic()
messages = [{"role": "user", "content": PROMPT}]

print()
print("=" * 72)
print(f"Running multi-agent scenario against {MODEL} (up to {MAX_TURNS} turns)")
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
print("Multi-agent demo complete.")
print("Claude reasoned across two agents' causal streams,")
print("competing projections, and counterfactual branches.")
print("=" * 72)
