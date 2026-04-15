#include "llm_tool_adapter.h"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace galahad;
using nlohmann::json;

int main() {
    using namespace std::chrono;

    TemporalCore core;
    TemporalEngine engine(core);
    LLMToolAdapter adapter(core, engine);

    // --- list_tools: discoverability ---
    auto tools = adapter.handleToolCall("list_tools", json::object());
    assert(tools["ok"] == true);
    assert(tools["result"].is_array());
    assert(tools["result"].size() >= 17);  // now 19 tools incl why_not + explain_with

    // --- now: monotonic clock visible through the adapter ---
    auto now1 = adapter.handleToolCall("now", json::object());
    assert(now1["ok"] == true);
    assert(now1["result"].contains("iso"));
    assert(now1["result"].contains("ns"));
    const std::int64_t base_ns = now1["result"]["ns"].get<std::int64_t>();

    // --- add_event via JSON: full round-trip through the adapter ---
    auto add_perceive = adapter.handleToolCall("add_event", json{
        {"id", "perceive"},
        {"valid_from", base_ns},
        {"valid_to", base_ns + 5'000'000},         // +5ms
        {"recorded_at", base_ns},
        {"type", "perception"},
        {"data", {{"obs", "door_open"}}},
        {"causal_links", json::array()}
    });
    assert(add_perceive["ok"] == true);

    auto add_infer = adapter.handleToolCall("add_event", json{
        {"id", "infer"},
        {"valid_from", base_ns + 5'000'000},
        {"valid_to", base_ns + 15'000'000},
        {"recorded_at", base_ns + 5'000'000},
        {"type", "inference"},
        {"data", {{"why", "wind"}}},
        {"causal_links", {"perceive"}}
    });
    assert(add_infer["ok"] == true);

    auto add_decide = adapter.handleToolCall("add_event", json{
        {"id", "decide"},
        {"valid_from", base_ns + 15'000'000},
        {"valid_to", base_ns + 20'000'000},
        {"recorded_at", base_ns + 15'000'000},
        {"type", "decision"},
        {"data", {{"choose", "close"}}},
        {"causal_links", {"infer"}}
    });
    assert(add_decide["ok"] == true);

    auto add_act = adapter.handleToolCall("add_event", json{
        {"id", "act"},
        {"valid_from", base_ns + 20'000'000},
        {"valid_to", base_ns + 30'000'000},
        {"recorded_at", base_ns + 200'000'000},  // late-arriving
        {"type", "action"},
        {"data", {{"do", "close"}}},
        {"causal_links", {"decide"}}
    });
    assert(add_act["ok"] == true);

    // --- explain("act"): full causal chain through the adapter ---
    auto why = adapter.handleToolCall("explain", json{{"id", "act"}});
    assert(why["ok"] == true);
    auto causes = why["result"]["causes"];
    assert(causes.size() == 3);
    assert(causes[0]["id"] == "perceive");
    assert(causes[1]["id"] == "infer");
    assert(causes[2]["id"] == "decide");
    // Data round-trip through JSON:
    assert(causes[0]["data"]["obs"] == "door_open");
    // ISO and ns both present on every timestamp:
    assert(causes[0].contains("valid_from"));
    assert(causes[0].contains("valid_from_ns"));

    // --- bitemporal honesty via adapter ---
    auto past = adapter.handleToolCall("explain", json{
        {"id", "act"},
        {"as_of", base_ns + 100'000'000}  // before act was recorded
    });
    assert(past["ok"] == true);
    assert(past["result"]["causes"].empty());

    // --- find_related: Allen through the adapter ---
    auto precedes = adapter.handleToolCall("find_related", json{
        {"id", "perceive"},
        {"relation", "precedes"}
    });
    assert(precedes["ok"] == true);
    auto pr = precedes["result"];
    // perceive [0,5]. infer [5,15] meets it, decide [15,20] and act [20,30]
    // both strictly after. So Precedes should return {decide, act}.
    assert(pr.size() == 2);

    auto meets = adapter.handleToolCall("find_related", json{
        {"id", "perceive"},
        {"relation", "meets"}
    });
    assert(meets["ok"] == true);
    assert(meets["result"].size() == 1);
    assert(meets["result"][0] == "infer");

    // --- get_ancestors through the adapter ---
    auto anc = adapter.handleToolCall("get_ancestors", json{{"id", "act"}});
    assert(anc["ok"] == true);
    assert(anc["result"].size() == 3);

    // --- branching: projection through the adapter ---
    auto add_proj = adapter.handleToolCall("add_projection", json{
        {"id", "fut_close"},
        {"valid_from", base_ns + 30'000'000},
        {"valid_to", base_ns + 40'000'000},
        {"recorded_at", base_ns + 20'000'000},
        {"type", "projected_action"},
        {"causal_links", {"act"}},
        {"branch_id", "close_door"},
        {"confidence", 0.7}
    });
    assert(add_proj["ok"] == true);

    // Branch-scoped query
    auto close_only = adapter.handleToolCall("query_range", json{
        {"start", base_ns},
        {"end", base_ns + 1'000'000'000},
        {"branch", "close_door"}
    });
    assert(close_only["ok"] == true);
    assert(close_only["result"].size() == 1);
    assert(close_only["result"][0]["id"] == "fut_close");
    assert(close_only["result"][0]["confidence"] == 0.7);

    // --- lifecycle: refute then is_refuted ---
    auto refuted = adapter.handleToolCall("refute_branch", json{
        {"branch", "close_door"}
    });
    assert(refuted["ok"] == true);

    auto check = adapter.handleToolCall("is_refuted", json{
        {"branch", "close_door"}
    });
    assert(check["ok"] == true);
    assert(check["result"] == true);

    // --- error handling: unknown tool ---
    auto bad = adapter.handleToolCall("nonexistent", json::object());
    assert(bad["ok"] == false);
    assert(bad["error"].get<std::string>().find("unknown tool") != std::string::npos);

    // --- error handling: bad Allen relation name ---
    auto bad_allen = adapter.handleToolCall("find_related", json{
        {"id", "perceive"}, {"relation", "bogus"}
    });
    assert(bad_allen["ok"] == false);

    // --- error handling: missing required arg ---
    auto missing = adapter.handleToolCall("explain", json::object());
    assert(missing["ok"] == false);

    // --- counterfactual queries via JSON tools ---
    // Build a fresh scenario on a separate core/engine/adapter triple
    // so it doesn't interact with the earlier test fixtures. Setup goes
    // through add_event/add_projection/refute_branch tool calls only,
    // matching the style of the rest of this file.
    {
        TemporalCore cf_core;
        TemporalEngine cf_engine(cf_core);
        LLMToolAdapter cf_adapter(cf_core, cf_engine);

        auto cf_now = cf_adapter.handleToolCall("now", json::object());
        const std::int64_t b = cf_now["result"]["ns"].get<std::int64_t>();

        // Main timeline: perceive -> decide
        cf_adapter.handleToolCall("add_event", json{
            {"id", "p"},
            {"valid_from", b},
            {"valid_to",   b + 5'000'000},   // +5 ms
            {"recorded_at", b},
            {"type", "perception"},
            {"causal_links", json::array()}
        });
        cf_adapter.handleToolCall("add_event", json{
            {"id", "d"},
            {"valid_from", b + 5'000'000},
            {"valid_to",   b + 10'000'000},
            {"recorded_at", b + 5'000'000},
            {"type", "decision"},
            {"causal_links", {"p"}}
        });

        // Refuted projection branch: fut_ignore depending on decide
        cf_adapter.handleToolCall("add_projection", json{
            {"id", "fut_ignore"},
            {"valid_from", b + 10'000'000},
            {"valid_to",   b + 15'000'000},
            {"recorded_at", b + 5'000'000},
            {"type", "projected_action"},
            {"data", {{"do", "wait"}}},
            {"causal_links", {"d"}},
            {"branch_id", "ignore"},
            {"confidence", 0.3}
        });
        cf_adapter.handleToolCall("refute_branch", json{{"branch", "ignore"}});

        // why_not on the refuted-branch event returns the branch + chain
        auto wn = cf_adapter.handleToolCall(
            "why_not", json{{"id", "fut_ignore"}});
        assert(wn["ok"] == true);
        auto arr = wn["result"];
        assert(arr.is_array());
        assert(arr.size() == 1);
        assert(arr[0]["branch"] == "ignore");
        assert(arr[0]["hypothetical_event"]["id"] == "fut_ignore");
        assert(arr[0]["hypothetical_event"]["data"]["do"] == "wait");
        assert(arr[0]["hypothetical_event"]["confidence"] == 0.3);
        assert(arr[0]["would_have_been_causes"].is_array());
        assert(arr[0]["would_have_been_causes"].size() == 2);  // d, p

        // why_not on a real event returns empty
        auto wn_real = cf_adapter.handleToolCall(
            "why_not", json{{"id", "d"}});
        assert(wn_real["ok"] == true);
        assert(wn_real["result"].empty());

        // why_not on a nonexistent event returns empty
        auto wn_none = cf_adapter.handleToolCall(
            "why_not", json{{"id", "does_not_exist"}});
        assert(wn_none["ok"] == true);
        assert(wn_none["result"].empty());

        // explain_with: add a hypothetical event citing `d`, ask explain
        // on it, get the full chain back from the fork. The real core
        // must NOT contain the hypothetical afterwards.
        json mutation = {
            {"id", "hyp"},
            {"valid_from", b + 20'000'000},
            {"valid_to",   b + 30'000'000},
            {"recorded_at", b + 20'000'000},
            {"type", "hypothetical_action"},
            {"causal_links", {"d"}}
        };
        auto ew = cf_adapter.handleToolCall(
            "explain_with",
            json{
                {"target_id", "hyp"},
                {"mutation", mutation}
            });
        assert(ew["ok"] == true);
        auto causes = ew["result"]["causes"];
        assert(causes.size() == 2);  // d, p in temporal order
        assert(causes[0]["id"] == "p");
        assert(causes[1]["id"] == "d");

        // Real core still does NOT contain the hypothetical event.
        auto check = cf_adapter.handleToolCall(
            "get_event", json{{"id", "hyp"}});
        assert(check["ok"] == true);
        assert(check["result"].is_null());
    }

    std::cout << "test_adapter: OK (LLM tool-call surface + counterfactuals)\n";
    return 0;
}
