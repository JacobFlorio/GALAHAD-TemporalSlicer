#!/usr/bin/env python3
"""GALAHAD v0.3.0 Multi-Agent Scenario.

Two agents (Network Ops + Security) share a GALAHAD temporal store.
Each perceives distinct events and projects competing futures. The
scenario forces cross-agent projection interaction, shared causal
reasoning, and counterfactual queries across agent boundaries.

This is the first demo to exercise the v0.3.0 anomaly detection
(confidence decay + co-occurrence) alongside the full
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
MAX_TURNS = 25
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

# --- Decision point ---
# After cross-correlation concludes DDoS is cover for breach:
#   - ratelimit refuted (too weak against active breach)
#   - scale refuted (scaling attacker traffic is counterproductive)
#   - quarantine promoted to ground truth (correct response to breach)
core.refute_branch("ratelimit")
core.refute_branch("scale")
core.promote_branch("quarantine")

# Post-promotion: the quarantine action becomes ground truth on main.
# A late-arriving confirmation event validates the decision.
core.add_event(mk("confirm_quarantine", t0 + sec(35), t0 + sec(40), t0 + sec(35),
                   "confirmation", {"agent": "joint", "result": "breach_contained",
                                    "hosts_isolated": "webserver_01,db_primary"},
                   ["cross_correlation"]))

print("GALAHAD v0.3.0 Multi-Agent Scenario populated:")
print("  Agent A (Network Ops):")
print("    main: net_spike -> net_dns_flood -> net_infer_ddos")
print("    branch 'scale':     fut_scale_up     (confidence 0.7) [REFUTED]")
print("    branch 'ratelimit': fut_ratelimit    (confidence 0.5) [REFUTED]")
print("  Agent B (Security):")
print("    main: sec_login -> sec_lateral -> sec_infer_breach")
print("    branch 'quarantine': fut_quarantine  (confidence 0.8) [PROMOTED]")
print("  Cross-agent:")
print("    main: cross_correlation -> confirm_quarantine")
print(f"  Anchor time t0 = {t0.isoformat()}")
print()

tools = adapter.get_tool_schemas()
print(f"Registered {len(tools)} GALAHAD tools with Claude.")

PROMPT = f"""I have a GALAHAD v0.3.0 temporal reasoning engine populated with a
multi-agent incident response scenario. Two agents share the same
temporal store:

**Agent A (Network Ops)** observed a traffic spike and DNS flood,
inferred a possible DDoS, and projected two futures: auto-scale
(branch "scale") and edge rate-limiting (branch "ratelimit"). Both
are now REFUTED — scaling attacker traffic is counterproductive, and
rate-limiting is too weak against an active breach.

**Agent B (Security)** observed a suspicious admin login and lateral
movement, inferred an active breach, and projected quarantine of
compromised hosts (branch "quarantine"). This branch has been
PROMOTED to ground truth.

**Cross-agent link**: A joint correlation event links both agents'
inferences, concluding the DDoS was cover for the breach. A
confirmation event validates the quarantine decision.

Anchor time t0 = {t0.isoformat()}.

Use GALAHAD tools to investigate this scenario. Walk through these
question shapes, using the tool that fits each question best:

1. **SHARED CAUSAL PICTURE** — Start from confirm_quarantine and walk
   backward through the full causal graph. Show how two independent
   perception streams converged into a joint decision.

2. **COUNTERFACTUAL SWEEP** — Both scale and ratelimit were refuted.
   Use why_not on both to see what each would have predicted. Compare
   the would-have-been causal chains. Which refutation was more
   consequential?

3. **BITEMPORAL REPLAY** — Query at as_of = {(t0 + sec(10)).isoformat()}.
   At that transaction time, which agent knew what? Had the cross-
   correlation been recorded? Had quarantine been promoted? Show how
   the system's belief state evolved.

4. **ALLEN INTERVAL ANALYSIS** — The traffic spike and suspicious login
   happen in overlapping intervals. Find all temporal co-occurrences
   using Allen relations. What does the overlap pattern reveal about
   attack coordination?

5. **ANOMALY DETECTION** — Use the new v0.3.0 anomaly tools:
   - detect_frequency_anomaly: compare baseline (t0 to t0+10s) vs
     current (t0+10s to t0+25s) — is there a spike?
   - detect_co_occurrence_break: do perception events that co-occurred
     in baseline stop co-occurring in a later window?
   - compute_decay: what is the confidence of the original net_spike
     event after 2 days with the network decay preset (172800s half-life)?

