# M-Cowork

An AI app that lets people use their AI like OpenCode. Cross-platform desktop UI built with Qt 6 Widgets (C++17) supporting Windows and Linux.

## Requirements

### Windows build machine
- Windows 10/11
- [MSYS2](https://www.msys2.org/) installed at `C:\msys64` (UCRT64 environment)

The packaged application is intended to run on Windows 10/11 without MSYS2 or Qt installed; MSYS2 is only needed to build it.

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

# 3. Create a portable ZIP (build.ps1 deploys runtime files automatically)
powershell -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1

# 4. Run using a clean PATH (ignores MSYS2/other Qt installations)
powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1
```

### Windows packaging and troubleshooting

`build.ps1` runs `windeployqt6` from the MSYS2 UCRT64 installation after linking, deploys Qt 6 runtime DLLs and platform plugins, recursively inspects PE imports with `objdump`, copies available non-system MinGW/UCRT DLLs, and verifies the essential files. It also places `assets\` and `tools\matricX-cowork\matricX-cowork.exe` beside the app so the executable can find them without relying on the source checkout.

Run `scripts\package-windows.ps1` to create `matricx-cowork-windows.zip`. Extract the ZIP to a folder and run `m_cowork_ui.exe`; do not extract only the executable. `scripts\run.ps1` limits `PATH` to Windows system directories and clears Qt plugin override variables, so other Qt installations should not shadow the bundled DLLs.

**Troubleshooting — “Entry Point Not Found” / QVariant symbol errors:** this usually indicates the executable and the Qt DLLs loaded at runtime are from different Qt builds/versions. Rebuild with `scripts\build.ps1 -Clean`, use the full packaged ZIP, and ensure `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `Qt6Svg.dll`, `platforms\qwindows.dll`, and compiler runtime DLLs come from the same MSYS2 UCRT64 installation. Avoid launching the EXE from a folder with stale Qt DLLs or a PATH pointing at another Qt installation. The build script verifies core deployment files, but the final ZIP should still be tested on a clean Windows machine.

## Linux (Bash / CMake)
```bash
# 1. Build using the build script or CMake
./scripts/build.sh

# 2. Run the application
./scripts/run.sh

# 3. Run the smoke test
./scripts/smoke-test.sh
```

## Manual GitHub Actions workflow

The `.github/workflows/manual-build.yml` workflow builds Linux and Windows on GitHub-hosted runners and can run the Linux JSON-lines/filesystem smoke test under Xvfb. It is triggered only by `workflow_dispatch`; it does not run on pushes or pull requests. The Windows job validates the tool schemas after building.

**GitHub limitation:** GitHub's Actions UI only offers a `workflow_dispatch` workflow once that workflow file also exists on the repository's default branch. This branch contains the workflow, but it has intentionally not been merged or copied to the default branch. Keep development on `feature/agent-tools`; do not merge it merely to expose the manual trigger without explicit approval. Once the workflow is approved for the default branch, open **Actions → Manual build and smoke tests → Run workflow**, choose `feature/agent-tools`, and select `all`, `linux`, or `windows`.

The Linux smoke test runs under a virtual X server and does not replace testing real desktop interaction. Windows currently validates a build and schemas but does not run a Windows protocol-level smoke test.

## AI Tool (`matricX-cowork`)
The computer-control engine communicates with the GUI via a JSON-lines protocol over standard I/O:
- In the chat box, type `/tools` to display all available OS automation actions and schemas.
- Run commands manually: `/tool mouse_move {"x":500,"y":300}`, `/tool screen_info`, or `/tool screenshot`.
- Filesystem actions are available on both platforms: `/tool fs_list {"path":"."}`, `/tool fs_read {"path":"README.md"}`, `/tool fs_write {"path":"notes.txt","content":"hello"}`, `/tool fs_mkdir {"path":"scratch/new"}`, and `/tool fs_delete {"path":"notes.txt"}`. File reads and writes are capped at 1 MiB; directory listings return at most 200 entries; file overwrite requires `overwrite:true`; directory deletion is intentionally disabled.
- Configure per-tool safety with `/tool-policy <name> <inherit|ask|session|allow>`; for example, `/tool-policy fs_delete ask`. `inherit` follows the global confirmation setting, `ask` prompts every time, `session` prompts until approved once for the current app session, and `allow` persists permission for that tool. Use `/tool-policy <name>` to inspect a policy.
- The AI provider loop will leverage the same schema interface.

## Status

### Done
- Cross-platform Qt 6 Widgets UI matching the OpenAI Codex / ChatGPT Desktop design: collapsible sidebar, centered chat canvas, floating input capsule, sleek macOS window controls.
- Linux and Windows support: Linux X11/Wayland automation engine + native Win32 computer control.
- Cross-platform CMake and PowerShell build systems (`scripts/build.sh`, `scripts/build.ps1`, `scripts/smoke-test.sh`).
- `matricX-cowork` v0.4.0 JSON-lines computer-control tool (screenshot, mouse move/click/drag/scroll, keyboard type/key, wait, end_session, `--describe` schemas, Esc to abort, idle timeout).
- Multimodal perception feedback loop: captures desktop screenshots and feeds them back directly as vision parts (base64) into Gemini, OpenAI, and Anthropic.
- In-app OS control: `/tools`, `/tool <name> {json}`, and interactive `ActionStepCard` widgets with live status badges and screenshot previews.
- Multiple providers supported: Google Gemini (with Google Antigravity OAuth), DeepSeek, OpenRouter, OpenAI, Anthropic Claude, Groq, Ollama.
- Dynamic model autodetection (`⚡ Autodetect`) across all providers.
- Multi-session lifecycle: persistent chat sessions, message storage, archive, restore, and 2-step permanent deletion.
- In-chat model switcher pill with search and Lucide Bot icon.
- Interactive safety confirmation: user permission prompt before executing OS actions.
- Interactive error cards with one-click Retry.
- Native markdown rendering and link handling for assistant messages.
- Appearance settings: Dark, Light, and System / Auto theme modes with immediate live updates.
