"""Exhaustive multi-client MCP stress test with benchmarking."""
import subprocess, json, threading, time, sys

PASS = 0
FAIL = 0

def check(label, condition, detail=""):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  [PASS] {label}")
    else:
        FAIL += 1
        print(f"  [FAIL] {label} -- {detail}")

def mcp_session(commands):
    """Send MCP commands via PowerShell named pipe, return list of parsed responses."""
    ps_lines = [
        "$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'RCMcpBridge', 'InOut')",
        "$pipe.Connect(5000)",
        "$writer = New-Object System.IO.StreamWriter($pipe)",
        "$reader = New-Object System.IO.StreamReader($pipe)",
        "$writer.AutoFlush = $true",
    ]
    for cmd in commands:
        escaped = json.dumps(cmd).replace('"', '`"').replace('$', '`$')
        ps_lines.append(f'$writer.WriteLine("{escaped}")')
        ps_lines.append(f'Write-Host $reader.ReadLine()')
    ps_lines.append("$pipe.Close()")
    script = "\n".join(ps_lines)
    r = subprocess.run(["powershell", "-Command", script],
                       capture_output=True, text=True, timeout=20)
    results = []
    for line in r.stdout.strip().split("\n"):
        line = line.strip()
        if not line:
            continue
        try:
            results.append(json.loads(line))
        except:
            results.append({"raw": line})
    return results

def init_cmd(client_id, req_id=1):
    return {"jsonrpc":"2.0","id":req_id,"method":"initialize",
            "params":{"protocolVersion":"2024-11-05","capabilities":{},
                      "clientInfo":{"name":f"client_{client_id}","version":"1.0"}}}

def tool_cmd(name, args, req_id=2):
    return {"jsonrpc":"2.0","id":req_id,"method":"tools/call",
            "params":{"name":name,"arguments":args}}

def tools_list_cmd(req_id=2):
    return {"jsonrpc":"2.0","id":req_id,"method":"tools/list","params":{}}

# ═══════════════════════════════════════════════════
# TEST 1: Basic single client — all tools respond
# ═══════════════════════════════════════════════════
def test_single_client():
    print("\n=== TEST 1: Single client — all tools ===")
    cmds = [
        init_cmd("solo"),
        tools_list_cmd(2),
        tool_cmd("project.state", {"depth":1,"limit":3}, 3),
        tool_cmd("source.modules", {}, 4),
        tool_cmd("hex.read", {"offset":0,"length":16}, 5),
        tool_cmd("tree.search", {"query":"TickCount","limit":5}, 6),
        tool_cmd("node.history", {"nodeIds":["47","48"]}, 7),
        tool_cmd("process.info", {}, 8),
        tool_cmd("status.set", {"text":"stress test","target":"statusBar"}, 9),
    ]
    t0 = time.time()
    results = mcp_session(cmds)
    elapsed = time.time() - t0

    check("Got 9 responses", len(results) == 9, f"got {len(results)}")
    check("Initialize succeeded", "result" in results[0] if results else False)
    check("tools/list has tools", "result" in results[1] and "tools" in results[1].get("result",{}))
    if len(results) > 1 and "result" in results[1]:
        tool_names = [t["name"] for t in results[1]["result"]["tools"]]
        check("Has source.modules", "source.modules" in tool_names)
        check("Has scanner.scan", "scanner.scan" in tool_names)
        check("Has scanner.scan_pattern", "scanner.scan_pattern" in tool_names)
        check("Has mcp.reconnect", "mcp.reconnect" in tool_names)
    check("project.state has tree", "result" in results[2] if len(results)>2 else False)
    check("source.modules returns array", "result" in results[3] if len(results)>3 else False)
    check("hex.read returns data", "result" in results[4] if len(results)>4 else False)
    check("tree.search returns results", "result" in results[5] if len(results)>5 else False)
    check("node.history returns data", "result" in results[6] if len(results)>6 else False)
    check("process.info has PEB", "result" in results[7] if len(results)>7 else False)
    check("status.set accepted", "result" in results[8] if len(results)>8 else False)
    print(f"  Elapsed: {elapsed:.2f}s for 9 commands ({elapsed/9*1000:.0f}ms avg)")

