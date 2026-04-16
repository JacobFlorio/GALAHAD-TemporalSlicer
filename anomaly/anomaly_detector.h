#pragma once

#include "temporal_core.h"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace galahad {

// Confidence-decay model.
// Exponential decay with configurable half-life, logarithmic reinforcement
// from repeated observations, and a floor that prevents full forgetting.
struct DecayConfig {
    double half_life_secs = 86400.0;     // 1 day default
    double reinforcement_bonus = 0.15;
    double floor = 0.05;
    double max_confidence = 1.0;
};

// Pre-built domain presets.
inline DecayConfig securityDecay()    { return {86400.0,   0.20, 0.05, 1.0}; }
inline DecayConfig agricultureDecay() { return {604800.0,  0.10, 0.10, 1.0}; }
inline DecayConfig financeDecay()     { return {7776000.0, 0.05, 0.10, 1.0}; }
inline DecayConfig networkDecay()     { return {172800.0,  0.15, 0.05, 1.0}; }  // 2-day for network ops

// Compute decayed confidence for an entity.
//   elapsed_secs    — seconds since last observation
//   observation_count — total observations of this entity
inline double computeDecay(double elapsed_secs,
                           int observation_count,
                           const DecayConfig& cfg = {}) {
    double time_factor = 1.0;
    if (cfg.half_life_secs > 0.0 && !std::isinf(cfg.half_life_secs)) {
        double decay_rate = std::log(2.0) / cfg.half_life_secs;
        time_factor = std::exp(-decay_rate * std::max(0.0, elapsed_secs));
    }
    double reinforcement = std::min(
        cfg.max_confidence,
        cfg.reinforcement_bonus * std::log(1.0 + observation_count));
    double raw = time_factor + reinforcement;
    return std::max(cfg.floor, std::min(cfg.max_confidence, raw));
}

// ---------- Anomaly types ----------

enum class AnomalyType {
    MissingEntity,       // expected entity absent during window
    FrequencySpike,      // event rate exceeds baseline by threshold
    FrequencyDrop,       // event rate drops below baseline
    CoOccurrenceBreak,   // expected co-occurring types, one absent
    Loitering,           // single event spans longer than threshold
    ConfidenceDecay      // decayed confidence below threshold
};

struct AnomalyResult {
    AnomalyType type;
    double severity;                        // 0.0–1.0
    std::string description;
    std::vector<std::string> involved_events;
};

// ---------- AnomalyDetector ----------
//
// Read-only queries over a TemporalCore. Each rule is expressed in terms
// of existing GALAHAD primitives (queryRange, Allen relations, causal DAG).
// No mutation, no new storage — pure composition.

class AnomalyDetector {
public:
    explicit AnomalyDetector(const TemporalCore& core);

    // Detect events of `entity_type` that were present in `baseline` but
    // absent in `current`. "Present" = at least one event whose valid
    // interval overlaps the window. Bitemporal as_of + branch supported.
    std::vector<AnomalyResult> detectMissing(
        const std::string& entity_type,
        TimeWindow baseline,
        TimeWindow current,
        int min_baseline_appearances = 2,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;

    // Compare event rate in `current` against `baseline`.
    // Fires when ratio > spike_threshold (spike) or < 1/spike_threshold (drop).
    std::vector<AnomalyResult> detectFrequencyAnomaly(
        TimeWindow baseline,
        TimeWindow current,
        double spike_threshold = 3.0,
        std::optional<std::string> event_type = std::nullopt,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;

    // Find pairs of event types that co-occur in `baseline` (Allen:
    // Overlaps/During/Contains/Equals within same window) but where one
    // is present and the other absent in `current`.
    std::vector<AnomalyResult> detectCoOccurrenceBreak(
        TimeWindow baseline,
        TimeWindow current,
        int min_co_occurrences = 2,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;

    // Find events whose valid interval (valid_to - valid_from) exceeds
    // `max_duration_secs`.
    std::vector<AnomalyResult> detectLoitering(
        TimeWindow window,
        double max_duration_secs,
        std::optional<std::string> event_type = std::nullopt,
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;

    // Find events whose confidence, after applying exponential decay from
    // recorded_at to `now`, falls below `threshold`.
    std::vector<AnomalyResult> detectConfidenceDecay(
        TimeWindow window,
        TimePoint now,
        double threshold = 0.3,
        const DecayConfig& decay = {},
        std::optional<TimePoint> as_of = std::nullopt,
        std::optional<std::string> branch = std::nullopt) const;

private:
    const TemporalCore& core_;
};

} // namespace galahad
