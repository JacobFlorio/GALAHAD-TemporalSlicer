#include "temporal_engine.h"
#include <algorithm>

namespace galahad {

TemporalEngine::TemporalEngine(TemporalCore& core) : core_(core) {}

TemporalEngine::Explanation TemporalEngine::explain(
    const EventId& id,
    std::optional<TimePoint> as_of,
    bool require_completed_before,
    std::optional<BranchId> branch) const {
    auto ancestors = core_.getAncestors(id, as_of, branch);
    Explanation exp;

    std::vector<TemporalEvent> events;
    for (const auto& aid : ancestors) {
        if (auto e = core_.get(aid, as_of, branch)) {
            events.push_back(*e);
        }
    }

    std::sort(events.begin(), events.end(),
              [](const TemporalEvent& a, const TemporalEvent& b) {
                  return a.valid_from < b.valid_from;
              });

    if (require_completed_before) {
        // Look up the target across any branch so we can always filter,
        // even when explaining a projection by its main-timeline causes.
        auto target = core_.get(id, as_of);
        if (target) {
            events.erase(
                std::remove_if(events.begin(), events.end(),
                    [&](const TemporalEvent& e) {
                        return e.valid_to > target->valid_from;
                    }),
                events.end());
        }
    }

    exp.causes = std::move(events);
    exp.completed_before_target = require_completed_before;
    return exp;
}

std::vector<TemporalEvent> TemporalEngine::whatHappenedDuring(
    const TimeWindow& window,
    std::optional<TimePoint> as_of,
    std::optional<BranchId> branch) const {
    return core_.queryRange(window, as_of, branch);
}

} // namespace galahad
