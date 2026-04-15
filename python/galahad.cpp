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
// Lifetime management: TemporalEngine, LLMToolAdapter, and
// TemporalPersistence all hold references to a TemporalCore (and to
// each other where relevant). Without explicit keep_alive directives,
// the Python garbage collector could reclaim a core while a derived
// object still held a dangling reference. Every holder-of-reference
// ctor here pins its backing objects via py::keep_alive so the
// lifetime graph matches C++ semantics.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/chrono.h>

#include "temporal_core.h"
#include "temporal_engine.h"
#include "llm_tool_adapter.h"
#include "persistence.h"

#include <nlohmann/json.hpp>

namespace py = pybind11;
using namespace galahad;

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
        .def("get_refuted_branches", &TemporalCore::getRefutedBranches);

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
             py::arg("branch") = py::none());

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

    m.attr("__version__") = "0.1";
}
