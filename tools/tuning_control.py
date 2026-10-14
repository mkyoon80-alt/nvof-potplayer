"""Windows-only live tuning transport. Standard library; no player automation."""
from __future__ import annotations

import contextlib
import ctypes as C
from ctypes import wintypes as W
import json
import math
import os
from pathlib import Path
import re
import struct
import time

ROOT = Path(__file__).resolve().parents[1]
# A deployed experimental build may intentionally precede pending release
# defaults. Its matching schema keeps resets and validation on that same ABI.
SPEC = Path(os.environ.get("NVOF_TUNING_SPEC") or ROOT / "bin/NvofTuning.schema.def")
if os.environ.get("NVOF_TUNING_SPEC") and not SPEC.is_file():
    raise ValueError("NVOF_TUNING_SPEC does not exist")
if not SPEC.is_file():
    SPEC = ROOT / "include/nvof/tuning_parameters.def"
ROW = re.compile(r'NVOF_TUNABLE\((\w+), ([\d.]+), ([\d.]+), ([\d.]+), "([^"]+)", "([^"]+)", "([^"]+)"\)')
PARAMETERS = [dict(name=m[0], default=float(m[1]), minimum=float(m[2]), maximum=float(m[3]),
                   group=m[4], unit=m[5], description=m[6]) for m in ROW.findall(SPEC.read_text(encoding="utf-8"))]
# Include the float32 representation of each endpoint so returned snapshots can
# be submitted again, including exact maximum values such as 0.99f.
for parameter in PARAMETERS:
    parameter["minimum"] = min(parameter["minimum"], C.c_float(parameter["minimum"]).value)
    parameter["maximum"] = max(parameter["maximum"], C.c_float(parameter["maximum"]).value)
NAMES = [p["name"] for p in PARAMETERS]
DEFAULTS = {p["name"]: p["default"] for p in PARAMETERS}
COUNT = len(PARAMETERS)
assert COUNT and COUNT % 4 == 0 and len(set(NAMES)) == COUNT
SESSION = re.compile(r"[1-9][0-9]*-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}")


def schema_hash():
    value = 2166136261
    for p in PARAMETERS:
        data = p["name"].encode("ascii") + b"\0" + struct.pack("<fff", p["default"], p["minimum"], p["maximum"])
        for byte in data:
            value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def schema():
    return {"version": 1, "schema_hash": schema_hash(), "parameters": PARAMETERS,
            "constraints": ["repair_min_motion <= repair_propagation_min_motion",
                            "evidence_fb_low < evidence_fb_high",
                            "evidence_residual_low < evidence_residual_high",
                            "evidence_photo_low < evidence_photo_high",
                            "stationary_error_low < stationary_error_high",
                            "stationary_detail_low < stationary_detail_high"],
            "application": "Atomically at the next successfully prepared input pair; current pair phases keep their snapshot."}


def validate(values, quantized=False):
    if set(values) != set(NAMES):
        raise ValueError("Parameter set must match this build's schema")
    for p in PARAMETERS:
        value = values[p["name"]]
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise ValueError(f'{p["name"]}: finite number required')
        low, high = p["minimum"], p["maximum"]
        if quantized:
            low, high = C.c_float(low).value, C.c_float(high).value
        if not low <= value <= high:
            raise ValueError(f'{p["name"]}: expected {p["minimum"]}..{p["maximum"]}')
    if values["repair_min_motion"] > values["repair_propagation_min_motion"]:
        raise ValueError("repair_min_motion must not exceed repair_propagation_min_motion")
    for prefix in ("evidence_fb", "evidence_residual", "evidence_photo", "stationary_error", "stationary_detail"):
        if values[prefix + "_low"] >= values[prefix + "_high"]:
            raise ValueError(prefix + "_low must be smaller than _high")


class Shared(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in (
        "magic", "version", "bytes", "parameter_count", "pid", "alive", "requested_revision", "applied_revision",
        "rejected_revision", "error_code", "pair_count", "phase_count", "width", "height", "prepared", "schema_hash"
    )] + [("requested", C.c_float * COUNT), ("applied", C.c_float * COUNT)]