# ═══════════════════════════════════════════════════
# TEST 2: Multi-client concurrent — 3 clients at once
# ═══════════════════════════════════════════════════
def test_multi_client():
    print("\n=== TEST 2: Multi-client concurrent (3 clients) ===")
    results = {}

    def run_client(name, cmds):
        try:
            results[name] = mcp_session(cmds)
        except Exception as e:
            results[name] = [{"error": str(e)}]

    cmds_a = [init_cmd("A"), tool_cmd("project.state",{"depth":1,"limit":5},2),
              tool_cmd("source.modules",{},3)]
    cmds_b = [init_cmd("B"), tool_cmd("hex.read",{"offset":0,"length":8},2),
              tool_cmd("tree.search",{"query":"System"},3)]
    cmds_c = [init_cmd("C"), tool_cmd("process.info",{},2),
              tool_cmd("node.history",{"nodeIds":["52"]},3)]

    threads = [
        threading.Thread(target=run_client, args=("A", cmds_a)),
        threading.Thread(target=run_client, args=("B", cmds_b)),
        threading.Thread(target=run_client, args=("C", cmds_c)),
    ]
    t0 = time.time()
    for i, t in enumerate(threads):
        t.start()
        if i < len(threads) - 1:
            time.sleep(0.15)  # slight stagger to avoid connection race
    for t in threads: t.join(timeout=20)
    elapsed = time.time() - t0

    for name in ["A","B","C"]:
        r = results.get(name, [])
        check(f"Client {name}: got 3 responses", len(r) == 3, f"got {len(r)}")
        check(f"Client {name}: init OK", len(r)>0 and "result" in r[0],
              f"got {r[0] if r else 'nothing'}")
        check(f"Client {name}: tool 1 OK", len(r)>1 and "result" in r[1])
        check(f"Client {name}: tool 2 OK", len(r)>2 and "result" in r[2])

    print(f"  All 3 clients completed in {elapsed:.2f}s")

# ═══════════════════════════════════════════════════
# TEST 3: Error handling — bad method, missing params
# ═══════════════════════════════════════════════════
def test_error_handling():
    print("\n=== TEST 3: Error handling ===")
    cmds = [
        init_cmd("errors"),
        {"jsonrpc":"2.0","id":2,"method":"nonexistent/method","params":{}},
        tool_cmd("scanner.scan", {}, 3),  # missing required valueType, value
        tool_cmd("hex.read", {"offset":0}, 4),  # missing length
        tool_cmd("tree.apply", {"operations":[{"op":"remove","nodeId":"99999999"}]}, 5),
    ]
    results = mcp_session(cmds)
    check("Init OK", len(results)>0 and "result" in results[0])
    check("Unknown method returns error", len(results)>1 and "error" in results[1],
          f"got {results[1] if len(results)>1 else 'nothing'}")
    check("Missing scan params handled", len(results)>2 and "result" in results[2])
    # hex.read with missing length should still return something (error or default)
    check("Missing hex.read length handled", len(results)>3)
    check("Remove nonexistent node handled", len(results)>4 and "result" in results[4])

# ═══════════════════════════════════════════════════
# TEST 4: Rapid fire — 20 sequential commands, benchmark
# ═══════════════════════════════════════════════════
def test_rapid_fire():
    print("\n=== TEST 4: Rapid fire (20 sequential commands) ===")
    cmds = [init_cmd("rapid")]
    for i in range(20):
        cmds.append(tool_cmd("hex.read", {"offset": i*16, "length": 16}, i+2))

    t0 = time.time()
    results = mcp_session(cmds)
    elapsed = time.time() - t0

    check("Got 21 responses (1 init + 20 reads)", len(results) == 21, f"got {len(results)}")
    errors = sum(1 for r in results[1:] if "error" in r)
    check("No errors in rapid fire", errors == 0, f"{errors} errors")
    print(f"  Elapsed: {elapsed:.2f}s for 20 hex.reads ({elapsed/20*1000:.0f}ms avg)")

# ═══════════════════════════════════════════════════
# TEST 5: Scanner — value + pattern scans with regions
# ═══════════════════════════════════════════════════
def test_scanner():
    print("\n=== TEST 5: Scanner scans ===")
    cmds = [
        init_cmd("scanner"),
        # Value scan — search for uint32 in constrained range
        tool_cmd("scanner.scan", {
            "valueType": "uint32", "value": "0",
            "regions": [["0x7FFE0000","0x7FFE0100"]]
        }, 2),
        # Float scan — writable only
        tool_cmd("scanner.scan", {
            "valueType": "float", "value": "0",
            "filterWritable": True,
            "regions": [["0x7FFE0000","0x7FFE0040"]]
        }, 3),
        # Pattern scan with wildcards
        tool_cmd("scanner.scan_pattern", {
            "pattern": "?? ?? ?? ?? 00 00 00 0F",
            "regions": [["0x7FFE0000","0x7FFE0100"]]
        }, 4),
        # Pattern scan — no match expected
        tool_cmd("scanner.scan_pattern", {
            "pattern": "DE AD BE EF DE AD BE EF DE AD",
            "regions": [["0x7FFE0000","0x7FFE0100"]]
        }, 5),
    ]
    results = mcp_session(cmds)
    check("Init OK", len(results)>0 and "result" in results[0])

    for i, label in enumerate(["uint32 scan","float scan","pattern wildcard","pattern no-match"], start=1):
        if len(results) > i:
            r = results[i]
            check(f"{label}: no error", "result" in r, f"got error: {r.get('error','?')}")
            if "result" in r:
                text = r["result"]["content"][0]["text"]
                check(f"{label}: has result count", "result(s)" in text, text[:80])

