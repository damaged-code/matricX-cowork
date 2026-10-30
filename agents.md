# AGENTS.md — M-Cowork

Practical guide for anyone (human or AI) working on this repository: what exists, how it
fits together, and what is still missing.

---

## 1. What M-Cowork is

A cross-platform desktop AI agent that chats with an LLM and lets that model **control the
computer** — move/click the mouse, type, press shortcuts, take screenshots, read and write
files, run allowed shell commands, and see the result. The UI imitates OpenAI Codex /
ChatGPT Desktop: frameless rounded window, collapsible sidebar, centered chat column,
floating input capsule.

- UI: Qt 6 Widgets, C++17 (`m_cowork_ui`)
- Engine: standalone native process (`matricX-cowork`, v0.5.0)
- Platforms: Windows 10/11, Linux (X11; Wayland with `grim`/`import` fallback)
- Tags: C++17, Qt6, CMake, JSON-lines IPC, Win32, X11

---

## 2. Architecture at a glance

```
┌──────────────────────────────┐        ┌─────────────────────────────┐
│  m_cowork_ui  (Qt process)   │        │  matricX-cowork  (engine)   │
│                              │        │                             │
│  ChatArea ── AiClient ───────┼──HTTP──▶ Gemini / OpenAI-compatible  │
│     │                        │        │ Anthropic / Groq / Ollama   │
│     │                        │        └─────────────────────────────┘
│     └── CoworkTool ──────────┼──JSONL──▶ mouse/keyboard/screenshot  │
│         (QProcess, stdin/stdout)       │ (Win32 or X11)              │
└──────────────────────────────┘        └─────────────────────────────┘
```

Two executables are built by one CMake project:

| Target | Source | Output |
|---|---|---|
| `m_cowork_ui` | `src/main.cpp` + all `src/` | `build/m_cowork_ui(.exe)` |
| `matricX-cowork` | `tools/matricX-cowork/matricX-cowork.cpp` (Win) / `...-linux.cpp` (Linux) | `tools/matricX-cowork/matricX-cowork(.exe)` |

The engine is found at runtime via `assets::toolPath("matricX-cowork/...")`
(`src/tools/CoworkTool.cpp`). If it is missing, `CoworkTool::available()` is false and every
tool call fails with a clear error.

---

## 3. Repository map

```
src/
  main.cpp                 QApplication, window icon/font, shows MainWindow
  ai/
    AiClient.{h,cpp}       Provider HTTP: streamed chat, tool calls, abort,
                           retry/backoff, model list, Whisper
    AntigravityAuth.{h,cpp} Google OAuth loopback flow (Antigravity/Gemini)
  core/
    Assets.{h,cpp}         Resource lookup (assets/ both on disk and packaged)
    AudioRecorder.{h,cpp}  Mic capture -> temp audio file
    ConversationCompactor.*  Per-request context budget / summary / merge
    Icons.{h,cpp}          Lucide-style icon painting to QPixmap/QIcon
    ProviderKeyManager.*   Multiple API keys per provider (metadata-only JSON)
    SecretStore.{h,cpp}    DPAPI / libsecret / obfuscated-file secret storage
    SessionManager.*       Sessions, messages, archive/restore/delete, sessions.json
    SettingsManager.*      QSettings-backed prefs + provider registry
    Theme.h                Geometry + Codex dark palette constants
    ThemeManager.*         Dark / Light / System, live switch
  tools/
    CoworkTool.{h,cpp}     QProcess wrapper around the engine (JSON-lines)
  ui/
    MainWindow.{h,cpp}     Frameless window, native hit-testing, drag/resize/maximize
    Sidebar.{h,cpp}        Session list, search, archive menu, settings button
    ChatArea.{h,cpp}       Agent loop, message rendering, input capsule, thinking bubble
    SettingsDialog.{h,cpp} Appearance / Providers / Archived tabs
  widgets/
    ActionStepCard.*       Collapsible tool-call card with status + screenshot preview
    ModelSelector.*        Searchable model pill
    MicButton.*            Recording state button
    WindowControls.*       macOS-style traffic lights
    DotMatrixWordmark.*    Empty-state animation
    LiquidGlass.*          Blur/glass helper
    CircleButton.* IconButton.* OutlinedLabel.*
tools/matricX-cowork/      Engine sources + built executable + cursor asset
assets/                    Icons and app resources
scripts/                   build/run/package/smoke-test helpers
.github/workflows/         Manual build + smoke tests; tag-driven release packaging
website/                   Static marketing site (deployed on Vercel)
packages/msys2-packages.txt
```

