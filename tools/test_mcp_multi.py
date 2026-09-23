"""Test MCP multi-client support against a running Reclass instance."""
import subprocess, json, threading, time, sys

def mcp_session(client_name, commands, results):
    """Run a series of MCP commands on a named pipe and collect results."""
    ps_lines = [
        "$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'RCMcpBridge', 'InOut')",
        "$pipe.Connect(5000)",
        "$writer = New-Object System.IO.StreamWriter($pipe)",
        "$reader = New-Object System.IO.StreamReader($pipe)",
        "$writer.AutoFlush = $true",
    ]
    for label, cmd in commands:
        escaped = json.dumps(cmd).replace('"', '`"')
        ps_lines.append(f'$writer.WriteLine("{escaped}")')
        ps_lines.append(f'Write-Host "{label}:" $reader.ReadLine()')

    ps_lines.append("$pipe.Close()")
    script = "\n".join(ps_lines)

    try:
        r = subprocess.run(
            ["powershell", "-Command", script],
            capture_output=True, text=True, timeout=15
        )
        results[client_name] = r.stdout.strip()
        if r.stderr.strip():
            results[client_name + "_err"] = r.stderr.strip()
    except Exception as e:
        results[client_name] = f"ERROR: {e}"


def main():
    results = {}

    # --- Client A: initialize, list tools, get modules ---
    cmds_a = [
        ("INIT", {"jsonrpc":"2.0","id":1,"method":"initialize",
                   "params":{"protocolVersion":"2024-11-05","capabilities":{},
                             "clientInfo":{"name":"clientA","version":"1.0"}}}),
        ("TOOLS", {"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}),
        ("MODULES", {"jsonrpc":"2.0","id":3,"method":"tools/call",
                      "params":{"name":"source.modules","arguments":{}}}),
    ]

    # --- Client B: initialize, get project state ---
    cmds_b = [
        ("INIT", {"jsonrpc":"2.0","id":1,"method":"initialize",
                   "params":{"protocolVersion":"2024-11-05","capabilities":{},
                             "clientInfo":{"name":"clientB","version":"1.0"}}}),
        ("STATE", {"jsonrpc":"2.0","id":2,"method":"tools/call",
                    "params":{"name":"project.state","arguments":{}}}),
    ]

    # --- Client C: initialize, call reconnect ---
    cmds_c = [
        ("INIT", {"jsonrpc":"2.0","id":1,"method":"initialize",
                   "params":{"protocolVersion":"2024-11-05","capabilities":{},
                             "clientInfo":{"name":"clientC","version":"1.0"}}}),
        ("RECONNECT", {"jsonrpc":"2.0","id":2,"method":"tools/call",
                        "params":{"name":"mcp.reconnect","arguments":{}}}),
    ]

    # Run A and B concurrently to test multi-client
    ta = threading.Thread(target=mcp_session, args=("ClientA", cmds_a, results))
    tb = threading.Thread(target=mcp_session, args=("ClientB", cmds_b, results))

    print("=== Starting Client A and Client B concurrently ===")
    ta.start()
    time.sleep(0.3)  # slight stagger so both connect
    tb.start()

    ta.join(timeout=20)
    tb.join(timeout=20)

    for key in sorted(results.keys()):
        print(f"\n--- {key} ---")
        # Pretty-print JSON responses
        for line in results[key].split("\n"):
            colon = line.find(":")
            if colon > 0:
                label = line[:colon].strip()
                body = line[colon+1:].strip()
                try:
                    parsed = json.loads(body)
                    # Truncate tools list for readability
                    if "result" in parsed and "tools" in parsed.get("result", {}):
                        tools = parsed["result"]["tools"]
                        names = [t["name"] for t in tools]
                        print(f"  {label}: {len(tools)} tools: {', '.join(names)}")
                    else:
                        compact = json.dumps(parsed, indent=None)
                        if len(compact) > 200:
                            compact = compact[:200] + "..."
                        print(f"  {label}: {compact}")
                except:
                    if len(body) > 200:
                        body = body[:200] + "..."
                    print(f"  {label}: {body}")
            else:
                print(f"  {line}")

    # Now test Client C (reconnect) separately
    print("\n=== Starting Client C (reconnect test) ===")
    results_c = {}
    mcp_session("ClientC", cmds_c, results_c)
    for key in sorted(results_c.keys()):
        print(f"\n--- {key} ---")
        for line in results_c[key].split("\n"):
            colon = line.find(":")
            if colon > 0:
                label = line[:colon].strip()
                body = line[colon+1:].strip()
                try:
                    parsed = json.loads(body)
                    compact = json.dumps(parsed, indent=None)
                    if len(compact) > 200:
                        compact = compact[:200] + "..."
                    print(f"  {label}: {compact}")
                except:
                    print(f"  {label}: {body}")
            else:
                print(f"  {line}")

    print("\n=== All tests complete ===")


if __name__ == "__main__":
    main()