# ═══════════════════════════════════════════════════
# TEST 6: tree.apply batch with placeholder chaining
# ═══════════════════════════════════════════════════
def test_tree_apply_placeholders():
    print("\n=== TEST 6: tree.apply placeholder chaining ===")
    cmds = [
        init_cmd("apply"),
        # Insert struct, insert child, rename child, change kind — all with $placeholders
        tool_cmd("tree.apply", {
            "macroName": "stress_test_batch",
            "operations": [
                {"op":"insert","kind":"Hex64","name":"stress_a","parentId":"676","offset":100},
                {"op":"insert","kind":"Hex32","name":"stress_b","parentId":"676","offset":104},
                {"op":"rename","nodeId":"$0","name":"renamed_a"},
                {"op":"change_kind","nodeId":"$0","kind":"UInt64"},
                {"op":"rename","nodeId":"$1","name":"renamed_b"},
                {"op":"change_kind","nodeId":"$1","kind":"Float"},
            ]
        }, 2),
        # Verify both exist with correct names/kinds
        tool_cmd("tree.search", {"query":"renamed_"}, 3),
        # Undo the batch
        tool_cmd("ui.action", {"action":"undo"}, 4, ),
        # Verify they're gone
        tool_cmd("tree.search", {"query":"renamed_"}, 5),
    ]
    # Fix: ui.action isn't a tools/call — it's a tool
    results = mcp_session(cmds)
    check("Init OK", len(results)>0 and "result" in results[0])
    if len(results) > 1:
        r = results[1]
        check("Batch applied (6 ops)", "result" in r)
        if "result" in r:
            text = r["result"]["content"][0]["text"]
            check("All 6 ops succeeded", "6 operations" in text, text)
    if len(results) > 2:
        r = results[2]
        if "result" in r:
            data = json.loads(r["result"]["content"][0]["text"])
            check("Found 2 renamed nodes", data.get("count") == 2, f"count={data.get('count')}")
            if data.get("count") == 2:
                kinds = {n["name"]: n["kind"] for n in data["results"]}
                check("renamed_a is UInt64", kinds.get("renamed_a") == "UInt64", f"got {kinds}")
                check("renamed_b is Float", kinds.get("renamed_b") == "Float", f"got {kinds}")
    if len(results) > 3:
        check("Undo succeeded", "result" in results[3])
    if len(results) > 4:
        r = results[4]
        if "result" in r:
            data = json.loads(r["result"]["content"][0]["text"])
            check("After undo: 0 renamed nodes", data.get("count") == 0, f"count={data.get('count')}")

# ═══════════════════════════════════════════════════
# TEST 7: Concurrent scanners (stress the request queue)
# ═══════════════════════════════════════════════════
def test_concurrent_scanners():
    print("\n=== TEST 7: Concurrent scanners (2 clients scanning) ===")
    results = {}

    def run_scanner(name, pattern):
        cmds = [
            init_cmd(name),
            tool_cmd("scanner.scan_pattern", {
                "pattern": pattern,
                "regions": [["0x7FFE0000","0x7FFE1000"]]
            }, 2),
        ]
        try:
            results[name] = mcp_session(cmds)
        except Exception as e:
            results[name] = [{"error": str(e)}]

    t0 = time.time()
    ta = threading.Thread(target=run_scanner, args=("scan_A", "00 00 00 00"))
    tb = threading.Thread(target=run_scanner, args=("scan_B", "?? ?? ?? 0F"))
    ta.start()
    tb.start()
    ta.join(timeout=20)
    tb.join(timeout=20)
    elapsed = time.time() - t0

    for name in ["scan_A","scan_B"]:
        r = results.get(name, [])
        check(f"{name}: got 2 responses", len(r) == 2, f"got {len(r)}")
        check(f"{name}: init OK", len(r)>0 and "result" in r[0])
        check(f"{name}: scan completed", len(r)>1 and "result" in r[1])
    print(f"  Both scanners completed in {elapsed:.2f}s")

# ═══════════════════════════════════════════════════
# TEST 8: Reconnect — client disconnects and reconnects
# ═══════════════════════════════════════════════════
def test_reconnect():
    print("\n=== TEST 8: Reconnect ===")
    # First session: init + reconnect
    cmds1 = [
        init_cmd("recon"),
        tool_cmd("mcp.reconnect", {}, 2),
    ]
    results1 = mcp_session(cmds1)
    check("Init OK", len(results1)>0 and "result" in results1[0])
    check("Reconnect response received", len(results1)>1 and "result" in results1[1])

    time.sleep(0.5)  # let server process disconnect

    # Second session: should be able to connect fresh
    cmds2 = [
        init_cmd("recon2"),
        tool_cmd("project.state", {"depth":1,"limit":2}, 2),
    ]
    results2 = mcp_session(cmds2)
    check("Re-init OK after reconnect", len(results2)>0 and "result" in results2[0])
    check("Tools work after reconnect", len(results2)>1 and "result" in results2[1])


def main():
    print("=" * 60)
    print("  MCP EXHAUSTIVE STRESS TEST")
    print("=" * 60)

    test_single_client()
    test_multi_client()
    test_error_handling()
    test_rapid_fire()
    test_scanner()
    test_tree_apply_placeholders()
    test_concurrent_scanners()
    test_reconnect()

    print("\n" + "=" * 60)
    print(f"  RESULTS: {PASS} passed, {FAIL} failed")
    print("=" * 60)
    sys.exit(1 if FAIL > 0 else 0)


if __name__ == "__main__":
    main()
