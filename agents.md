# AGENTS.md — M-Cowork

Practical guide for anyone (human or AI) working on this repository: what exists, how it
fits together, and what is still missing.

---

## 1. What M-Cowork is

A cross-platform desktop AI agent that chats with an LLM and lets that model **control the
computer** — move/click the mouse, type, press shortcuts, take screenshots, and see the
result. The UI imitates OpenAI Codex / ChatGPT Desktop: frameless rounded window,
collapsible sidebar, centered chat column, floating input capsule.

- UI: Qt 6 Widgets, C++17 (`m_cowork_ui`)
- Engine: standalone native process (`matricX-cowork`, v0.4.0)
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
    AiClient.{h,cpp}       Provider HTTP: chat, tool calls, model list, Whisper
    AntigravityAuth.{h,cpp} Google OAuth loopback flow (Antigravity/Gemini)
  core/
    Assets.{h,cpp}         Resource lookup (assets/ both on disk and packaged)
    AudioRecorder.{h,cpp}  Mic capture -> temp audio file
    Icons.{h,cpp}          Lucide-style icon painting to QPixmap/QIcon
    ProviderKeyManager.*   Multiple API keys per provider
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
.github/workflows/         Manual build + smoke tests (Linux & Windows)
website/                   Static marketing site (deployed on Vercel)
packages/msys2-packages.txt
```

---

## 4. The agent loop (most important code path)

`src/ui/ChatArea.cpp`

1. `send()` — handles slash commands (`/help`, `/tools`, `/tool <name> {json}`,
   `/models`, `/clear`) or appends the user message to `SessionManager` and `m_history`.
2. `runAgentLoop()` — sets `m_busy`, shows the thinking bubble, sends
   `m_history` + `CoworkTool::definitions()` to `AiClient::sendMessage`.
3. Response handling:
   - error → unlock input, show `addErrorMessage()` (with Retry).
   - `reasoning` → persisted as role `thinking` when `showReasoning()`.
   - `text` → persisted and rendered as markdown.
   - no tool calls → done, unlock input, remove thinking bubble.
   - tool calls → `executeNextToolCall(calls, 0)`.
4. `executeNextToolCall()` — per call: render `ActionStepCard`, optionally show an
   Allow/Deny prompt (`requireConfirmation()`), call `CoworkTool::call`, append
   `"Tool result for <name>: <json>"` to `m_history`, then recurse to the next call.
5. After the last call → `runAgentLoop()` again (ReAct loop) until the model stops
   requesting tools.

Screenshot results carry `imagePath`; `AiClient` base64-encodes the PNG and sends it as a
vision part for Gemini, OpenAI-compatible, and Anthropic requests.

The Stop button calls `m_tool.endSession()` and clears `m_busy` — see gaps in §11.

---

## 5. Engine protocol (`matricX-cowork`)

- Launched lazily: `matricX-cowork --glow green --speed normal --idle-timeout 120`
- `--describe` prints a JSON array of tool schemas (cached in `m_defs`).
- One JSON object per line on stdin; replies echo `id`; lines without `id` are events
  (`ready`, `stopped`, aborts).
- 10 tools:

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
| `end_session` | — | release control, remove overlay |

Esc aborts; idle timeout cleans up. There is **no shell, PowerShell, or filesystem tool**
on `main`.

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
Also: `fetchAvailableModels()` (⚡ autodetect) and `transcribeAudio()` (Whisper via
Groq/OpenAI). Gemini can use the API key or Google Antigravity OAuth
(`AntigravityAuth`: loopback server, `state` check, 180 s timeout, refresh token).

---

## 7. State, storage, settings

| Data | Location | Format |
|---|---|---|
| Sessions + messages | `QStandardPaths::AppDataLocation/sessions.json` | JSON |
| Provider keys | same dir, `provider_keys.json` | JSON, **plaintext** |
| Legacy single key | `QSettings("MCowork","MCowork")` → `ai/<provider>/apiKey` | QSettings/registry |
| Prefs | same QSettings | `ai/provider`, `ai/<p>/model`, `ai/<p>/baseUrl`, `safety/requireConfirmation`, `ui/*` |

Notable defaults: provider `gemini`, `requireConfirmation = false`, `showReasoning = true`,
window controls `titlebar / right / circle / medium`.

Session lifecycle: create, rename, set model, archive, restore, permanent delete.
`ChatMessageItem` stores only `role`, `content`, `timestamp` — **not** image paths.

---

## 8. UI composition

- `MainWindow` — frameless; Windows `WM_NCHITTEST`/`WM_NCCALCSIZE`, manual maximize;
  other platforms use drag + edge resize.
- `Sidebar` — search filter over titles, session menu (rename/archive/delete), collapse.
- `ChatArea` — top bar (controls/title/status), scrollable centered 780 px column,
  markdown assistant cards, user bubbles, thinking bubble, action cards, error cards,
  floating capsule (model selector, mic, send/stop), sidebar-collapsed controls.
- `SettingsDialog` — Appearance (theme, controls place/position/shape/size, show
  reasoning), Providers (keys, base URL, autodetect, OAuth), Archived sessions.
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
./scripts/smoke-test.sh        # JSON-lines protocol under Xvfb in CI
./scripts/package-linux.sh
```
Requires Qt6 Widgets/Svg/Network (+ optional Multimedia), X11 Xtst/Xfixes, libpng.

### CI
`.github/workflows/manual-build.yml` (`workflow_dispatch` only, default ref `main`):
- Linux: apt deps → CMake/Ninja → build → `xvfb-run ./scripts/smoke-test.sh` → artifact.
- Windows: MSYS2 UCRT64 → CMake/Ninja → build → `--describe` schema validation →
  artifact. Validation expects the 10 computer-control tool names above.
- Both jobs upload 14-day artifacts; there is no automatic tag/release pipeline.

---

## 10. What is already done

- Codex-style Qt UI, frameless window, custom chrome, dark/light/system themes.
- 7 providers, per-provider keys/models/base URLs, model autodetect.
- Session lifecycle with archive/restore/2-step delete and search.
- Streaming-free chat with markdown rendering, thinking/reasoning cards, error retry.
- Agent loop with sequential tool calls, per-call action cards, Allow/Deny prompt.
- Multimodal feedback: screenshots fed back as vision input.
- Voice input with Whisper transcription.
- Windows + Linux engine, JSON-lines protocol, `--describe`, idle timeout, Esc abort.
- Build/package scripts, manual CI on both platforms.
- OAuth hardening and request timeouts (PR #2, merged as `f216ae2`).

---

## 11. What is left to build

Ordered by impact. Each item names the place to start.

### P0 — correctness / safety

1. **Context management.** `m_history` grows forever; nothing truncates or summarizes.
   Long sessions will exceed provider context limits and fail. Add token estimation,
   windowing (keep system + recent turns), optional summarization, and a visible token
   budget. Start in `ChatArea::runAgentLoop` / a new `ConversationCompactor`.
2. **Real cancellation.** The Stop button only calls `endSession()` and flips `m_busy`;
   the in-flight `QNetworkReply` is never aborted, so late callbacks can still mutate UI
   and history. Keep the active reply in `AiClient`, add `abort()`, and guard callbacks
   with a run/generation id.
3. **Secure secret storage.** API keys live in plaintext (`provider_keys.json` and
   QSettings). Use Windows DPAPI (`CryptProtectData`), Linux libsecret/keyring, or an
   encrypted store; migrate existing keys.
4. **Safe-by-default permissions.** `requireConfirmation` defaults to `false`, so OS
   actions run without asking. Default it on (or require first-run consent), and consider
   a per-tool allowlist.

### P1 — capability

5. **Response streaming.** `AiClient` waits for the full body; the UI only shows a
   generic thinking bubble. Add SSE/streaming for OpenAI-compatible, Gemini
   (`streamGenerateContent`), and Anthropic, with incremental markdown rendering.
6. **Filesystem / shell / remote tools.** `main` has none. The remote
   `feature/agent-tools` branch and the old CI check (`fs_list`, `fs_read`, `fs_write`,
   `fs_mkdir`, `fs_delete`) indicate planned filesystem tools. See §12 for the full
   planned tool surface; every one of them needs a sandbox, limits, and confirmation.
7. **Persist attachments.** `ChatMessageItem` drops `imagePath`, so screenshots vanish
   from history after restart. Store the path (and copy into app data) and restore the
   preview.
8. **Auto-title sessions.** New chats stay "New Chat"; derive a title from the first user
   message (local heuristic or a cheap model call) via `SessionManager::renameSession`.
9. **Parallel tool calls.** Independent calls run sequentially. Batch non-conflicting
   calls and keep ordering for input-dependent ones.
10. **Retry/backoff.** Add exponential backoff for 429/5xx and per-request timeouts
    everywhere (model list/transcription already have 120 s).

### P2 — polish / reach

11. **Tests.** Only the shell smoke test exists. Add unit tests for provider payload
    builders/parsers, `AntigravityAuth::parseTokenResponse`, session persistence, and the
    JSON-lines reader; add a Qt integration test for the agent loop with a fake provider.
12. **Release pipeline.** `dist/` is gitignored and there are no GitHub Releases, so
    packaged builds are local-only. Add a tag workflow that builds both platforms and
    attaches the zips to a release.
13. **Streaming token/usage display.** No token or cost accounting is shown to the user.
14. **Native Wayland.** Capture currently shells out to `grim`/`import`; prefer the
    xdg-desktop-portal Screenshot API.
15. **User attachments.** Only tool screenshots can be attached; add paste/drag-drop
    images and file references for the model.
16. **macOS.** Not supported; would need a Cocoa/Quartz engine implementation.
17. **Accessibility / i18n.** Keyboard navigation and translations are not implemented.
18. **Refactor `ChatArea.cpp`.** ~940 lines mixing UI construction, the agent loop,
    rendering, and input handling. Extract `MessageListView`, `ThinkingBubble`,
    `InputCapsule`, and `AgentRunner`.

### Known rough edges

- Tool results are injected as `role: "user"` text (`"Tool result for ..."`), not native
  tool-result messages. Works, but providers cannot distinguish tool output from user
  speech.
- The Stop path can leave a tool session and a network request in inconsistent states.
- `SettingsManager` stores keys in QSettings while `ProviderKeyManager` stores them in
  JSON; the migration is one-way and duplicates can drift.
- `--describe` is cached per `CoworkTool` instance; rebuilding the engine while the app
  runs requires a restart.

---

## 12. Planned tools (not implemented yet)

The engine currently exposes **only the 10 GUI tools** from §5. The tools below are the
intended expansion of `matricX-cowork --describe`; none exist on `main` today. Add them to
the engine dispatch, the `--describe` schemas, and the CI validation list together.

| Tool id | Capability | Risk | Required guardrails |
|---|---|---|---|
| `fs_list` | List a directory (name, size, mtime, type) | low | allowed roots, depth limit |
| `fs_read` | Read a UTF-8/binary file | medium | allowed roots, max bytes, redact secrets |
| `fs_write` | Create/overwrite a file | high | allowed roots, backup/atomic write, confirmation |
| `fs_mkdir` | Create a directory | low | allowed roots |
| `fs_delete` | Delete file/dir | high | allowed roots, no recursive by default, confirmation |
| `fs_search` | Glob/regex search across files | medium | allowed roots, result cap, ignore list |
| `shell_exec` | Run a command, return stdout/stderr/exit code | **critical** | explicit opt-in, argv (no shell string), cwd allowlist, timeout, output cap, confirmation |
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
