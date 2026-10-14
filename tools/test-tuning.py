"""Validate MCP requests and optionally live GPU application, without touching the player."""
import argparse
import io
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import tuning_control as control
import tuning_mcp as mcp


class ProtocolTests(unittest.TestCase):
    def test_schema_and_validation(self):
        control.validate(control.DEFAULTS)
        for patch in ({"warp_photo_scale": float("nan")}, {"warp_photo_scale": True},
                      {"warp_photo_scale": 0}, {"evidence_fb_low": 2}, {"repair_min_motion": 128}):
            with self.assertRaises(ValueError):
                control.validate(dict(control.DEFAULTS, **patch))
        self.assertEqual(len(mcp.tool_list()), 5)
        rounded = {k: control.C.c_float(v).value for k, v in control.DEFAULTS.items()}
        rounded["pattern_improvement_ratio"] = control.C.c_float(0.99).value
        control.validate(rounded, quantized=True)

    def test_protocol(self):
        requests = [
            {"jsonrpc": "2.0", "id": 1, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 2, "method": "initialize", "params": {"protocolVersion": "2099-01-01"}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "tuning_schema"}},
            {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "performance_set"}},
            {"jsonrpc": "2.0", "id": 6, "method": "tools/call", "params": {"name": "tuning_schema", "arguments": {"extra": 1}}},
            {"jsonrpc": "2.0", "id": 7, "method": "ping"},
        ]
        output = io.StringIO()
        mcp.serve(None, io.StringIO("\n".join(map(json.dumps, requests))), output)
        answers = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(len(answers), 7)
        self.assertIn("error", answers[0])
        self.assertEqual(answers[1]["result"]["protocolVersion"], "2025-06-18")
        self.assertEqual(len(answers[2]["result"]["tools"]), 5)
        self.assertFalse(answers[3]["result"].get("isError", False))
        self.assertTrue(answers[4]["result"]["isError"])
        self.assertTrue(answers[5]["result"]["isError"])
        self.assertEqual(answers[6]["result"], {})


class Process:
    def __init__(self, command, env):
        self.process = subprocess.Popen(command, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)

    def read(self):
        try:
            line = self.lines.get(timeout=30)
        except queue.Empty:
            raise RuntimeError("Diagnostic process response timed out")
        if line is None:
            raise RuntimeError("Diagnostic process exited: " + self.process.stderr.read())
        return line

    def write(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=10)


def live_test(executable):
    # Inspect all processes successfully before starting a GPU test.
    guard = subprocess.run(["powershell", "-NoProfile", "-Command",
        "$ErrorActionPreference='Stop'; $p=Get-Process; if($p | Where-Object {$_.ProcessName -match '^PotPlayer'}) {exit 2}"],
        capture_output=True, text=True)
    if guard.returncode:
        raise RuntimeError("Close PotPlayer before GPU verification (or process enumeration failed)")
    with tempfile.TemporaryDirectory(prefix="nvof-live-tuning-", dir=control.ROOT / "build") as directory:
        env = {k.upper(): v for k, v in os.environ.items()}
        env["NVOF_TUNING_DIR"] = directory
        host = Process([str(executable)], env)
        server = None
        try:
            assert host.read().startswith("READY ")
            server = Process([sys.executable, str(control.ROOT / "tools/tuning_mcp.py"), "--directory", directory], env)
            request_id = 0

            def rpc(method, params):
                nonlocal request_id
                request_id += 1
                server.write(json.dumps(dict(jsonrpc="2.0", id=request_id, method=method, params=params)))
                answer = json.loads(server.read())
                assert answer["id"] == request_id and "error" not in answer, answer
                return answer["result"]

            def call(name, arguments=None, error=False):
                result = rpc("tools/call", dict(name=name, arguments=arguments or {}))
                assert bool(result.get("isError")) == error, result
                return result if error else json.loads(result["content"][0]["text"])

            rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
            server.write(json.dumps(dict(jsonrpc="2.0", method="notifications/initialized")))
            assert len(rpc("tools/list", {})["tools"]) == 5
            sessions = call("tuning_list")["sessions"]
            assert len(sessions) == 1, sessions
            session = sessions[0]["session"]

            def host_call(command):
                host.write(command)
                return host.read()

            assert host_call("prepare") == "PREPARED 1"
            baseline = [host_call("phase " + phase) for phase in ("0.25", "0.5", "0.75")]
            initial = call("tuning_get", {"session": session})
            assert initial["applied_revision"] == 1
            for bad in ({"warp_photo_scale": 0}, {"evidence_fb_low": 2}, {"no_such_parameter": 1}, {"warp_photo_scale": True}):
                call("tuning_set", {"session": session, "values": bad}, error=True)
            call("tuning_set", {"session": session, "values": {"warp_photo_scale": 4}, "expected_revision": 99}, error=True)
            assert call("tuning_get", {"session": session})["requested_revision"] == 1
            submitted = call("tuning_set", {"session": session, "values": {"warp_photo_scale": 1}, "expected_revision": 1, "wait_ms": 20})
            assert submitted["submission_status"] == "pending_next_pair" and submitted["applied_revision"] == 1
            unchanged = [host_call("phase " + phase) for phase in ("0.25", "0.5", "0.75")]
            assert unchanged == baseline, (baseline, unchanged)
            assert host_call("prepare") == "PREPARED 1"
            changed = [host_call("phase " + phase) for phase in ("0.25", "0.5", "0.75")]
            assert changed != baseline, "Numeric update did not change actual GPU output"
            applied = call("tuning_get", {"session": session})
            assert applied["applied_revision"] == 2 and applied["applied"]["warp_photo_scale"] == 1
            call("tuning_reset", {"session": session, "expected_revision": 2})
            assert host_call("prepare") == "PREPARED 1"
            restored = [host_call("phase " + phase) for phase in ("0.25", "0.5", "0.75")]
            assert restored == baseline, "Default reset did not restore identical pixels"
            assert call("tuning_get", {"session": session})["applied_revision"] == 3
            assert host_call("reset") == "RESET"
            assert call("tuning_get", {"session": session})["prepared"] == 0
            assert host_call("prepare") == "PREPARED 1"
            assert host_call("phase 0.5") == baseline[1]
            host.write("quit")
            assert host.process.wait(timeout=10) == 0
            assert call("tuning_list")["sessions"] == []
            call("tuning_get", {"session": session}, error=True)
            print(json.dumps({"result": "PASS", "baseline_hashes": baseline, "changed_hashes": changed,
                              "restored_hashes": restored, "checks": "MCP stdio, validation, CAS, next-pair latch, GPU pixel change, exact reset, seek, disconnect"}))
        finally:
            host.close()
            if server:
                server.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live-exe", type=Path)
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ProtocolTests))
    if not result.wasSuccessful():
        sys.exit(1)
    if args.live_exe:
        live_test(args.live_exe.resolve())
