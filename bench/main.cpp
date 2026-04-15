#include "temporal_core.h"
#include "temporal_engine.h"

#include <iostream>

// Placeholder. Do not publish numbers from this binary yet.
//
// The current core is still a research skeleton: get() is O(N), causal
// queries call get() in a hot loop, getDescendants rebuilds reverse
// adjacency on every call, and events are std::vector<TemporalEvent> with
// heap-allocated strings. Benchmarks taken against this implementation
// would not reflect GALAHAD's real potential.
//
// This file exists so `bench_temporal` is wired into CMake and ready to
// host real benchmarks once the index refactor lands. Planned workloads
// (for when perf work begins):
//
//   1. 1M events insert / point query / range query
//   2. explain() on a linear causal chain of depth 1000
//   3. Mixed Allen + causal query over 100k events
//   4. 100 projection branches, queries isolated by branch
//   5. Memory footprint per event
//
// What matters first:
//   - id -> sorted version list for O(log) get()
//   - auto-maintained forward + reverse causal index with dirty flag
//   - branch_id / event_id interning so comparisons are pointer equality
//   - flat event storage, no per-event heap churn

int main() {
    std::cout << "bench_temporal: placeholder. Index refactor pending; "
                 "no real benchmarks yet.\n";
    // Smoke test that the libraries link and basic ops run.
    galahad::TemporalCore core;
    galahad::TemporalEngine engine(core);
    (void)engine;
    return 0;
}
