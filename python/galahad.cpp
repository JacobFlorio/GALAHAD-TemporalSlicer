// GALAHAD-TemporalSlicer Python bindings (pybind11).
//
// Exposes the full public surface: TemporalCore, TemporalEngine,
// LLMToolAdapter, TemporalPersistence, plus the supporting value types
// TemporalEvent, TimeWindow, AllenRelation, and Explanation.
//
// The `nlohmann::json` type used by the adapter is translated to and
// from Python dict/list/scalars via an inline custom type caster
// (see below). A Python dict passed to `handle_tool_call(name, args)`
// becomes a json object inside C++; the returned envelope becomes a
// Python dict back.
//
// Timezone-correct chrono caster: pybind11/chrono.h's default caster
// for `std::chrono::system_clock::time_point` treats Python datetimes
// as local wall-clock time and silently corrupts cross-timezone data.
// We replace it with a caster that REQUIRES tz-aware datetimes on the
// Python->C++ side and returns tz-aware UTC datetimes on the way back.
// Naive datetimes are rejected with a clear error instead of being
// silently misinterpreted.
//
// Lifetime management: TemporalEngine, LLMToolAdapter, and
// TemporalPersistence all hold references to a TemporalCore (and to
// each other where relevant). Without explicit keep_alive directives,
// the Python garbage collector could reclaim a core while a derived
// object still held a dangling reference. Every holder-of-reference
// ctor here pins its backing objects via py::keep_alive so the
// lifetime graph matches C++ semantics.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
// Deliberately NOT including <pybind11/chrono.h>. Its default
// time_point caster uses local wall-clock time and corrupts timezones.
// The custom caster below replaces it.

#include "temporal_core.h"
#include "temporal_engine.h"
#include "llm_tool_adapter.h"
#include "anomaly_detector.h"
#include "persistence.h"

#include <nlohmann/json.hpp>

#include <datetime.h>   // CPython datetime C API (PyDateTime_IMPORT, PyDateTime_Check)
#include <chrono>
#include <cmath>
#include <cstdint>

namespace py = pybind11;
using namespace galahad;

// ---------- std::chrono::system_clock::time_point <-> Python datetime ----------
//
// pybind11/chrono.h's default time_point caster uses local wall-clock
// time: for a tz-aware Python datetime at 12:00 UTC on a UTC-4 machine,
// it stores the TimePoint as the instant that represents 12:00 LOCAL
// time (which is 16:00 UTC as a raw instant). The round-trip through
// the binding silently undoes the offset so pure-Python workflows
// appear to work, but any code path that reads the TimePoint directly
// (the LLM adapter's ISO serialization via `gmtime_r`, for example)
// sees the shifted value.
//
// This caster fixes it end-to-end:
//   Python -> C++: REQUIRE a tz-aware datetime. Use `.timestamp()`
//                  which returns UTC epoch seconds regardless of
//                  tzinfo. Naive datetimes are rejected with a clear
//                  error because silent local-time interpretation is
//                  exactly what caused the original bug.
//   C++ -> Python: return a tz-aware datetime constructed via
//                  `datetime.fromtimestamp(ts, tz=utc)`. Never naive.
//
// Precision: Python datetime has microsecond resolution. The caster
// round-trips with microsecond fidelity via `llround(ts * 1e6)`.
// For timestamps up to ~year 2250 this multiplication is exact in
// double precision. Sub-microsecond values from C++ are truncated.
namespace pybind11 { namespace detail {

template <>
struct type_caster<std::chrono::system_clock::time_point> {
public:
    PYBIND11_TYPE_CASTER(std::chrono::system_clock::time_point,
                         const_name("datetime.datetime"));

