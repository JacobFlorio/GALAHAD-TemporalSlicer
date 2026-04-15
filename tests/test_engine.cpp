#include "temporal_engine.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>

using namespace galahad;

static bool containsId(const std::vector<TemporalEvent>& v, const EventId& id) {
    return std::any_of(v.begin(), v.end(),
                       [&](const TemporalEvent& e) { return e.id == id; });
}

int main() {
    using namespace std::chrono;
    const auto t0 = Clock::now();
    auto ms = [](int n) { return milliseconds(n); };

    TemporalCore core;
    // perceive [0,5] -> infer [5,15] -> decide [15,20] -> act [20,30]
    core.addEvent({"perceive",  t0,         t0+ms(5),   t0,         "perception", {{"obs","door_open"}}, {}});
    core.addEvent({"infer",     t0+ms(5),   t0+ms(15),  t0+ms(5),   "inference",  {{"why","wind"}},      {"perceive"}});
    core.addEvent({"decide",    t0+ms(15),  t0+ms(20),  t0+ms(15),  "decision",   {{"choose","close"}},  {"infer"}});
    core.addEvent({"act",       t0+ms(20),  t0+ms(30),  t0+ms(200), "action",     {{"do","close"}},      {"decide"}});
    core.buildCausalGraph();

    TemporalEngine engine(core);

    // --- explain returns full events in causal/temporal order ---
    auto exp = engine.explain("act");
    assert(exp.causes.size() == 3);
    assert(exp.causes[0].id == "perceive");
    assert(exp.causes[1].id == "infer");
    assert(exp.causes[2].id == "decide");
    // Returning full events, not bare IDs: the metadata is there.
    assert(exp.causes[0].data.at("obs") == "door_open");

    // --- require_completed_before filter ---
    // Add an ongoing "context" event that overlaps everything but is
    // a (claimed) cause of act. With the filter on, it must be excluded
    // because it hasn't finished before act starts.
    core.addEvent({"ongoing",   t0,         t0+ms(100), t0,         "context",
                   {{"state","storm"}},    {}});
    core.addEvent({"act2",      t0+ms(20),  t0+ms(30),  t0+ms(25),  "action",
                   {{"do","brace"}},       {"ongoing", "decide"}});
    core.buildCausalGraph();

    auto exp2 = engine.explain("act2");  // filter on by default
    // "ongoing" runs [0,100], act2 starts at 20 — not completed before target.
    assert(!containsId(exp2.causes, "ongoing"));
    // decide [15,20] completes exactly at act2's start — allowed (<=).
    assert(containsId(exp2.causes, "decide"));

    auto exp2_loose = engine.explain("act2", std::nullopt, /*require_completed_before=*/false);
    assert(containsId(exp2_loose.causes, "ongoing"));
    assert(containsId(exp2_loose.causes, "decide"));

    // --- bitemporal: as-of before act was recorded, explanation is empty ---
    auto past = engine.explain("act", t0+ms(100));
    assert(past.causes.empty());

    // --- whatHappenedDuring now returns full events ---
    auto during = engine.whatHappenedDuring({t0+ms(10), t0+ms(18)});
    // infer [5,15], decide [15,20], ongoing [0,100] all overlap [10,18].
    assert(during.size() == 3);
    assert(containsId(during, "infer"));
    assert(containsId(during, "decide"));
    assert(containsId(during, "ongoing"));

    // ---------- branching futures through the engine ----------
    {
        TemporalCore c;
        c.addEvent({"perceive", t0, t0+ms(5), t0,
                    "perception", {{"obs","door_open"}}, {}});
        c.addEvent({"infer", t0+ms(5), t0+ms(15), t0+ms(5),
                    "inference", {{"why","wind"}}, {"perceive"}});
        c.addEvent({"decide", t0+ms(15), t0+ms(20), t0+ms(15),
                    "decision", {{"choose","close"}}, {"infer"}});

        // Two competing projected futures depending on "decide".
        TemporalEvent futClose;
        futClose.id = "fut_close";
        futClose.valid_from = t0+ms(20);
        futClose.valid_to = t0+ms(30);
        futClose.recorded_at = t0+ms(15);
        futClose.type = "projected_action";
        futClose.data = {{"do","close"}};
        futClose.causal_links = {"decide"};
        futClose.branch_id = "close_door";
        futClose.confidence = 0.7;
        c.addProjection(futClose);

        TemporalEvent futIgnore;
        futIgnore.id = "fut_ignore";
        futIgnore.valid_from = t0+ms(20);
        futIgnore.valid_to = t0+ms(30);
        futIgnore.recorded_at = t0+ms(15);
        futIgnore.type = "projected_inaction";
        futIgnore.data = {{"do","wait"}};
        futIgnore.causal_links = {"decide"};
        futIgnore.branch_id = "ignore";
        futIgnore.confidence = 0.3;
        c.addProjection(futIgnore);

        c.buildCausalGraph();

        TemporalEngine eng(c);

        // Explaining a projection: the causes are on main.
        auto why_close = eng.explain("fut_close");
        assert(why_close.causes.size() == 3);
        assert(why_close.causes[0].id == "perceive");
        assert(why_close.causes[1].id == "infer");
        assert(why_close.causes[2].id == "decide");

        // The two projections must not contaminate each other.
        // fut_ignore is not an ancestor of fut_close.
        for (const auto& e : why_close.causes) {
            assert(e.id != "fut_ignore");
        }

        // Branch-filtered: explain fut_close, main-only causes (same thing here).
        auto why_close_main = eng.explain("fut_close", std::nullopt, true,
                                          std::optional<BranchId>{"main"});
        assert(why_close_main.causes.size() == 3);

        // whatHappenedDuring with branch filter: ground truth only.
        auto main_window = eng.whatHappenedDuring(
            {t0, t0+ms(500)}, std::nullopt, std::optional<BranchId>{"main"});
        assert(main_window.size() == 3);  // perceive, infer, decide
        assert(!containsId(main_window, "fut_close"));
        assert(!containsId(main_window, "fut_ignore"));

        // whatHappenedDuring on a projection branch: only that branch.
        auto close_window = eng.whatHappenedDuring(
            {t0, t0+ms(500)}, std::nullopt, std::optional<BranchId>{"close_door"});
        assert(close_window.size() == 1);
        assert(close_window[0].id == "fut_close");
        assert(close_window[0].confidence == 0.7);

        // No filter: all branches visible.
        auto all_window = eng.whatHappenedDuring({t0, t0+ms(500)});
        assert(all_window.size() == 5);
    }

    // ---------- counterfactual queries: whyNot + explainWith ----------
    //
    // Build a small scenario with two competing projection branches, refute
    // one of them, then verify:
    //   - whyNot on the refuted branch's event returns the branch + the
    //     would-have-been causal chain walked across refuted nodes
    //   - whyNot on the non-refuted branch's event returns empty
    //   - whyNot on an event that actually happened on main returns empty
    //   - explainWith adds a hypothetical event and returns its ancestors
    //     without mutating the real core
    {
        TemporalCore cf_core;
        const auto t0 = Clock::now();

        cf_core.addEvent({"gt_perceive", t0,         t0+ms(5),  t0,
                          "perception", {{"obs","door_open"}}, {}});
        cf_core.addEvent({"gt_infer",    t0+ms(5),   t0+ms(15), t0+ms(5),
                          "inference",  {{"why","wind"}}, {"gt_perceive"}});
        cf_core.addEvent({"gt_decide",   t0+ms(15),  t0+ms(20), t0+ms(15),
                          "decision",   {{"choose","close"}}, {"gt_infer"}});

        // Two projected futures depending on the real decide.
        TemporalEvent fut_close;
        fut_close.id = "fut_close";
        fut_close.valid_from = t0+ms(20);
        fut_close.valid_to   = t0+ms(30);
        fut_close.recorded_at = t0+ms(15);
        fut_close.type = "projected_action";
        fut_close.data = {{"do","close"}};
        fut_close.causal_links = {"gt_decide"};
        fut_close.branch_id = "close_door";
        fut_close.confidence = 0.7;
        cf_core.addProjection(fut_close);

        TemporalEvent fut_ignore = fut_close;
        fut_ignore.id = "fut_ignore";
        fut_ignore.branch_id = "ignore";
        fut_ignore.confidence = 0.3;
        fut_ignore.data = {{"do","wait"}};
        cf_core.addProjection(fut_ignore);

        // Reality unfolds: refute the ignore branch.
        cf_core.refuteBranch("ignore");
        cf_core.buildCausalGraph();

        // whyNot on the refuted branch's event:
        auto cfs = cf_core.whyNot("fut_ignore");
        assert(cfs.size() == 1);
        assert(cfs[0].branch == "ignore");
        assert(cfs[0].hypothetical_event.id == "fut_ignore");
        assert(cfs[0].hypothetical_event.data.at("do") == "wait");
        // Would-have-been ancestors: gt_decide -> gt_infer -> gt_perceive.
        // All are on main, not on a refuted branch, but the whyNot walk
        // crosses branches so all three must be present.
        assert(cfs[0].would_have_been_causes.size() == 3);
        std::vector<std::string> got_ids;
        for (const auto& c : cfs[0].would_have_been_causes) got_ids.push_back(c.id);
        assert(std::find(got_ids.begin(), got_ids.end(), "gt_decide")   != got_ids.end());
        assert(std::find(got_ids.begin(), got_ids.end(), "gt_infer")    != got_ids.end());
        assert(std::find(got_ids.begin(), got_ids.end(), "gt_perceive") != got_ids.end());

        // whyNot on the still-open projection branch: empty (not refuted).
        auto cfs_close = cf_core.whyNot("fut_close");
        assert(cfs_close.empty());

        // whyNot on an event that actually happened on main: empty.
        auto cfs_main = cf_core.whyNot("gt_decide");
        assert(cfs_main.empty());

        // whyNot on a non-existent event: empty.
        auto cfs_none = cf_core.whyNot("never_existed");
        assert(cfs_none.empty());

        // ---- explainWith: hypothetical mutation, fork, query, discard ----
        //
        // Add a hypothetical "hyp_act" event that cites gt_decide as its
        // cause. The real core does not contain hyp_act; explainWith
        // should return the full causal chain (decide -> infer -> perceive)
        // as if hyp_act existed, without mutating the real core.
        TemporalEvent hyp;
        hyp.id = "hyp_act";
        hyp.valid_from = t0+ms(25);
        hyp.valid_to   = t0+ms(35);
        hyp.recorded_at = t0+ms(25);
        hyp.type = "hypothetical_action";
        hyp.data = {{"do","close"}};
        hyp.causal_links = {"gt_decide"};

        TemporalEngine engine_cf(cf_core);
        auto hyp_exp = engine_cf.explainWith("hyp_act", hyp);
        assert(hyp_exp.causes.size() == 3);
        assert(hyp_exp.causes[0].id == "gt_perceive");
        assert(hyp_exp.causes[1].id == "gt_infer");
        assert(hyp_exp.causes[2].id == "gt_decide");

        // The real core must be unchanged — hyp_act does not exist in it.
        assert(!cf_core.get("hyp_act").has_value());

        // And a normal explain on the real core returns empty for hyp_act
        // (it genuinely isn't there).
        auto real_exp = engine_cf.explain("hyp_act");
        assert(real_exp.causes.empty());

        // ---- clone: modifications on the clone must not affect origin ----
        auto orig_size = cf_core.queryRange({t0, t0+ms(1000)}).size();
        TemporalCore forked = cf_core.clone();
        TemporalEvent extra;
        extra.id = "only_in_fork";
        extra.valid_from = t0+ms(50);
        extra.valid_to   = t0+ms(55);
        extra.recorded_at = t0+ms(50);
        extra.type = "test";
        forked.addEvent(extra);

        // Fork has the extra event.
        assert(forked.get("only_in_fork").has_value());
        // Origin does not.
        assert(!cf_core.get("only_in_fork").has_value());
        assert(cf_core.queryRange({t0, t0+ms(1000)}).size() == orig_size);
    }

    std::cout << "test_engine: OK (counterfactual: whyNot + explainWith + clone)\n";
    return 0;
}
