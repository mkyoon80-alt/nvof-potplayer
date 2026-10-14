"""Small local STDIO MCP server (2025-06-18), with no third-party dependencies."""
import argparse
import json
import sys
from tuning_control import Controller, PARAMETERS, dispatch


def tool_list():
    session = {"type": "string", "description": "Exact session ID returned by tuning_list"}
    common = {"session": session, "expected_revision": {"type": "integer", "minimum": 1},
              "wait_ms": {"type": "integer", "minimum": 0, "maximum": 3000, "default": 0}}
    values = {"type": "object", "additionalProperties": False, "minProperties": 1,
              "properties": {p["name"]: {"type": "number", "minimum": p["minimum"], "maximum": p["maximum"],
                                         "description": p["description"] + " (" + p["unit"] + ")"} for p in PARAMETERS}}
    specifications = [
        ("tuning_schema", "Read numeric ranges, defaults, units and relational constraints.", {}, [], True),
        ("tuning_list", "Discover active experimental engine instances on this Windows session.", {}, [], True),
        ("tuning_get", "Read requested/applied values and revisions; pending is not applied.", {"session": session}, ["session"], True),
        ("tuning_set", "Queue a validated parameter patch for the next input pair. Current pair phases stay unchanged. This does not imply quality improvement.",
         dict(common, values=values), ["session", "values"], False),
        ("tuning_reset", "Queue the compiled baseline defaults for the next input pair.", common, ["session"], False),
    ]
    return [{"name": name, "description": description,
             "inputSchema": {"type": "object", "properties": properties, "required": required, "additionalProperties": False},
             "annotations": {"readOnlyHint": readonly, "destructiveHint": False, "idempotentHint": readonly, "openWorldHint": False}}
            for name, description, properties, required, readonly in specifications]


def serve(controller, source, output):
    initialized, ready = False, False

    def send(message):
        output.write(json.dumps(message, ensure_ascii=False, allow_nan=False) + "\n")
        output.flush()

    def error(identifier, code, message):
        send({"jsonrpc": "2.0", "id": identifier, "error": {"code": code, "message": message}})

    for line in source:
        if not line.strip():
            continue
        try:
            if len(line) > 1024 * 1024:
                raise ValueError("Message too large")
            request = json.loads(line, parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
        except (ValueError, json.JSONDecodeError):
            error(None, -32700, "Invalid JSON")
            continue
        if not isinstance(request, dict) or request.get("jsonrpc") != "2.0" or not isinstance(request.get("method"), str):
            error(None, -32600, "Invalid request")
            continue
        identifier, method = request.get("id"), request["method"]
        if "id" not in request:
            if method == "notifications/initialized" and initialized:
                ready = True
            continue
        if isinstance(identifier, bool) or not isinstance(identifier, (str, int)):
            error(None, -32600, "Invalid request ID")
            continue
        params = request.get("params", {})
        if not isinstance(params, dict):
            error(identifier, -32602, "Parameters must be an object")
            continue
        if method == "initialize":
            if initialized or not isinstance(params.get("protocolVersion"), str):
                error(identifier, -32602, "Invalid initialization")
                continue
            requested = params["protocolVersion"]
            protocol = requested if requested in ("2024-11-05", "2025-03-26", "2025-06-18") else "2025-06-18"
            result = {"protocolVersion": protocol, "capabilities": {"tools": {"listChanged": False}},
                      "serverInfo": {"name": "nvof-tuning", "version": "1.2.0"}}
            initialized = True
        elif method == "ping":
            result = {}
        elif not ready:
            error(identifier, -32000, "Initialize and send notifications/initialized first")
            continue
        elif method == "tools/list":
            result = {"tools": tool_list()}
        elif method == "tools/call":
            try:
                name = params.get("name")
                value = dispatch(controller, name, params.get("arguments", {}))
                result = {"content": [{"type": "text", "text": json.dumps(value, ensure_ascii=False, allow_nan=False)}]}
            except (ValueError, RuntimeError, OSError, TypeError) as failure:
                result = {"isError": True, "content": [{"type": "text", "text": str(failure)}]}
        else:
            error(identifier, -32601, "Method not found")
            continue
        send({"jsonrpc": "2.0", "id": identifier, "result": result})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", help="Same absolute NVOF_TUNING_DIR as the engine; default LOCALAPPDATA/NvofPotPlayer/Tuning")
    args = parser.parse_args()
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    serve(Controller(args.directory), sys.stdin, sys.stdout)
