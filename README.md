# M-Cowork

An AI app that lets people use their AI like OpenCode. Cross-platform desktop UI built with Qt 6 Widgets (C++17) supporting Windows and Linux.

## Requirements

### Windows
- Windows 10/11
- [MSYS2](https://www.msys2.org/) installed at `C:\msys64` (UCRT64 environment)

### Linux
- Modern Linux distribution (Arch/Ubuntu/Fedora/Debian)
- GCC / Clang (C++17)
- CMake (>= 3.16)
- Qt 6 (Widgets, Svg)
- X11 development headers & extensions (`libx11`, `libxtst`, `libxfixes`)
- `libpng`
- Optional: `grim` (for native Wayland screen captures)

## Setup & Build

### Windows (PowerShell)
```powershell
# 1. Install MSYS2 build packages
powershell -ExecutionPolicy Bypass -File .\scripts\install-packages.ps1

# 2. Build the UI and computer-control tool
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1          # incremental
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Clean   # full rebuild

# 3. Run the application
powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1
```

### Linux (Bash / CMake)
```bash
# 1. Build using the build script or CMake
./scripts/build.sh

# 2. Run the application
./scripts/run.sh

# 3. Run the smoke test
./scripts/smoke-test.sh
```

## AI Tool (`matricX-cowork`)
The computer-control engine communicates with the GUI via a JSON-lines protocol over standard I/O:
- In the chat box, type `/tools` to display all available OS automation actions and schemas.
- Run commands manually: `/tool mouse_move {"x":500,"y":300}`, `/tool screen_info`, or `/tool screenshot`.
- The AI provider loop will leverage the same schema interface.

## Status

### Done
- Cross-platform Qt 6 Widgets UI matching the OpenAI Codex / ChatGPT Desktop design: collapsible sidebar, centered chat canvas, floating input capsule, sleek macOS window controls.
- Linux and Windows support: Linux X11/Wayland automation engine + native Win32 computer control.
- Cross-platform CMake and PowerShell build systems (`scripts/build.sh`, `scripts/build.ps1`, `scripts/smoke-test.sh`).
- `matricX-cowork` v0.5.0 JSON-lines computer-control tool: screenshot, mouse move/click/drag/scroll, keyboard type/key, wait, filesystem tools (`fs_list`/`fs_read`/`fs_write`/`fs_mkdir`/`fs_delete`/`fs_search`), sandboxed `shell_exec` (opt-in, argv-only), `end_session`, `--describe` schemas, Esc to abort, idle timeout.
- Multimodal perception feedback loop: captures desktop screenshots and feeds them back directly as vision parts (base64) into Gemini, OpenAI, and Anthropic.
- Streamed responses (SSE) with a live output card, automatic retry/backoff for transient failures, and real Stop (cancels the in-flight request and ends the engine session).
- Context compaction: long sessions are summarized instead of failing on provider limits.
- Secure API-key storage: Windows DPAPI, Linux keyring (libsecret), obfuscated-file fallback — no more plaintext keys on disk.
- Filesystem and shell tool results persist with their screenshots and reload after restart.
- In-app OS control: `/tools`, `/tool <name> {json}`, and interactive `ActionStepCard` widgets with live status badges and screenshot previews.
- Multiple providers supported: Google Gemini (with Google Antigravity OAuth), DeepSeek, OpenRouter, OpenAI, Anthropic Claude, Groq, Ollama.
- Dynamic model autodetection (`⚡ Autodetect`) across all providers.
- Multi-session lifecycle: persistent chat sessions, message storage, archive, restore, and 2-step permanent deletion.
- In-chat model switcher pill with search and Lucide Bot icon.
- Interactive safety confirmation: user permission prompt before executing OS actions (on by default).
- Shell execution is opt-in: `shell_exec` stays hidden from the AI until enabled in Settings, then runs argv commands only, sandboxed to your home folder (plus explicit roots), with output caps and a hard timeout.
- Interactive error cards with one-click Retry.
- Native markdown rendering and link handling for assistant messages.
- Appearance settings: Dark, Light, and System / Auto theme modes with immediate live updates.
