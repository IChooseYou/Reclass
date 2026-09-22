import subprocess, json, sys, time

proc = subprocess.Popen(
    ['E:/game_dev/util/reclass2027-main/build/rcx-mcp-stdio.exe'],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE
)

def send(msg):
    data = json.dumps(msg) + '\n'
    proc.stdin.write(data.encode())
    proc.stdin.flush()

def recv():
    line = proc.stdout.readline()
    if line:
        return json.loads(line.decode() if isinstance(line, bytes) else line)
    return None

call_id = 0

def call_tool(name, args):
    global call_id
    call_id += 1
    send({
        'jsonrpc': '2.0',
        'id': call_id,
        'method': 'tools/call',
        'params': {
            'name': name,
            'arguments': args
        }
    })
    r = recv()
    return r

# Handshake
send({'jsonrpc':'2.0','id':0,'method':'initialize','params':{'protocolVersion':'2024-11-05','capabilities':{},'clientInfo':{'name':'claude','version':'1.0'}}})
time.sleep(0.3)
init_resp = recv()
print("=== MCP Server initialized ===")
print(json.dumps(init_resp, indent=2))

send({'jsonrpc':'2.0','method':'notifications/initialized'})
time.sleep(0.2)

# ============ STEP 1: source.switch ============
print("\n" + "="*60)
print("STEP 1: source.switch")
print("="*60)
t1_start = time.time()
r1 = call_tool('source.switch', {'pid': 47912, 'processName': 'notepad.exe'})
t1_end = time.time()
print(f"Time: {t1_end - t1_start:.3f}s")
print(json.dumps(r1, indent=2))

# ============ STEP 2: project.state depth=2 ============
print("\n" + "="*60)
print("STEP 2: project.state (depth=2)")
print("="*60)
t2_start = time.time()
r2 = call_tool('project.state', {'depth': 2})
t2_end = time.time()
print(f"Time: {t2_end - t2_start:.3f}s")
print(json.dumps(r2, indent=2))

# Parse the result to find all node IDs
# The result content is in r2['result']['content'][0]['text'] as JSON
state_text = r2['result']['content'][0]['text']
state = json.loads(state_text)

# Collect all node IDs except the first struct's own ID
all_node_ids = []
structs = state.get('tree', [])
first_struct_id = structs[0]['id'] if structs else None

# For each struct, collect children
for i, s in enumerate(structs):
    children = s.get('children', [])
    for child in children:
        all_node_ids.append(child['id'])
    # Secondary structs (not the first one) should also be removed
    if i > 0:
        all_node_ids.append(s['id'])

print(f"\nFirst struct ID: {first_struct_id}")
print(f"Node IDs to remove: {all_node_ids}")

# ============ STEP 3: tree.apply (atomic batch) ============
print("\n" + "="*60)
print("STEP 3: tree.apply (Build IMAGE_DOS_HEADER)")
print("="*60)

operations = []

# Remove children first, then secondary structs
# Children of all structs
for s in structs:
    children = s.get('children', [])
    for child in reversed(children):
        operations.append({'op': 'remove', 'nodeId': child['id']})

# Secondary structs (in reverse order)
for s in reversed(structs[1:]):
    operations.append({'op': 'remove', 'nodeId': s['id']})

# Change first struct type
operations.append({'op': 'change_struct_type', 'nodeId': '1', 'structTypeName': 'IMAGE_DOS_HEADER'})
operations.append({'op': 'change_class_keyword', 'nodeId': '1', 'classKeyword': 'struct'})
operations.append({'op': 'rename', 'nodeId': '1', 'name': ''})

# Insert fields
fields = [
    {'kind': 'UInt16', 'name': 'e_magic', 'offset': 0},
    {'kind': 'UInt16', 'name': 'e_cblp', 'offset': 2},
    {'kind': 'UInt16', 'name': 'e_cp', 'offset': 4},
    {'kind': 'UInt16', 'name': 'e_crlc', 'offset': 6},
    {'kind': 'UInt16', 'name': 'e_cparhdr', 'offset': 8},
    {'kind': 'UInt16', 'name': 'e_minalloc', 'offset': 10},
    {'kind': 'UInt16', 'name': 'e_maxalloc', 'offset': 12},
    {'kind': 'UInt16', 'name': 'e_ss', 'offset': 14},
    {'kind': 'UInt16', 'name': 'e_sp', 'offset': 16},
    {'kind': 'UInt16', 'name': 'e_csum', 'offset': 18},
    {'kind': 'UInt16', 'name': 'e_ip', 'offset': 20},
    {'kind': 'UInt16', 'name': 'e_cs', 'offset': 22},
    {'kind': 'UInt16', 'name': 'e_lfarlc', 'offset': 24},
    {'kind': 'UInt16', 'name': 'e_ovno', 'offset': 26},
    {'kind': 'Array', 'name': 'e_res', 'offset': 28},
    {'kind': 'UInt16', 'name': 'e_oemid', 'offset': 36},
    {'kind': 'UInt16', 'name': 'e_oeminfo', 'offset': 38},
    {'kind': 'Array', 'name': 'e_res2', 'offset': 40},
    {'kind': 'UInt32', 'name': 'e_lfanew', 'offset': 60},
]

for f in fields:
    operations.append({
        'op': 'insert',
        'parentId': '1',
        'kind': f['kind'],
        'name': f['name'],
        'offset': f['offset']
    })

# change_array_meta for e_res (insert index 14, 0-based among inserts) and e_res2 (insert index 17)
operations.append({'op': 'change_array_meta', 'nodeId': '$14', 'elementKind': 'UInt16', 'arrayLen': 4})
operations.append({'op': 'change_array_meta', 'nodeId': '$17', 'elementKind': 'UInt16', 'arrayLen': 10})

print(f"Total operations: {len(operations)}")
for i, op in enumerate(operations):
    print(f"  [{i}] {op}")

t3_start = time.time()
r3 = call_tool('tree.apply', {'operations': operations, 'macroName': 'Build IMAGE_DOS_HEADER'})
t3_end = time.time()
print(f"\nTime: {t3_end - t3_start:.3f}s")
print(json.dumps(r3, indent=2))

# ============ STEP 4: project.state depth=2 (verify) ============
print("\n" + "="*60)
print("STEP 4: project.state (depth=2) - VERIFY")
print("="*60)
t4_start = time.time()
r4 = call_tool('project.state', {'depth': 2})
t4_end = time.time()
print(f"Time: {t4_end - t4_start:.3f}s")
print(json.dumps(r4, indent=2))

# Summary
print("\n" + "="*60)
print("TIMING SUMMARY")
print("="*60)
print(f"Step 1 (source.switch):     {t1_end - t1_start:.3f}s")
print(f"Step 2 (project.state):     {t2_end - t2_start:.3f}s")
print(f"Step 3 (tree.apply):        {t3_end - t3_start:.3f}s")
print(f"Step 4 (project.state):     {t4_end - t4_start:.3f}s")
print(f"Total:                      {(t1_end-t1_start)+(t2_end-t2_start)+(t3_end-t3_start)+(t4_end-t4_start):.3f}s")

proc.terminate()
