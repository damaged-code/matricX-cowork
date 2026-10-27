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
import subprocess, json

proc = subprocess.Popen(["${EXE}", "--idle-timeout", "10"],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

ready = proc.stdout.readline()
assert "ready" in ready, f"Unexpected ready event: {ready}"

for cmd in [{"id": 1, "cmd": "screen_info"},
            {"id": 2, "cmd": "wait", "ms": 20},
            {"id": 3, "cmd": "screenshot"},
            {"id": 4, "cmd": "end_session"}]:
    proc.stdin.write(json.dumps(cmd) + "\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    data = json.loads(line)
    assert data.get("ok") is True, f"Command failed: {line}"

stopped = proc.stdout.readline()
assert "stopped" in stopped, f"Unexpected stopped event: {stopped}"
proc.wait()
print("All smoke tests passed successfully.")
EOF
