# Experimental live tuning

Normal releases embed the selected quality and scheduling defaults. They do not load live tuning profiles or publish MCP sessions. Playback diagnostics are separate: leave `TracePerformance=0` and `ProbeServices=0` in the filter's INI unless investigating a problem.

## Build and connect

Build with `tools/build.ps1 -ExperimentalTuning` to enable the existing `tools/tuning_mcp.py` STDIO server. See `config/tuning-mcp.example.toml` for a Python launch example. The server uses the Python standard library and communicates locally with experimental quality engines.

The five quality tools expose 32 values from `include/nvof/tuning_parameters.def`. Ask the schema tool for current ranges and units before editing, then list quality-engine sessions. A request marked pending is not proof that it has been applied. Playback performance MCP tools, shared memory, profile loading/saving and per-frame polling were removed. Old performance tool calls are rejected; restart an already connected server to refresh its tool list.

## Application boundaries

| Values | Applied when |
| --- | --- |
| Quality parameters | Next input pair; all phases of the current pair retain their snapshot |
| Playback scheduling limits | Compile-time constants in `include/nvof/playback_limits.hpp`; no live/profile interface |

Quality values latch for the next input pair. Original frames, timestamps, metadata, GPU completion and resource lifetime are not tuning shortcuts. The first picture does not wait for a queue to fill. Accepted scheduling values remain unchanged: two queued decoder samples, 40 ms prepared-output capacity, two extra frames, a 1 GiB ready-surface budget, a 200 ms seek-preview window, 64 ms allocator headroom and a 1.5 GiB allocator budget above 120fps. Existing host minimums and hard frame limits still apply.

## Validation

Run `python tools/test-tuning.py` for protocol tests. Its optional live executable argument exercises an isolated GPU host; inspect the script's help. Stop other playback before measuring performance. Record source, scene, output rate, binary identity and applied values. Compare image quality separately from timing; pixel readback changes performance.

The common defaults are one configuration, not separate presets named after GPU models. More queue capacity can absorb a short stall but consumes memory and cannot fix insufficient sustained throughput. Compare startup, seeking and continuous playback independently.
