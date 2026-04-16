#include "temporal_core.h"
#include "anomaly_detector.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>

using namespace galahad;
using namespace std::chrono;

namespace {

TimePoint tp(int secs) {
    return TimePoint(seconds(secs));
}

TemporalEvent ev(const std::string& id, int from, int to,
                 const std::string& type = "default",
                 const std::string& branch = "main",
                 double confidence = 1.0) {
    TemporalEvent e;
    e.id = id;
    e.valid_from = tp(from);
    e.valid_to = tp(to);
    e.recorded_at = tp(from);
    e.type = type;
    e.branch_id = branch;
    e.confidence = confidence;
    return e;
}

// ---------- computeDecay ----------

void test_decay_basic() {
    std::cout << "  decay_basic ... ";
    DecayConfig cfg;
    cfg.half_life_secs = 86400.0;
    cfg.reinforcement_bonus = 0.0;
    cfg.floor = 0.0;

    // At t=0, confidence = 1.0
    assert(std::abs(computeDecay(0.0, 0, cfg) - 1.0) < 1e-9);

    // At half-life, confidence = 0.5
    assert(std::abs(computeDecay(86400.0, 0, cfg) - 0.5) < 1e-6);

    // At 2x half-life, confidence = 0.25
    assert(std::abs(computeDecay(172800.0, 0, cfg) - 0.25) < 1e-6);

    std::cout << "ok\n";
}

void test_decay_reinforcement() {
    std::cout << "  decay_reinforcement ... ";
    DecayConfig cfg;
    cfg.half_life_secs = 86400.0;
    cfg.reinforcement_bonus = 0.15;
    cfg.floor = 0.05;

    // More observations = higher confidence at same elapsed time.
    double c1 = computeDecay(86400.0, 1, cfg);
    double c10 = computeDecay(86400.0, 10, cfg);
    assert(c10 > c1);

    // Floor is respected.
    double c_old = computeDecay(1e9, 0, cfg);
    assert(c_old >= cfg.floor);

    std::cout << "ok\n";
}

void test_decay_presets() {
    std::cout << "  decay_presets ... ";
    auto sec = securityDecay();
    auto net = networkDecay();
    auto fin = financeDecay();

    // Security decays faster than finance.
    double sec_val = computeDecay(86400.0, 0, sec);
    double fin_val = computeDecay(86400.0, 0, fin);
    assert(sec_val < fin_val);

    // Network has 2-day half-life.
    assert(std::abs(net.half_life_secs - 172800.0) < 1e-6);

    std::cout << "ok\n";
}

// ---------- MissingEntity ----------

void test_missing_entity() {
    std::cout << "  missing_entity ... ";
    TemporalCore core;
    // Baseline: server_a appears 3 times, server_b appears 1 time.
    core.addEvent(ev("server_a_1", 100, 200, "heartbeat"));
    core.addEvent(ev("server_a_2", 200, 300, "heartbeat"));
    core.addEvent(ev("server_a_3", 300, 400, "heartbeat"));
    core.addEvent(ev("server_b_1", 150, 250, "heartbeat"));
    // Current: only server_b present.
    core.addEvent(ev("server_b_2", 500, 600, "heartbeat"));

    AnomalyDetector det(core);
    auto results = det.detectMissing(
        "heartbeat",
        {tp(100), tp(400)},  // baseline
        {tp(500), tp(600)},  // current
        2);                  // min_baseline_appearances

    // server_a had 3 appearances in baseline, absent in current.
    // server_b had only 1 appearance — below threshold.
    assert(results.size() == 1);
    assert(results[0].type == AnomalyType::MissingEntity);
    assert(results[0].description.find("server_a") != std::string::npos);
    assert(results[0].severity > 0.0);

    std::cout << "ok\n";
}

// ---------- FrequencyAnomaly ----------

void test_frequency_spike() {
    std::cout << "  frequency_spike ... ";
    TemporalCore core;
    // Baseline: 2 events in 100s = 0.02/s
    core.addEvent(ev("base_1", 100, 110, "alert"));
    core.addEvent(ev("base_2", 150, 160, "alert"));
    // Current: 20 events in 100s = 0.2/s (10x spike)
    for (int i = 0; i < 20; ++i) {
        core.addEvent(ev("cur_" + std::to_string(i),
                         500 + i * 5, 505 + i * 5, "alert"));
    }

    AnomalyDetector det(core);
    auto results = det.detectFrequencyAnomaly(
        {tp(100), tp(200)},  // baseline
        {tp(500), tp(600)},  // current
        3.0,                 // threshold
        std::string("alert"));

    assert(results.size() == 1);
    assert(results[0].type == AnomalyType::FrequencySpike);
    assert(results[0].severity > 0.0);

    std::cout << "ok\n";
}

void test_frequency_drop() {
    std::cout << "  frequency_drop ... ";
    TemporalCore core;
    // Baseline: 20 events in 100s
    for (int i = 0; i < 20; ++i) {
        core.addEvent(ev("base_" + std::to_string(i),
                         100 + i * 5, 105 + i * 5, "ping"));
    }
    // Current: 1 event in 100s
    core.addEvent(ev("cur_0", 500, 510, "ping"));

    AnomalyDetector det(core);
    auto results = det.detectFrequencyAnomaly(
        {tp(100), tp(200)},
        {tp(500), tp(600)},
        3.0,
        std::string("ping"));

    assert(results.size() == 1);
    assert(results[0].type == AnomalyType::FrequencyDrop);

    std::cout << "ok\n";
}

// ---------- CoOccurrenceBreak ----------

void test_co_occurrence_break() {
    std::cout << "  co_occurrence_break ... ";
    TemporalCore core;
    // Baseline: "dns_query" and "dns_response" always overlap.
    for (int i = 0; i < 5; ++i) {
        int t = 100 + i * 50;
        core.addEvent(ev("q_" + std::to_string(i), t, t + 30, "dns_query"));
        core.addEvent(ev("r_" + std::to_string(i), t + 10, t + 40, "dns_response"));
    }
    // Current: dns_query present, dns_response absent.
    core.addEvent(ev("q_cur", 500, 530, "dns_query"));

    AnomalyDetector det(core);
    auto results = det.detectCoOccurrenceBreak(
        {tp(100), tp(400)},
        {tp(500), tp(600)},
        2);

    assert(results.size() == 1);
    assert(results[0].type == AnomalyType::CoOccurrenceBreak);
    assert(results[0].description.find("dns_query") != std::string::npos);
    assert(results[0].description.find("dns_response") != std::string::npos);

    std::cout << "ok\n";
}

// ---------- Loitering ----------

void test_loitering() {
    std::cout << "  loitering ... ";
    TemporalCore core;
    core.addEvent(ev("short_session", 100, 200, "connection"));  // 100s
    core.addEvent(ev("long_session",  100, 1000, "connection")); // 900s

    AnomalyDetector det(core);
    auto results = det.detectLoitering(
        {tp(0), tp(2000)},
        300.0,  // max 5 minutes
        std::string("connection"));

    assert(results.size() == 1);
    assert(results[0].type == AnomalyType::Loitering);
    assert(results[0].involved_events[0] == "long_session");

    std::cout << "ok\n";
}

// ---------- ConfidenceDecay ----------

void test_confidence_decay_detection() {
    std::cout << "  confidence_decay ... ";
    TemporalCore core;
    // Old event recorded long ago.
    core.addEvent(ev("old_event", 100, 200, "status"));
    // Recent event.
    core.addEvent(ev("new_event", 86500, 86600, "status"));

    AnomalyDetector det(core);
    DecayConfig cfg;
    cfg.half_life_secs = 86400.0;
    cfg.reinforcement_bonus = 0.0;
    cfg.floor = 0.0;

    // Check from perspective of t=90000 (old_event is ~90000s old, new_event ~3500s old).
    auto results = det.detectConfidenceDecay(
        {tp(0), tp(90000)},
        tp(90000),
        0.4,  // threshold
        cfg);

    // old_event should be decayed below 0.4 (it's >1 half-life old).
    // new_event should still be above 0.4.
    bool found_old = false;
    for (const auto& r : results) {
        if (r.involved_events[0] == "old_event") found_old = true;
        // new_event should NOT appear.
        assert(r.involved_events[0] != "new_event");
    }
    assert(found_old);

    std::cout << "ok\n";
}

// ---------- Bitemporal integration ----------

void test_anomaly_with_as_of() {
    std::cout << "  anomaly_with_as_of ... ";
    TemporalCore core;
    // Event recorded at t=100.
    core.addEvent(ev("early", 50, 150, "heartbeat"));
    // Event recorded at t=500 (late-arriving correction).
    {
        TemporalEvent late;
        late.id = "late";
        late.valid_from = tp(60);
        late.valid_to = tp(160);
        late.recorded_at = tp(500);
        late.type = "heartbeat";
        core.addEvent(std::move(late));
    }

    AnomalyDetector det(core);
    // As-of t=200: only "early" visible. Missing entity check against empty current.
    auto results_past = det.detectMissing(
        "heartbeat",
        {tp(50), tp(160)},
        {tp(1000), tp(1100)},
        1,
        tp(200));  // as_of: only see what was known at t=200

    // Should find "early" missing but not "late" (not yet recorded).
    bool found_early = false, found_late = false;
    for (const auto& r : results_past) {
        for (const auto& id : r.involved_events) {
            if (id == "early") found_early = true;
            if (id == "late") found_late = true;
        }
    }
    assert(found_early);
    assert(!found_late);

    std::cout << "ok\n";
}

// ---------- Branch integration ----------

void test_anomaly_on_branch() {
    std::cout << "  anomaly_on_branch ... ";
    TemporalCore core;
    core.addEvent(ev("main_ev", 100, 200, "metric"));
    core.addProjection(ev("branch_ev", 100, 200, "metric", "hypothesis"));

    AnomalyDetector det(core);
    // Loitering check on branch "hypothesis" only.
    auto results = det.detectLoitering(
        {tp(0), tp(300)},
        50.0,
        std::nullopt,
        std::nullopt,
        std::string("hypothesis"));

    // Should find branch_ev (100s > 50s threshold) but NOT main_ev.
    assert(results.size() == 1);
    assert(results[0].involved_events[0] == "branch_ev");

    std::cout << "ok\n";
}

} // namespace

int main() {
    std::cout << "test_anomaly:\n";

    test_decay_basic();
    test_decay_reinforcement();
    test_decay_presets();
    test_missing_entity();
    test_frequency_spike();
    test_frequency_drop();
    test_co_occurrence_break();
    test_loitering();
    test_confidence_decay_detection();
    test_anomaly_with_as_of();
    test_anomaly_on_branch();

    std::cout << "All anomaly tests passed.\n";
    return 0;
}
