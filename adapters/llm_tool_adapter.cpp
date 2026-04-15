#include "llm_tool_adapter.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

using nlohmann::json;

namespace galahad {

namespace {

// ---------- TimePoint <-> string / int64 ----------

std::string tpToIso(TimePoint tp) {
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    auto ms_part =
        std::chrono::duration_cast<std::chrono::milliseconds>(tp - secs).count();
    std::time_t t = Clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char iso[32];
    std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[64];
    std::snprintf(out, sizeof(out), "%s.%03ldZ", iso,
                  static_cast<long>(ms_part));
    return out;
}

std::int64_t tpToNanos(TimePoint tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               tp.time_since_epoch())
        .count();
}

TimePoint tpFromNanos(std::int64_t ns) {
    // Match TimePoint's native resolution via duration_cast so the code
    // compiles on both libstdc++ (clock = nanoseconds) and libc++ (clock
    // = microseconds). See persistence.cpp for the precision note.
    return TimePoint(
        std::chrono::duration_cast<TimePoint::duration>(
            std::chrono::nanoseconds(ns)));
}

TimePoint tpFromIso(const std::string& s) {
    std::tm tm{};
    const char* res = strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
    if (!res) throw std::runtime_error("bad ISO timestamp: " + s);
    auto secs = Clock::from_time_t(timegm(&tm));
    long ms_part = 0;
    if (*res == '.') {
        ++res;
        std::string frac;
        while (*res >= '0' && *res <= '9') frac += *res++;
        if (!frac.empty()) {
            if (frac.size() < 3) frac.append(3 - frac.size(), '0');
            ms_part = std::stol(frac.substr(0, 3));
        }
    }
    return secs + std::chrono::milliseconds(ms_part);
}

TimePoint tpFromJson(const json& j) {
    if (j.is_string()) return tpFromIso(j.get<std::string>());
    if (j.is_number_integer()) return tpFromNanos(j.get<std::int64_t>());
    throw std::runtime_error("timestamp must be ISO string or int64 nanoseconds");
}

// ---------- Event <-> JSON ----------

json eventToJson(const TemporalEvent& e) {
    json j;
    j["id"] = e.id;
    j["valid_from"] = tpToIso(e.valid_from);
    j["valid_from_ns"] = tpToNanos(e.valid_from);
    j["valid_to"] = tpToIso(e.valid_to);
    j["valid_to_ns"] = tpToNanos(e.valid_to);
    j["recorded_at"] = tpToIso(e.recorded_at);
    j["recorded_at_ns"] = tpToNanos(e.recorded_at);
    j["type"] = e.type;
    j["data"] = e.data;  // std::map<string,string> -> JSON object
    j["causal_links"] = e.causal_links;
    j["branch_id"] = e.branch_id;
    j["confidence"] = e.confidence;
    return j;
}

TemporalEvent eventFromJson(const json& j) {
    TemporalEvent e;
    e.id = j.at("id").get<std::string>();
    e.valid_from = tpFromJson(j.at("valid_from"));
    e.valid_to = tpFromJson(j.at("valid_to"));
    if (j.contains("recorded_at")) {
        e.recorded_at = tpFromJson(j["recorded_at"]);
    } else {
        e.recorded_at = Clock::now();
    }
    if (j.contains("type")) e.type = j["type"].get<std::string>();
    if (j.contains("data") && j["data"].is_object()) {
        for (auto& [k, v] : j["data"].items()) {
            e.data[k] = v.get<std::string>();
        }
    }
    if (j.contains("causal_links")) {
        for (auto& link : j["causal_links"]) {
            e.causal_links.push_back(link.get<std::string>());
        }
    }
    if (j.contains("branch_id")) {
        e.branch_id = j["branch_id"].get<std::string>();
    }
    if (j.contains("confidence")) {
        e.confidence = j["confidence"].get<double>();
    }
    return e;
}

// ---------- Allen relation <-> snake_case string ----------

AllenRelation allenFromString(const std::string& s) {
    if (s == "precedes")      return AllenRelation::Precedes;
    if (s == "meets")         return AllenRelation::Meets;
    if (s == "overlaps")      return AllenRelation::Overlaps;
    if (s == "finished_by")   return AllenRelation::FinishedBy;
    if (s == "contains")      return AllenRelation::Contains;
    if (s == "starts")        return AllenRelation::Starts;
    if (s == "equals")        return AllenRelation::Equals;
    if (s == "started_by")    return AllenRelation::StartedBy;
    if (s == "during")        return AllenRelation::During;
    if (s == "finishes")      return AllenRelation::Finishes;
    if (s == "overlapped_by") return AllenRelation::OverlappedBy;
    if (s == "met_by")        return AllenRelation::MetBy;
    if (s == "preceded_by")   return AllenRelation::PrecededBy;
    throw std::runtime_error("unknown Allen relation: " + s);
}

// ---------- envelopes ----------

json ok(json result) {
    return json{{"ok", true}, {"result", std::move(result)}};
}

json err(const std::string& msg) {
    return json{{"ok", false}, {"error", msg}};
}

// ---------- arg helpers ----------

std::optional<TimePoint> optTp(const json& args, const char* key) {
    if (!args.contains(key) || args[key].is_null()) return std::nullopt;
    return tpFromJson(args[key]);
}

std::optional<std::string> optStr(const json& args, const char* key) {
    if (!args.contains(key) || args[key].is_null()) return std::nullopt;
    return args[key].get<std::string>();
}

json eventsToJson(const std::vector<TemporalEvent>& events) {
    json arr = json::array();
    for (const auto& e : events) arr.push_back(eventToJson(e));
    return arr;
}

// ---------- schema builder ----------

json strProp(const std::string& desc) {
    return json{{"type", "string"}, {"description", desc}};
}

json boolProp(const std::string& desc, bool dflt) {
    return json{{"type", "boolean"}, {"description", desc}, {"default", dflt}};
}

json tsProp(const std::string& desc) {
    return json{{"type", "string"},
                {"description", desc + " (ISO 8601 UTC or int64 ns)"}};
}

json tool(const std::string& name, const std::string& desc,
          json properties, std::vector<std::string> required) {
    return json{
        {"name", name},
        {"description", desc},
        {"input_schema", {
            {"type", "object"},
            {"properties", properties},
            {"required", required}
        }}
    };
}

} // namespace