After the tool rounds, synthesize a **full incident report** for a
NOC manager. The report must:
- Name the moment of commitment (when streams converged)
- Explain why quarantine was promoted over scale/ratelimit
- Ground every claim in specific tool results
- Include the confidence decay projection for ongoing monitoring"""

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

    # 1. Shared causal picture: explain confirm_quarantine
    r1 = adapter.handle_tool_call("explain", {"id": "confirm_quarantine"})
    assert r1["ok"] is True, r1
    cause_ids = [c["id"] for c in r1["result"]["causes"]]
    print(f"  explain('confirm_quarantine')   -> {len(cause_ids)} causes: {cause_ids}")
    assert "cross_correlation" in cause_ids

    # 2. Projections: all refuted or promoted — no active projections
    r2 = adapter.handle_tool_call("get_projections", {})
    assert r2["ok"] is True, r2
    proj_ids = [p["id"] for p in r2["result"]]
    print(f"  get_projections()               -> {proj_ids}")

    # 3. Counterfactual: why_not on both refuted projections
    r3a = adapter.handle_tool_call("why_not", {"id": "fut_ratelimit"})
    assert r3a["ok"] is True, r3a
    assert len(r3a["result"]) == 1
    assert r3a["result"][0]["branch"] == "ratelimit"
    wh_a = [c["id"] for c in r3a["result"][0]["would_have_been_causes"]]
    print(f"  why_not('fut_ratelimit')        -> branch=ratelimit, causes={wh_a}")

    r3b = adapter.handle_tool_call("why_not", {"id": "fut_scale_up"})
    assert r3b["ok"] is True, r3b
    assert len(r3b["result"]) == 1
    assert r3b["result"][0]["branch"] == "scale"
    wh_b = [c["id"] for c in r3b["result"][0]["would_have_been_causes"]]
    print(f"  why_not('fut_scale_up')         -> branch=scale, causes={wh_b}")

    # 4. Bitemporal replay at t0+10s
    as_of_iso = (t0 + sec(10)).isoformat()
    r4 = adapter.handle_tool_call("what_happened_during",
                                  {"start": t0.isoformat(),
                                   "end": (t0 + sec(60)).isoformat(),
                                   "as_of": as_of_iso})
    assert r4["ok"] is True, r4
    past_ids = [e["id"] for e in r4["result"]]
    print(f"  what_happened_during(as_of=t0+10s) -> {past_ids}")
    assert "cross_correlation" not in past_ids  # not recorded yet

    # 5. Allen: find events overlapping with net_spike
    r5 = adapter.handle_tool_call("find_related",
                                  {"id": "net_spike", "relation": "overlaps"})
    assert r5["ok"] is True, r5
    print(f"  find_related('net_spike', overlaps)      -> {r5['result']}")

    # 6. Anomaly tools via adapter (new in v0.3.0)
    r6a = adapter.handle_tool_call("detect_frequency_anomaly", {
        "baseline_start": t0.isoformat(),
        "baseline_end": (t0 + sec(10)).isoformat(),
        "current_start": (t0 + sec(10)).isoformat(),
        "current_end": (t0 + sec(25)).isoformat(),
    })
    assert r6a["ok"] is True, r6a
    print(f"  detect_frequency_anomaly()      -> {len(r6a['result'])} anomalies")
    for a in r6a["result"]:
        print(f"    {a['type']}: {a['description'][:70]}...")

    r6b = adapter.handle_tool_call("compute_decay", {
        "elapsed_secs": 172800.0,
        "observation_count": 1,
        "half_life_secs": 172800.0,
    })
    assert r6b["ok"] is True, r6b
    print(f"  compute_decay(2d, 1 obs, 2d hl) -> {r6b['result']:.4f}")

    r6c = adapter.handle_tool_call("detect_co_occurrence_break", {
        "baseline_start": t0.isoformat(),
        "baseline_end": (t0 + sec(20)).isoformat(),
        "current_start": (t0 + sec(30)).isoformat(),
        "current_end": (t0 + sec(50)).isoformat(),
        "min_co_occurrences": 1,
    })
    assert r6c["ok"] is True, r6c
    print(f"  detect_co_occurrence_break()    -> {len(r6c['result'])} breaks")
    for b in r6c["result"][:3]:
        print(f"    {b['description'][:80]}...")

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
        max_tokens=8192,
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
