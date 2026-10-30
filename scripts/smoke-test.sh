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
"${EXE}" --describe --enable-shell > /dev/null

echo "Testing JSON command protocol (GUI + filesystem tools)..."
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT
python3 - "${EXE}" "${WORK_DIR}" << EOF
import subprocess, json, os, sys

exe, work = sys.argv[1], sys.argv[2]

# The engine always reports all 17 tool schemas from --describe; hiding
# shell_exec before opt-in is the UI's job (CoworkTool filters it out).
for extra in ([], ["--enable-shell"]):
    out = subprocess.run([exe, "--describe"] + extra, capture_output=True, text=True).stdout
    names = [t["name"] for t in json.loads(out)]
    assert len(names) == 17, f"expected 17 tools, got {len(names)}"
    assert "shell_exec" in names, "shell_exec schema missing from --describe"

proc = subprocess.Popen([exe, "--idle-timeout", "10", "--enable-shell", "--allow-root", work],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

ready = proc.stdout.readline()
assert "ready" in ready, f"Unexpected ready event: {ready}"

def call(cmd):
    proc.stdin.write(json.dumps(cmd) + "\n")
    proc.stdin.flush()
    return json.loads(proc.stdout.readline())

for cmd in [{"id": 1, "cmd": "screen_info"},
            {"id": 2, "cmd": "wait", "ms": 20},
            {"id": 3, "cmd": "screenshot"}]:
    data = call(cmd)
    assert data.get("ok") is True, f"Command failed: {data}"

# Filesystem sandbox
assert call({"id": 10, "cmd": "fs_mkdir", "path": os.path.join(work, "sub")})["ok"] is True
assert call({"id": 11, "cmd": "fs_write", "path": os.path.join(work, "sub", "a.txt"),
             "text": "hello smoke\n"})["ok"] is True
r = call({"id": 12, "cmd": "fs_read", "path": os.path.join(work, "sub", "a.txt")})
assert r["ok"] and r["content"].strip() == "hello smoke", r
assert call({"id": 13, "cmd": "fs_read", "path": "/etc/shadow"})["ok"] is False, "sandbox escape!"
assert call({"id": 14, "cmd": "fs_search", "path": work, "pattern": "a*.txt"})["ok"] is True
assert call({"id": 15, "cmd": "fs_list", "path": os.path.join(work, "sub")})["ok"] is True
assert call({"id": 16, "cmd": "fs_delete", "path": os.path.join(work, "sub")})["ok"] is False
assert call({"id": 17, "cmd": "fs_delete", "path": os.path.join(work, "sub"), "recursive": True})["ok"] is True

# shell_exec (enabled above): exit codes, sandboxed cwd
if os.path.exists("/bin/sh"):
    r = call({"id": 20, "cmd": "shell_exec", "argv": ["/bin/sh", "-c", "exit 7"], "cwd": work})
    assert r["ok"] and r.get("exit_code") == 7, r
else:
    print("sh not present; skipping shell_exec exit-code check")

assert call({"id": 30, "cmd": "end_session"})["ok"] is True

stopped = proc.stdout.readline()
assert "stopped" in stopped, f"Unexpected stopped event: {stopped}"
proc.wait()
print("All smoke tests passed successfully.")
EOF