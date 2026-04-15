#include "temporal_core.h"

#include <algorithm>
#include <queue>
#include <unordered_set>

namespace galahad {

namespace {

// Branch filter test over the stored event (uint32 comparisons). If an
// explicit branch is set the event must match it; otherwise we honor the
// refuted-branch set.
bool passesBranch(InternalBranchId event_branch,
                  const std::optional<InternalBranchId>& filter,
                  const std::unordered_set<InternalBranchId>& refuted) {
    if (filter) return event_branch == *filter;
    return refuted.find(event_branch) == refuted.end();
}

AllenRelation allenOf(TimePoint as, TimePoint ae, TimePoint bs, TimePoint be) {
    if (ae < bs) return AllenRelation::Precedes;
    if (bs == ae && !(as == ae && as == be)) return AllenRelation::Meets;
    if (be < as) return AllenRelation::PrecededBy;
    if (be == as && !(as == ae && as == be)) return AllenRelation::MetBy;

    if (as == bs && ae == be) return AllenRelation::Equals;
    if (as == bs) return ae < be ? AllenRelation::Starts : AllenRelation::StartedBy;
    if (ae == be) return as > bs ? AllenRelation::Finishes : AllenRelation::FinishedBy;
    if (as < bs && ae > be) return AllenRelation::Contains;
    if (as > bs && ae < be) return AllenRelation::During;
    if (as < bs && ae < be) return AllenRelation::Overlaps;
    return AllenRelation::OverlappedBy;
}

} // namespace

// ---------- construction ----------

TemporalCore::TemporalCore() {
    // Reserve branch handle 0 for "main" so branch comparisons on the hot
    // path are uint32 equality and the default branch costs nothing.
    branchIdPool.emplace("main", 0);
    branchIdReverse.emplace(0, "main");
}

// ---------- interning ----------

InternalEventId TemporalCore::internEventId(const std::string& s) {
    auto it = eventIdPool.find(s);
    if (it != eventIdPool.end()) return it->second;
    const InternalEventId h = nextEventId++;
    eventIdPool.emplace(s, h);
    eventIdReverse.emplace(h, s);
    return h;
}

InternalBranchId TemporalCore::internBranchId(const std::string& s) {
    auto it = branchIdPool.find(s);
    if (it != branchIdPool.end()) return it->second;
    const InternalBranchId h = nextBranchId++;
    branchIdPool.emplace(s, h);
    branchIdReverse.emplace(h, s);
    return h;
}

std::optional<InternalEventId> TemporalCore::lookupEventId(
    const std::string& s) const {
    auto it = eventIdPool.find(s);
    if (it == eventIdPool.end()) return std::nullopt;
    return it->second;
}

std::optional<InternalBranchId> TemporalCore::lookupBranchId(
    const std::string& s) const {
    auto it = branchIdPool.find(s);
    if (it == branchIdPool.end()) return std::nullopt;
    return it->second;
}

std::string TemporalCore::resolveEventId(InternalEventId id) const {
    auto it = eventIdReverse.find(id);
    if (it == eventIdReverse.end()) return {};
    return it->second;
}

std::string TemporalCore::resolveBranchId(InternalBranchId id) const {
    auto it = branchIdReverse.find(id);
    if (it == branchIdReverse.end()) return {};
    return it->second;
}

TemporalCore::StoredEvent TemporalCore::store(TemporalEvent&& e) {
    StoredEvent s;
    s.id = internEventId(e.id);
    s.valid_from = e.valid_from;
    s.valid_to = e.valid_to;
    s.recorded_at = e.recorded_at;
    s.type = std::move(e.type);
    s.branch_id = internBranchId(e.branch_id);
    s.confidence = e.confidence;

    // std::map iterates sorted by key, so moving into a vector preserves
    // the sort order and we never have to re-sort.
    s.data.reserve(e.data.size());
    for (auto& [k, v] : e.data) {
        s.data.emplace_back(k, std::move(v));
    }

    s.causal_links.reserve(e.causal_links.size());
    for (const auto& link : e.causal_links) {
        s.causal_links.push_back(internEventId(link));
    }
    return s;
}

TemporalEvent TemporalCore::materialize(const StoredEvent& s) const {
    TemporalEvent e;
    e.id = resolveEventId(s.id);
    e.valid_from = s.valid_from;
    e.valid_to = s.valid_to;
    e.recorded_at = s.recorded_at;
    e.type = s.type;
    // Rebuild the public map from the flat sorted vector. std::map with
    // sorted-input hint is O(N) instead of O(N log N).
    for (const auto& [k, v] : s.data) {
        e.data.emplace_hint(e.data.end(), k, v);
    }
    e.causal_links.reserve(s.causal_links.size());
    for (auto link : s.causal_links) {
        e.causal_links.push_back(resolveEventId(link));
    }
    e.branch_id = resolveBranchId(s.branch_id);
    e.confidence = s.confidence;
    return e;
}

// ---------- monotonic clock ----------

TimePoint TemporalCore::now() const {
    auto wall = Clock::now();
    if (wall <= lastNow) wall = lastNow + std::chrono::nanoseconds(1);
    lastNow = wall;
    return wall;
}

TimePoint TemporalCore::getMonotonicNow() {
    return now();
}

TimePoint TemporalCore::computePromoteTime() const {
    auto t = now();
    for (const auto& e : events) {
        if (e.recorded_at >= t) {
            t = e.recorded_at + std::chrono::nanoseconds(1);
        }
    }
    lastNow = t;
    return t;
}

// ---------- index maintenance ----------

void TemporalCore::updateIndices(const StoredEvent& /*e*/, EventIndex idx) {
    auto& versions = idIndex[events[idx].id];
    auto pos = std::upper_bound(
        versions.begin(), versions.end(), idx,
        [this](EventIndex a, EventIndex b) {
            return events[a].recorded_at > events[b].recorded_at;
        });
    versions.insert(pos, idx);

    dirtyTime = true;
    dirtyBranchTime = true;
    dirtyCausal = true;
}

void TemporalCore::rebuildTimeIndex() const {
    timeIndex.clear();
    timeIndex.reserve(events.size());
    for (EventIndex i = 0; i < events.size(); ++i) {
        timeIndex.emplace_back(events[i].valid_from, i);
    }
    std::sort(timeIndex.begin(), timeIndex.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    dirtyTime = false;
}

void TemporalCore::rebuildBranchTimeIndices() const {
    branchTimeIndices.clear();
    for (EventIndex i = 0; i < events.size(); ++i) {
        branchTimeIndices[events[i].branch_id].emplace_back(
            events[i].valid_from, i);
    }
    for (auto& [_, vec] : branchTimeIndices) {
        std::sort(vec.begin(), vec.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    dirtyBranchTime = false;
}

void TemporalCore::ensureTimeFresh() const {
    if (dirtyTime) rebuildTimeIndex();
}

void TemporalCore::ensureBranchTimeFresh() const {
    if (dirtyBranchTime) rebuildBranchTimeIndices();
}

void TemporalCore::rebuildCausalIndices() const {
    causalGraph.clear();
    reverseCausal.clear();
    std::unordered_set<InternalEventId> known;
    known.reserve(events.size());
    for (const auto& e : events) known.insert(e.id);

    for (const auto& e : events) {
        auto& causes = causalGraph[e.id];
        for (auto link : e.causal_links) {
            if (known.count(link)) {
                causes.push_back(link);
                reverseCausal[link].push_back(e.id);
            }
        }
    }
    dirtyCausal = false;
}

void TemporalCore::ensureCausalFresh() const {
    if (dirtyCausal) rebuildCausalIndices();
}

void TemporalCore::buildCausalGraph() {
    rebuildCausalIndices();
}

const std::vector<std::pair<TimePoint, EventIndex>>*
TemporalCore::pickTimeIndex(const std::optional<InternalBranchId>& branch) const {
    if (branch) {
        ensureBranchTimeFresh();
        auto it = branchTimeIndices.find(*branch);
        if (it == branchTimeIndices.end()) return nullptr;
        return &it->second;
    }
    ensureTimeFresh();
    return &timeIndex;
}

// ---------- mutation ----------

void TemporalCore::addEvent(TemporalEvent event) {
    events.push_back(store(std::move(event)));
    updateIndices(events.back(), events.size() - 1);
}

void TemporalCore::addProjection(TemporalEvent event) {
    if (event.branch_id == "main") return;
    addEvent(std::move(event));
}

// ---------- core queries ----------

std::vector<TemporalEvent> TemporalCore::queryRange(
    TimeWindow window,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {

    std::optional<InternalBranchId> branch_h;
    if (branch) {
        auto h = lookupBranchId(*branch);
        if (!h) return {};  // branch has no events
        branch_h = *h;
    }

    const auto* idx = pickTimeIndex(branch_h);
    if (!idx) return {};

    std::vector<TemporalEvent> out;
    auto upper = std::upper_bound(
        idx->begin(), idx->end(), window.end,
        [](const TimePoint& v, const std::pair<TimePoint, EventIndex>& p) {
            return v < p.first;
        });

    for (auto it = idx->begin(); it != upper; ++it) {
        const auto& e = events[it->second];
        if (e.valid_to < window.start) continue;
        if (as_of && e.recorded_at > *as_of) continue;
        if (!branch_h && refutedBranches.count(e.branch_id)) continue;
        out.push_back(materialize(e));
    }
    return out;
}

std::optional<TemporalEvent> TemporalCore::get(
    const std::string& id,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {

    auto id_h = lookupEventId(id);
    if (!id_h) return std::nullopt;

    std::optional<InternalBranchId> branch_h;
    if (branch) {
        auto h = lookupBranchId(*branch);
        if (!h) return std::nullopt;
        branch_h = *h;
    }

    auto it = idIndex.find(*id_h);
    if (it == idIndex.end()) return std::nullopt;
    for (EventIndex idx : it->second) {
        const auto& e = events[idx];
        if (as_of && e.recorded_at > *as_of) continue;
        if (!passesBranch(e.branch_id, branch_h, refutedBranches)) continue;
        return materialize(e);
    }
    return std::nullopt;
}

// ---------- causal queries ----------
//
// These all take string inputs (public API) and convert once at the top
// to uint32 handles. All traversal, filter, and set-membership work on
// handles — no string hashing or comparison in the hot loop.

bool TemporalCore::hasCycle() const {
    ensureCausalFresh();
    enum Color { White, Gray, Black };
    std::unordered_map<InternalEventId, Color> color;
    color.reserve(causalGraph.size());
    for (const auto& [id, _] : causalGraph) color[id] = White;

    for (const auto& [start, _] : causalGraph) {
        if (color[start] != White) continue;
        color[start] = Gray;
        std::vector<std::pair<InternalEventId, std::size_t>> stack;
        stack.push_back({start, 0});

        while (!stack.empty()) {
            auto& top = stack.back();
            auto it = causalGraph.find(top.first);
            if (it != causalGraph.end() && top.second < it->second.size()) {
                const InternalEventId next = it->second[top.second++];
                auto cIt = color.find(next);
                if (cIt == color.end()) continue;
                if (cIt->second == Gray) return true;
                if (cIt->second == White) {
                    cIt->second = Gray;
                    stack.push_back({next, 0});
                }
            } else {
                color[top.first] = Black;
                stack.pop_back();
            }
        }
    }
    return false;
}

namespace {
// Same filter logic as passesBranch + as_of gate, but takes raw fields
// so it can live outside the class (StoredEvent is private).
bool eventVisible(TimePoint recorded_at,
                  InternalBranchId branch_id,
                  const std::optional<TimePoint>& as_of,
                  const std::optional<InternalBranchId>& branch_h,
                  const std::unordered_set<InternalBranchId>& refuted) {
    if (as_of && recorded_at > *as_of) return false;
    return passesBranch(branch_id, branch_h, refuted);
}
} // namespace

std::vector<std::string> TemporalCore::getCauses(
    const std::string& id,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {
    ensureCausalFresh();

    auto id_h = lookupEventId(id);
    if (!id_h) return {};
    // Source event must exist at as_of (across any branch).
    if (as_of) {
        auto it = idIndex.find(*id_h);
        if (it == idIndex.end()) return {};
        bool seen = false;
        for (EventIndex idx : it->second) {
            if (events[idx].recorded_at <= *as_of) { seen = true; break; }
        }
        if (!seen) return {};
    }

    std::optional<InternalBranchId> branch_h;
    if (branch) {
        auto h = lookupBranchId(*branch);
        if (!h) return {};
        branch_h = *h;
    }

    auto it = causalGraph.find(*id_h);
    if (it == causalGraph.end()) return {};
    std::vector<std::string> out;
    for (auto cause : it->second) {
        auto vit = idIndex.find(cause);
        if (vit == idIndex.end()) continue;
        for (EventIndex idx : vit->second) {
            if (eventVisible(events[idx].recorded_at, events[idx].branch_id, as_of, branch_h, refutedBranches)) {
                out.push_back(resolveEventId(cause));
                break;
            }
        }
    }
    return out;
}

std::vector<std::string> TemporalCore::getEffects(
    const std::string& id,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {
    ensureCausalFresh();

    auto id_h = lookupEventId(id);
    if (!id_h) return {};
    if (as_of) {
        auto it = idIndex.find(*id_h);
        if (it == idIndex.end()) return {};
        bool seen = false;
        for (EventIndex idx : it->second) {
            if (events[idx].recorded_at <= *as_of) { seen = true; break; }
        }
        if (!seen) return {};
    }

    std::optional<InternalBranchId> branch_h;
    if (branch) {
        auto h = lookupBranchId(*branch);
        if (!h) return {};
        branch_h = *h;
    }

    auto it = reverseCausal.find(*id_h);
    if (it == reverseCausal.end()) return {};
    std::vector<std::string> out;
    for (auto child : it->second) {
        auto vit = idIndex.find(child);
        if (vit == idIndex.end()) continue;
        for (EventIndex idx : vit->second) {
            if (eventVisible(events[idx].recorded_at, events[idx].branch_id, as_of, branch_h, refutedBranches)) {
                out.push_back(resolveEventId(child));
                break;
            }
        }
    }
    return out;
}

std::vector<std::string> TemporalCore::getAncestors(
    const std::string& id,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {
    ensureCausalFresh();

    auto id_h = lookupEventId(id);
    if (!id_h) return {};
    if (as_of) {
        auto it = idIndex.find(*id_h);
        if (it == idIndex.end()) return {};
        bool seen = false;
        for (EventIndex idx : it->second) {
            if (events[idx].recorded_at <= *as_of) { seen = true; break; }
        }
        if (!seen) return {};
    }

    std::optional<InternalBranchId> branch_h;
    if (branch) {
        auto h = lookupBranchId(*branch);
        if (!h) return {};
        branch_h = *h;
    }

    auto nodeVisible = [&](InternalEventId node) {
        auto vit = idIndex.find(node);
        if (vit == idIndex.end()) return false;
        for (EventIndex idx : vit->second) {
            if (eventVisible(events[idx].recorded_at, events[idx].branch_id, as_of, branch_h, refutedBranches))
                return true;
        }
        return false;
    };

    std::vector<std::string> out;
    std::unordered_set<InternalEventId> seen;
    std::queue<InternalEventId> frontier;
    frontier.push(*id_h);
    seen.insert(*id_h);

    while (!frontier.empty()) {
        InternalEventId cur = frontier.front();
        frontier.pop();
        auto it = causalGraph.find(cur);
        if (it == causalGraph.end()) continue;
        for (auto parent : it->second) {
            if (seen.count(parent)) continue;
            if (!nodeVisible(parent)) continue;
            seen.insert(parent);
            out.push_back(resolveEventId(parent));
            frontier.push(parent);
        }
    }
    return out;
}

std::vector<std::string> TemporalCore::getDescendants(
    const std::string& id,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {
    ensureCausalFresh();

    auto id_h = lookupEventId(id);
    if (!id_h) return {};
    if (as_of) {
        auto it = idIndex.find(*id_h);
        if (it == idIndex.end()) return {};
        bool seen = false;
        for (EventIndex idx : it->second) {
            if (events[idx].recorded_at <= *as_of) { seen = true; break; }
        }
        if (!seen) return {};
    }

    std::optional<InternalBranchId> branch_h;
    if (branch) {
        auto h = lookupBranchId(*branch);
        if (!h) return {};
        branch_h = *h;
    }

    auto nodeVisible = [&](InternalEventId node) {
        auto vit = idIndex.find(node);
        if (vit == idIndex.end()) return false;
        for (EventIndex idx : vit->second) {
            if (eventVisible(events[idx].recorded_at, events[idx].branch_id, as_of, branch_h, refutedBranches))
                return true;
        }
        return false;
    };

    std::vector<std::string> out;
    std::unordered_set<InternalEventId> seen;
    std::queue<InternalEventId> frontier;
    frontier.push(*id_h);
    seen.insert(*id_h);

    while (!frontier.empty()) {
        InternalEventId cur = frontier.front();
        frontier.pop();
        auto it = reverseCausal.find(cur);
        if (it == reverseCausal.end()) continue;
        for (auto child : it->second) {
            if (seen.count(child)) continue;
            if (!nodeVisible(child)) continue;
            seen.insert(child);
            out.push_back(resolveEventId(child));
            frontier.push(child);
        }
    }
    return out;
}

// ---------- Allen ----------

AllenRelation TemporalCore::getAllenRelation(
    const TemporalEvent& a, const TemporalEvent& b) const {
    return allenOf(a.valid_from, a.valid_to, b.valid_from, b.valid_to);
}

bool TemporalCore::holds(
    AllenRelation r, const TemporalEvent& a, const TemporalEvent& b) const {
    return getAllenRelation(a, b) == r;
}

std::vector<std::string> TemporalCore::findRelated(
    const std::string& id, AllenRelation r,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {

    auto id_h = lookupEventId(id);
    if (!id_h) return {};

    // Find the latest visible version of the source at as_of (across any branch).
    auto vit = idIndex.find(*id_h);
    if (vit == idIndex.end()) return {};
    const StoredEvent* src = nullptr;
    for (EventIndex idx : vit->second) {
        const auto& e = events[idx];
        if (as_of && e.recorded_at > *as_of) continue;
        src = &e;
        break;
    }
    if (!src) return {};

    std::optional<InternalBranchId> branch_h;
    if (branch) {
        auto h = lookupBranchId(*branch);
        if (!h) return {};
        branch_h = *h;
    }

    const auto* idx = pickTimeIndex(branch_h);
    if (!idx) return {};

    auto accept = [&](const StoredEvent& e) {
        if (e.id == *id_h) return false;
        if (as_of && e.recorded_at > *as_of) return false;
        if (!branch_h && refutedBranches.count(e.branch_id)) return false;
        return allenOf(src->valid_from, src->valid_to,
                       e.valid_from, e.valid_to) == r;
    };

    std::vector<std::string> out;

    switch (r) {
    case AllenRelation::Precedes:
    case AllenRelation::Meets: {
        auto lower = std::lower_bound(
            idx->begin(), idx->end(), src->valid_to,
            [](const std::pair<TimePoint, EventIndex>& p, const TimePoint& v) {
                return p.first < v;
            });
        for (auto it = lower; it != idx->end(); ++it) {
            const auto& e = events[it->second];
            if (accept(e)) out.push_back(resolveEventId(e.id));
        }
        return out;
    }
    case AllenRelation::PrecededBy:
    case AllenRelation::MetBy: {
        auto upper = std::upper_bound(
            idx->begin(), idx->end(), src->valid_from,
            [](const TimePoint& v, const std::pair<TimePoint, EventIndex>& p) {
                return v < p.first;
            });
        for (auto it = idx->begin(); it != upper; ++it) {
            const auto& e = events[it->second];
            if (accept(e)) out.push_back(resolveEventId(e.id));
        }
        return out;
    }
    default: {
        auto upper = std::upper_bound(
            idx->begin(), idx->end(), src->valid_to,
            [](const TimePoint& v, const std::pair<TimePoint, EventIndex>& p) {
                return v < p.first;
            });
        for (auto it = idx->begin(); it != upper; ++it) {
            const auto& e = events[it->second];
            if (e.valid_to < src->valid_from) continue;
            if (accept(e)) out.push_back(resolveEventId(e.id));
        }
        return out;
    }
    }
}

// ---------- projections ----------

std::vector<TemporalEvent> TemporalCore::getProjections(
    std::optional<std::string> branch) const {
    std::optional<InternalBranchId> branch_h;
    if (branch) {
        auto h = lookupBranchId(*branch);
        if (!h) return {};
        branch_h = *h;
    }

    std::vector<TemporalEvent> out;
    for (const auto& e : events) {
        if (e.branch_id == 0) continue;  // 0 == "main"
        if (branch_h) {
            if (e.branch_id != *branch_h) continue;
        } else {
            if (refutedBranches.count(e.branch_id)) continue;
        }
        out.push_back(materialize(e));
    }
    return out;
}

// ---------- lifecycle ----------

bool TemporalCore::isRefuted(const std::string& branch) const {
    auto h = lookupBranchId(branch);
    if (!h) return false;
    return refutedBranches.count(*h) > 0;
}

void TemporalCore::promoteBranch(const std::string& branch) {
    if (branch == "main") return;
    auto h = lookupBranchId(branch);
    if (!h) return;
    const InternalBranchId bh = *h;

    const auto promote_time = computePromoteTime();

    std::vector<StoredEvent> to_add;
    for (const auto& e : events) {
        if (e.branch_id != bh) continue;
        StoredEvent copy = e;
        copy.branch_id = 0;  // main
        copy.recorded_at = promote_time;
        copy.confidence = 1.0;
        to_add.push_back(std::move(copy));
    }
    for (auto& e : to_add) {
        events.push_back(std::move(e));
        updateIndices(events.back(), events.size() - 1);
    }

    refutedBranches.erase(bh);
}

void TemporalCore::refuteBranch(const std::string& branch) {
    if (branch == "main") return;
    const auto h = internBranchId(branch);  // allow refuting before use
    refutedBranches.insert(h);
}

void TemporalCore::pruneBranch(const std::string& branch) {
    if (branch == "main") return;
    auto h = lookupBranchId(branch);
    if (!h) return;
    const InternalBranchId bh = *h;

    std::deque<StoredEvent> kept;
    for (auto& e : events) {
        if (e.branch_id != bh) kept.push_back(std::move(e));
    }
    events = std::move(kept);

    idIndex.clear();
    for (EventIndex i = 0; i < events.size(); ++i) {
        idIndex[events[i].id].push_back(i);
    }
    for (auto& [_, versions] : idIndex) {
        std::sort(versions.begin(), versions.end(),
            [this](EventIndex a, EventIndex b) {
                return events[a].recorded_at > events[b].recorded_at;
            });
    }

    dirtyTime = true;
    dirtyBranchTime = true;
    dirtyCausal = true;
    refutedBranches.erase(bh);
}

} // namespace galahad
