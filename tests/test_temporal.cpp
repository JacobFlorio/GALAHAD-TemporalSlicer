#include "temporal_core.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>

using namespace galahad;

static bool contains(const std::vector<EventId>& v, const EventId& id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

static TemporalEvent mk(const EventId& id, TimePoint s, TimePoint e) {
    return TemporalEvent{id, s, e, s, "x", {}, {}};
}

int main() {
    using namespace std::chrono;
    const auto t0 = Clock::now();
    auto ms = [](int n) { return milliseconds(n); };

    // ---------- bitemporal + causal sanity (regression guard) ----------
    {
        TemporalCore core;
        core.addEvent({"e1", t0, t0, t0, "perception", {{"obs","door_open"}}, {}});
        core.addEvent({"e2", t0+ms(10), t0+ms(100), t0+ms(10), "inference", {{"why","wind"}}, {"e1"}});
        core.addEvent({"e3", t0+ms(50), t0+ms(50), t0+ms(200), "action", {{"do","close"}}, {"e2"}});
        core.buildCausalGraph();

        assert(core.queryRange({t0, t0+ms(500)}).size() == 3);
        assert(core.queryRange({t0, t0+ms(500)}, t0+ms(100)).size() == 2);
        assert(!core.hasCycle());

        auto anc = core.getAncestors("e3");
        assert(anc.size() == 2 && contains(anc, "e1") && contains(anc, "e2"));

        auto desc_past = core.getDescendants("e1", t0+ms(100));
        assert(desc_past.size() == 1 && desc_past[0] == "e2");
    }

    // ---------- Allen's 13 relations, exhaustively ----------
    {
        TemporalCore c;
        // Reference interval R = [100, 200]
        const auto r0 = t0 + ms(100);
        const auto r1 = t0 + ms(200);
        TemporalEvent R = mk("R", r0, r1);

        struct Case { EventId id; int s; int e; AllenRelation rel; };
        std::vector<Case> cases = {
            {"P",   0,  50,  AllenRelation::Precedes},     // ends before R starts
            {"M",   0, 100,  AllenRelation::Meets},        // ends when R starts
            {"O",  50, 150,  AllenRelation::Overlaps},     // straddles R's start
            {"FB",  0, 200,  AllenRelation::FinishedBy},   // same end, earlier start
            {"DI",  0, 300,  AllenRelation::Contains},     // strictly contains R
            {"S", 100, 150,  AllenRelation::Starts},       // same start, earlier end
            {"EQ",100, 200,  AllenRelation::Equals},
            {"SI",100, 300,  AllenRelation::StartedBy},    // same start, later end
            {"D", 120, 180,  AllenRelation::During},       // strictly inside R
            {"F", 150, 200,  AllenRelation::Finishes},     // same end, later start
            {"OI",150, 300,  AllenRelation::OverlappedBy}, // straddles R's end
            {"MI",200, 300,  AllenRelation::MetBy},        // starts when R ends
            {"PI",250, 300,  AllenRelation::PrecededBy},   // starts after R ends
        };

        for (const auto& k : cases) {
            TemporalEvent e = mk(k.id, t0+ms(k.s), t0+ms(k.e));
            auto rel = c.getAllenRelation(e, R);
            if (rel != k.rel) {
                std::cerr << "Allen mismatch for " << k.id
                          << ": got " << static_cast<int>(rel)
                          << " expected " << static_cast<int>(k.rel) << "\n";
            }
            assert(rel == k.rel);
            assert(c.holds(k.rel, e, R));
        }

        // Reflexivity: R equals itself.
        assert(c.getAllenRelation(R, R) == AllenRelation::Equals);

        // Converse pairs: Precedes <-> PrecededBy, etc.
        TemporalEvent P = mk("P", t0, t0+ms(50));
        assert(c.getAllenRelation(R, P) == AllenRelation::PrecededBy);
        assert(c.getAllenRelation(P, R) == AllenRelation::Precedes);

        TemporalEvent O = mk("O", t0+ms(50), t0+ms(150));
        assert(c.getAllenRelation(R, O) == AllenRelation::OverlappedBy);
        assert(c.getAllenRelation(O, R) == AllenRelation::Overlaps);
    }

    // ---------- findRelated in a populated core ----------
    {
        TemporalCore c;
        // Reference event R = [100, 200]
        c.addEvent(mk("R", t0+ms(100), t0+ms(200)));
        c.addEvent(mk("during1", t0+ms(120), t0+ms(140)));
        c.addEvent(mk("during2", t0+ms(150), t0+ms(180)));
        c.addEvent(mk("before",  t0,        t0+ms(50)));
        c.addEvent(mk("overlap", t0+ms(50), t0+ms(150)));

        auto inside = c.findRelated("R", AllenRelation::Contains);
        assert(inside.size() == 2);
        assert(contains(inside, "during1") && contains(inside, "during2"));

        auto overlapped = c.findRelated("R", AllenRelation::OverlappedBy);
        assert(overlapped.size() == 1 && overlapped[0] == "overlap");

        auto earlier = c.findRelated("R", AllenRelation::PrecededBy);
        assert(earlier.size() == 1 && earlier[0] == "before");
    }

    // ---------- branching futures ----------
    {
        TemporalCore c;
        // Ground truth: a single perception on main.
        TemporalEvent gt;
        gt.id = "gt";
        gt.valid_from = t0;
        gt.valid_to = t0 + ms(5);
        gt.recorded_at = t0;
        gt.type = "perception";
        gt.data = {{"obs", "door_open"}};
        c.addEvent(gt);

        // Projection A: "close the door" on branch close_door, depends on gt.
        TemporalEvent pA;
        pA.id = "pA";
        pA.valid_from = t0 + ms(10);
        pA.valid_to = t0 + ms(20);
        pA.recorded_at = t0 + ms(5);
        pA.type = "action";
        pA.data = {{"do", "close"}};
        pA.causal_links = {"gt"};
        pA.branch_id = "close_door";
        pA.confidence = 0.7;
        c.addProjection(pA);

        // Projection B: "ignore" on branch ignore, also depends on gt.
        TemporalEvent pB;
        pB.id = "pB";
        pB.valid_from = t0 + ms(10);
        pB.valid_to = t0 + ms(20);
        pB.recorded_at = t0 + ms(5);
        pB.type = "inaction";
        pB.data = {{"do", "wait"}};
        pB.causal_links = {"gt"};
        pB.branch_id = "ignore";
        pB.confidence = 0.3;
        c.addProjection(pB);

        // Attempting to add a projection with branch_id == "main" is a no-op.
        TemporalEvent bad;
        bad.id = "bad";
        bad.valid_from = t0;
        bad.valid_to = t0;
        bad.recorded_at = t0;
        bad.branch_id = "main";
        c.addProjection(bad);

        // --- queryRange branch filtering ---
        auto all = c.queryRange({t0, t0 + ms(500)});
        assert(all.size() == 3);  // gt + pA + pB; bad rejected

        auto main_only = c.queryRange({t0, t0 + ms(500)}, std::nullopt,
                                      std::optional<BranchId>{"main"});
        assert(main_only.size() == 1 && main_only[0].id == "gt");

        auto close_only = c.queryRange({t0, t0 + ms(500)}, std::nullopt,
                                       std::optional<BranchId>{"close_door"});
        assert(close_only.size() == 1 && close_only[0].id == "pA");
        assert(close_only[0].confidence == 0.7);

        auto ignore_only = c.queryRange({t0, t0 + ms(500)}, std::nullopt,
                                        std::optional<BranchId>{"ignore"});
        assert(ignore_only.size() == 1 && ignore_only[0].id == "pB");

        // --- getProjections ---
        auto all_projs = c.getProjections();
        assert(all_projs.size() == 2);

        auto close_projs = c.getProjections(std::optional<BranchId>{"close_door"});
        assert(close_projs.size() == 1 && close_projs[0].id == "pA");

        // --- causal traversal across branches ---
        c.buildCausalGraph();

        // Effects of gt without branch filter: both projections.
        auto effs = c.getEffects("gt");
        assert(effs.size() == 2);
        assert(contains(effs, "pA") && contains(effs, "pB"));

        // Effects of gt restricted to close_door branch: only pA.
        auto effs_close = c.getEffects("gt", std::nullopt,
                                       std::optional<BranchId>{"close_door"});
        assert(effs_close.size() == 1 && effs_close[0] == "pA");

        // Effects of gt restricted to main: empty — its causal children are
        // projections, not main events. This is the isolation we want.
        auto effs_main = c.getEffects("gt", std::nullopt,
                                      std::optional<BranchId>{"main"});
        assert(effs_main.empty());

        // Ancestors of pA (a projection) with no branch filter: {gt}.
        // The source is seeded regardless of its own branch.
        auto anc_pA = c.getAncestors("pA");
        assert(anc_pA.size() == 1 && anc_pA[0] == "gt");

        // Ancestors of pA restricted to main: same result, since gt is main.
        auto anc_pA_main = c.getAncestors("pA", std::nullopt,
                                          std::optional<BranchId>{"main"});
        assert(anc_pA_main.size() == 1 && anc_pA_main[0] == "gt");

        // Ancestors of pA restricted to ignore branch: empty — pB is not
        // an ancestor of pA, even though both are on "their" branch.
        auto anc_pA_ignore = c.getAncestors("pA", std::nullopt,
                                            std::optional<BranchId>{"ignore"});
        assert(anc_pA_ignore.empty());

        // --- branch isolation: pA and pB must not see each other ---
        auto anc_pB = c.getAncestors("pB");
        assert(anc_pB.size() == 1 && anc_pB[0] == "gt");
        assert(!contains(anc_pB, "pA"));

        // --- findRelated respects branch ---
        auto related_all = c.findRelated("gt", AllenRelation::Precedes);
        assert(related_all.size() == 2);  // pA and pB both after gt

        auto related_close = c.findRelated(
            "gt", AllenRelation::Precedes, std::nullopt,
            std::optional<BranchId>{"close_door"});
        assert(related_close.size() == 1 && related_close[0] == "pA");
    }

    // ---------- branch lifecycle ----------
    {
        TemporalCore c;
        TemporalEvent gt;
        gt.id = "gt"; gt.valid_from = t0; gt.valid_to = t0+ms(5);
        gt.recorded_at = t0; gt.type = "perception";
        c.addEvent(gt);

        TemporalEvent pA;
        pA.id = "pA";
        pA.valid_from = t0+ms(10); pA.valid_to = t0+ms(20);
        pA.recorded_at = t0+ms(5);
        pA.type = "action"; pA.data = {{"do","close"}};
        pA.causal_links = {"gt"};
        pA.branch_id = "close_door"; pA.confidence = 0.7;
        c.addProjection(pA);

        TemporalEvent pB;
        pB.id = "pB";
        pB.valid_from = t0+ms(10); pB.valid_to = t0+ms(20);
        pB.recorded_at = t0+ms(5);
        pB.type = "inaction";
        pB.causal_links = {"gt"};
        pB.branch_id = "ignore"; pB.confidence = 0.3;
        c.addProjection(pB);

        // --- refute: nullopt queries hide refuted branch, explicit still sees it ---
        c.refuteBranch("ignore");
        assert(c.isRefuted("ignore"));

        auto hidden = c.queryRange({t0, t0+ms(500)});
        // gt + pA visible; pB hidden.
        assert(hidden.size() == 2);
        assert(std::none_of(hidden.begin(), hidden.end(),
                            [](const TemporalEvent& e){ return e.id == "pB"; }));

        auto explicit_ignore = c.queryRange({t0, t0+ms(500)}, std::nullopt,
                                            std::optional<BranchId>{"ignore"});
        assert(explicit_ignore.size() == 1 && explicit_ignore[0].id == "pB");

        // get() hides refuted under nullopt, sees it under explicit branch.
        assert(!c.get("pB").has_value());
        auto pB_explicit = c.get("pB", std::nullopt, std::optional<BranchId>{"ignore"});
        assert(pB_explicit.has_value());

        // getProjections default hides refuted.
        auto projs_visible = c.getProjections();
        assert(projs_visible.size() == 1 && projs_visible[0].id == "pA");

        // Refuting main is a no-op.
        c.refuteBranch("main");
        assert(!c.isRefuted("main"));

        // --- promote: main copy appears with fresh recorded_at ---
        c.promoteBranch("close_door");

        // After promotion, get("pA") with no filter returns the main copy.
        auto pA_now = c.get("pA");
        assert(pA_now.has_value());
        assert(pA_now->branch_id == "main");
        assert(pA_now->confidence == 1.0);  // promoted = certain

        // The original projection record is still there on close_door.
        auto pA_orig = c.get("pA", std::nullopt,
                             std::optional<BranchId>{"close_door"});
        assert(pA_orig.has_value());
        assert(pA_orig->branch_id == "close_door");
        assert(pA_orig->confidence == 0.7);

        // Bitemporal honesty: as-of before the promotion (recorded_at = now),
        // get("pA") with no filter returns the pre-promotion record.
        // We use t0+ms(5) which is the projection's recorded_at; that predates
        // the promotion's fresh recorded_at.
        auto pA_past = c.get("pA", t0+ms(5));
        assert(pA_past.has_value());
        assert(pA_past->branch_id == "close_door");
        assert(pA_past->confidence == 0.7);

        // Main-only query now sees gt + promoted pA.
        auto main_after = c.queryRange({t0, t0+ms(500)}, std::nullopt,
                                       std::optional<BranchId>{"main"});
        assert(main_after.size() == 2);

        // Promoting re-refuted branch clears the refutation.
        c.refuteBranch("close_door");
        assert(c.isRefuted("close_door"));
        c.promoteBranch("close_door");
        assert(!c.isRefuted("close_door"));

        // --- prune: destroys events on branch, unrefutes it ---
        const std::size_t before = c.queryRange({t0, t0+ms(500)}, std::nullopt,
                                                std::optional<BranchId>{"ignore"}).size();
        assert(before == 1);
        c.pruneBranch("ignore");
        auto after_prune = c.queryRange({t0, t0+ms(500)}, std::nullopt,
                                        std::optional<BranchId>{"ignore"});
        assert(after_prune.empty());
        assert(!c.isRefuted("ignore"));

        // Pruning main is a no-op.
        const std::size_t main_before = c.queryRange({t0, t0+ms(500)}, std::nullopt,
                                                     std::optional<BranchId>{"main"}).size();
        c.pruneBranch("main");
        const std::size_t main_after_prune = c.queryRange({t0, t0+ms(500)}, std::nullopt,
                                                          std::optional<BranchId>{"main"}).size();
        assert(main_before == main_after_prune);
    }

    // ---------- time index correctness under scale ----------
    //
    // This is a correctness stress test, not a benchmark. 10k point events
    // spread across a synthetic timeline; we verify that queryRange and
    // findRelated agree with a ground-truth linear scan on random windows.
    {
        TemporalCore c;
        const int N = 10000;
        for (int i = 0; i < N; ++i) {
            TemporalEvent e;
            e.id = "e" + std::to_string(i);
            e.valid_from = t0 + ms(i);
            e.valid_to   = t0 + ms(i + 3);  // small interval
            e.recorded_at = t0 + ms(i);
            e.type = "tick";
            c.addEvent(e);
        }

        // Ground-truth reference walker.
        auto reference_range = [&](TimeWindow w) {
            std::vector<EventId> ids;
            for (int i = 0; i < N; ++i) {
                auto vf = t0 + ms(i);
                auto vt = t0 + ms(i + 3);
                if (vf <= w.end && vt >= w.start) {
                    ids.push_back("e" + std::to_string(i));
                }
            }
            std::sort(ids.begin(), ids.end());
            return ids;
        };

        auto range_ids = [&](TimeWindow w) {
            auto got = c.queryRange(w);
            std::vector<EventId> ids;
            for (const auto& e : got) ids.push_back(e.id);
            std::sort(ids.begin(), ids.end());
            return ids;
        };

        struct Window { int lo; int hi; };
        std::vector<Window> windows = {
            {0, 10}, {4500, 4510}, {9990, 10005}, {100, 200}, {0, N+100}
        };
        for (const auto& w : windows) {
            auto ref = reference_range({t0+ms(w.lo), t0+ms(w.hi)});
            auto got = range_ids({t0+ms(w.lo), t0+ms(w.hi)});
            assert(ref == got);
        }

        // findRelated across the three dispatch paths (Precedes, PrecededBy,
        // Contains). Pick a reference event mid-timeline.
        const int pivot = 5000;
        const EventId pivot_id = "e" + std::to_string(pivot);

        // Precedes: events e where pivot ends before e starts.
        // pivot valid_to = t0+ms(pivot+3). Any event with valid_from > that
        // satisfies Precedes. That's events pivot+4 onward.
        auto precedes = c.findRelated(pivot_id, AllenRelation::Precedes);
        assert(precedes.size() == static_cast<std::size_t>(N - (pivot + 4)));

        // PrecededBy: events e where e ends before pivot starts.
        // pivot.valid_from = t0+ms(pivot). Events with valid_to < that are
        // events whose (i+3) < pivot -> i < pivot-3 -> i in [0, pivot-4].
        auto preceded_by = c.findRelated(pivot_id, AllenRelation::PrecededBy);
        assert(preceded_by.size() == static_cast<std::size_t>(pivot - 3));

        // Meets: events e where pivot.valid_to == e.valid_from.
        // e.valid_from = t0+ms(i), pivot.valid_to = t0+ms(pivot+3).
        // i = pivot+3 -> e(pivot+3) meets pivot.
        auto meets = c.findRelated(pivot_id, AllenRelation::Meets);
        assert(meets.size() == 1 && meets[0] == "e" + std::to_string(pivot + 3));

        // MetBy: e.valid_to == pivot.valid_from. e.valid_to = t0+ms(i+3),
        // pivot.valid_from = t0+ms(pivot). i+3 = pivot -> i = pivot - 3.
        auto met_by = c.findRelated(pivot_id, AllenRelation::MetBy);
        assert(met_by.size() == 1 && met_by[0] == "e" + std::to_string(pivot - 3));

        // Overlaps: pivot overlaps e iff pivot.valid_from < e.valid_from
        // and pivot.valid_to is within (e.valid_from, e.valid_to).
        // pivot starts at t0+ms(pivot), ends at t0+ms(pivot+3).
        // Events starting at pivot+1 or pivot+2 have valid_from in
        // (pivot, pivot+3) and valid_to > pivot+3 -> Overlaps.
        auto overlaps = c.findRelated(pivot_id, AllenRelation::Overlaps);
        assert(overlaps.size() == 2);
        assert(contains(overlaps, "e" + std::to_string(pivot + 1)));
        assert(contains(overlaps, "e" + std::to_string(pivot + 2)));
    }

    // ---------- per-branch time index: 100 branches x 100 events ----------
    //
    // Correctness test that the per-branch path isolates branches properly.
    // 100 branches each hold 100 events at staggered offsets; main holds its
    // own 100 events. A branch-scoped query must see only its own branch.
    {
        TemporalCore c;
        const int BRANCHES = 100;
        const int PER = 100;

        // Main timeline: e_main_0..99 at t0+[0,100)
        for (int i = 0; i < PER; ++i) {
            TemporalEvent e;
            e.id = "main_" + std::to_string(i);
            e.valid_from = t0 + ms(i);
            e.valid_to = t0 + ms(i + 1);
            e.recorded_at = t0 + ms(i);
            e.type = "main_tick";
            c.addEvent(e);
        }

        // 100 projection branches, each with its own 100 events at offset
        // shifted by b*1000 so branches don't share timestamps.
        for (int b = 0; b < BRANCHES; ++b) {
            const std::string branch = "branch_" + std::to_string(b);
            for (int i = 0; i < PER; ++i) {
                TemporalEvent e;
                e.id = "b" + std::to_string(b) + "_" + std::to_string(i);
                e.valid_from = t0 + ms(1000 + b * 1000 + i);
                e.valid_to   = t0 + ms(1000 + b * 1000 + i + 1);
                e.recorded_at = t0 + ms(b * 10 + i);
                e.type = "proj_tick";
                e.branch_id = branch;
                e.confidence = 0.5;
                c.addProjection(e);
            }
        }

        // Sanity: total events.
        // Wide window spanning everything, no filter, no refutations.
        auto everything = c.queryRange({t0, t0 + ms(1'000'000)});
        assert(everything.size() ==
               static_cast<std::size_t>(PER + BRANCHES * PER));

        // Main-only query: exactly PER events and all ids prefixed "main_".
        auto main_only = c.queryRange(
            {t0, t0 + ms(1'000'000)}, std::nullopt,
            std::optional<BranchId>{"main"});
        assert(main_only.size() == static_cast<std::size_t>(PER));
        for (const auto& e : main_only) {
            assert(e.branch_id == "main");
            assert(e.id.rfind("main_", 0) == 0);
        }

        // Spot-check 10 branches: each one returns exactly its own PER events.
        for (int b : {0, 7, 13, 42, 50, 77, 88, 91, 95, 99}) {
            const std::string branch = "branch_" + std::to_string(b);
            auto got = c.queryRange(
                {t0, t0 + ms(1'000'000)}, std::nullopt,
                std::optional<BranchId>{branch});
            assert(got.size() == static_cast<std::size_t>(PER));
            const std::string prefix = "b" + std::to_string(b) + "_";
            for (const auto& e : got) {
                assert(e.branch_id == branch);
                assert(e.id.rfind(prefix, 0) == 0);
            }
        }

        // Narrow window inside branch 42's timeline: should return only
        // the handful of branch_42 events whose valid_from is in that window,
        // never leaking any from branch_41 or branch_43 even though their
        // timelines are numerically adjacent.
        {
            const int b = 42;
            const int base = 1000 + b * 1000;  // branch_42 starts here
            auto got = c.queryRange(
                {t0 + ms(base + 10), t0 + ms(base + 15)}, std::nullopt,
                std::optional<BranchId>{"branch_42"});
            // Events b42_10..b42_14 (5 events) have valid_from in [base+10, base+14]
            // and valid_to in [base+11, base+15]. All overlap window.
            // Also b42_9 (valid_to = base+10 = window.start) overlaps.
            // And b42_15 is EXCLUDED (valid_from = base+15 = window.end, included;
            // valid_to = base+16, overlap). So 7 events total: 9..15.
            assert(got.size() == 7);
            for (const auto& e : got) {
                assert(e.branch_id == "branch_42");
            }
        }

        // Refute branch 13 and verify nullopt queries hide it, but explicit
        // branch_13 still sees it.
        c.refuteBranch("branch_13");
        auto after_refute = c.queryRange({t0, t0 + ms(1'000'000)});
        assert(after_refute.size() ==
               static_cast<std::size_t>(PER + (BRANCHES - 1) * PER));
        auto still_there = c.queryRange(
            {t0, t0 + ms(1'000'000)}, std::nullopt,
            std::optional<BranchId>{"branch_13"});
        assert(still_there.size() == static_cast<std::size_t>(PER));

        // Prune branch 77: events gone from every view.
        c.pruneBranch("branch_77");
        auto pruned = c.queryRange(
            {t0, t0 + ms(1'000'000)}, std::nullopt,
            std::optional<BranchId>{"branch_77"});
        assert(pruned.empty());
        // Total is now down by PER events.
        auto after_prune = c.queryRange({t0, t0 + ms(1'000'000)});
        assert(after_prune.size() ==
               static_cast<std::size_t>(PER + (BRANCHES - 2) * PER));
    }

    std::cout << "test_temporal: OK (per-branch time index, 100 branches)\n";
    return 0;
}