---

## 4. The agent loop (most important code path)

`src/ui/ChatArea.cpp`

1. `send()` — handles slash commands (`/help`, `/tools`, `/tool <name> {json}`,
   `/models`, `/clear`) or appends the user message to `SessionManager` and `m_history`.
2. `runAgentLoop()` — takes a `runId = ++m_runId`, sets `m_busy`, shows the thinking
   bubble, compacts the history **on a copy** with `ConversationCompactor::compact`
   (token estimate shown in the bubble), and sends the payload +
   `CoworkTool::definitions()` to `AiClient::sendMessage` with a stream callback.
3. Streaming callback — guarded by `runId != m_runId`; appends text deltas to a live
   streaming card and reasoning deltas to the thinking bubble preview.
4. Response handling (guarded the same way):
   - error → unlock input, show `addErrorMessage()` (with Retry).
   - `reasoning` → persisted as role `thinking` when `showReasoning()`.
   - `text` → persisted and rendered as markdown.
   - no tool calls → done, unlock input, remove thinking bubble.
   - tool calls → `executeNextToolCall(calls, 0)`.
5. `executeNextToolCall()` — per call: render `ActionStepCard`, optionally show an
   Allow/Deny prompt (`requireConfirmation()`), call `CoworkTool::call`, persist the
   result as a session message with role `tool` (attachments copied into app data via
   `persistAttachment`), append `"Tool result for <name>: <json>"` to `m_history`,
   then recurse to the next call.
6. After the last call → `runAgentLoop()` again (ReAct loop) until the model stops
   requesting tools.

Screenshot results carry `imagePath`; `AiClient` base64-encodes the PNG and sends it as a
vision part for Gemini, OpenAI-compatible, and Anthropic requests. Persisted tool results
reload as quiet gray rows and are fed back to the model as `role: "user"` messages.

The Stop button invalidates the whole run: `++m_runId`, `m_ai.abort()` (kills the
in-flight `QNetworkReply`), and `m_tool.endSession()`. All callbacks compare their
captured `runId` against `m_runId` and drop out silently when stale.

---

## 5. Engine protocol (`matricX-cowork`)

- Launched lazily from the UI:
  `matricX-cowork --glow green --speed normal --idle-timeout 120 [--enable-shell]`
- `--describe` prints a JSON array of **all 17 tool schemas** regardless of
  `--enable-shell`; hiding `shell_exec` before opt-in is the UI's job
  (`CoworkTool::definitions()` filters it). Cache lives in `m_defs`.
- One JSON object per line on stdin; replies echo `id`; lines without `id` are events
  (`ready`, `stopped`, aborts).
- 17 tools:

| Tool | Args | Notes |
|---|---|---|
| `screen_info` | — | screen size + cursor position |
| `screenshot` | `path?` | PNG; returns path/width/height; glow overlay excluded |
| `mouse_move` | `x,y` | smooth human-like path |
| `mouse_click` | `x?,y?,button?,count?` | 1–3 clicks |
| `mouse_drag` | `x,y,from_x?,from_y?` | press → move → release |
| `mouse_scroll` | `dy,dx?,x?,y?` | wheel notches |
| `keyboard_type` | `text,delay_ms?` | newline = Enter, tab = Tab |
| `keyboard_key` | `keys` | `enter`, `ctrl+c`, `alt+tab`, `f1–f24`, … |
| `wait` | `ms` | ≤ 60 000 |
| `fs_list` | `path` | directory listing inside allowed roots |
| `fs_read` | `path` | text read, 1 MiB cap, allowed roots only |
| `fs_write` | `path,text,append?` | write/create, 2 MiB cap, allowed roots only |
| `fs_mkdir` | `path` | create directories inside allowed roots |
| `fs_delete` | `path,recursive?` | delete file/dir; recursive requires the flag |
| `fs_search` | `path,pattern` | glob search with result cap |
| `shell_exec` | `argv,cwd?,timeout_ms?` | **argv array only** (never a shell string), 256 KiB output cap, 300 s timeout, exit code returned; requires `--enable-shell` |
| `end_session` | — | release control, remove overlay |