    bool load(handle src, bool /*convert*/) {
        if (!src || src.is_none()) return false;
        if (!PyDateTime_Check(src.ptr())) return false;

        // Refuse naive datetimes — they are the reason this caster exists.
        auto tz = getattr(src, "tzinfo", none());
        if (tz.is_none()) {
            throw value_error(
                "GALAHAD time_point caster: naive datetime is ambiguous. "
                "Pass a tz-aware datetime, e.g. datetime.now(timezone.utc).");
        }

        // .timestamp() returns UTC epoch seconds for any tz-aware datetime.
        double ts = src.attr("timestamp")().cast<double>();
        auto us = static_cast<std::int64_t>(std::llround(ts * 1e6));
        value = std::chrono::system_clock::time_point(
            std::chrono::microseconds(us));
        return true;
    }

    static handle cast(const std::chrono::system_clock::time_point& src,
                       return_value_policy /*policy*/,
                       handle /*parent*/) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      src.time_since_epoch()).count();
        double ts = static_cast<double>(us) / 1e6;

        auto datetime_mod = module_::import("datetime");
        auto utc = datetime_mod.attr("timezone").attr("utc");
        auto dt = datetime_mod.attr("datetime").attr("fromtimestamp")(ts, utc);
        return dt.release();
    }
};

}} // namespace pybind11::detail

// ---------- nlohmann::json <-> Python caster ----------
//
// pybind11 has no built-in caster for nlohmann::json, so we provide one
// inline. Scalars (null, bool, int, float, string) map element-wise;
// arrays round-trip via py::list; objects via py::dict. Recursive
// containers are handled by recursing into the same caster.
namespace pybind11 { namespace detail {

template <> struct type_caster<nlohmann::json> {
public:
    PYBIND11_TYPE_CASTER(nlohmann::json, const_name("json"));

    // Python -> json
    bool load(handle src, bool /*convert*/) {
        if (src.is_none()) {
            value = nullptr;
            return true;
        }
        if (py::isinstance<py::bool_>(src)) {
            value = src.cast<bool>();
            return true;
        }
        if (py::isinstance<py::int_>(src)) {
            value = src.cast<std::int64_t>();
            return true;
        }
        if (py::isinstance<py::float_>(src)) {
            value = src.cast<double>();
            return true;
        }
        if (py::isinstance<py::str>(src)) {
            value = src.cast<std::string>();
            return true;
        }
        if (py::isinstance<py::list>(src) || py::isinstance<py::tuple>(src)) {
            nlohmann::json arr = nlohmann::json::array();
            for (auto item : src) {
                type_caster<nlohmann::json> c;
                if (!c.load(item, true)) return false;
                arr.push_back(std::move(c.value));
            }
            value = std::move(arr);
            return true;
        }
        if (py::isinstance<py::dict>(src)) {
            nlohmann::json obj = nlohmann::json::object();
            for (auto item : src.cast<py::dict>()) {
                std::string key = py::str(item.first);
                type_caster<nlohmann::json> c;
                if (!c.load(item.second, true)) return false;
                obj[key] = std::move(c.value);
            }
            value = std::move(obj);
            return true;
        }
        return false;
    }

    // json -> Python
    static handle cast(const nlohmann::json& src,
                       return_value_policy /*policy*/, handle /*parent*/) {
        if (src.is_null())          return py::none().release();
        if (src.is_boolean())       return py::bool_(src.get<bool>()).release();
        if (src.is_number_integer())return py::int_(src.get<std::int64_t>()).release();
        if (src.is_number_float())  return py::float_(src.get<double>()).release();
        if (src.is_string())        return py::str(src.get<std::string>()).release();
        if (src.is_array()) {
            py::list l;
            for (const auto& item : src) {
                l.append(py::reinterpret_steal<py::object>(
                    cast(item, return_value_policy::automatic, handle())));
            }
            return l.release();
        }
        if (src.is_object()) {
            py::dict d;
            for (auto it = src.begin(); it != src.end(); ++it) {
                d[py::str(it.key())] = py::reinterpret_steal<py::object>(
                    cast(it.value(), return_value_policy::automatic, handle()));
            }
            return d.release();
        }
        return py::none().release();
    }
};

}} // namespace pybind11::detail

// ---------- module ----------

