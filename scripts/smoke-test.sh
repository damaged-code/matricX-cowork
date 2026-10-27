#!/usr/bin/env bash
# Smoke test for matricX-cowork command protocol on Linux
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="${ROOT_DIR}/tools/matricX-cowork/matricX-cowork"

if [[ ! -f "${EXE}" ]]; then
    echo "matricX-cowork binary not found. Run scripts/build.sh first."
    exit 1
fi

echo "Testing matricX-cowork --describe..."
"${EXE}" --describe > /dev/null

echo "Validating tool schemas..."
python3 - << EOF
import json, subprocess
tools = json.loads(subprocess.check_output(["${EXE}", "--describe"], text=True))
assert isinstance(tools, list) and tools, "tool schema list is empty"
names = [tool.get("name") for tool in tools]
assert all(isinstance(name, str) and name for name in names), "tool name missing"
assert len(names) == len(set(names)), "duplicate tool names found"
for tool in tools:
    assert isinstance(tool.get("description"), str) and tool["description"], tool.get("name")
    schema = tool.get("input_schema")
    assert isinstance(schema, dict) and schema.get("type") == "object", tool.get("name")
    properties = schema.get("properties", {})
    assert isinstance(properties, dict), tool.get("name")
    required = schema.get("required", [])
    assert isinstance(required, list), tool.get("name")
    assert all(key in properties for key in required), tool.get("name")
expected = {"fs_list", "fs_read", "fs_write", "fs_mkdir", "fs_delete"}
assert expected.issubset(set(names)), f"missing filesystem tools: {expected - set(names)}"
print(f"Validated {len(tools)} unique tool schemas.")
EOF

echo "Testing JSON command protocol..."
python3 - << EOF
import subprocess, json, tempfile, os

proc = subprocess.Popen(["${EXE}", "--idle-timeout", "10"],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

ready = proc.stdout.readline()
assert "ready" in ready, f"Unexpected ready event: {ready}"

base = tempfile.mkdtemp(prefix="matricx-cowork-smoke-")
subdir = os.path.join(base, "created")
filepath = os.path.join(subdir, "sample.txt")
commands = [
    {"id": 1, "cmd": "screen_info"},
    {"id": 2, "cmd": "wait", "ms": 20},
    {"id": 3, "cmd": "fs_list", "path": base},
    {"id": 4, "cmd": "fs_mkdir", "path": subdir},
    {"id": 5, "cmd": "fs_write", "path": filepath, "content": "filesystem smoke test"},
    {"id": 6, "cmd": "fs_read", "path": filepath},
    {"id": 7, "cmd": "fs_delete", "path": filepath},
    {"id": 8, "cmd": "screenshot"},
    {"id": 9, "cmd": "end_session"},
]
for cmd in commands:
    proc.stdin.write(json.dumps(cmd) + "\\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    data = json.loads(line)
    assert data.get("ok") is True, f"Command failed: {line}"
    if cmd["cmd"] == "fs_read":
        assert data.get("content") == "filesystem smoke test", f"Unexpected file content: {data}"
    if cmd["cmd"] == "fs_list":
        assert isinstance(data.get("entries"), list), f"Missing directory entries: {data}"

stopped = proc.stdout.readline()
assert "stopped" in stopped, f"Unexpected stopped event: {stopped}"
proc.wait(timeout=5)
os.rmdir(subdir)
os.rmdir(base)
print("JSON-lines and filesystem smoke tests passed successfully.")
EOF