Sandbox: filesystem tools and shell `cwd` are restricted to `$HOME` + system temp +
any `--allow-root <dir>` values on the command line. Outside those roots the engine
refuses with `ok: false` and an explanatory error. Esc aborts; idle timeout cleans up.

---

## 6. AI providers

`SettingsManager::availableProviders()` (`src/core/SettingsManager.cpp`):

| id | Label | Base URL | Default model |
|---|---|---|---|
| `gemini` | Google Gemini / Antigravity | `generativelanguage.googleapis.com` | `gemini-2.5-flash` |
| `deepseek` | DeepSeek | `api.deepseek.com` | `deepseek-chat` |
| `openrouter` | OpenRouter | `openrouter.ai/api/v1` | `deepseek/deepseek-chat` |
| `openai` | OpenAI | `api.openai.com/v1` | `gpt-4o` |
| `anthropic` | Anthropic Claude | `api.anthropic.com/v1` | `claude-3-7-sonnet-20250219` |
| `groq` | Groq | `api.groq.com/openai/v1` | `llama-3.3-70b-versatile` |
| `ollama` | Ollama / local | `localhost:11434/v1` | `llama3.3` |

Dispatch in `AiClient`: `sendGemini`, `sendOpenAiCompatible`, `sendAnthropic`.
All three run through a shared `postWithRetry` engine: SSE streaming (diffs forwarded to
the `StreamCallback`), automatic fallback to parsing the full body when the server never
streams (some proxies ignore `stream`), and retry (≤ 3 attempts, backoff 1.2 s / 3.2 s)
only for transport failures and 429/5xx responses **before** any streamed output — never
for 4xx bodies and never mid-stream. `abort()` cancels the current request; its callback
does not fire.
Also: `fetchAvailableModels()` (⚡ autodetect) and `transcribeAudio()` (Whisper via
Groq/OpenAI). Gemini can use the API key or Google Antigravity OAuth
(`AntigravityAuth`: loopback server, `state` check, 180 s timeout, refresh token; OAuth
requests stay non-streamed).

---

## 7. State, storage, settings

| Data | Location | Format |
|---|---|---|
| Sessions + messages | `QStandardPaths::AppDataLocation/sessions.json` | JSON; `ChatMessageItem.role` may be `user`/`assistant`/`tool`/`thinking`, optional `imagePath` |
| Tool attachments | same dir, `attachments/` | images copied here by `persistAttachment` so previews survive restarts |
| Provider keys (per-entry metadata) | same dir, `provider_keys.json` | JSON, **no secrets** — id/label/isActive/createdAt only |
| Provider key secrets | `SecretStore` (`providerKey/<pid>`, `pkmKey/<entryId>`) | Windows DPAPI; Linux libsecret when available; otherwise an obfuscated, chmod 600 `secrets.json` in the same dir |
| Legacy single key | `QSettings("MCowork","MCowork")` → `ai/<provider>/apiKey` | migrated into `SecretStore` on first read, then scrubbed |
| Prefs | same QSettings | `ai/provider`, `ai/<p>/model`, `ai/<p>/baseUrl`, `safety/requireConfirmation`, `safety/allowShell`, `ui/*` |

Notable defaults: provider `gemini`, `requireConfirmation = true`, `allowShell = false`,
`showReasoning = true`, window controls `titlebar / right / circle / medium`.

Session lifecycle: create, rename, set model, archive, restore, permanent delete.
Auto-title: sessions titled `New Session`/`Current Session`/`New Chat` are renamed from
the first real user message (tool results and slash commands excluded).

---

## 8. UI composition

- `MainWindow` — frameless; Windows `WM_NCHITTEST`/`WM_NCCALCSIZE`, manual maximize;
  other platforms use drag + edge resize.
