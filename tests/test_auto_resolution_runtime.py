"""Start the real RC + stdio MCP bridge headlessly, in an isolated profile/pipe.

No pip packages. On Windows, MSVC builds a known-layout process/PDB fixture.
Run: python tests/test_auto_resolution_runtime.py --build-dir build
All subprocesses belong to this test; existing RC sessions are untouched.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import threading
import time
import uuid


class Client:
    def __init__(self, exe, env, log, sampling=False):
        self.proc = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=log, env=env, creationflags=subprocess.CREATE_NO_WINDOW)
        self.messages = queue.Queue()
        self.next_id = 0
        self.sampling = lambda request: {"fields": []}
        self.pending = []
        threading.Thread(target=self._read, daemon=True).start()
        try:
            self.rpc("initialize", {"protocolVersion": "2024-11-05",
                     "capabilities": {"sampling": {}} if sampling else {},
                     "clientInfo": {"name": "RC runtime integration test", "version": "1"}})
        except Exception:
            self.close()
            raise
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _read(self):
        for line in self.proc.stdout:
            try:
                self.messages.put(json.loads(line))
            except ValueError:
                self.messages.put({"transportError": line.decode(errors="replace")})
        self.messages.put({"transportError": "MCP transport closed"})

    def send(self, message):
        self.proc.stdin.write(json.dumps(message).encode() + b"\n")
        self.proc.stdin.flush()

    def respond(self, request, value):
        text = value if isinstance(value, str) else json.dumps(value)
        self.send({"jsonrpc": "2.0", "id": request["id"], "result": {
            "role": "assistant", "content": {"type": "text", "text": text}, "model": "controlled-test-double"}})

    def rpc(self, method, params=None, timeout=30):
        self.next_id += 1
        request_id = self.next_id
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}})
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            message = self.messages.get(timeout=max(.01, deadline - time.monotonic()))
            assert "transportError" not in message, message
            if message.get("method") == "sampling/createMessage":
                self.pending.append(message)
                value = self.sampling(message)
                if value is not None:
                    self.respond(message, value)
            elif message.get("id") == request_id:
                assert "error" not in message, message
                return message["result"]
        raise TimeoutError(method)

    def tool(self, name, args=None, error=False):
        result = self.rpc("tools/call", {"name": name, "arguments": args or {}})
        assert bool(result.get("isError")) == error, result
        if "structuredContent" in result:
            return result["structuredContent"]
        text = result["content"][0]["text"]
        try:
            return json.loads(text)
        except ValueError:
            return {"text": text}

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=3)


def build_fixture(repo, directory, wrong=False):
    directory.mkdir(parents=True, exist_ok=True)
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.check_output([str(vswhere), "-latest", "-property", "installationPath"], text=True).strip()
    vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
    source = repo / "tests/auto_resolution_fixture.cpp"
    # A fixed build script keeps cmd quoting out of JSON/RPC data entirely.
    script = directory / "build_fixture.cmd"
    script.write_text(f'@echo off\ncall "{vcvars}" >nul\n'
                      f'cl /nologo /Od /Zi /EHsc {"/DWRONG_SCHEMA" if wrong else ""} '
                      f'"{source}" /Fe:auto_fixture.exe /Fd:compile.pdb '
                      '/link /DEBUG:FULL /INCREMENTAL:NO /PDB:auto_fixture.pdb\n', encoding="utf-8")
    result = subprocess.run(["cmd", "/d", "/c", str(script)], cwd=directory,
                            capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW, timeout=60)
    (directory / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode == 0, result.stdout + result.stderr
    return directory / "auto_fixture.exe"


def wait_for(client, tool, args, states, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = client.tool(tool, args)
        if result.get("state") in states:
            return result
        if result.get("state") in {"failed", "stale"}:
            detail = client.tool("analysis.infer_types", {"action": "status", "jobId": result["jobId"]}) \
                if tool == "ui.action" and result.get("jobId") else result
            raise AssertionError(detail)
        time.sleep(.08)
    raise AssertionError(f"Timeout waiting for {states}: {result}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    options = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    build = Path(options.build_dir).resolve()
    run_dir = build / ("mcp-runtime-" + uuid.uuid4().hex[:10])
    run_dir.mkdir()
    env = os.environ.copy()
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    variables = dict(line.split("=", 1) for line in cache.splitlines() if "=" in line and not line.startswith(("#", "//")))
    qt = Path(variables["Qt6_DIR:PATH"]).parents[2]
    compiler = Path(next(value for key, value in variables.items() if key.startswith("CMAKE_CXX_COMPILER:")))
    env.update(QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORM_PLUGIN_PATH=str(qt / "plugins/platforms"),
               QT_QPA_FONTDIR=str(repo / "src/fonts"),
               RCX_HEADLESS_MCP="1", RCX_SETTINGS_DIR=str(run_dir / "settings"),
               RCX_HEADLESS_PLUGIN=str(build / "Plugins/libProcessMemoryPlugin.dll"),
               RCX_MCP_PIPE="RC-AUTO-test-" + uuid.uuid4().hex, QT_FORCE_STDERR_LOGGING="1")
    env["PATH"] = str(qt / "bin") + os.pathsep + str(compiler.parent) + os.pathsep + env["PATH"]
    app_log = (run_dir / "rc.log").open("wb")
    bridge_log = (run_dir / "bridge.log").open("wb")
    app = subprocess.Popen([str(build / "RC.exe")], env=env, stdout=app_log, stderr=app_log,
                           creationflags=subprocess.CREATE_NO_WINDOW)
    clients = []
    fixture = None
    passed = []
    def mark(name):
        passed.append(name)
        print("PASS", name, flush=True)
    try:
        deadline = time.monotonic() + 20
        while not ctypes.windll.kernel32.WaitNamedPipeW("\\\\.\\pipe\\" + env["RCX_MCP_PIPE"], 100):
            assert app.poll() is None, f"RC exited: {app.returncode}; see {run_dir / 'rc.log'}"
            assert time.monotonic() < deadline, "RC did not start its private MCP endpoint"
            time.sleep(.05)
        client = Client(build / "RcMcpBridge.exe", env, bridge_log)
        clients.append(client)
        assert "tools" in client.rpc("tools/list")
        mark("real application and MCP transport start automatically without windows")
        client.tool("ui.action", {"action": "sample_class"})
        sample_state = client.tool("project.state", {"depth": 0, "limit": 500})
        ball_node = next(n for n in sample_state["tree"]["nodes"] if n["name"] == "sim")
        ball = ball_node["id"]
        ball_span = ball_node["computedSize"]
        def ball_tree():
            return client.tool("project.state", {"parentId": ball, "depth": 1})["tree"]["nodes"]
        members = ball_tree()
        original_offsets = {n["name"]: n["offset"] for n in members if n["name"]}
        kind_sizes = {"UInt32": 4, "Double": 8, "Vec3": 12, "Float": 4,
                      "Mat4x4": 64, "Vec4": 16, "Pointer64": 8, "Hex32": 4}
        raw_ops = [{"op": "remove", "nodeId": n["id"]} for n in members]
        for member in members:
            cursor = member["offset"]
            remaining = kind_sizes[member["kind"]]
            first = True
            while remaining:
                size = next(size for size in (8, 4, 2, 1) if size <= remaining)
                raw_ops.append({"op": "insert", "kind": f"Hex{size * 8}", "parentId": ball,
                                "offset": cursor,
                                "name": member["name"] if first and member["name"] else f"field_{cursor:04x}"})
                cursor += size
                remaining -= size
                first = False
        client.tool("tree.apply", {"operations": raw_ops})
        raw_named_ball = ball_tree()
        assert all(n["kind"].startswith("Hex") for n in raw_named_ball), raw_named_ball
        raw_sizes = {"Hex8": 1, "Hex16": 2, "Hex32": 4, "Hex64": 8, "Hex128": 16}
        assert max(n["offset"] + raw_sizes[n["kind"]] for n in raw_named_ball) == ball_span == 0x98
        # AUTO owns only the short recording it starts, and keeps its history.
        client.tool("ui.action", {"action": "auto_resolve"})
        deadline = time.monotonic() + 8
        while True:
            progress = client.tool("ui.action", {"action": "auto_status"})
            if progress.get("recording") and progress.get("recordedSamples", 0):
                break
            assert time.monotonic() < deadline, progress
            time.sleep(.05)
        stopped = client.tool("ui.action", {"action": "auto_resolve"})
        assert stopped["state"] == "cancelled" and not stopped["recording"] and stopped["recordedSamples"] > 0
        assert ball_tree() == raw_named_ball
        mark("cancelling AUTO stops its short capture and preserves the shared recording")
        client.tool("ui.action", {"action": "auto_resolve"})
        world = next(n["id"] for n in sample_state["tree"]["nodes"] if n["name"] == "World")
        client.tool("ui.action", {"action": "set_view_root", "nodeId": world})
        wait_for(client, "ui.action", {"action": "auto_status"}, {"stale"})
        client.tool("ui.action", {"action": "set_view_root", "nodeId": ball})
        assert not client.tool("ui.action", {"action": "auto_status"})["recording"]
        assert ball_tree() == raw_named_ball
        mark("switching classes invalidates AUTO and releases its original class recording")
        client.tool("ui.action", {"action": "timeline_record"})
        client.tool("ui.action", {"action": "auto_resolve"})
        outcome = wait_for(client, "ui.action", {"action": "auto_status"}, {"applied"})
        assert outcome["recording"] and outcome["evidenceSource"] == "recording", outcome
        client.tool("ui.action", {"action": "timeline_record"})
        resolved_named_ball = ball_tree()
        inferred = [n for n in resolved_named_ball if not n["kind"].startswith("Hex")]
        assert inferred, resolved_named_ball
        assert any(n["offset"] == 0x08 and n["kind"] == "Double" and n["name"] == "time"
                   for n in inferred), inferred
        assert any(n["offset"] == 0x10 and n["kind"] == "Float" and n["name"] == "position"
                   for n in inferred), inferred
        assert any(n["offset"] == 0x88 and n["kind"] == "Pointer64" and n["name"] == "world"
                   for n in inferred), inferred
        for offset, kind, name in ((0x28, "Float", "radius"), (0x2C, "Float", "restitution"),
                                   (0x30, "Mat4x4", "transform")):
            assert any(n["offset"] == offset and n["kind"] == kind and n["name"] == name
                       for n in inferred), inferred
        for node in inferred:
            if node["name"] in original_offsets:
                assert node["offset"] == original_offsets[node["name"]], node
        assert client.rpc("ping") == {}
        client.tool("ui.action", {"action": "undo"})
        assert ball_tree() == raw_named_ball
        mark("size-preserving raw BouncingBall keeps names and gains correct primitive types without a model")
        client.tool("ui.action", {"action": "auto_resolve"})
        reused = wait_for(client, "ui.action", {"action": "auto_status"}, {"applied"})
        assert reused["reusedSamples"] >= 8 and not reused["recording"], reused
        client.tool("ui.action", {"action": "undo"})
        assert ball_tree() == raw_named_ball
        mark("AUTO reuses existing history without restarting capture; manual capture stays user-owned")
        client.tool("ui.action", {"action": "timeline_view", "timeMs": int(reused["latestRecordTimeMs"]) - 250})
        time.sleep(.15)
        client.tool("ui.action", {"action": "auto_resolve"})
        snapshot_result = wait_for(client, "ui.action", {"action": "auto_status"}, {"applied"})
        assert snapshot_result["evidenceSource"] == "snapshot" and snapshot_result["viewingPast"], snapshot_result
        client.tool("ui.action", {"action": "timeline_live"})
        client.tool("ui.action", {"action": "undo"})
        assert ball_tree() == raw_named_ball
        mark("AUTO analyzes the recorded snapshot while remaining in history view")
        # Remove the retained semantic labels too: exercise the whole raw object,
        # including its real world pointer and invalid pointer-like float bytes.
        client.tool("tree.apply", {"operations": [
            {"op": "remove", "nodeId": n["id"]} for n in raw_named_ball] + [
            {"op": "insert", "kind": "Hex64", "parentId": ball, "offset": offset,
             "name": f"field_{offset:04x}"} for offset in range(0, 0x98, 8)]})
        raw_ball = ball_tree()
        client.tool("ui.action", {"action": "auto_resolve"})
        outcome = wait_for(client, "ui.action", {"action": "auto_status"}, {"applied"})
        resolved_ball = ball_tree()
        expected_float_offsets = set(range(0x10, 0x80, 4)) | {0x84, 0x90}
        for node in resolved_ball:
            if node["kind"] == "Float":
                assert node["offset"] in expected_float_offsets, node
            elif not node["kind"].startswith("Hex"):
                assert (node["offset"], node["kind"]) in {
                    (0x08, "Double"), (0x30, "Mat4x4"), (0x88, "Pointer64")}, node
        assert any(n["offset"] == 0x08 and n["kind"] == "Double" for n in resolved_ball), resolved_ball
        assert any(n["offset"] == 0x88 and n["kind"] == "Pointer64" for n in resolved_ball), resolved_ball
        for offset, kind in ((0x28, "Float"), (0x2C, "Float"), (0x30, "Mat4x4")):
            assert any(n["offset"] == offset and n["kind"] == kind for n in resolved_ball), resolved_ball
        assert client.rpc("ping") == {}
        client.tool("ui.action", {"action": "undo"})
        assert ball_tree() == raw_ball
        mark("fully raw live ball recovers constant floats, its affine matrix, and its validated world pointer")

        ball_ai = Client(build / "RcMcpBridge.exe", env, bridge_log, sampling=True)
        clients.append(ball_ai)
        ball_rounds = []
        def analyze_ball(request):
            packet = json.loads(request["params"]["messages"][0]["content"]["text"])
            ball_rounds.append(packet)
            evidence = packet["evidence"]
            assert evidence["automaticCandidates"] and evidence["reviewCandidates"]
            return {"action": "apply", "fields": evidence["suggestedFields"],
                    "reason": "Selected non-overlapping high-confidence representations"}
        ball_ai.sampling = analyze_ball
        ball_ai.tool("ui.action", {"action": "auto_resolve"})
        outcome = wait_for(ball_ai, "ui.action", {"action": "auto_status"}, {"applied"})
        assert outcome["modelRound"] == 1 and len(ball_rounds) == 1
        assert any(not n["kind"].startswith("Hex") for n in ball_tree())
        ball_ai.tool("ui.action", {"action": "undo"})
        ball_ai.close()
        mark("connected sampling-capable MCP model automatically analyzes and applies the validated ball layout")
        raw = run_dir / "samples.bin"
        raw.write_bytes(struct.pack("<fff", 1.25, 2.5, 3.75) + bytes(20))
        client.tool("project.state")
        client.tool("source.switch", {"filePath": str(raw)})
        client.tool("tree.apply", {"operations": [{"op": "insert", "kind": "Struct", "name": "AutoHarness"}]})
        state = client.tool("project.state", {"depth": 0, "limit": 500})
        root = next(n["id"] for n in state["tree"]["nodes"] if n["name"] == "AutoHarness")
        client.tool("tree.apply", {"operations": [
            {"op": "insert", "kind": "Hex64", "parentId": root, "name": f"field_{offset:04x}", "offset": offset}
            for offset in range(0, 32, 8)] + [{"op": "change_base", "baseAddress": "0"}]})
        client.tool("ui.action", {"action": "set_view_root", "nodeId": root})
        def tree():
            return client.tool("project.state", {"parentId": root, "depth": 2})["tree"]["nodes"]
        original = tree()
        job = client.tool("analysis.infer_types", {"action": "start", "samples": 1, "length": 16})
        ready = wait_for(client, "analysis.infer_types", {"action": "status", "jobId": job["jobId"]}, {"ready"})
        candidate = next(c for c in ready["candidates"] if c["offset"] == 0 and c["kind"] == "Float")
        proposal = {"jobId": job["jobId"], "revision": job["revision"], "fields": [{"candidateId": candidate["candidateId"]}]}
        client.tool("tree.apply", {"resolution": dict(proposal, validateOnly=True)})
        validated = wait_for(client, "analysis.infer_types", {"action": "status", "jobId": job["jobId"]}, {"ready"})
        assert validated["application"]["valid"] and tree() == original
        client.tool("tree.apply", {"resolution": dict(proposal, validateOnly=False)})
        wait_for(client, "analysis.infer_types", {"action": "status", "jobId": job["jobId"]}, {"applied"})
        assert any(n["kind"] == "Float" and n["offset"] == 0 for n in tree())
        client.tool("ui.action", {"action": "undo"})
        assert tree() == original
        mark("manual evidence workflow validates, applies, and undoes atomically")

        fresh_job = client.tool("analysis.infer_types", {"action": "start", "samples": 1, "length": 16})
        fresh_ready = wait_for(client, "analysis.infer_types", {"action": "status", "jobId": fresh_job["jobId"]}, {"ready"})
        fresh_candidate = next(c for c in fresh_ready["candidates"] if c["offset"] == 0 and c["kind"] == "Float")
        fresh_proposal = {"jobId": fresh_job["jobId"], "revision": fresh_job["revision"],
                          "fields": [{"candidateId": fresh_candidate["candidateId"]}], "validateOnly": False}
        # The same evidence ID twice is an overlapping proposal, not two fields.
        client.tool("tree.apply", {"resolution": dict(fresh_proposal, fields=fresh_proposal["fields"] * 2)}, error=True)
        assert tree() == original
        mark("overlapping proposal is rejected before mutation")
        # FileProvider intentionally caches backing files; mutate its session
        # bytes through MCP so the active source, rather than the disk, changes.
        client.tool("hex.write", {"offset": 0, "hexBytes": "0000c07f"})
        client.tool("tree.apply", {"resolution": fresh_proposal})
        contradictory = wait_for(client, "analysis.infer_types", {"action": "status", "jobId": fresh_job["jobId"]}, {"stale"})
        assert not contradictory["application"]["valid"] and tree() == original
        client.tool("ui.action", {"action": "undo"})
        mark("fresh contradictory bytes invalidate a previously compatible interpretation")

        stale_job = client.tool("analysis.infer_types", {"action": "start", "samples": 1, "length": 16})
        stale_ready = wait_for(client, "analysis.infer_types", {"action": "status", "jobId": stale_job["jobId"]}, {"ready"})
        first = original[0]["id"]
        client.tool("tree.apply", {"operations": [{"op": "rename", "nodeId": first, "name": "manual_field"}]})
        client.tool("tree.apply", {"resolution": {"jobId": stale_job["jobId"], "revision": stale_job["revision"],
                    "fields": [{"candidateId": stale_ready["candidates"][0]["candidateId"]}], "validateOnly": False}}, error=True)
        client.tool("ui.action", {"action": "undo"})
        assert tree() == original
        mark("tree changes invalidate existing evidence revisions")

        def auto(c=client):
            c.tool("ui.action", {"action": "auto_resolve"})
            return wait_for(c, "ui.action", {"action": "auto_status"},
                            {"abstained", "applied", "rejected", "declined", "disconnected", "cancelled"})
        offline = auto()
        assert offline["state"] == "applied" and offline["evidenceSource"] == "snapshot" and not offline["recording"], offline
        assert {n["offset"] for n in tree() if n["kind"] == "Float"} == {0, 4, 8}
        client.tool("ui.action", {"action": "undo"})
        assert tree() == original
        mark("AUTO resolves contextual floats from a file without a live source and undoes atomically")
        partial_job = client.tool("analysis.infer_types", {"action": "start", "automatic": True})
        partial_ready = wait_for(client, "analysis.infer_types", {"action": "status", "jobId": partial_job["jobId"]}, {"ready"})
        client.tool("hex.write", {"offset": 0, "hexBytes": "0000c07f"})
        client.tool("tree.apply", {"resolution": {"jobId": partial_job["jobId"], "revision": partial_job["revision"],
            "fields": partial_ready["suggestedFields"], "validateOnly": False}})
        partial = wait_for(client, "analysis.infer_types", {"action": "status", "jobId": partial_job["jobId"]}, {"applied"})
        assert partial["application"]["skippedFields"] == ["0:4:Float"], partial
        assert {n["offset"] for n in tree() if n["kind"] == "Float"} == {4, 8}
        client.tool("ui.action", {"action": "undo"})
        assert tree() == original
        client.tool("ui.action", {"action": "undo"})
        mark("AUTO leaves contradicted fields raw and applies the valid remainder as one undoable edit")
        ambiguous_file = run_dir / "ambiguous.bin"
        ambiguous_file.write_bytes(struct.pack("<f", 1.25) + bytes(28))
        client.tool("source.switch", {"filePath": str(ambiguous_file)})
        assert auto()["state"] == "abstained" and tree() == original
        mark("AUTO without a model abstains on ambiguous bytes")
        ai = Client(build / "RcMcpBridge.exe", env, bridge_log, sampling=True)
        clients.append(ai)
        def hallucinate(request):
            packet = json.loads(request["params"]["messages"][0]["content"]["text"])
            evidence = packet["evidence"]
            assert evidence["reviewCandidates"] and not evidence["automaticCandidates"]
            return {"action": "apply", "fields": [{"candidateId": evidence["reviewCandidates"][0]["candidateId"]}],
                    "reason": "attempt to promote a hypothesis"}
        ai.sampling = hallucinate
        assert auto(ai)["state"] == "abstained" and tree() == original
        mark("iterative MCP gate refuses a model-selected hypothesis after bounded correction attempts")
        ai.sampling = lambda request: "not valid JSON"
        assert auto(ai)["state"] == "abstained" and tree() == original
        mark("malformed model output exhausts bounded retries and makes no edits")
        collect_rounds = []
        def collect_then_stop(request):
            packet = json.loads(request["params"]["messages"][0]["content"]["text"])
            collect_rounds.append(packet)
            if len(collect_rounds) == 1:
                return {"action": "collect", "samples": 8, "intervalMs": 50,
                        "reason": "check whether static candidates change"}
            return {"action": "abstain", "reason": "the extra observations remain ambiguous"}
        ai.sampling = collect_then_stop
        outcome = auto(ai)
        assert outcome["state"] == "abstained" and outcome["modelRound"] == 2 and len(collect_rounds) == 2
        assert tree() == original
        assert "fixed snapshot" in collect_rounds[-1]["validationNote"]
        mark("AUTO explains why resampling a fixed file cannot add evidence")
        ai.sampling = lambda request: {"action": "abstain", "reason": "insufficient evidence"}
        assert auto(ai)["state"] == "abstained" and tree() == original
        mark("model abstention preserves original node IDs and layout")
        ai.sampling = lambda request: None
        ai.tool("ui.action", {"action": "auto_resolve"})
        wait_for(ai, "ui.action", {"action": "auto_status"}, {"waiting_model"})
        late = ai.pending[-1]
        stopped = ai.tool("ui.action", {"action": "auto_resolve"})
        assert stopped["state"] == "cancelled"
        ai.respond(late, {"action": "apply", "fields": [{"candidateId": candidate["candidateId"]}]})
        assert ai.rpc("ping") == {} and tree() == original
        mark("STOP cancels; a late model reply cannot apply anything")
        ai.tool("ui.action", {"action": "auto_resolve"})
        wait_for(ai, "ui.action", {"action": "auto_status"}, {"waiting_model"})
        ai.close()
        wait_for(client, "ui.action", {"action": "auto_status"}, {"abstained"})
        assert tree() == original
        mark("model disconnect leaves the project unchanged")

        exe = build_fixture(repo, run_dir / "fixture")
        build_fixture(repo, run_dir / "wrong", wrong=True)
        fixture = subprocess.Popen([str(exe)], stdout=subprocess.PIPE, stderr=bridge_log,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        info = json.loads(fixture.stdout.readline())
        client.tool("source.switch", {"pid": info["pid"], "processName": "auto_fixture.exe"})
        client.tool("tree.apply", {"operations": [{"op": "change_base", "baseAddress": info["address"]}]})
        client.tool("symbols.load", {"pdbPath": str(run_dir / "wrong/auto_fixture.pdb")})
        assert auto()["state"] == "abstained" and tree() == original
        mark("mismatched executable/PDB identity is rejected")
        client.tool("symbols.load", {"pdbPath": str(run_dir / "fixture/auto_fixture.pdb")})
        before_memory = client.tool("hex.read", {"offset": info["address"], "length": 32})
        outcome = auto()
        assert outcome["state"] == "applied", outcome
        fields = {(n["offset"], n["name"], n["kind"]) for n in tree() if not n["kind"].startswith("Hex")}
        expected = {(0, "health", "Int32"), (4, "team", "UInt32"), (8, "position", "Float"),
                    (16, "weight", "Double"), (24, "flag", "UInt8")}
        assert fields == expected, fields
        assert client.tool("hex.read", {"offset": info["address"], "length": 32}) == before_memory
        client.tool("ui.action", {"action": "undo"})
        assert tree() == original
        client.tool("ui.action", {"action": "redo"})
        assert {(n["offset"], n["name"], n["kind"]) for n in tree() if not n["kind"].startswith("Hex")} == expected
        client.tool("ui.action", {"action": "undo"})
        mark("matching PDB recovers all five known fields, preserves memory, and supports one-step undo/redo")
        first = original[0]["id"]
        client.tool("tree.apply", {"operations": [{"op": "rename", "nodeId": first, "name": "manual_field"}]})
        assert auto()["state"] == "applied"
        assert any(n["offset"] == 0 and n["name"] == "manual_field" and n["kind"] == "Int32" for n in tree())
        assert not any(n["name"] == "health" for n in tree())
        client.tool("ui.action", {"action": "undo"})
        client.tool("ui.action", {"action": "undo"})
        assert tree() == original
        mark("verified metadata recovers the type while preserving a manually supplied field name")
        client.tool("tree.apply", {"operations": [{"op": "change_base", "baseAddress": info["pointer"]}]})
        assert auto()["state"] == "applied"
        assert [(n["offset"], n["kind"]) for n in tree() if not n["kind"].startswith("Hex")] == [(0, "Pointer64")]
        client.tool("ui.action", {"action": "undo"})
        assert tree() == original
        mark("pointer global is recovered only as a pointer, never mistaken for its pointee object")
        client.tool("tree.apply", {"operations": [{"op": "change_base", "baseAddress": hex(int(info["address"], 16) + 1)}]})
        assert auto()["state"] == "abstained" and tree() == original
        mark("nearby symbol address is not accepted as an exact object binding")
        (run_dir / "results.json").write_text(json.dumps({"passed": passed, "count": len(passed)}, indent=2), encoding="utf-8")
        print(f"{len(passed)} runtime scenarios passed. Artifacts: {run_dir}", flush=True)
    finally:
        for c in clients:
            c.close()
        for proc in (fixture, app):
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)
        app_log.close()
        bridge_log.close()
        print(f"Logs: {run_dir}", flush=True)


if __name__ == "__main__":
    main()
