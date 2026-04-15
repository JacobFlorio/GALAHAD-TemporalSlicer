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
    assert(tools["result"].size() >= 15);  // core surface covered

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

    std::cout << "test_adapter: OK (LLM tool-call surface)\n";
    return 0;
}
