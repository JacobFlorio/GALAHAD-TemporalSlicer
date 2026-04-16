# Multi-Agent Incident Response Demo — Live Transcript

**GALAHAD version:** v0.3.0 (cross-pollination branch)
**Model:** Claude Opus 4.6 (`claude-opus-4-6`)
**Date:** 2026-04-16
**Tools registered:** 28 (22 core + 6 anomaly detection)
**Turns:** 16 of 25 max

## Scenario

Two agents (Network Ops + Security) share a GALAHAD temporal store.
Network perceives traffic spike + DNS flood, infers DDoS, projects
scale-up and rate-limiting. Security perceives suspicious login +
lateral movement, infers active breach, projects quarantine. A joint
cross-correlation event links both inferences, concluding the DDoS is
cover for the breach. Quarantine promoted, scale + ratelimit refuted.
A confirmation event validates the decision.

## Key Findings

1. **Shared Causal Graph**: `explain(confirm_quarantine)` traced 12
   ancestors spanning both agents' full perception-inference chains,
   converging at `cross_correlation` (t0+14s).

2. **Counterfactual Sweep**: `why_not` on both refuted branches revealed
   identical single-agent causal roots (`infer_ddos` <- `net_spike` +
   `dns_flood`). Scale refutation was more consequential — it would have
   amplified the attacker's exfiltration bandwidth.

3. **Bitemporal Replay**: At `as_of=t0+10s`, the system knew about raw
   perceptions from both agents but had NOT yet synthesized
   `infer_breach`, `cross_correlation`, or `confirm_quarantine`. A
   5-second blind window (t+9s to t+14s) existed where only the
   refuted single-agent mitigations were on the table.

4. **Allen Interval Analysis**: `suspicious_login DURING net_spike`
   proves temporal coordination — the unauthorized login fell entirely
   within the DDoS window. `dns_flood overlaps lateral_movement` shows
   the DNS amplification provided continuous cover for internal pivoting.

5. **Anomaly Detection** (v0.3.0 tools, first live use):
   - Inference rate spiked 2.67x between baseline and current windows
   - `confirm_quarantine` triggered max-severity frequency spike (new
     activity type — 0 decisions in baseline)
   - Loitering detection flagged `net_spike` and `dns_flood` (10s
     duration exceeding 8s threshold)
   - Confidence decay: `net_spike` drops from 0.95 to 0.604 after 2
     days with network preset (172800s half-life). With 3 reinforcing
     observations: 0.708.

## Incident Report Produced

Claude synthesized a full NOC-grade incident report including:
- Causal graph visualization (ASCII art)
- Counterfactual comparative analysis (scale vs ratelimit)
- Bitemporal belief-state evolution table
- Allen interval coordination fingerprint
- Confidence decay projections for ongoing monitoring
- 4 actionable recommendations grounded in tool results

## Raw Tool Call Log

### Turn 1
```
[tool_use] now({})
-> {"iso": "2026-04-16T05:40:20.875Z", "ns": 1776318020875000973}
```

### Turns 2-7: Scenario Population
Claude populated the full event graph in dependency order:
- Turn 2: Agent A perceptions (net_spike, dns_flood) + inference (infer_ddos)
- Turn 3: Agent B perceptions (suspicious_login, lateral_movement) + inference (infer_breach)
- Turn 4: Cross-correlation + confirm_quarantine
- Turn 5: Scale + ratelimit projections (with outcomes)
- Turn 6: Quarantine projections (with outcomes)
- Turn 7: refute_branch(scale), refute_branch(ratelimit), promote_branch(quarantine)

### Turn 8: Shared Causal Picture
```
[tool_use] explain({"id": "confirm_quarantine", "require_completed_before": false})
-> 12 causes spanning both agents' chains

[tool_use] get_ancestors({"id": "confirm_quarantine"})
-> [cross_correlation, net_infer_ddos, sec_infer_breach, infer_ddos,
    infer_breach, net_spike, net_dns_flood, sec_login, sec_lateral,
    dns_flood, suspicious_login, lateral_movement]
```

### Turn 9: Counterfactual Sweep
```
[tool_use] why_not({"id": "proj_autoscale"})
-> branch=scale, causes=[infer_ddos, net_spike, dns_flood]

[tool_use] why_not({"id": "proj_ratelimit"})
-> branch=ratelimit, causes=[infer_ddos, net_spike, dns_flood]

[tool_use] why_not({"id": "proj_scale_outcome"})
-> branch=scale, outcome: "DDoS absorbed but breach continues"

[tool_use] why_not({"id": "proj_ratelimit_outcome"})
-> branch=ratelimit, outcome: "Rate limiting irrelevant to lateral movement"
```

### Turns 10-11: Bitemporal Replay
```
[tool_use] what_happened_during(as_of=t0+10s)
-> [net_spike, sec_login, net_dns_flood, dns_flood, suspicious_login,
    lateral_movement, sec_lateral, infer_ddos]
   (8 events known — no cross-correlation, no breach inference, no quarantine)

[tool_use] get_event("cross_correlation", as_of=t0+10s)
-> null (not recorded yet)

[tool_use] what_happened_during(as_of=t0+15s)
-> Full picture including infer_breach, cross_correlation, proj_quarantine
```

### Turn 12: Allen Interval Analysis
```
[tool_use] find_related("net_spike", overlaps)
-> [dns_flood, net_dns_flood, lateral_movement]

[tool_use] find_related("net_spike", contains)
-> [suspicious_login, sec_login, infer_ddos, infer_breach]

[tool_use] find_related("suspicious_login", during)
-> [net_spike] (coordination fingerprint)

[tool_use] find_related("dns_flood", overlaps)
-> [net_dns_flood, lateral_movement, proj_quarantine, sec_lateral]
```

### Turns 13-14: Anomaly Detection
```
[tool_use] detect_frequency_anomaly(baseline=t0..t0+10s, current=t0+10s..t0+25s)
-> inference events spiked 2.67x (severity 0.89)

[tool_use] detect_frequency_anomaly(event_type="decision")
-> new activity (severity 1.0) — confirm_quarantine

[tool_use] detect_loitering(max_duration=8s, event_type="perception")
-> net_spike (10s, severity 0.625), dns_flood (10s, severity 0.625)

[tool_use] compute_decay(elapsed=172800s, half_life=172800s, obs=1)
-> 0.6040

[tool_use] compute_decay(elapsed=172800s, half_life=172800s, obs=3)
-> 0.7079
```

### Turn 15: Branch Verification
```
[tool_use] get_refuted_branches()
-> ["scale", "ratelimit"]

[tool_use] get_projections()
-> [fut_quarantine, proj_quarantine, proj_quarantine_outcome] (promoted)

[tool_use] get_projections(branch="scale")
-> [fut_scale_up, proj_autoscale, proj_scale_outcome] (refuted but visible)
```

### Turn 16: Final Synthesis
Claude produced the full incident report (see above).
