"""Run one decoded clip with an acknowledged live parameter snapshot.

The experimental video_x2_repro binary is built once. Every trial uses the
same controller/validation/shared-memory path as the MCP server. No settings
are loaded into the installed player. Caller owns the output directory.
"""
import json
import os
from pathlib import Path
import queue
import subprocess
import threading

import tuning_control as control


def run(executable, source, timestamps, output, width, height, duration,
        values=None, fmt="nv12", cap=0, flow_dimension=1920):
    output = Path(output).resolve()
    if not output.is_relative_to(control.ROOT / "build") or output.exists():
        raise ValueError("Choose a new directory below build/")
    merged = dict(control.DEFAULTS, **(values or {}))
    control.validate(merged)
    output.mkdir(parents=True)
    env = {k.upper(): v for k, v in os.environ.items()}
    env["NVOF_TUNING_DIR"] = str(output / "ipc")
    env["LOCALAPPDATA"] = str(output / "localappdata")
    env["NVOF_TUNING_CONTROL"] = "1"
    Path(env["LOCALAPPDATA"]).mkdir()
    command = list(map(str, [executable, control.ROOT / "build/absent-runtime",
        source, timestamps, output, "native", duration, fmt, width, height,
        flow_dimension, 0, 4, "medium", "reuse", cap]))
    lines = queue.Queue()
    process = subprocess.Popen(command, env=env, cwd=control.ROOT,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace")

    def reader():
        with (output / "run.log").open("w", encoding="utf-8") as log:
            for line in process.stdout:
                log.write(line); log.flush(); lines.put(line.strip())
        lines.put(None)

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    def receive(prefix):
        while True:
            line = lines.get(timeout=120)
            if line is None:
                raise RuntimeError((output / "run.log").read_text(encoding="utf-8"))
            if line.startswith(prefix):
                return line

    def send(text):
        process.stdin.write(text + "\n"); process.stdin.flush()

    try:
        receive("TUNING_READY ")
        controller = control.Controller(Path(env["NVOF_TUNING_DIR"]))
        sessions = controller.list()["sessions"]
        if len(sessions) != 1 or sessions[0]["pid"] != process.pid:
            raise RuntimeError("Unexpected diagnostic engine session")
        session = sessions[0]["session"]
        submitted = controller.set(session, merged, expected_revision=1)
        send("run")
        receive("TUNING_DONE")
        applied = controller.get(session)
        if applied["applied_revision"] != submitted["submitted_revision"]:
            raise RuntimeError("No prepared pair acknowledged the requested values")
        quantized = {k: control.C.c_float(v).value for k, v in merged.items()}
        if applied["applied"] != quantized or applied["rejected_revision"]:
            raise RuntimeError("The measured snapshot differs from the requested values")
        (output / "applied.json").write_text(json.dumps(applied, indent=2))
        send("quit")
        if process.wait(timeout=15):
            raise RuntimeError("Diagnostic engine failed")
        thread.join(timeout=5)
        return applied
    finally:
        if process.poll() is None:
            process.terminate(); process.wait(timeout=15)
        process.stdin.close()
        thread.join(timeout=5)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spec", type=Path, help="JSON keyword arguments to run()")
    args = parser.parse_args()
    run(**json.loads(args.spec.read_text(encoding="utf-8-sig")))