- `Sidebar` — search filter over titles, session menu (rename/archive/delete), collapse.
- `ChatArea` — top bar (controls/title/status), scrollable centered 780 px column,
  markdown assistant cards, user bubbles (with optional image preview), live streaming
  card for model deltas, thinking bubble, action cards, quiet gray tool-result rows
  (with screenshot preview), error cards, floating capsule (model selector, mic,
  send/stop), sidebar-collapsed controls.
- `SettingsDialog` — Appearance (theme, controls place/position/shape/size, show
  reasoning), Providers (keys, base URL, autodetect, OAuth), Archived sessions, plus a
  safety block: confirmation prompt toggle and an explicit `shell_exec` opt-in.
- `ActionStepCard` — Running / Success / Failed badge, collapsible args + result, image
  preview for screenshots.
- `ThemeManager` — Dark / Light / System with live repaint.

---

## 9. Build, run, package

### Windows (MSYS2 UCRT64 at `C:\msys64`)
```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install-packages.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1          # incremental
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Clean   # full
powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1
```
`build.ps1` runs `moc` manually, invokes `g++` per translation unit, links
`m_cowork_ui.exe`, then compiles the engine if its source is newer.
`package-windows.ps1` stages `dist/m-cowork-windows-x86_64/`, runs `windeployqt`, zips it.

