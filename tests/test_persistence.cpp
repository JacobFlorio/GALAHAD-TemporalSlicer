#include "persistence.h"
#include "temporal_engine.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

using namespace galahad;

static bool containsId(const std::vector<TemporalEvent>& v,
                       const std::string& id) {
    return std::any_of(v.begin(), v.end(),
                       [&](const TemporalEvent& e) { return e.id == id; });
}

int main() {
    using namespace std::chrono;
    namespace fs = std::filesystem;

    const auto t0 = Clock::now();
    auto ms = [](int n) { return milliseconds(n); };

    const fs::path tmp =
        fs::temp_directory_path() / "galahad_persistence_roundtrip.gtp";
    if (fs::exists(tmp)) fs::remove(tmp);

    // --- Build a rich core state and save it ---
    {
        TemporalCore core;

        // A four-step perceive -> infer -> decide -> act chain.
        core.addEvent({"perceive",  t0,        t0+ms(5),   t0,
                       "perception", {{"obs","door_open"}}, {}});
        core.addEvent({"infer",     t0+ms(5),  t0+ms(15),  t0+ms(5),
                       "inference", {{"why","wind"}},      {"perceive"}});
        core.addEvent({"decide",    t0+ms(15), t0+ms(20),  t0+ms(15),
                       "decision",  {{"choose","close"}},  {"infer"}});
        // Late-arriving: action occurred at t0+20 but was recorded at t0+200.
        core.addEvent({"act",       t0+ms(20), t0+ms(30),  t0+ms(200),
                       "action",    {{"do","close"}},      {"decide"}});

        // Two competing projections on different branches with confidence.
        TemporalEvent fut_close;
        fut_close.id = "fut_close";
        fut_close.valid_from = t0+ms(30);
        fut_close.valid_to   = t0+ms(40);
        fut_close.recorded_at = t0+ms(20);
        fut_close.type = "projected_action";
        fut_close.data = {{"do","close"}};
        fut_close.causal_links = {"act"};
        fut_close.branch_id = "close_door";
        fut_close.confidence = 0.7;
        core.addProjection(fut_close);

        TemporalEvent fut_ignore = fut_close;
        fut_ignore.id = "fut_ignore";
        fut_ignore.branch_id = "ignore";
        fut_ignore.confidence = 0.3;
        fut_ignore.data = {{"do","wait"}};
        core.addProjection(fut_ignore);

        // Refute one of the branches so the refutation set is non-empty.
        core.refuteBranch("ignore");
        assert(core.isRefuted("ignore"));

        // Snapshot to disk.
        TemporalPersistence(core).save(tmp.string());
        assert(fs::exists(tmp));
        assert(fs::file_size(tmp) > 100);  // non-trivial binary body
    }

    // --- Load into a fresh core and verify the full state round-trips ---
    {
        TemporalCore core;
        TemporalPersistence(core).load(tmp.string());

        // Default-filter query sees 5 events: 4 main + 1 non-refuted projection.
        auto visible = core.queryRange({t0, t0+ms(1000)});
        assert(visible.size() == 5);
        assert(containsId(visible, "perceive"));
        assert(containsId(visible, "infer"));
        assert(containsId(visible, "decide"));
        assert(containsId(visible, "act"));
        assert(containsId(visible, "fut_close"));
        assert(!containsId(visible, "fut_ignore"));  // refuted, hidden

        // Main-only query: exactly the four ground-truth events.
        auto main_only = core.queryRange({t0, t0+ms(1000)}, std::nullopt,
                                         std::optional<std::string>{"main"});
        assert(main_only.size() == 4);

        // close_door branch has its one projected event with confidence 0.7.
        auto close = core.queryRange({t0, t0+ms(1000)}, std::nullopt,
                                     std::optional<std::string>{"close_door"});
        assert(close.size() == 1);
        assert(close[0].id == "fut_close");
        assert(close[0].confidence == 0.7);
        assert(close[0].data.at("do") == "close");

        // Refutation survived the round-trip.
        assert(core.isRefuted("ignore"));

        // Explicit-name query still reaches the refuted branch.
        auto ignore_events = core.queryRange({t0, t0+ms(1000)}, std::nullopt,
                                             std::optional<std::string>{"ignore"});
        assert(ignore_events.size() == 1);
        assert(ignore_events[0].id == "fut_ignore");
        assert(ignore_events[0].confidence == 0.3);

        // Causal chain rebuilt on load: explain("act") returns all three
        // ancestors in temporal order, including the action's full metadata.
        TemporalEngine engine(core);
        auto why = engine.explain("act");
        assert(why.causes.size() == 3);
        assert(why.causes[0].id == "perceive");
        assert(why.causes[1].id == "infer");
        assert(why.causes[2].id == "decide");
        assert(why.causes[0].data.at("obs") == "door_open");
        assert(why.causes[1].data.at("why") == "wind");

        // Bitemporal honesty survives save/load: as-of t0+100ms, the action
        // had not yet been recorded (recorded_at = t0+200ms), so an
        // explanation at that transaction time is empty.
        auto past = engine.explain("act", t0+ms(100));
        assert(past.causes.empty());

        // Allen find_related still works after load.
        auto meets = core.findRelated("perceive", AllenRelation::Meets);
        assert(meets.size() == 1 && meets[0] == "infer");

        // Causal transitive queries still work after load.
        auto anc = core.getAncestors("act");
        assert(anc.size() == 3);
    }

    // --- Error handling: bad magic ---
    {
        const fs::path bad =
            fs::temp_directory_path() / "galahad_persistence_bad.gtp";
        {
            std::ofstream f(bad, std::ios::binary);
            const char junk[] = "NOT-GALAHAD";
            f.write(junk, 10);  // same length as real magic
        }
        TemporalCore core;
        bool threw = false;
        try {
            TemporalPersistence(core).load(bad.string());
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
        fs::remove(bad);
    }

    // --- Error handling: missing file ---
    {
        TemporalCore core;
        bool threw = false;
        try {
            TemporalPersistence(core).load("/this/path/does/not/exist.gtp");
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
    }

    // --- Error handling: truncated body (magic OK, partial version int) ---
    {
        const fs::path trunc =
            fs::temp_directory_path() / "galahad_persistence_trunc.gtp";
        {
            std::ofstream f(trunc, std::ios::binary);
            f.write("GALAHAD-TP", 10);
            const char partial[] = "\x01\x00";  // 2 bytes, need 4
            f.write(partial, 2);
        }
        TemporalCore core;
        bool threw = false;
        try {
            TemporalPersistence(core).load(trunc.string());
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
        fs::remove(trunc);
    }

    // --- Round-trip an empty core: save/load must not fail on zero events. ---
    {
        const fs::path empty =
            fs::temp_directory_path() / "galahad_persistence_empty.gtp";
        TemporalCore source;
        TemporalPersistence(source).save(empty.string());
        TemporalCore target;
        TemporalPersistence(target).load(empty.string());
        assert(target.queryRange({t0 - ms(1), t0 + ms(1000)}).empty());
        fs::remove(empty);
    }

    fs::remove(tmp);
    std::cout << "test_persistence: OK (binary round-trip with causal + "
              << "branching + refutation + bitemporal)\n";
    return 0;
}
