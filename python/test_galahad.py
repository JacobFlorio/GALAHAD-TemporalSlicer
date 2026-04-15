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
    assert len(schemas) >= 15
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

    print("\ntest_galahad: OK (core + engine + adapter + persistence)")


if __name__ == "__main__":
    main()
