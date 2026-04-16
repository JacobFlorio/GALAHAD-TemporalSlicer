#include "anomaly_detector.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace galahad {

AnomalyDetector::AnomalyDetector(const TemporalCore& core) : core_(core) {}

// ---------- helpers ----------

namespace {

double windowSecs(TimeWindow w) {
    auto d = std::chrono::duration_cast<std::chrono::milliseconds>(
        w.end - w.start);
    return d.count() / 1000.0;
}

double elapsedSecs(TimePoint from, TimePoint to) {
    auto d = std::chrono::duration_cast<std::chrono::milliseconds>(to - from);
    return d.count() / 1000.0;
}

double eventDurationSecs(const TemporalEvent& e) {
    return elapsedSecs(e.valid_from, e.valid_to);
}

// Collect unique event types present in a range query result.
std::unordered_map<std::string, std::vector<std::string>>
groupByType(const std::vector<TemporalEvent>& events) {
    std::unordered_map<std::string, std::vector<std::string>> out;
    for (const auto& e : events) {
        out[e.type].push_back(e.id);
    }
    return out;
}

} // namespace

// ---------- MissingEntity ----------

std::vector<AnomalyResult> AnomalyDetector::detectMissing(
    const std::string& entity_type,
    TimeWindow baseline,
    TimeWindow current,
    int min_baseline_appearances,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {

    auto baseline_events = core_.queryRange(baseline, as_of, branch);
    auto current_events = core_.queryRange(current, as_of, branch);

    // Count appearances per event ID of matching type in baseline.
    std::unordered_map<std::string, int> baseline_counts;
    for (const auto& e : baseline_events) {
        if (e.type == entity_type) {
            baseline_counts[e.id]++;
        }
    }

    // Collect IDs present in current window.
    std::unordered_set<std::string> current_ids;
    for (const auto& e : current_events) {
        if (e.type == entity_type) {
            current_ids.insert(e.id);
        }
    }

    std::vector<AnomalyResult> out;
    for (const auto& [id, count] : baseline_counts) {
        if (count < min_baseline_appearances) continue;
        if (current_ids.count(id)) continue;

        double severity = std::min(1.0,
            static_cast<double>(count) / std::max(1, min_baseline_appearances));

        out.push_back({
            AnomalyType::MissingEntity,
            severity,
            "Entity '" + id + "' (type: " + entity_type +
                ") appeared " + std::to_string(count) +
                " times in baseline but is absent in current window",
            {id}
        });
    }
    return out;
}

// ---------- FrequencyAnomaly ----------

std::vector<AnomalyResult> AnomalyDetector::detectFrequencyAnomaly(
    TimeWindow baseline,
    TimeWindow current,
    double spike_threshold,
    std::optional<std::string> event_type,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {

    auto baseline_events = core_.queryRange(baseline, as_of, branch);
    auto current_events = core_.queryRange(current, as_of, branch);

    // Filter by type if specified.
    auto filter = [&](std::vector<TemporalEvent>& evts) {
        if (!event_type) return;
        evts.erase(
            std::remove_if(evts.begin(), evts.end(),
                [&](const TemporalEvent& e) { return e.type != *event_type; }),
            evts.end());
    };
    filter(baseline_events);
    filter(current_events);

    double baseline_secs = windowSecs(baseline);
    double current_secs = windowSecs(current);
    if (baseline_secs <= 0.0 || current_secs <= 0.0) return {};

    double baseline_rate = baseline_events.size() / baseline_secs;
    double current_rate = current_events.size() / current_secs;

    if (baseline_rate <= 0.0) {
        if (current_rate <= 0.0) return {};
        // Infinite ratio — new activity where there was none.
        std::vector<std::string> ids;
        for (const auto& e : current_events) ids.push_back(e.id);
        return {{
            AnomalyType::FrequencySpike,
            1.0,
            "Event rate went from 0 to " +
                std::to_string(current_rate) + "/s (new activity)",
            std::move(ids)
        }};
    }

    double ratio = current_rate / baseline_rate;
    std::vector<AnomalyResult> out;

    if (ratio > spike_threshold) {
        double severity = std::min(1.0, ratio / (spike_threshold * 2.0));
        std::vector<std::string> ids;
        for (const auto& e : current_events) ids.push_back(e.id);
        out.push_back({
            AnomalyType::FrequencySpike,
            severity,
            "Event rate spiked: " + std::to_string(current_rate) +
                "/s vs baseline " + std::to_string(baseline_rate) +
                "/s (ratio: " + std::to_string(ratio) + "x)",
            std::move(ids)
        });
    } else if (ratio > 0.0 && ratio < 1.0 / spike_threshold) {
        double severity = std::min(1.0, (1.0 / ratio) / (spike_threshold * 2.0));
        std::vector<std::string> ids;
        for (const auto& e : current_events) ids.push_back(e.id);
        out.push_back({
            AnomalyType::FrequencyDrop,
            severity,
            "Event rate dropped: " + std::to_string(current_rate) +
                "/s vs baseline " + std::to_string(baseline_rate) +
                "/s (ratio: " + std::to_string(ratio) + "x)",
            std::move(ids)
        });
    }

    return out;
}

// ---------- CoOccurrenceBreak ----------

std::vector<AnomalyResult> AnomalyDetector::detectCoOccurrenceBreak(
    TimeWindow baseline,
    TimeWindow current,
    int min_co_occurrences,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {

    auto baseline_events = core_.queryRange(baseline, as_of, branch);
    auto current_events = core_.queryRange(current, as_of, branch);

    // Build co-occurrence counts from baseline: for each pair of event types
    // that temporally overlap (Allen: not Precedes/PrecededBy/Meets/MetBy),
    // count how many times they co-occur.
    using TypePair = std::pair<std::string, std::string>;
    std::map<TypePair, int> co_counts;

    for (std::size_t i = 0; i < baseline_events.size(); ++i) {
        for (std::size_t j = i + 1; j < baseline_events.size(); ++j) {
            const auto& a = baseline_events[i];
            const auto& b = baseline_events[j];
            if (a.type == b.type) continue;

            auto rel = core_.getAllenRelation(a, b);
            // Co-occurring = temporally overlapping in some way.
            if (rel == AllenRelation::Precedes ||
                rel == AllenRelation::PrecededBy ||
                rel == AllenRelation::Meets ||
                rel == AllenRelation::MetBy) continue;

            auto key = a.type < b.type
                ? TypePair{a.type, b.type}
                : TypePair{b.type, a.type};
            co_counts[key]++;
        }
    }

    // Collect types present in current window.
    std::unordered_set<std::string> current_types;
    for (const auto& e : current_events) {
        current_types.insert(e.type);
    }

    std::vector<AnomalyResult> out;
    std::set<TypePair> reported;

    for (const auto& [pair, count] : co_counts) {
        if (count < min_co_occurrences) continue;
        bool a_present = current_types.count(pair.first) > 0;
        bool b_present = current_types.count(pair.second) > 0;

        if (a_present == b_present) continue;  // both or neither
        if (reported.count(pair)) continue;
        reported.insert(pair);

        const auto& present = a_present ? pair.first : pair.second;
        const auto& missing = a_present ? pair.second : pair.first;

        out.push_back({
            AnomalyType::CoOccurrenceBreak,
            std::min(1.0, static_cast<double>(count) / (min_co_occurrences * 2.0)),
            "Type '" + present + "' is present but co-occurring type '" +
                missing + "' is absent (baseline co-occurrences: " +
                std::to_string(count) + ")",
            {}
        });
    }
    return out;
}

// ---------- Loitering ----------

std::vector<AnomalyResult> AnomalyDetector::detectLoitering(
    TimeWindow window,
    double max_duration_secs,
    std::optional<std::string> event_type,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {

    auto events = core_.queryRange(window, as_of, branch);

    std::vector<AnomalyResult> out;
    for (const auto& e : events) {
        if (event_type && e.type != *event_type) continue;
        double dur = eventDurationSecs(e);
        if (dur <= max_duration_secs) continue;

        out.push_back({
            AnomalyType::Loitering,
            std::min(1.0, dur / (max_duration_secs * 2.0)),
            "Event '" + e.id + "' (type: " + e.type +
                ") spans " + std::to_string(dur) +
                "s, exceeding threshold of " +
                std::to_string(max_duration_secs) + "s",
            {e.id}
        });
    }
    return out;
}

// ---------- ConfidenceDecay ----------

std::vector<AnomalyResult> AnomalyDetector::detectConfidenceDecay(
    TimeWindow window,
    TimePoint now,
    double threshold,
    const DecayConfig& decay,
    std::optional<TimePoint> as_of,
    std::optional<std::string> branch) const {

    auto events = core_.queryRange(window, as_of, branch);

    // Group by event ID to count observations (for reinforcement).
    std::unordered_map<std::string, int> obs_counts;
    for (const auto& e : events) {
        obs_counts[e.id]++;
    }

    std::vector<AnomalyResult> out;
    std::unordered_set<std::string> reported;

    for (const auto& e : events) {
        if (reported.count(e.id)) continue;
        double elapsed = elapsedSecs(e.recorded_at, now);
        double decayed = computeDecay(elapsed, obs_counts[e.id], decay);

        if (decayed >= threshold) continue;
        reported.insert(e.id);

        out.push_back({
            AnomalyType::ConfidenceDecay,
            std::min(1.0, (threshold - decayed) / threshold),
            "Event '" + e.id + "' confidence decayed to " +
                std::to_string(decayed) + " (threshold: " +
                std::to_string(threshold) + ", elapsed: " +
                std::to_string(elapsed) + "s)",
            {e.id}
        });
    }
    return out;
}

} // namespace galahad
