// GALAHAD-TemporalSlicer benchmark harness.
//
// Seven workloads measure the core, engine, and persistence paths on a
// single-threaded machine. Every workload warms the relevant index before
// measuring so we time steady-state operation, not the first lazy rebuild.
//
// Release build recommended (the project's CMakeLists defaults to Release
// if you didn't pick a type). Debug numbers are meaningless.

#include "temporal_core.h"
#include "temporal_engine.h"
#include "persistence.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace galahad;
using namespace std::chrono;

// ---------- formatting ----------

std::string fmt_ns(nanoseconds ns) {
    double n = static_cast<double>(ns.count());
    std::ostringstream o;
    o << std::fixed << std::setprecision(2);
    if (n >= 1e9)      o << (n / 1e9)  << " s";
    else if (n >= 1e6) o << (n / 1e6)  << " ms";
    else if (n >= 1e3) o << (n / 1e3)  << " us";
    else               o << n          << " ns";
    return o.str();
}

std::string fmt_rate(std::size_t n, nanoseconds d) {
    if (d.count() <= 0) return "inf ops/s";
    double per_sec = (static_cast<double>(n) * 1e9) /
                     static_cast<double>(d.count());
    std::ostringstream o;
    o << std::fixed << std::setprecision(0) << per_sec << " ops/s";
    return o.str();
}

struct Stats {
    nanoseconds min, p50, p95, p99, max, mean;
    std::size_t n;
};

Stats stats_of(std::vector<nanoseconds> samples) {
    std::sort(samples.begin(), samples.end());
    Stats s{};
    s.n = samples.size();
    s.min  = samples.front();
    s.max  = samples.back();
    s.p50  = samples[(s.n *  50) / 100];
    s.p95  = samples[(s.n *  95) / 100];
    s.p99  = samples[(s.n *  99) / 100];
    nanoseconds sum{0};
    for (auto x : samples) sum += x;
    s.mean = sum / s.n;
    return s;
}

void print_section(const std::string& title) {
    std::cout << "\n=== " << title << " ===\n";
}

void print_line(const std::string& label, const std::string& value) {
    std::cout << "  " << std::left << std::setw(36) << label
              << "  " << value << "\n";
}

void print_stats(const std::string& label, const Stats& s) {
    std::ostringstream o;
    o << "min=" << fmt_ns(s.min)
      << "  p50=" << fmt_ns(s.p50)
      << "  p99=" << fmt_ns(s.p99)
      << "  max=" << fmt_ns(s.max)
      << "  (n=" << s.n << ")";
    print_line(label, o.str());
}

template <typename F>
nanoseconds time_once(F f) {
    auto s = steady_clock::now();
    f();
    return steady_clock::now() - s;
}

// ---------- workloads ----------

void bench_bulk_insert() {
    print_section("Bulk insert");
    const std::size_t N = 1'000'000;
    TemporalCore core;
    const auto t0 = Clock::now();

    auto d = time_once([&]() {
        for (std::size_t i = 0; i < N; ++i) {
            TemporalEvent e;
            e.id = "e" + std::to_string(i);
            e.valid_from  = t0 + microseconds(i);
            e.valid_to    = e.valid_from + microseconds(1);
            e.recorded_at = e.valid_from;
            e.type = "tick";
            core.addEvent(std::move(e));
        }
    });

    print_line("1M events addEvent",
               fmt_ns(d) + "  (" + fmt_rate(N, d) + ")");
}

void bench_get_latency() {
    print_section("Point lookup: get()");
    const std::size_t N = 100'000;
    TemporalCore core;
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < N; ++i) {
        TemporalEvent e;
        e.id = "e" + std::to_string(i);
        e.valid_from  = t0 + microseconds(i);
        e.valid_to    = e.valid_from + microseconds(1);
        e.recorded_at = e.valid_from;
        e.type = "tick";
        core.addEvent(std::move(e));
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> pick(0, N - 1);
    const int samples = 10'000;
    std::vector<nanoseconds> times;
    times.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        auto id = "e" + std::to_string(pick(rng));
        auto start = steady_clock::now();
        auto r = core.get(id);
        times.push_back(steady_clock::now() - start);
        if (!r) { std::cout << "  BUG: get returned nothing\n"; return; }
    }
    print_stats("get() over 100k events", stats_of(std::move(times)));
}

