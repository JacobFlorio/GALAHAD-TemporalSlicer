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

// A counterfactual explanation returned by TemporalCore::whyNot(id).
// One entry per refuted projection branch that contained a prediction
// of the queried event. The would-have-been causes walk is performed
// without refutation filtering so the caller sees the full hypothetical
// causal chain, not just the subset that survived.
struct CounterfactualExplanation {
    std::string branch;                                   // refuted branch name
    TemporalEvent hypothetical_event;                     // the predicted event
    std::vector<TemporalEvent> would_have_been_causes;    // transitive ancestors
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

    // Full event snapshot in insertion order. Materializes every stored
    // event (including refuted branches and historical versions) back into
    // public TemporalEvent form. Intended for persistence, debugging, and
    // any consumer that needs to walk the complete state.
    std::vector<TemporalEvent> getAllEvents() const;

    // Names of branches currently marked refuted.
    std::vector<std::string> getRefutedBranches() const;

    // Deep copy of the current core — independent event store, indices,
    // pools, refutation set, and clock. Intended for hypothetical
    // reasoning: clone, mutate the copy, query, drop. All members are
    // value types so the default member-wise copy is a true deep copy
    // with no shared state.
    TemporalCore clone() const;

    // Counterfactual query: "why did this event NOT happen?"
    // Returns one entry per refuted projection branch that contained a
    // prediction of `id`, including the predicted event and its would-
    // have-been causal chain (walked without refutation filtering so the
    // caller sees the full hypothetical ancestry). Empty if the event
    // actually happened on main, if no prediction of it was ever made,
    // or if every predicting branch was promoted rather than refuted.
    std::vector<CounterfactualExplanation> whyNot(const std::string& id) const;

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
