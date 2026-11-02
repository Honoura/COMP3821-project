#!/usr/bin/env python3
"""Local server for the PC visualiser.

Serves the web UI and compiles/runs uploaded C++ strategies against
simulator/include/tetris.h and simulator/runner/runner.cpp.

    python3 visualiser/server.py [--port 8000] [--cxx c++]

Then open http://localhost:8000. Strategies run as native code on this
machine, so the server only listens on localhost.
"""

import argparse
import json
import os
import shutil
import subprocess
import tempfile
import time
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PUBLIC = ROOT / "visualiser" / "public"
SIM = ROOT / "simulator"
PC_BINARY = ROOT / "perfect-tetris" / "zig-out" / "bin" / "pc"
SFINDER_JAR = ROOT / "tools" / "sfinder" / "sfinder.jar"
DOWNLOADS = {
    "tetris.h": SIM / "include" / "tetris.h",
    "basic.cpp": SIM / "strategies" / "basic.cpp",
    "dp.cpp": SIM / "strategies" / "dp.cpp",
    "sfinder_dp.cpp": SIM / "strategies" / "sfinder_dp.cpp",
}
MAX_SOURCE_BYTES = 2_000_000
MAX_LOG_CHARS = 200_000

CONFIG = {"cxx": os.environ.get("CXX", "c++")}


def clip(text):
    if len(text) <= MAX_LOG_CHARS:
        return text
    return text[:MAX_LOG_CHARS] + f"\n... ({len(text) - MAX_LOG_CHARS} more characters truncated)"


def run_strategy(req):
    source = req.get("source", "")
    if not source.strip():
        return {"ok": False, "stage": "input", "message": "No strategy source was sent."}
    if len(source) > MAX_SOURCE_BYTES:
        return {"ok": False, "stage": "input", "message": "Strategy file is too large."}

    args = ["--length", str(int(req.get("length", 200)))]
    queue = "".join(c for c in str(req.get("queue", "")).upper() if c in "IOTLJSZ")
    if queue:
        args += ["--queue", queue]
    else:
        args += ["--seed", str(int(req.get("seed", 0)))]
    if int(req.get("cap", 0)) > 0:
        args += ["--cap", str(int(req["cap"]))]
    if not req.get("hold", True):
        args += ["--no-hold"]
    timeout = max(1, min(int(req.get("timeout", 300)), 1800))

    with tempfile.TemporaryDirectory(prefix="pcvis-") as tmp:
        tmp = Path(tmp)
        (tmp / "strategy.cpp").write_text(source)
        exe = tmp / "run"
        compile_cmd = [
            CONFIG["cxx"], "-std=c++17", "-O2", "-pthread", "-I", str(SIM / "include"),
            str(SIM / "runner" / "runner.cpp"), str(tmp / "strategy.cpp"), "-o", str(exe),
        ]
        t0 = time.monotonic()
        try:
            build = subprocess.run(compile_cmd, capture_output=True, text=True, timeout=120)
        except FileNotFoundError:
            return {"ok": False, "stage": "compile", "message": f"Compiler '{CONFIG['cxx']}' not found."}
        except subprocess.TimeoutExpired:
            return {"ok": False, "stage": "compile", "message": "Compilation timed out."}
        compile_ms = (time.monotonic() - t0) * 1000
        compile_log = (build.stdout + build.stderr).replace(str(tmp) + "/", "")
        if build.returncode != 0:
            return {"ok": False, "stage": "compile", "message": "Compilation failed.", "log": clip(compile_log)}

        out = tmp / "game.json"
        t0 = time.monotonic()
        try:
            env = dict(os.environ)
            if PC_BINARY.exists():
                env.setdefault("PERFECT_TETRIS_PC", str(PC_BINARY))
            if SFINDER_JAR.exists():
                env.setdefault("SFINDER_JAR", str(SFINDER_JAR))
            proc = subprocess.run([str(exe), *args, "--out", str(out)], capture_output=True, text=True,
                                  timeout=timeout, cwd=tmp, env=env)
        except subprocess.TimeoutExpired as e:
            def text(b):
                return b.decode(errors="replace") if isinstance(b, bytes) else (b or "")
            return {"ok": False, "stage": "run", "message": f"Strategy exceeded the {timeout}s time limit.",
                    "stdout": clip(text(e.stdout)), "stderr": clip(text(e.stderr)), "compileLog": clip(compile_log)}
        run_ms = (time.monotonic() - t0) * 1000

        result = {"ok": True, "compileLog": clip(compile_log), "stdout": clip(proc.stdout),
                  "stderr": clip(proc.stderr), "compileMs": compile_ms, "runMs": run_ms,
                  "exitCode": proc.returncode}
        if not out.exists():
            result.update(ok=False, stage="run",
                          message=f"Strategy crashed (exit code {proc.returncode}) before a game log was written.")
            return result
        result["game"] = json.loads(out.read_text())
        return result


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(PUBLIC), **kwargs)

    def log_message(self, fmt, *args):
        if not self.path.startswith("/api/"):
            return
        super().log_message(fmt, *args)

    def send_json(self, obj, status=HTTPStatus.OK):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/health":
            return self.send_json({"ok": True, "cxx": CONFIG["cxx"], "cxxFound": bool(shutil.which(CONFIG["cxx"])),
                                   "perfectTetris": PC_BINARY.exists()})
        if self.path.startswith("/download/"):
            name = self.path[len("/download/"):]
            path = DOWNLOADS.get(name)
            if not path:
                return self.send_error(HTTPStatus.NOT_FOUND)
            body = path.read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith("/source/"):
            path = DOWNLOADS.get(self.path[len("/source/"):])
            if not path:
                return self.send_error(HTTPStatus.NOT_FOUND)
            return self.send_json({"source": path.read_text()})
        return super().do_GET()

    def do_POST(self):
        if self.path != "/api/run":
            return self.send_error(HTTPStatus.NOT_FOUND)
        try:
            length = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(length))
            result = run_strategy(req)
        except (ValueError, json.JSONDecodeError) as e:
            result = {"ok": False, "stage": "input", "message": f"Bad request: {e}"}
        self.send_json(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--cxx", default=CONFIG["cxx"], help="C++ compiler (default: $CXX or c++)")
    args = parser.parse_args()
    CONFIG["cxx"] = args.cxx
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"PC visualiser running at http://localhost:{args.port}  (compiler: {args.cxx})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