PYBIND11_MODULE(galahad, m) {
    // Initialize the CPython datetime C API once per module load. Without
    // this, PyDateTime_Check in the time_point caster dereferences null
    // function pointers and crashes.
    PyDateTime_IMPORT;

    m.doc() =
        "GALAHAD-TemporalSlicer: unified temporal reasoning engine.\n"
        "Bitemporal events, causal DAG, Allen interval algebra, branching\n"
        "projections, lifecycle ops, LLM tool-call adapter, and binary\n"
        "persistence — all in one substrate.";

    // ---- AllenRelation ----
    py::enum_<AllenRelation>(m, "AllenRelation")
        .value("Precedes",     AllenRelation::Precedes)
        .value("Meets",        AllenRelation::Meets)
        .value("Overlaps",     AllenRelation::Overlaps)
        .value("FinishedBy",   AllenRelation::FinishedBy)
        .value("Contains",     AllenRelation::Contains)
        .value("Starts",       AllenRelation::Starts)
        .value("Equals",       AllenRelation::Equals)
        .value("StartedBy",    AllenRelation::StartedBy)
        .value("During",       AllenRelation::During)
        .value("Finishes",     AllenRelation::Finishes)
        .value("OverlappedBy", AllenRelation::OverlappedBy)
        .value("MetBy",        AllenRelation::MetBy)
        .value("PrecededBy",   AllenRelation::PrecededBy);

    // ---- TemporalEvent ----
    py::class_<TemporalEvent>(m, "TemporalEvent")
        .def(py::init<>())
        .def_readwrite("id",            &TemporalEvent::id)
        .def_readwrite("valid_from",    &TemporalEvent::valid_from)
        .def_readwrite("valid_to",      &TemporalEvent::valid_to)
        .def_readwrite("recorded_at",   &TemporalEvent::recorded_at)
        .def_readwrite("type",          &TemporalEvent::type)
        .def_readwrite("data",          &TemporalEvent::data)
        .def_readwrite("causal_links",  &TemporalEvent::causal_links)
        .def_readwrite("branch_id",     &TemporalEvent::branch_id)
        .def_readwrite("confidence",    &TemporalEvent::confidence)
        .def("__repr__", [](const TemporalEvent& e) {
            return "<TemporalEvent id='" + e.id +
                   "' branch='" + e.branch_id +
                   "' type='" + e.type + "'>";
        });

    // ---- TimeWindow ----
    py::class_<TimeWindow>(m, "TimeWindow")
        .def(py::init<>())
        .def(py::init([](TimePoint s, TimePoint e) {
            return TimeWindow{s, e};
        }), py::arg("start"), py::arg("end"))
        .def_readwrite("start", &TimeWindow::start)
        .def_readwrite("end",   &TimeWindow::end);

    // ---- Explanation ----
    py::class_<TemporalEngine::Explanation>(m, "Explanation")
        .def_readonly("causes",
                      &TemporalEngine::Explanation::causes)
        .def_readonly("completed_before_target",
                      &TemporalEngine::Explanation::completed_before_target);

    // ---- CounterfactualExplanation ----
    py::class_<CounterfactualExplanation>(m, "CounterfactualExplanation")
        .def_readonly("branch",
                      &CounterfactualExplanation::branch)
        .def_readonly("hypothetical_event",
                      &CounterfactualExplanation::hypothetical_event)
        .def_readonly("would_have_been_causes",
                      &CounterfactualExplanation::would_have_been_causes)
        .def("__repr__", [](const CounterfactualExplanation& cf) {
            return "<CounterfactualExplanation branch='" + cf.branch +
                   "' event='" + cf.hypothetical_event.id +
                   "' causes=" + std::to_string(cf.would_have_been_causes.size()) + ">";
        });

    // ---- TemporalCore ----
    py::class_<TemporalCore>(m, "TemporalCore")
        .def(py::init<>())
        .def("add_event",        &TemporalCore::addEvent, py::arg("event"))
        .def("add_projection",   &TemporalCore::addProjection, py::arg("event"))
        .def("query_range",      &TemporalCore::queryRange,
             py::arg("window"),
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("get",              &TemporalCore::get,
             py::arg("id"),
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("build_causal_graph", &TemporalCore::buildCausalGraph)
        .def("has_cycle",        &TemporalCore::hasCycle)
        .def("get_ancestors",    &TemporalCore::getAncestors,
             py::arg("id"),
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("get_descendants",  &TemporalCore::getDescendants,
             py::arg("id"),
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("get_causes",       &TemporalCore::getCauses,
             py::arg("id"),
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("get_effects",      &TemporalCore::getEffects,
             py::arg("id"),
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("get_allen_relation", &TemporalCore::getAllenRelation,
             py::arg("a"), py::arg("b"))
        .def("holds",            &TemporalCore::holds,
             py::arg("relation"), py::arg("a"), py::arg("b"))
        .def("find_related",     &TemporalCore::findRelated,
             py::arg("id"), py::arg("relation"),
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("get_projections",  &TemporalCore::getProjections,
             py::arg("branch") = py::none())
        .def("promote_branch",   &TemporalCore::promoteBranch, py::arg("branch"))
        .def("refute_branch",    &TemporalCore::refuteBranch,  py::arg("branch"))
        .def("prune_branch",     &TemporalCore::pruneBranch,   py::arg("branch"))
        .def("is_refuted",       &TemporalCore::isRefuted,     py::arg("branch"))
        .def("now",              &TemporalCore::now)
        .def("get_all_events",   &TemporalCore::getAllEvents)
        .def("get_refuted_branches", &TemporalCore::getRefutedBranches)
        .def("clone",            &TemporalCore::clone,
             "Deep-copy the core into an independent TemporalCore. Use "
             "for hypothetical reasoning: clone, mutate the copy, query, "
             "drop.")
        .def("why_not",          &TemporalCore::whyNot,
             py::arg("id"),
             "Counterfactual: return one CounterfactualExplanation per "
             "refuted projection branch that contained a prediction of "
             "`id`. Empty if the event actually happened on main.");

    // ---- TemporalEngine ----
    py::class_<TemporalEngine>(m, "TemporalEngine")
        .def(py::init<TemporalCore&>(),
             py::arg("core"),
             py::keep_alive<1, 2>())  // engine keeps its core alive
        .def("explain", &TemporalEngine::explain,
             py::arg("id"),
             py::arg("as_of") = py::none(),
             py::arg("require_completed_before") = true,
             py::arg("branch") = py::none())
        .def("what_happened_during", &TemporalEngine::whatHappenedDuring,
             py::arg("window"),
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("explain_with", &TemporalEngine::explainWith,
             py::arg("target_id"),
             py::arg("mutation"),
             py::arg("as_of")  = py::none(),
             py::arg("require_completed_before") = true,
             py::arg("branch") = py::none(),
             "Hypothetical explain: clone the core, apply the mutation, "
             "and run explain() on the target in the fork. Original core "
             "is untouched.");

    // ---- LLMToolAdapter ----
    py::class_<LLMToolAdapter>(m, "LLMToolAdapter")
        .def(py::init<TemporalCore&, TemporalEngine&>(),
             py::arg("core"), py::arg("engine"),
             py::keep_alive<1, 2>(),   // adapter pins core
             py::keep_alive<1, 3>())   // adapter pins engine
        .def("get_tool_schemas",
             &LLMToolAdapter::getToolSchemas)
        .def("handle_tool_call",
             &LLMToolAdapter::handleToolCall,
             py::arg("tool_name"), py::arg("args"));

    // ---- TemporalPersistence ----
    py::class_<TemporalPersistence>(m, "TemporalPersistence")
        .def(py::init<TemporalCore&>(),
             py::arg("core"),
             py::keep_alive<1, 2>())
        .def("save", &TemporalPersistence::save, py::arg("path"))
        .def("load", &TemporalPersistence::load, py::arg("path"))
        .def_readonly_static("format_version",
                             &TemporalPersistence::kFormatVersion);

    // ---- AnomalyType ----
    py::enum_<AnomalyType>(m, "AnomalyType")
        .value("MissingEntity",     AnomalyType::MissingEntity)
        .value("FrequencySpike",    AnomalyType::FrequencySpike)
        .value("FrequencyDrop",     AnomalyType::FrequencyDrop)
        .value("CoOccurrenceBreak", AnomalyType::CoOccurrenceBreak)
        .value("Loitering",         AnomalyType::Loitering)
        .value("ConfidenceDecay",   AnomalyType::ConfidenceDecay);

    // ---- AnomalyResult ----
    py::class_<AnomalyResult>(m, "AnomalyResult")
        .def_readonly("type",            &AnomalyResult::type)
        .def_readonly("severity",        &AnomalyResult::severity)
        .def_readonly("description",     &AnomalyResult::description)
        .def_readonly("involved_events", &AnomalyResult::involved_events)
        .def("__repr__", [](const AnomalyResult& r) {
            return "<AnomalyResult severity=" + std::to_string(r.severity) +
                   " desc='" + r.description.substr(0, 60) + "...'>";
        });

    // ---- DecayConfig ----
    py::class_<DecayConfig>(m, "DecayConfig")
        .def(py::init<>())
        .def(py::init([](double hl, double rb, double fl, double mx) {
            return DecayConfig{hl, rb, fl, mx};
        }), py::arg("half_life_secs") = 86400.0,
            py::arg("reinforcement_bonus") = 0.15,
            py::arg("floor") = 0.05,
            py::arg("max_confidence") = 1.0)
        .def_readwrite("half_life_secs",      &DecayConfig::half_life_secs)
        .def_readwrite("reinforcement_bonus",  &DecayConfig::reinforcement_bonus)
        .def_readwrite("floor",               &DecayConfig::floor)
        .def_readwrite("max_confidence",      &DecayConfig::max_confidence);

    // Decay preset factory functions.
    m.def("security_decay",    &securityDecay);
    m.def("agriculture_decay", &agricultureDecay);
    m.def("finance_decay",     &financeDecay);
    m.def("network_decay",     &networkDecay);

    // Standalone decay computation.
    m.def("compute_decay", &computeDecay,
          py::arg("elapsed_secs"),
          py::arg("observation_count"),
          py::arg("config") = DecayConfig{},
          "Compute decayed confidence using ConsciousMem2 exponential model.");

    // ---- AnomalyDetector ----
    py::class_<AnomalyDetector>(m, "AnomalyDetector")
        .def(py::init<const TemporalCore&>(),
             py::arg("core"),
             py::keep_alive<1, 2>())
        .def("detect_missing", &AnomalyDetector::detectMissing,
             py::arg("entity_type"),
             py::arg("baseline"),
             py::arg("current"),
             py::arg("min_baseline_appearances") = 2,
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("detect_frequency_anomaly", &AnomalyDetector::detectFrequencyAnomaly,
             py::arg("baseline"),
             py::arg("current"),
             py::arg("spike_threshold") = 3.0,
             py::arg("event_type") = py::none(),
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("detect_co_occurrence_break", &AnomalyDetector::detectCoOccurrenceBreak,
             py::arg("baseline"),
             py::arg("current"),
             py::arg("min_co_occurrences") = 2,
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("detect_loitering", &AnomalyDetector::detectLoitering,
             py::arg("window"),
             py::arg("max_duration_secs"),
             py::arg("event_type") = py::none(),
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none())
        .def("detect_confidence_decay", &AnomalyDetector::detectConfidenceDecay,
             py::arg("window"),
             py::arg("now"),
             py::arg("threshold") = 0.3,
             py::arg("decay") = DecayConfig{},
             py::arg("as_of")  = py::none(),
             py::arg("branch") = py::none());

    m.attr("__version__") = "0.3.0";
}
