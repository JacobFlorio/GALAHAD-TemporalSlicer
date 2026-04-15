#pragma once
#include "../core/temporal_core.h"
#include <vector>

namespace galahad {

class TemporalEngine {
public:
    explicit TemporalEngine(TemporalCore& core);

    struct Explanation {
        std::vector<TemporalEvent> causes;   // ordered by valid_from
        bool completed_before_target = true;
    };

    Explanation explain(const EventId& id,
                       std::optional<TimePoint> as_of = std::nullopt,
                       bool require_completed_before = true,
                       std::optional<BranchId> branch = std::nullopt) const;

    std::vector<TemporalEvent> whatHappenedDuring(
        const TimeWindow& window,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<BranchId> branch = std::nullopt) const;

private:
    TemporalCore& core_;
};

} // namespace galahad