void bench_range_query() {
    print_section("Range query: queryRange()");
    const std::size_t N = 100'000;
    TemporalCore core;
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < N; ++i) {
        TemporalEvent e;
        e.id = "e" + std::to_string(i);
        e.valid_from  = t0 + microseconds(i);
        e.valid_to    = e.valid_from + microseconds(10);
        e.recorded_at = e.valid_from;
        e.type = "tick";
        core.addEvent(std::move(e));
    }
    // Warm the time index so we don't pay the rebuild in sample 0.
    (void)core.queryRange({t0, t0 + microseconds(N)});

    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> pick(0, N - 101);
    const int samples = 1000;
    std::vector<nanoseconds> times;
    times.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        auto off = pick(rng);
        TimeWindow w{t0 + microseconds(off), t0 + microseconds(off + 100)};
        auto start = steady_clock::now();
        auto r = core.queryRange(w);
        times.push_back(steady_clock::now() - start);
        if (r.empty()) { std::cout << "  BUG: empty window result\n"; return; }
    }
    print_stats("queryRange 100us window / 100k", stats_of(std::move(times)));
}

void bench_branch_isolation() {
    print_section("Per-branch time index");
    const int BRANCHES = 100;
    const int PER = 1000;
    TemporalCore core;
    const auto t0 = Clock::now();

    for (int b = 0; b < BRANCHES; ++b) {
        const std::string branch = "branch_" + std::to_string(b);
        for (int i = 0; i < PER; ++i) {
            TemporalEvent e;
            e.id = "b" + std::to_string(b) + "_" + std::to_string(i);
            e.valid_from  = t0 + microseconds(b * PER + i);
            e.valid_to    = e.valid_from + microseconds(5);
            e.recorded_at = e.valid_from;
            e.type = "tick";
            e.branch_id = branch;
            core.addProjection(e);
        }
    }
    // Warm both indices
    (void)core.queryRange({t0, t0 + seconds(1)});
    (void)core.queryRange({t0, t0 + seconds(1)}, std::nullopt,
                          std::optional<std::string>{"branch_42"});

    const int samples = 1000;

    // Global query: every branch, no filter
    std::vector<nanoseconds> global_times;
    global_times.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        auto start = steady_clock::now();
        auto r = core.queryRange({t0, t0 + seconds(1)});
        global_times.push_back(steady_clock::now() - start);
        (void)r;
    }
    print_stats("queryRange all branches (100k)",
                stats_of(std::move(global_times)));

    // Branch-scoped: pick a random branch each call
    std::vector<nanoseconds> branch_times;
    branch_times.reserve(samples);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> pick(0, BRANCHES - 1);
    for (int i = 0; i < samples; ++i) {
        auto b = std::string("branch_") + std::to_string(pick(rng));
        auto start = steady_clock::now();
        auto r = core.queryRange({t0, t0 + seconds(1)}, std::nullopt,
                                 std::optional<std::string>{b});
        branch_times.push_back(steady_clock::now() - start);
        (void)r;
    }
    print_stats("queryRange one branch (1k)",
                stats_of(std::move(branch_times)));
}

