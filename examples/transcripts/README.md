# Live LLM transcripts

This directory contains verbatim transcripts of Claude Opus 4.6 running
GALAHAD's demo scripts against live PyPI releases. They're preserved here
as documentation-by-artifact — evidence of what an LLM actually produces
when it reaches for GALAHAD's tools, rather than a claim about what it
*should* produce.

Each transcript file documents one run. The format is:

- **Frontmatter** — date, model, PyPI version, scenario, what the run
  demonstrates, and a one-line summary.
- **Verbatim transcript** in a fenced code block, exactly as it was
  produced by the agent loop in the corresponding demo script.
- **Annotations** — specific lines worth quoting, behaviors worth
  noting, bugs surfaced in the wild.

## Runs

| File | Demo | PyPI version | Date | Turns |
|---|---|---|---|---|
| [counterfactual_demo_transcript.md](counterfactual_demo_transcript.md) | `anthropic_demo.py` | 0.2.0 | 2026-04-15 | 4 |
| [traffic_agent_demo_transcript.md](traffic_agent_demo_transcript.md) | `traffic_agent_demo.py` | 0.2.1 | 2026-04-15 | 5 + synthesis |

## Reproducing

```bash
pip install galahad-temporal[anthropic]
export ANTHROPIC_API_KEY=sk-ant-...
python3 examples/anthropic_demo.py        # counterfactual demo
python3 examples/traffic_agent_demo.py    # 6-tick traffic agent demo
```

Both scripts have a dry-run mode that exercises the full GALAHAD surface
without calling the API — set `GALAHAD_DEMO_DRY_RUN=1` or simply omit
the `ANTHROPIC_API_KEY` environment variable to use it.
