#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace galahad {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

using EventId = std::string;
using BranchId = std::string;

using InternalEventId = std::uint32_t;
using InternalBranchId = std::uint32_t;
using EventIndex = std::size_t;

struct TemporalEvent {
    std::string id;
    TimePoint valid_from;
    TimePoint valid_to;
    TimePoint recorded_at;
    std::string type;
    std::map<std::string, std::string> data;
    std::vector<std::string> causal_links;
    std::string branch_id = "main";
    double confidence = 1.0;
};

struct TimeWindow {
    TimePoint start;
    TimePoint end;
};

enum class AllenRelation {
    Precedes, Meets, Overlaps, FinishedBy, Contains, Starts, Equals,
    StartedBy, During, Finishes, OverlappedBy, MetBy, PrecededBy
};

class TemporalCore {
public:
    TemporalCore();

    void addEvent(TemporalEvent event);
    void addProjection(TemporalEvent event);

    std::vector<TemporalEvent> queryRange(
        TimeWindow window,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;

    std::optional<TemporalEvent> get(
        const std::string& id,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;

    void buildCausalGraph();
    bool hasCycle() const;

    std::vector<std::string> getAncestors(
        const std::string& id,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;
    std::vector<std::string> getDescendants(
        const std::string& id,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;
    std::vector<std::string> getCauses(
        const std::string& id,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;
    std::vector<std::string> getEffects(
        const std::string& id,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;

    AllenRelation getAllenRelation(const TemporalEvent& a, const TemporalEvent& b) const;
    bool holds(AllenRelation r, const TemporalEvent& a, const TemporalEvent& b) const;
    std::vector<std::string> findRelated(
        const std::string& id, AllenRelation r,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;

    std::vector<TemporalEvent> getProjections(
        std::optional<std::string> branch = std::nullopt) const;

    void promoteBranch(const std::string& branch);
    void refuteBranch(const std::string& branch);
    void pruneBranch(const std::string& branch);
    bool isRefuted(const std::string& branch) const;

    TimePoint now() const;

private:
    struct StoredEvent {
        InternalEventId id;
        TimePoint valid_from;
        TimePoint valid_to;
        TimePoint recorded_at;
        std::string type;
        // Flat, sorted-by-key pair list. One allocation per event instead
        // of N red-black-tree nodes. std::map iterates in key order so
        // the copy-in path naturally produces a sorted vector.
        std::vector<std::pair<std::string, std::string>> data;
        std::vector<InternalEventId> causal_links;
        InternalBranchId branch_id;
        double confidence = 1.0;
    };

    std::deque<StoredEvent> events;

    std::unordered_map<InternalEventId, std::vector<EventIndex>> idIndex;
    mutable std::vector<std::pair<TimePoint, EventIndex>> timeIndex;
    mutable std::unordered_map<InternalBranchId,
        std::vector<std::pair<TimePoint, EventIndex>>> branchTimeIndices;
    mutable std::unordered_map<InternalEventId, std::vector<InternalEventId>> causalGraph;
    mutable std::unordered_map<InternalEventId, std::vector<InternalEventId>> reverseCausal;
    std::unordered_set<InternalBranchId> refutedBranches;

    std::unordered_map<std::string, InternalEventId> eventIdPool;
    std::unordered_map<std::string, InternalBranchId> branchIdPool;
    std::unordered_map<InternalEventId, std::string> eventIdReverse;
    std::unordered_map<InternalBranchId, std::string> branchIdReverse;
    InternalEventId nextEventId = 1;
    InternalBranchId nextBranchId = 1;  // 0 reserved for "main"

    mutable TimePoint lastNow = TimePoint::min();
    mutable bool dirtyCausal = true;
    mutable bool dirtyTime = true;
    mutable bool dirtyBranchTime = true;

    InternalEventId internEventId(const std::string& s);
    InternalBranchId internBranchId(const std::string& s);
    std::optional<InternalEventId> lookupEventId(const std::string& s) const;
    std::optional<InternalBranchId> lookupBranchId(const std::string& s) const;
    std::string resolveEventId(InternalEventId id) const;
    std::string resolveBranchId(InternalBranchId id) const;

    StoredEvent store(TemporalEvent&& e);
    TemporalEvent materialize(const StoredEvent& se) const;

    void updateIndices(const StoredEvent& e, EventIndex idx);
    void rebuildCausalIndices() const;
    void rebuildTimeIndex() const;
    void rebuildBranchTimeIndices() const;
    void ensureCausalFresh() const;
    void ensureTimeFresh() const;
    void ensureBranchTimeFresh() const;
    TimePoint getMonotonicNow();
    TimePoint computePromoteTime() const;

    const std::vector<std::pair<TimePoint, EventIndex>>* pickTimeIndex(
        const std::optional<InternalBranchId>& branch) const;
};

} // namespace galahad