void bench_explain_depth() {
    print_section("Causal explain()");
    const int DEPTH = 1000;
    TemporalCore core;
    const auto t0 = Clock::now();

    for (int i = 0; i < DEPTH; ++i) {
        TemporalEvent e;
        e.id = "chain_" + std::to_string(i);
        e.valid_from  = t0 + microseconds(i);
        e.valid_to    = e.valid_from + microseconds(1);
        e.recorded_at = e.valid_from;
        e.type = "link";
        if (i > 0) e.causal_links = {"chain_" + std::to_string(i - 1)};
        core.addEvent(std::move(e));
    }
    TemporalEngine engine(core);

    // Warm the causal graph
    (void)engine.explain("chain_" + std::to_string(DEPTH - 1));

    const int samples = 50;
    std::vector<nanoseconds> times;
    times.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        auto start = steady_clock::now();
        auto exp = engine.explain("chain_" + std::to_string(DEPTH - 1));
        times.push_back(steady_clock::now() - start);
        if (exp.causes.size() != static_cast<std::size_t>(DEPTH - 1)) {
            std::cout << "  BUG: expected " << (DEPTH - 1)
                      << " causes, got " << exp.causes.size() << "\n";
            return;
        }
    }
    print_stats("explain() at depth 1000", stats_of(std::move(times)));
}

void bench_allen() {
    print_section("Allen find_related (100k events)");
    const std::size_t N = 100'000;
    TemporalCore core;
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < N; ++i) {
        TemporalEvent e;
        e.id = "e" + std::to_string(i);
        e.valid_from  = t0 + microseconds(i);
        e.valid_to    = e.valid_from + microseconds(3);
        e.recorded_at = e.valid_from;
        e.type = "tick";
        core.addEvent(std::move(e));
    }
    // Warm the time index so dispatch paths hit steady state
    (void)core.findRelated("e50000", AllenRelation::Precedes);

    const int samples = 100;
    auto run = [&](const std::string& name, AllenRelation r) {
        std::vector<nanoseconds> times;
        times.reserve(samples);
        for (int i = 0; i < samples; ++i) {
            auto start = steady_clock::now();
            auto ids = core.findRelated("e50000", r);
            times.push_back(steady_clock::now() - start);
            (void)ids;
        }
        print_stats(name + " from midpoint", stats_of(std::move(times)));
    };
    run("Precedes",   AllenRelation::Precedes);
    run("PrecededBy", AllenRelation::PrecededBy);
    run("Meets",      AllenRelation::Meets);
    run("Overlaps",   AllenRelation::Overlaps);
}

void bench_persistence() {
    print_section("Persistence round-trip");
    const std::size_t N = 100'000;
    TemporalCore source;
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < N; ++i) {
        TemporalEvent e;
        e.id = "e" + std::to_string(i);
        e.valid_from  = t0 + microseconds(i);
        e.valid_to    = e.valid_from + microseconds(5);
        e.recorded_at = e.valid_from;
        e.type = "tick";
        e.data["k"] = "v";
        source.addEvent(std::move(e));
    }

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "galahad_bench.gtp";
    if (fs::exists(tmp)) fs::remove(tmp);

    auto save_d = time_once([&]() {
        TemporalPersistence(source).save(tmp.string());
    });
    auto size = fs::file_size(tmp);

    TemporalCore dest;
    auto load_d = time_once([&]() {
        TemporalPersistence(dest).load(tmp.string());
    });

    print_line("save 100k events",
               fmt_ns(save_d) + "  (" + fmt_rate(N, save_d) + ")");
    print_line("file size",
               std::to_string(size / 1024) + " KiB  (" +
               std::to_string(size / N) + " bytes/event)");
    print_line("load 100k events",
               fmt_ns(load_d) + "  (" + fmt_rate(N, load_d) + ")");

    // Round-trip sanity
    auto loaded = dest.queryRange({t0 - microseconds(1),
                                   t0 + microseconds(N + 1)});
    if (loaded.size() != N) {
        std::cout << "  BUG: loaded " << loaded.size()
                  << " events, expected " << N << "\n";
    }

    fs::remove(tmp);
}

int main() {
    std::cout << "GALAHAD-TemporalSlicer benchmark harness\n";
    std::cout << "========================================\n";
    std::cout << "Single-threaded, in-memory. Release build assumed.\n";

    bench_bulk_insert();
    bench_get_latency();
    bench_range_query();
    bench_branch_isolation();
    bench_explain_depth();
    bench_allen();
    bench_persistence();

    std::cout << "\nDone.\n";
    return 0;
}
