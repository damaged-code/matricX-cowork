# Runs the app using only bundled Qt/runtime files plus Windows system directories.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'build\m_cowork_ui.exe'
if (-not (Test-Path $exe)) { & (Join-Path $PSScriptRoot 'build.ps1') }
if (-not (Test-Path $exe)) { throw "Executable not found: $exe" }

# Do not inherit MSYS2, Anaconda, or other Qt directories. Windows searches the executable
# directory for its private DLLs; PATH is limited to Windows system directories for children.
$windows = $env:SystemRoot
$env:Path = "$windows\System32;$windows"
Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QML2_IMPORT_PATH -ErrorAction SilentlyContinue
Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe)