assert C.sizeof(Shared) == 64 + COUNT * 8


def kernel():
    if os.name != "nt":
        raise RuntimeError("Live engine IPC requires Windows")
    dll = C.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "OpenFileMappingW": ([W.DWORD, W.BOOL, W.LPCWSTR], W.HANDLE),
        "MapViewOfFile": ([W.HANDLE, W.DWORD, W.DWORD, W.DWORD, C.c_size_t], C.c_void_p),
        "UnmapViewOfFile": ([C.c_void_p], W.BOOL),
        "OpenMutexW": ([W.DWORD, W.BOOL, W.LPCWSTR], W.HANDLE),
        "WaitForSingleObject": ([W.HANDLE, W.DWORD], W.DWORD),
        "ReleaseMutex": ([W.HANDLE], W.BOOL),
        "CloseHandle": ([W.HANDLE], W.BOOL),
        "OpenProcess": ([W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
    }
    for name, (args, result) in signatures.items():
        function = getattr(dll, name)
        function.argtypes, function.restype = args, result
    return dll


class Controller:
    def __init__(self, directory=None):
        self.directory = Path(directory or os.environ.get("NVOF_TUNING_DIR") or
                              Path(os.environ.get("LOCALAPPDATA", "")) / "NvofPotPlayer/Tuning")
        if not self.directory.is_absolute():
            raise ValueError("Tuning directory must be absolute")
        self.win = kernel()

    @contextlib.contextmanager
    def connect(self, session):
        if not isinstance(session, str) or not SESSION.fullmatch(session):
            raise ValueError("Invalid session ID; use tuning_list")
        if not (self.directory / (session + ".session")).is_file():
            raise ValueError("Session is no longer advertised; use tuning_list")
        name = "Local\\NvofTuning-" + session
        mapping = mutex = address = process = None
        held = False
        try:
            mapping = self.win.OpenFileMappingW(0x0006, False, name)  # read + write
            if not mapping:
                raise ValueError("Session has ended or is inaccessible")
            address = self.win.MapViewOfFile(mapping, 0x0006, 0, 0, C.sizeof(Shared))
            mutex = self.win.OpenMutexW(0x00100001, False, name + "-lock")
            if not address or not mutex:
                raise RuntimeError("Cannot open tuning state")
            held = self.win.WaitForSingleObject(mutex, 200) in (0, 0x80)
            if not held:
                raise RuntimeError("Tuning controller is busy; retry")
            state = Shared.from_address(address)
            if (state.magic, state.version, state.bytes, state.parameter_count, state.schema_hash) != (
                    0x4e565431, 1, C.sizeof(Shared), COUNT, schema_hash()):
                raise ValueError("Engine/controller schema mismatch; rebuild or use the matching controller")
            if not state.alive or state.pid != int(session.split("-", 1)[0]):
                raise ValueError("Session is not alive")
            process = self.win.OpenProcess(0x00100000, False, state.pid)
            if not process or self.win.WaitForSingleObject(process, 0) != 0x102:
                raise ValueError("Engine process has ended or is inaccessible")
            yield state
        finally:
            if held:
                self.win.ReleaseMutex(mutex)
            if address:
                self.win.UnmapViewOfFile(address)
            for handle in (process, mutex, mapping):
                if handle:
                    self.win.CloseHandle(handle)

    @staticmethod
    def snapshot(session, state):
        result = {name: int(getattr(state, name)) for name, _ in Shared._fields_[:-2]}
        result.update(session=session, requested=dict(zip(NAMES, state.requested)),
                      applied=dict(zip(NAMES, state.applied)),
                      pending=state.requested_revision != state.applied_revision)
        result["applied_meaning"] = "Submitted successfully during prepare; not a display/presentation acknowledgement"
        return result

    def get(self, session):
        with self.connect(session) as state:
            return self.snapshot(session, state)

    def list(self):
        active, unavailable = [], 0
        for path in sorted(self.directory.glob("*.session")):
            try:
                state = self.get(path.stem)
                active.append({k: v for k, v in state.items() if k not in ("requested", "applied")})
            except (ValueError, RuntimeError, OSError):
                unavailable += 1
        return {"sessions": active, "unavailable_manifests": unavailable, "directory": str(self.directory)}

    def set(self, session, values, expected_revision=None, wait_ms=0):
        if not isinstance(values, dict) or not values or set(values) - set(NAMES):
            raise ValueError("Provide a non-empty patch containing only known parameter names")
        if isinstance(wait_ms, bool) or not isinstance(wait_ms, int) or not 0 <= wait_ms <= 3000:
            raise ValueError("wait_ms must be an integer from 0 to 3000")
        if expected_revision is not None and (isinstance(expected_revision, bool) or
                not isinstance(expected_revision, int) or not 1 <= expected_revision <= 0xffffffff):
            raise ValueError("Invalid expected_revision")
        with self.connect(session) as state:
            if expected_revision is not None and expected_revision != state.requested_revision:
                raise ValueError("Revision conflict; read current values before retrying")
            merged = dict(zip(NAMES, state.requested))
            merged.update(values)
            validate(merged)
            # Validate float32 values too: rounding must not collapse a smoothstep interval.
            rounded = {k: C.c_float(v).value for k, v in merged.items()}
            validate(rounded, quantized=True)
            if state.requested_revision == 0xffffffff:
                raise ValueError("Revision limit reached; start a new engine session")
            state.requested[:] = [rounded[k] for k in NAMES]
            state.requested_revision += 1
            revision = int(state.requested_revision)
            result = self.snapshot(session, state)
        deadline = time.monotonic() + wait_ms / 1000
        while result["applied_revision"] != revision and time.monotonic() < deadline:
            time.sleep(0.01)
            result = self.get(session)
            if result["requested_revision"] != revision or result["rejected_revision"] == revision:
                break
        result["submitted_revision"] = revision
        result["submission_status"] = ("applied" if result["applied_revision"] == revision else
            "rejected" if result["rejected_revision"] == revision else
            "superseded" if result["requested_revision"] != revision else "pending_next_pair")
        return result


def dispatch(controller, name, arguments):
    if not isinstance(arguments, dict):
        raise ValueError("Arguments must be an object")
    allowed = {"tuning_schema": set(), "tuning_list": set(), "tuning_get": {"session"},
               "tuning_set": {"session", "values", "expected_revision", "wait_ms"},
               "tuning_reset": {"session", "expected_revision", "wait_ms"}}
    if name not in allowed or set(arguments) - allowed[name]:
        raise ValueError("Unknown tool or arguments")
    if name == "tuning_schema":
        return schema()
    if name == "tuning_list":
        return controller.list()
    if "session" not in arguments:
        raise ValueError("session is required")
    if name == "tuning_get":
        return controller.get(arguments["session"])
    if name == "tuning_reset":
        return controller.set(values=DEFAULTS, **arguments)
    if "values" not in arguments:
        raise ValueError("values is required")
    return controller.set(**arguments)


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path)
    parser.add_argument("command", choices=["schema", "list", "get", "set", "reset"])
    parser.add_argument("session", nargs="?")
    parser.add_argument("values", nargs="?", help='JSON patch or @path-to-json; set only')
    parser.add_argument("--expected-revision", type=int)
    parser.add_argument("--wait-ms", type=int, default=0)
    args = parser.parse_args()
    try:
        arguments = {} if args.command in ("schema", "list") else {"session": args.session}
        if args.command in ("set", "reset"):
            arguments.update(expected_revision=args.expected_revision, wait_ms=args.wait_ms)
        if args.command == "set":
            if args.values is None:
                raise ValueError("set requires a JSON patch")
            text = Path(args.values[1:]).read_text(encoding="utf-8-sig") if args.values.startswith("@") else args.values
            arguments["values"] = json.loads(text)
        controller = None if args.command == "schema" else Controller(args.directory)
        print(json.dumps(dispatch(controller, "tuning_" + args.command, arguments), indent=2, ensure_ascii=False))
    except (ValueError, RuntimeError, OSError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