// ---------- class ----------

LLMToolAdapter::LLMToolAdapter(TemporalCore& core, TemporalEngine& engine)
    : core_(core), engine_(engine) {}

json LLMToolAdapter::getToolSchemas() const {
    json tools = json::array();

    tools.push_back(tool(
        "now",
        "Current monotonic transaction time. Returns {iso, ns}. Use this as "
        "an offset anchor when constructing future timestamps.",
        json::object(), {}));

    tools.push_back(tool(
        "add_event",
        "Add a ground-truth event to the main branch. Required fields: id, "
        "valid_from, valid_to. Optional: type, data, causal_links, "
        "recorded_at (defaults to now), branch_id, confidence.",
        json{
            {"id", strProp("Stable event identifier")},
            {"valid_from", tsProp("Start of the event's valid interval")},
            {"valid_to", tsProp("End of the event's valid interval")},
            {"recorded_at", tsProp("When the system learned of the event")},
            {"type", strProp("Event category (e.g. perception, action)")},
            {"data", json{{"type", "object"}, {"description", "string -> string key-value metadata"}}},
            {"causal_links", json{{"type", "array"}, {"items", {{"type", "string"}}},
                                  {"description", "ids of events this event depends on"}}},
            {"branch_id", strProp("Branch name; defaults to main")},
            {"confidence", json{{"type", "number"}, {"description", "0..1 probability"}}}
        },
        {"id", "valid_from", "valid_to"}));

    tools.push_back(tool(
        "add_projection",
        "Add a hypothetical/future event on a non-main branch. Same fields "
        "as add_event but branch_id must not be 'main'.",
        json{
            {"id", strProp("Stable event identifier")},
            {"valid_from", tsProp("Start of the projected interval")},
            {"valid_to", tsProp("End of the projected interval")},
            {"recorded_at", tsProp("When the projection was made")},
            {"type", strProp("Event category")},
            {"data", json{{"type", "object"}}},
            {"causal_links", json{{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"branch_id", strProp("Projection branch name (must not be 'main')")},
            {"confidence", json{{"type", "number"}, {"description", "0..1 probability"}}}
        },
        {"id", "valid_from", "valid_to", "branch_id"}));

    tools.push_back(tool(
        "get_event",
        "Look up the latest version of an event by id. Optional as_of to "
        "replay the system's past belief state. Optional branch to filter.",
        json{
            {"id", strProp("Event id")},
            {"as_of", tsProp("Transaction-time ceiling")},
            {"branch", strProp("Branch filter")}
        },
        {"id"}));

    tools.push_back(tool(
        "query_range",
        "Events whose valid interval overlaps [start, end]. Bitemporal and "
        "branch-aware.",
        json{
            {"start", tsProp("Window start")},
            {"end", tsProp("Window end")},
            {"as_of", tsProp("Transaction-time ceiling")},
            {"branch", strProp("Branch filter")}
        },
        {"start", "end"}));

    tools.push_back(tool(
        "explain",
        "Causal ancestry of an event, returned as full events in temporal "
        "order. Answers 'why did this happen?' from the system's belief "
        "state at the given transaction time.",
        json{
            {"id", strProp("Event to explain")},
            {"as_of", tsProp("Transaction-time ceiling")},
            {"require_completed_before", boolProp(
                "If true, only include causes whose valid_to is at or before "
                "the target's valid_from (completed causes). If false, also "
                "include ongoing context events.", true)},
            {"branch", strProp("Branch filter")}
        },
        {"id"}));

    tools.push_back(tool(
        "what_happened_during",
        "Events overlapping a time window, returned as full events. Answers "
        "'what was going on during this interval?'.",
        json{
            {"start", tsProp("Window start")},
            {"end", tsProp("Window end")},
            {"as_of", tsProp("Transaction-time ceiling")},
            {"branch", strProp("Branch filter")}
        },
        {"start", "end"}));

    tools.push_back(tool(
        "get_ancestors",
        "Transitive causal ancestors of an event (ids only).",
        json{
            {"id", strProp("Event id")},
            {"as_of", tsProp("Transaction-time ceiling")},
            {"branch", strProp("Branch filter")}
        },
        {"id"}));

    tools.push_back(tool(
        "get_descendants",
        "Transitive causal descendants of an event (ids only).",
        json{
            {"id", strProp("Event id")},
            {"as_of", tsProp("Transaction-time ceiling")},
            {"branch", strProp("Branch filter")}
        },
        {"id"}));

    tools.push_back(tool(
        "get_causes",
        "Direct causal parents of an event (ids only).",
        json{
            {"id", strProp("Event id")},
            {"as_of", tsProp("Transaction-time ceiling")},
            {"branch", strProp("Branch filter")}
        },
        {"id"}));

    tools.push_back(tool(
        "get_effects",
        "Direct causal children of an event (ids only).",
        json{
            {"id", strProp("Event id")},
            {"as_of", tsProp("Transaction-time ceiling")},
            {"branch", strProp("Branch filter")}
        },
        {"id"}));

    tools.push_back(tool(
        "find_related",
        "Find events standing in a given Allen interval relation to a source "
        "event. Relation names: precedes, meets, overlaps, finished_by, "
        "contains, starts, equals, started_by, during, finishes, "
        "overlapped_by, met_by, preceded_by.",
        json{
            {"id", strProp("Source event id")},
            {"relation", strProp("Allen relation name (snake_case)")},
            {"as_of", tsProp("Transaction-time ceiling")},
            {"branch", strProp("Branch filter")}
        },
        {"id", "relation"}));

    tools.push_back(tool(
        "promote_branch",
        "Promote a projection branch to ground truth. Appends main-branch "
        "copies of every event on the source branch with a fresh monotonic "
        "recorded_at, preserving the originals so bitemporal as_of queries "
        "before the promotion still see the projection form. Clears any "
        "refutation marker on the branch.",
        json{{"branch", strProp("Branch name to promote (not 'main')")}},
        {"branch"}));

    tools.push_back(tool(
        "refute_branch",
        "Mark a projection branch as refuted. Non-destructive: event data is "
        "preserved, but default queries will skip this branch. Explicit "
        "branch-name queries still see it.",
        json{{"branch", strProp("Branch name to refute (not 'main')")}},
        {"branch"}));

    tools.push_back(tool(
        "prune_branch",
        "Destructively remove every event on a projection branch and clear "
        "any refutation marker. Use for cleanup. Main branch is protected.",
        json{{"branch", strProp("Branch name to prune (not 'main')")}},
        {"branch"}));

    tools.push_back(tool(
        "is_refuted",
        "Check whether a branch is currently marked as refuted.",
        json{{"branch", strProp("Branch name")}},
        {"branch"}));

    tools.push_back(tool(
        "get_projections",
        "List every projection event (i.e. every non-main-branch event). "
        "Without a branch filter, skips refuted branches so you see only "
        "the open projections. Pass an explicit branch name to retrieve "
        "events on a specific projection branch even if it is refuted. "
        "Use this to enumerate the open forward-looking predictions the "
        "agent is currently entertaining, or to fetch the events on a "
        "named branch for audit purposes.",
        json{
            {"branch", strProp(
                "Optional branch filter. Omit to list all non-refuted "
                "projection branches; name a specific branch to include "
                "it even if refuted.")}
        },
        {}));

    tools.push_back(tool(
        "get_refuted_branches",
        "List the names of every projection branch currently marked as "
        "refuted. Refuted branches are preserved (their events are still "
        "queryable by explicit branch name and via why_not) but default "
        "queries skip them. Use this as the starting point for a "
        "counterfactual sweep — pair it with why_not to enumerate every "
        "refuted prediction and its would-have-been causal chain.",
        json::object(), {}));

    tools.push_back(tool(
        "get_all_events",
        "Return every event currently in the store, across all branches "
        "(including refuted) and all versions, in insertion order. "
        "Useful for introspection when you need the full shape of the "
        "memory and the targeted query tools (explain, query_range, "
        "get_event) are too narrow. Returns events as full records with "
        "id, valid_from, valid_to, recorded_at, type, data, causal_links, "
        "branch_id, and confidence.",
        json::object(), {}));

    tools.push_back(tool(
        "why_not",
        "Counterfactual query: 'why did this event NOT happen?' Returns "
        "one entry per refuted projection branch that contained a "
        "prediction of the given event id. Each entry has the refuted "
        "branch name, the predicted event (with data/confidence/links), "
        "and the would-have-been causal chain walked across all branches "
        "(not just refuted ones) so you see the full hypothetical "
        "ancestry. Empty if the event actually happened on main, if no "
        "prediction of it was ever made, or if every predicting branch "
        "was promoted rather than refuted. Use this to answer 'what "
        "almost happened?' without losing the bitemporal distinction.",
        json{{"id", strProp("Event id that did not happen")}},
        {"id"}));

    tools.push_back(tool(
        "explain_with",
        "Hypothetical explain: ask 'if this event had happened, what "
        "would its causal explanation look like?' Clones the current "
        "core, applies the supplied mutation event via add_event on the "
        "fork, then runs explain() against the fork. The original core "
        "is untouched — callers can test causal hypotheses without "
        "committing them. The `mutation` argument takes the same shape "
        "as add_event (id, valid_from, valid_to required; type, data, "
        "causal_links, branch_id, confidence, recorded_at optional).",
        json{
            {"target_id", strProp("Event id to explain in the fork")},
            {"mutation",  json{{"type", "object"},
                               {"description", "Event to add to the fork"}}},
            {"as_of", tsProp("Transaction-time ceiling for the explain")},
            {"require_completed_before", boolProp(
                "Only include ancestors completed before target start", true)},
            {"branch", strProp("Branch filter for the explain")}
        },
        {"target_id", "mutation"}));

    tools.push_back(tool(
        "list_tools",
        "Return the full list of available tools with their input schemas.",
        json::object(), {}));

    return tools;
}

json LLMToolAdapter::handleToolCall(
    const std::string& tool_name, const json& args) const {
    try {
        if (tool_name == "list_tools") {
            return ok(getToolSchemas());
        }
        if (tool_name == "now") {
            auto tp = core_.now();
            return ok(json{{"iso", tpToIso(tp)}, {"ns", tpToNanos(tp)}});
        }
        if (tool_name == "add_event") {
            core_.addEvent(eventFromJson(args));
            return ok(json{{"status", "added"}});
        }
        if (tool_name == "add_projection") {
            core_.addProjection(eventFromJson(args));
            return ok(json{{"status", "added"}});
        }
        if (tool_name == "get_event") {
            auto result = core_.get(
                args.at("id").get<std::string>(),
                optTp(args, "as_of"),
                optStr(args, "branch"));
            if (result) return ok(eventToJson(*result));
            return ok(nullptr);
        }
        if (tool_name == "query_range") {
            TimeWindow w{tpFromJson(args.at("start")), tpFromJson(args.at("end"))};
            auto events = core_.queryRange(w, optTp(args, "as_of"),
                                           optStr(args, "branch"));
            return ok(eventsToJson(events));
        }
        if (tool_name == "explain") {
            auto exp = engine_.explain(
                args.at("id").get<std::string>(),
                optTp(args, "as_of"),
                args.value("require_completed_before", true),
                optStr(args, "branch"));
            return ok(json{
                {"causes", eventsToJson(exp.causes)},
                {"completed_before_target", exp.completed_before_target}
            });
        }
        if (tool_name == "what_happened_during") {
            TimeWindow w{tpFromJson(args.at("start")), tpFromJson(args.at("end"))};
            auto events = engine_.whatHappenedDuring(
                w, optTp(args, "as_of"), optStr(args, "branch"));
            return ok(eventsToJson(events));
        }
        if (tool_name == "get_ancestors") {
            return ok(core_.getAncestors(
                args.at("id").get<std::string>(),
                optTp(args, "as_of"), optStr(args, "branch")));
        }
        if (tool_name == "get_descendants") {
            return ok(core_.getDescendants(
                args.at("id").get<std::string>(),
                optTp(args, "as_of"), optStr(args, "branch")));
        }
        if (tool_name == "get_causes") {
            return ok(core_.getCauses(
                args.at("id").get<std::string>(),
                optTp(args, "as_of"), optStr(args, "branch")));
        }
        if (tool_name == "get_effects") {
            return ok(core_.getEffects(
                args.at("id").get<std::string>(),
                optTp(args, "as_of"), optStr(args, "branch")));
        }
        if (tool_name == "find_related") {
            auto rel = allenFromString(args.at("relation").get<std::string>());
            return ok(core_.findRelated(
                args.at("id").get<std::string>(), rel,
                optTp(args, "as_of"), optStr(args, "branch")));
        }
        if (tool_name == "promote_branch") {
            core_.promoteBranch(args.at("branch").get<std::string>());
            return ok(json{{"status", "promoted"}});
        }
        if (tool_name == "refute_branch") {
            core_.refuteBranch(args.at("branch").get<std::string>());
            return ok(json{{"status", "refuted"}});
        }
        if (tool_name == "prune_branch") {
            core_.pruneBranch(args.at("branch").get<std::string>());
            return ok(json{{"status", "pruned"}});
        }
        if (tool_name == "is_refuted") {
            return ok(core_.isRefuted(args.at("branch").get<std::string>()));
        }
        if (tool_name == "get_projections") {
            auto events = core_.getProjections(optStr(args, "branch"));
            return ok(eventsToJson(events));
        }
        if (tool_name == "get_refuted_branches") {
            return ok(core_.getRefutedBranches());
        }
        if (tool_name == "get_all_events") {
            return ok(eventsToJson(core_.getAllEvents()));
        }
        if (tool_name == "why_not") {
            auto cfs = core_.whyNot(args.at("id").get<std::string>());
            json arr = json::array();
            for (const auto& cf : cfs) {
                json item;
                item["branch"] = cf.branch;
                item["hypothetical_event"] = eventToJson(cf.hypothetical_event);
                item["would_have_been_causes"] =
                    eventsToJson(cf.would_have_been_causes);
                arr.push_back(std::move(item));
            }
            return ok(arr);
        }
        if (tool_name == "explain_with") {
            auto mutation = eventFromJson(args.at("mutation"));
            auto exp = engine_.explainWith(
                args.at("target_id").get<std::string>(),
                mutation,
                optTp(args, "as_of"),
                args.value("require_completed_before", true),
                optStr(args, "branch"));
            return ok(json{
                {"causes", eventsToJson(exp.causes)},
                {"completed_before_target", exp.completed_before_target}
            });
        }
        return err("unknown tool: " + tool_name);
    } catch (const std::exception& e) {
        return err(e.what());
    }
}

} // namespace galahad
