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

    std::cout << "test_engine: OK (with branching futures)\n";
    return 0;
}
