#pragma once
#include "../core/temporal_core.h"
#include "../engine/temporal_engine.h"
#include "../anomaly/anomaly_detector.h"

#include <nlohmann/json.hpp>
#include <string>

namespace galahad {

// LLMToolAdapter exposes GALAHAD's reasoning surface as JSON tool calls
// an LLM framework (Anthropic, OpenAI, etc.) can register and invoke.
//
// Two entry points:
//
//   getToolSchemas()  - returns a JSON array of tool descriptors with
//                       Anthropic-style {name, description, input_schema}.
//                       The caller maps these into its tool-use API.
//
//   handleToolCall()  - executes a single tool call and returns either
//                       {"ok": true, "result": ...} or
//                       {"ok": false, "error": "..."}.
//
// Timestamps are accepted as ISO 8601 UTC strings ("2026-04-14T12:00:00.000Z")
// or int64 nanoseconds since epoch. Results always include both forms so
// the consumer can pick.
//
// Note on constness: handleToolCall is const even though individual tools
// may mutate the core via the held reference. That matches the semantics
// of "calling a tool does not change the adapter itself."
class LLMToolAdapter {
public:
    explicit LLMToolAdapter(TemporalCore& core, TemporalEngine& engine);

    nlohmann::json getToolSchemas() const;

    nlohmann::json handleToolCall(const std::string& tool_name,
                                  const nlohmann::json& args) const;

private:
    TemporalCore& core_;
    TemporalEngine& engine_;
};

} // namespace galahad