### Linux
```bash
./scripts/build.sh
./scripts/run.sh
./scripts/smoke-test.sh        # JSON-lines protocol + fs/shell sandbox under Xvfb
./scripts/package-linux.sh
```
Requires Qt6 Widgets/Svg/Network (+ optional Multimedia), X11 Xtst/Xfixes, libpng.
`SecretStore` uses libsecret when present at runtime (dlopen'd, not a build dep) and
falls back to an obfuscated file otherwise.

### CI
`.github/workflows/manual-build.yml` (`workflow_dispatch`, any ref via the `ref` input):
- Linux: apt deps → CMake/Ninja → build → `xvfb-run ./scripts/smoke-test.sh` →
  `--describe` schema validation asserting all **17** tool names → artifact.
- Windows: MSYS2 UCRT64 → CMake/Ninja → build → `--describe` schema validation
  (expects the same 17 tool names) → artifact.
- Both jobs upload 14-day artifacts.

`.github/workflows/release.yml` (push tags `v*`): builds both platforms via the
existing `package-linux.sh` / `package-windows.ps1`, re-validates tool schemas +
smoke test, then attaches `dist/*.tar.gz` and `dist/*.zip` to a GitHub Release
(`softprops/action-gh-release@v2`).

---

## 10. What is already done

- Codex-style Qt UI, frameless window, custom chrome, dark/light/system themes.
- 7 providers, per-provider keys/models/base URLs, model autodetect.
- Session lifecycle with archive/restore/2-step delete and search.
- Streamed chat with markdown rendering, thinking/reasoning cards, error retry.
- Agent loop with sequential tool calls, per-call action cards, Allow/Deny prompt.
- Multimodal feedback: screenshots fed back as vision input.
- Voice input with Whisper transcription.
- Windows + Linux engine, JSON-lines protocol, `--describe`, idle timeout, Esc abort.
- Build/package scripts, manual CI on both platforms.
- OAuth hardening and request timeouts (PR #2, merged as `f216ae2`).
- **Context compaction** (`ConversationCompactor`): 48 k token budget, newest-tail
  windowing (28 k), dropped-image trimming, consecutive-message merge, and a short
  summary message for the omitted prefix; operates on a copy only.
- **Real cancellation + streaming**: `AiClient` is a QObject with `abort()` and SSE
  streaming for Gemini/OpenAI-compatible/Anthropic; `Stop` invalidates the run
  (`m_runId`), aborts the reply, and ends the engine session.
- **Secure secret storage** (`SecretStore`): DPAPI on Windows, libsecret (dlopen) on
  Linux, obfuscated chmod-600 file fallback; legacy plaintext keys migrate and are
  scrubbed.
- **Safe-by-default permissions**: `requireConfirmation` now defaults to `true`.
- **Filesystem + shell tools** on both engines (`fs_list/read/write/mkdir/delete/search`,
  `shell_exec`), sandboxed to `$HOME` + temp + `--allow-root`, argv-only shell, output
  caps, timeout; `shell_exec` hidden until enabled in Settings.
- **Persisted tool results & attachments**: role `tool` messages with `imagePath`,
  attachments copied under app data, previews restored after restart.
- **Auto-titled sessions** from the first real user message.
- **Tag-driven release pipeline** (`.github/workflows/release.yml`).

---

## 11. What is left to build

Ordered by impact. Each item names the place to start. (2026-10 status: former items
1–4 of P0, 5–8 and 10 of P1, and 12 of P2 are **done** — see §10.)

### P0 — correctness / safety

*(none open — context compaction, real cancellation, secret storage, and
confirm-by-default are all implemented)*

### P1 — capability

9. **Parallel tool calls.** Independent calls run sequentially. Batch non-conflicting
   calls and keep ordering for input-dependent ones.
10. *(done — retry/backoff with SSE-guard is in `AiClient::postWithRetry`.)*

### P2 — polish / reach

11. **Tests.** Only the shell smoke test exists. Add unit tests for provider payload
    builders/parsers, `AntigravityAuth::parseTokenResponse`, session persistence, and the
    JSON-lines reader; add a Qt integration test for the agent loop with a fake provider.
12. *(done — `.github/workflows/release.yml` packages both platforms on `v*` tags.)*
13. **Streaming token/usage display.** No token or cost accounting is shown to the user.
14. **Native Wayland.** Capture currently shells out to `grim`/`import`; prefer the
    xdg-desktop-portal Screenshot API.
15. **User attachments.** Only tool screenshots can be attached; add paste/drag-drop
    images and file references for the model.
16. **macOS.** Not supported; would need a Cocoa/Quartz engine implementation.
17. **Accessibility / i18n.** Keyboard navigation and translations are not implemented.
18. **Refactor `ChatArea.cpp`.** ~1100 lines mixing UI construction, the agent loop,
    rendering, and input handling. Extract `MessageListView`, `ThinkingBubble`,
    `InputCapsule`, and `AgentRunner`.

### Known rough edges

- Tool results are injected as `role: "user"` text (`"Tool result for ..."`), not native
  provider tool-result messages. Works, but providers cannot distinguish tool output
  from user speech.
- `shell_exec` on Linux resolves inside the sandbox roots but a hardcoded absolute path
  (e.g. `/usr/bin/env`) is allowed to bypass the cwd jail by design; `-p` argv style is
  not validated beyond the array requirement.
- The engine's `--describe` is cached per `CoworkTool` instance; rebuilding the engine
  while the app runs requires a restart (toggling the shell setting also wants a fresh
  engine start to take effect).
- Remote/git/MCP engine tools (§12) remain spec-only; secret routing for them is ready
  (`SecretStore`) but no engine plumbing exists yet.
- CI validates the Windows engine's compile + schemas, but no Windows smoke run against
  real fs/shell ops yet (`smoke-test.ps1` only sends safe happy-path commands).

---

## 12. Planned tools (not implemented yet)

The engine now exposes **the 10 GUI tools plus the 6 filesystem tools and `shell_exec`**
(§5) — those lists are live and CI-validated. Everything below remains spec-only; when
adding one, update the engine dispatch, the `--describe` schema, and the CI validation
list **together** (engine version bump included).

| Tool id | Capability | Risk | Required guardrails |
|---|---|---|---|
| ~~`fs_list`~~ | **implemented** | low | allowed roots, result cap (done) |
| ~~`fs_read`~~ | **implemented** | medium | allowed roots, 1 MiB cap (done) |
| ~~`fs_write`~~ | **implemented** | high | allowed roots, 2 MiB cap (done) |
| ~~`fs_mkdir`~~ | **implemented** | low | allowed roots (done) |
| ~~`fs_delete`~~ | **implemented** | high | allowed roots, explicit `recursive` flag (done) |
| ~~`fs_search`~~ | **implemented** | medium | allowed roots, result cap (done) |
| ~~`shell_exec`~~ | **implemented** (both engines) | **critical** | `--enable-shell` opt-in in Settings, argv-only, `$HOME`/temp/`--allow-root` sandbox, 256 KiB output cap, 300 s timeout (done) |
| `powershell` | Run a PowerShell script/command (Windows) | **critical** | opt-in, `-NoProfile -NonInteractive`, timeout, transcript log |
| `bash` | Run a bash script/command (Linux) | **critical** | opt-in, timeout, no login shell, output cap |
| `ssh_exec` | Run a command on a remote host over SSH | **critical** | host allowlist, key/agent auth, timeout, audit log |
| `ssh_shell` | Persistent interactive SSH session (send/expect) | **critical** | host allowlist, idle timeout, session cap |
| `sftp_list` | List a remote directory | medium | host allowlist, path jail |
| `sftp_download` | Download a remote file | high | host allowlist, size cap, local path jail |
| `sftp_upload` | Upload a local file | high | host allowlist, size cap, remote path jail |
| `git_status` | Branch, staged/unstaged/untracked summary | low | repo path allowlist |
| `git_diff` | Diff working tree / commit range | low | repo allowlist, size cap |
| `git_log` | Recent commits with author/date | low | repo allowlist, count limit |
| `git_branch` | List/create/switch/delete branches | high | repo allowlist, no force-delete without confirmation |
| `git_commit` | Stage paths + commit with message | high | repo allowlist, show diff first, no `--force` |
| `git_push` / `git_pull` | Sync with a remote | **critical** | repo allowlist, remote allowlist, never force-push, confirmation |
| `git_clone` | Clone a repository | medium | URL scheme allowlist (`https`/`ssh`), destination jail, depth cap |
| `http_request` | GET/POST an HTTP(S) URL, return status/headers/body | high | method allowlist, body/response caps, no credentials by default, domain allowlist |
| `web_fetch` | Fetch a page and convert to markdown/text | medium | size cap, robots/timeout |
| `clipboard_read` | Read clipboard text/image | medium | text only by default, no history |
| `clipboard_write` | Write clipboard text | low | size cap |
| `process_list` | List running processes (pid, name, cpu, mem) | low | — |
| `process_kill` | Terminate a process by pid/name | **critical** | allowlist, never kill self/system pids, confirmation |
| `package_run` | Invoke winget/apt/pacman/npm/pip | **critical** | command allowlist, dry-run first, confirmation |
| `open_url` | Open a URL in the default browser | low | scheme allowlist (`http`/`https`) |
| `notification` | Show a desktop notification | low | — |
| `ocr_screen` | OCR a screen region and return text | medium | region bounds, no persistence |
| `mcp_call` | Call a tool on a configured MCP server | varies | per-server allowlist, timeout, schema validation |

Implementation notes:

- Prefer **argv arrays** over shell strings to avoid quoting/injection bugs.
- Every tool gets: timeout, output size cap, structured `{ok, error}` reply, and an audit
  line (timestamp, tool, args hash, duration, exit status).
- Group gating in settings: `tools.filesystem`, `tools.shell`, `tools.remote`,
  `tools.git`, `tools.network`, `tools.process`. Default everything except read-only
  filesystem/git inspection to **off**.
- Remote tools should resolve credentials from the OS keychain/agent, never from
  `provider_keys.json`.
- The engine is a separate binary: adding a tool means rebuilding
  `tools/matricX-cowork/` and restarting the app (`--describe` is cached per process).

---

## 13. Conventions for agents working here

- Match the existing Qt/C++ style: `mcw::` namespace, header/source pairs, 4 spaces,
  forward declarations, `QString`/`QJsonObject` over STL strings.
- Do not add dependencies without checking `CMakeLists.txt` and
  `packages/msys2-packages.txt`.
- Keep `build.ps1` working: it compiles every `src/**/*.cpp` and runs `moc` on headers
  containing `Q_OBJECT`.
- Any new engine command must be added to: the engine dispatch, its `--describe` schema,
  and the CI validation list in `.github/workflows/manual-build.yml`.
- Never commit `build/`, `dist/`, or `*.exe` (gitignored). Do not claim a build was tested
  unless it was actually run.
- Prefer official Qt/provider docs over guessing; verify payload shapes against existing
  code before editing.
