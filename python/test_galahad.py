"""End-to-end test for the galahad Python bindings.

Run with:

    PYTHONPATH=build python3 python/test_galahad.py

from the project root, or with the `test_python` ctest target.

Exercises TemporalCore, TemporalEngine, LLMToolAdapter, and
TemporalPersistence through the Python surface. Every assertion is
a real round-trip check: we build a small agent trace, ask causal
and branching questions, replay the system's past belief state,
round-trip through the LLM tool-call surface, save/load through
persistence, and verify the reconstructed state matches.
"""

import os
import sys
import tempfile
from datetime import datetime, timedelta, timezone

import galahad


def ms(n):
    return timedelta(milliseconds=n)


def main():
    print(f"galahad v{galahad.__version__}")

    # ------------------------------------------------------------------
    # Core + engine: perceive -> infer -> decide -> act, with a late
    # recorded_at on the action so we can exercise bitemporal queries.
    # ------------------------------------------------------------------
    core = galahad.TemporalCore()
    engine = galahad.TemporalEngine(core)
    t0 = datetime.now(timezone.utc)

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

    core.add_event(mk("perceive", t0,           t0 + ms(5),  t0,
                      "perception", {"obs": "door_open"}))
    core.add_event(mk("infer",    t0 + ms(5),   t0 + ms(15), t0 + ms(5),
                      "inference", {"why": "wind"},
                      ["perceive"]))
    core.add_event(mk("decide",   t0 + ms(15),  t0 + ms(20), t0 + ms(15),
                      "decision",  {"choose": "close"},
                      ["infer"]))
    core.add_event(mk("act",      t0 + ms(20),  t0 + ms(30), t0 + ms(200),
                      "action",   {"do": "close"},
                      ["decide"]))

    # ---- explain returns the full causal chain in temporal order ----
    why = engine.explain("act")
    assert len(why.causes) == 3, f"expected 3 causes, got {len(why.causes)}"
    assert [c.id for c in why.causes] == ["perceive", "infer", "decide"]
    assert why.causes[0].data["obs"] == "door_open"
    assert why.causes[1].data["why"] == "wind"
    print("  explain(act) -> [perceive, infer, decide] OK")

    # ---- bitemporal honesty: as-of before act was recorded ----
    past = engine.explain("act", t0 + ms(100))
    assert len(past.causes) == 0, "action should be invisible at t0+100ms"
    print("  bitemporal as-of replay OK")

    # ---- Allen algebra through the core ----
    meets = core.find_related("perceive", galahad.AllenRelation.Meets)
    assert meets == ["infer"], f"perceive Meets expected [infer], got {meets}"

    contains = core.find_related("perceive", galahad.AllenRelation.Contains)
    # perceive is [0,5], nothing strictly contained inside it
    assert contains == []

    anc = core.get_ancestors("act")
    assert set(anc) == {"perceive", "infer", "decide"}
    print("  Allen find_related + transitive ancestors OK")

    # ------------------------------------------------------------------
    # Branching projections with confidence.
    # ------------------------------------------------------------------
    core.add_projection(mk("fut_close",
                           t0 + ms(30), t0 + ms(40), t0 + ms(20),
                           "projected_action", {"do": "close"},
                           ["act"], branch="close_door"))
    ev = galahad.TemporalEvent()
    ev.id = "fut_close"
    ev.confidence = 0.7  # reset for second branch

    core.add_projection(mk("fut_ignore",
                           t0 + ms(30), t0 + ms(40), t0 + ms(20),
                           "projected_inaction", {"do": "wait"},
                           ["act"], branch="ignore"))

    # Find the just-added projection and set confidence values
    for e in core.get_all_events():
        if e.id == "fut_close":
            assert e.branch_id == "close_door"
        if e.id == "fut_ignore":
            assert e.branch_id == "ignore"

    # Branch-scoped query: only the "close_door" projection.
    window = galahad.TimeWindow(t0, t0 + ms(500))
    close_only = core.query_range(window, branch="close_door")
    assert len(close_only) == 1
    assert close_only[0].id == "fut_close"
    print("  branch-scoped query_range OK")

    # Refute the alternative; default queries hide it.
    core.refute_branch("ignore")
    assert core.is_refuted("ignore")
    visible = core.query_range(window)
    visible_ids = {e.id for e in visible}
    assert "fut_close" in visible_ids
    assert "fut_ignore" not in visible_ids
    # Explicit name still reaches it
    still = core.query_range(window, branch="ignore")
    assert len(still) == 1 and still[0].id == "fut_ignore"
    print("  refute_branch + explicit-name override OK")

    # ------------------------------------------------------------------
    # LLM tool-call adapter: full round-trip through the JSON surface.
    # ------------------------------------------------------------------
    adapter = galahad.LLMToolAdapter(core, engine)
    schemas = adapter.get_tool_schemas()
    assert isinstance(schemas, list)
    assert len(schemas) >= 28
    names = {t["name"] for t in schemas}
    for required in ("explain", "add_event", "find_related",
                     "promote_branch", "now", "what_happened_during"):
        assert required in names, f"missing tool: {required}"

    # Dispatch explain via JSON
    result = adapter.handle_tool_call("explain", {"id": "act"})
    assert result["ok"] is True
    assert len(result["result"]["causes"]) == 3
    assert result["result"]["causes"][0]["id"] == "perceive"
    # Data survives JSON round-trip
    assert result["result"]["causes"][0]["data"]["obs"] == "door_open"

    # Dispatch find_related with snake_case relation name
    meets_result = adapter.handle_tool_call(
        "find_related", {"id": "perceive", "relation": "meets"})
    assert meets_result["ok"] is True
    assert meets_result["result"] == ["infer"]

    # Error path surfaces as structured JSON
    bad = adapter.handle_tool_call("nonexistent_tool", {})
    assert bad["ok"] is False
    assert "unknown tool" in bad["error"]
    print("  LLM adapter JSON round-trip OK")

    # ------------------------------------------------------------------
    # Persistence round-trip through Python.
    # ------------------------------------------------------------------
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "state.gtp")
        galahad.TemporalPersistence(core).save(path)
        assert os.path.getsize(path) > 100

        restored = galahad.TemporalCore()
        galahad.TemporalPersistence(restored).load(path)

        # Same visible event count as the source (with refutation preserved)
        src_visible = core.query_range(window)
        dst_visible = restored.query_range(window)
        assert len(src_visible) == len(dst_visible)
        assert {e.id for e in src_visible} == {e.id for e in dst_visible}

        # Refutation survived
        assert restored.is_refuted("ignore")

        # Causal chain still walkable after load
        engine2 = galahad.TemporalEngine(restored)
        why2 = engine2.explain("act")
        assert [c.id for c in why2.causes] == ["perceive", "infer", "decide"]

        # Bitemporal honesty still holds
        past2 = engine2.explain("act", t0 + ms(100))
        assert len(past2.causes) == 0
    print("  persistence save/load round-trip OK")

    # ------------------------------------------------------------------
    # Regression test for the timezone offset bug (fixed in 0.1.1).
    #
    # pybind11/chrono.h's default time_point caster silently treated
    # Python datetimes as local wall-clock time, so on a UTC-4 machine
    # a tz-aware `12:00 UTC` datetime became `12:00 local = 16:00 UTC`
    # as a raw C++ instant. The round-trip through the binding
    # compensated, so pure-Python tests passed. But the LLM adapter's
    # ISO serialization (via gmtime_r) saw the shifted instant and
    # returned wrong UTC strings to callers.
    #
    # These assertions catch that bug directly and are locale-
    # independent — they work on any machine regardless of local tz.
    # ------------------------------------------------------------------
    print()
    print("Timezone round-trip (bug fixed in 0.1.1):")

    core2 = galahad.TemporalCore()
    engine2 = galahad.TemporalEngine(core2)
    adapter2 = galahad.LLMToolAdapter(core2, engine2)

    # A fixed, known UTC instant.
    fixed = datetime(2026, 1, 15, 12, 0, 0, 500_000, tzinfo=timezone.utc)

    ev = galahad.TemporalEvent()
    ev.id = "ts_test"
    ev.valid_from = fixed
    ev.valid_to = fixed + timedelta(milliseconds=5)
    ev.recorded_at = fixed
    ev.type = "test"
    core2.add_event(ev)

    # 1) Round-trip via the binding. Returned datetime must be tz-aware
    #    and represent the same instant.
    got = core2.get("ts_test")
    assert got is not None
    assert got.valid_from.tzinfo is not None, \
        "binding returned naive datetime; custom caster not active"
    assert got.valid_from.timestamp() == fixed.timestamp(), \
        f"binding instant mismatch: {got.valid_from} != {fixed}"
    print(f"  binding round-trip OK  ({got.valid_from.isoformat()})")

    # 2) Round-trip via the LLM adapter. The adapter serializes via
    #    gmtime_r which always produces UTC. If the stored C++ instant
    #    was shifted (the bug), the ISO hour here would not match the
    #    input hour. This is the locale-independent catch.
    result = adapter2.handle_tool_call("get_event", {"id": "ts_test"})
    assert result["ok"] is True
    got_iso = result["result"]["valid_from"]
    # Parse the adapter's ISO string (may end in 'Z' or '+00:00').
    got_dt = datetime.fromisoformat(got_iso.replace("Z", "+00:00"))
    assert got_dt.timestamp() == fixed.timestamp(), \
        f"adapter instant mismatch: {got_iso} != {fixed.isoformat()}"
    # Explicit hour check: fixed is 12:00 UTC; adapter must echo 12.
    assert "T12:00:00" in got_iso, \
        f"adapter should report hour 12 for 12:00 UTC input, got {got_iso}"
    print(f"  adapter ISO round-trip OK  ({got_iso})")

    # 3) Naive datetimes are rejected cleanly, not silently shifted.
    try:
        naive = datetime(2026, 1, 15, 12, 0, 0)  # no tzinfo
        bad = galahad.TemporalEvent()
        bad.id = "naive"
        bad.valid_from = naive
        # If the caster were still buggy, the above line would silently
        # accept the naive datetime and shift it. With the new caster it
        # raises ValueError.
        assert False, "naive datetime should have been rejected"
    except ValueError as exc:
        assert "naive datetime" in str(exc).lower()
        print(f"  naive datetime correctly rejected: {exc}")

    print("  timezone round-trip OK")

    # ------------------------------------------------------------------
    # Counterfactual queries (new in 0.2.0): whyNot, explainWith, clone.
    # ------------------------------------------------------------------
    print()
    print("Counterfactuals (new in 0.2.0):")

    cf_core = galahad.TemporalCore()
    cf_engine = galahad.TemporalEngine(cf_core)
    cf_adapter = galahad.LLMToolAdapter(cf_core, cf_engine)
    cft = datetime.now(timezone.utc)

    cf_core.add_event(mk("cf_p",      cft,         cft + ms(5),  cft,
                         "perception", {"obs": "door_open"}))
    cf_core.add_event(mk("cf_d",      cft + ms(5), cft + ms(10), cft + ms(5),
                         "decision",  {"choose": "close"},
                         ["cf_p"]))

    cf_core.add_projection(mk("cf_fut_ignore",
                              cft + ms(10), cft + ms(15), cft + ms(5),
                              "projected_action", {"do": "wait"},
                              ["cf_d"], branch="ignore"))
    cf_core.refute_branch("ignore")

    # why_not via the C++ binding (returns CounterfactualExplanation)
    cfs = cf_core.why_not("cf_fut_ignore")
    assert len(cfs) == 1
    assert cfs[0].branch == "ignore"
    assert cfs[0].hypothetical_event.id == "cf_fut_ignore"
    assert cfs[0].hypothetical_event.data["do"] == "wait"
    # Would-have-been chain: cf_d and cf_p
    causes_ids = [e.id for e in cfs[0].would_have_been_causes]
    assert "cf_d" in causes_ids and "cf_p" in causes_ids
    print(f"  binding why_not('cf_fut_ignore') -> "
          f"branch={cfs[0].branch}, "
          f"causes={sorted(causes_ids)} OK")

    # why_not on a real event
    assert cf_core.why_not("cf_d") == []
    assert cf_core.why_not("nonexistent") == []

    # Adapter why_not via JSON round-trip
    wn = cf_adapter.handle_tool_call("why_not", {"id": "cf_fut_ignore"})
    assert wn["ok"] is True
    assert len(wn["result"]) == 1
    assert wn["result"][0]["branch"] == "ignore"
    assert wn["result"][0]["hypothetical_event"]["data"]["do"] == "wait"
    print(f"  adapter why_not JSON round-trip OK "
          f"({len(wn['result'][0]['would_have_been_causes'])} would-have-been causes)")

    # explain_with via the engine
    hyp = galahad.TemporalEvent()
    hyp.id = "cf_hyp"
    hyp.valid_from = cft + ms(20)
    hyp.valid_to = cft + ms(30)
    hyp.recorded_at = cft + ms(20)
    hyp.type = "hypothetical"
    hyp.causal_links = ["cf_d"]
    hyp_exp = cf_engine.explain_with("cf_hyp", hyp)
    assert len(hyp_exp.causes) == 2
    assert [c.id for c in hyp_exp.causes] == ["cf_p", "cf_d"]

    # Real core unaffected by the hypothetical explain_with call.
    assert cf_core.get("cf_hyp") is None
    print("  engine.explain_with(hyp) OK (core unmutated)")

    # clone: deep copy, independent mutations
    forked = cf_core.clone()
    extra = galahad.TemporalEvent()
    extra.id = "only_in_fork"
    extra.valid_from = cft + ms(50)
    extra.valid_to = cft + ms(55)
    extra.recorded_at = cft + ms(50)
    extra.type = "test"
    forked.add_event(extra)
    assert forked.get("only_in_fork") is not None
    assert cf_core.get("only_in_fork") is None
    print("  core.clone() -> independent fork OK")

    print("  counterfactual queries OK")

    # ------------------------------------------------------------------
    # Anomaly detection + confidence decay (new in 0.3.0).
    # ------------------------------------------------------------------
    print()
    print("Anomaly detection (new in 0.3.0):")

    a_core = galahad.TemporalCore()
    at = datetime(2026, 4, 15, 3, 0, 0, tzinfo=timezone.utc)

    def amk(id_, from_s, to_s, type_, links=None):
        e = galahad.TemporalEvent()
        e.id = id_
        e.valid_from = at + timedelta(seconds=from_s)
        e.valid_to = at + timedelta(seconds=to_s)
        e.recorded_at = at + timedelta(seconds=from_s)
        e.type = type_
        e.causal_links = links or []
        return e

    # Baseline: server_a heartbeat appears 3 times (same ID, different intervals)
    # server_b heartbeat appears once
    for i in range(3):
        e = galahad.TemporalEvent()
        e.id = "server_a"
        e.valid_from = at + timedelta(seconds=i * 10)
        e.valid_to = at + timedelta(seconds=i * 10 + 5)
        e.recorded_at = at + timedelta(seconds=i * 10)
        e.type = "heartbeat"
        a_core.add_event(e)
    a_core.add_event(amk("server_b", 5, 10, "heartbeat"))
    # Current: only server_b
    e2 = galahad.TemporalEvent()
    e2.id = "server_b"
    e2.valid_from = at + timedelta(seconds=100)
    e2.valid_to = at + timedelta(seconds=105)
    e2.recorded_at = at + timedelta(seconds=100)
    e2.type = "heartbeat"
    a_core.add_event(e2)
    # Long-running event for loitering
    a_core.add_event(amk("long_conn", 0, 600, "connection"))

    det = galahad.AnomalyDetector(a_core)

    # MissingEntity: hb_a* appeared 3x in baseline, absent in current
    baseline = galahad.TimeWindow(at, at + timedelta(seconds=30))
    current = galahad.TimeWindow(at + timedelta(seconds=100),
                                 at + timedelta(seconds=110))
    missing = det.detect_missing("heartbeat", baseline, current, 2)
    assert len(missing) == 1
    assert missing[0].type == galahad.AnomalyType.MissingEntity
    assert "server_a" in missing[0].involved_events
    print(f"  detect_missing -> {len(missing)} missing entities OK")

    # Loitering: long_conn is 600s, threshold 300s
    full_window = galahad.TimeWindow(at, at + timedelta(seconds=700))
    loiter = det.detect_loitering(full_window, 300.0, "connection")
    assert len(loiter) == 1
    assert loiter[0].type == galahad.AnomalyType.Loitering
    assert loiter[0].involved_events == ["long_conn"]
    print(f"  detect_loitering -> severity {loiter[0].severity:.2f} OK")

    # DecayConfig + compute_decay
    cfg = galahad.network_decay()
    assert cfg.half_life_secs == 172800.0
    c_fresh = galahad.compute_decay(0.0, 1, cfg)
    assert abs(c_fresh - 1.0) < 0.01
    c_old = galahad.compute_decay(172800.0, 0, cfg)
    assert 0.4 < c_old < 0.6, f"expected ~0.5 at half-life, got {c_old}"
    print(f"  compute_decay: fresh={c_fresh:.3f}, at_half_life={c_old:.3f} OK")

    # ConfidenceDecay detection
    decay_results = det.detect_confidence_decay(
        full_window,
        at + timedelta(days=5),  # 5 days later
        0.3,
        galahad.network_decay())
    assert len(decay_results) > 0
    for r in decay_results:
        assert r.type == galahad.AnomalyType.ConfidenceDecay
    print(f"  detect_confidence_decay -> {len(decay_results)} decayed OK")

    # Anomaly tools via LLM adapter
    a_engine = galahad.TemporalEngine(a_core)
    a_adapter = galahad.LLMToolAdapter(a_core, a_engine)
    schemas = a_adapter.get_tool_schemas()
    tool_names = {t["name"] for t in schemas}
    for required in ("detect_missing", "detect_frequency_anomaly",
                     "detect_co_occurrence_break", "detect_loitering",
                     "detect_confidence_decay", "compute_decay"):
        assert required in tool_names, f"missing anomaly tool: {required}"

    # compute_decay via adapter
    dc = a_adapter.handle_tool_call("compute_decay", {
        "elapsed_secs": 86400.0,
        "observation_count": 3,
        "half_life_secs": 86400.0,
    })
    assert dc["ok"] is True
    assert 0.5 < dc["result"] < 0.9
    print(f"  adapter compute_decay -> {dc['result']:.4f} OK")

    # detect_loitering via adapter
    dl = a_adapter.handle_tool_call("detect_loitering", {
        "start": at.isoformat(),
        "end": (at + timedelta(seconds=700)).isoformat(),
        "max_duration_secs": 300.0,
        "event_type": "connection",
    })
    assert dl["ok"] is True
    assert len(dl["result"]) == 1
    assert dl["result"][0]["type"] == "loitering"
    print(f"  adapter detect_loitering JSON round-trip OK")

    print("  anomaly detection + decay OK")

    print("\ntest_galahad: OK (core + engine + adapter + persistence + "
          "timezone + counterfactuals + anomaly)")


if __name__ == "__main__":
    main()
