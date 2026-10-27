# Builds M-Cowork into build\m_cowork_ui.exe (incremental). Use -Clean for a full rebuild.
param([switch]$Clean)
$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$src   = Join-Path $root 'src'
$build = Join-Path $root 'build'
$obj   = Join-Path $build 'obj'
$exe   = Join-Path $build 'm_cowork_ui.exe'

$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }
New-Item -ItemType Directory -Force $obj | Out-Null

$cflags = @((& pkg-config --cflags Qt6Widgets Qt6Svg Qt6Network) -split '\s+' | Where-Object { $_ })
if ($LASTEXITCODE -ne 0) { throw 'Qt 6 Widgets/Svg/Network not found. Run scripts\install-packages.ps1 first.' }
$libs = @((& pkg-config --libs Qt6Widgets Qt6Svg Qt6Network) -split '\s+' | Where-Object { $_ })
$objects = @()

# Run moc on headers with Q_OBJECT
$mocCandidates = @(
    'C:\msys64\ucrt64\share\qt6\bin\moc.exe',
    'C:\msys64\ucrt64\bin\moc.exe'
)
$moc = $mocCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $moc) { throw 'Qt moc.exe not found. Run scripts\install-packages.ps1 first.' }
foreach ($h in Get-ChildItem $src -Recurse -Filter *.h) {
    $content = Get-Content $h.FullName -Raw
    if ($content -match 'Q_OBJECT') {
        $mocCpp = Join-Path $obj ('moc_' + $h.BaseName + '.cpp')
        $stale = (-not (Test-Path $mocCpp)) -or ((Get-Item $mocCpp).LastWriteTime -lt $h.LastWriteTime)
        if ($stale) {
            Write-Host "moc      $($h.Name)"
            & $moc $h.FullName -o $mocCpp -I $src
            if ($LASTEXITCODE -ne 0) { throw "moc failed: $($h.Name)" }
        }
        $o = Join-Path $obj ('moc_' + $h.BaseName + '.o')
        $objects += $o
        if ((-not (Test-Path $o)) -or ((Get-Item $o).LastWriteTime -lt (Get-Item $mocCpp).LastWriteTime)) {
            & g++ -std=c++17 -O2 -Wall -I $src @cflags -c $mocCpp -o $o
            if ($LASTEXITCODE -ne 0) { throw "moc compilation failed: $($h.Name)" }
        }
    }
}

$newestHeader = (Get-ChildItem $src -Recurse -Filter *.h | Measure-Object LastWriteTime -Maximum).Maximum
foreach ($cpp in Get-ChildItem $src -Recurse -Filter *.cpp) {
    $rel = $cpp.FullName.Substring($src.Length + 1)
    $o = Join-Path $obj (($rel -replace '[\\/]', '_') + '.o')
    $objects += $o
    $stale = (-not (Test-Path $o)) -or
             ((Get-Item $o).LastWriteTime -lt $cpp.LastWriteTime) -or
             ((Get-Item $o).LastWriteTime -lt $newestHeader)
    if ($stale) {
        Write-Host "compile  $rel"
        & g++ -std=c++17 -O2 -Wall -I $src @cflags -c $cpp.FullName -o $o
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $rel" }
    }
}

Write-Host 'link     m_cowork_ui.exe'
& g++ @objects -o $exe @libs -mwindows
if ($LASTEXITCODE -ne 0) { throw 'Linking failed.' }
Write-Host "built    $exe"

$toolSrc = Join-Path $root 'tools\matricX-cowork\matricX-cowork.cpp'
$toolExe = Join-Path $root 'tools\matricX-cowork\matricX-cowork.exe'
if (Test-Path $toolSrc) {
    $toolStale = (-not (Test-Path $toolExe)) -or ((Get-Item $toolSrc).LastWriteTime -gt (Get-Item $toolExe).LastWriteTime)
    if ($toolStale) {
        Write-Host 'compile  tools\matricX-cowork\matricX-cowork.cpp'
        & g++ -std=c++17 -O2 -Wall -Wextra -static $toolSrc -o $toolExe -luser32 -lgdi32 -lgdiplus -lwinmm
        if ($LASTEXITCODE -ne 0) { throw 'matricX-cowork tool compilation failed.' }
        Write-Host "built    $toolExe"
    }
}


# Deploy Qt and compiler runtimes beside the executable so the build folder is portable.
$qtBin = 'C:\msys64\ucrt64\bin'
$pluginRoot = 'C:\msys64\ucrt64\share\qt6\plugins'
$deployCandidates = @(
    (Join-Path $qtBin 'windeployqt6.exe'),
    (Join-Path $qtBin 'windeployqt.exe')
)
$windeployqt = $deployCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $windeployqt) { throw "windeployqt6.exe was not found in $qtBin. Install the MSYS2 UCRT64 Qt6 base package." }

Write-Host 'deploy   Qt runtime and plugins'
& $windeployqt --release --compiler-runtime --no-translations --dir $build $exe
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed to deploy Qt runtime files.' }

# Copy assets and the helper tool into the app-local layout expected by Assets.cpp.
$assetsSrc = Join-Path $root 'assets'
$assetsDst = Join-Path $build 'assets'
if (Test-Path $assetsSrc) {
    New-Item -ItemType Directory -Force $assetsDst | Out-Null
    Copy-Item (Join-Path $assetsSrc '*') $assetsDst -Recurse -Force
} else {
    throw "Required assets directory not found: $assetsSrc"
}
$packagedTool = Join-Path $build 'tools\matricX-cowork'
New-Item -ItemType Directory -Force $packagedTool | Out-Null
if (Test-Path $toolExe) {
    Copy-Item $toolExe (Join-Path $packagedTool 'matricX-cowork.exe') -Force
} else {
    throw "Required helper executable was not built: $toolExe"
}

# Preserve optional plugin groups used by Qt image loading, styles, icon engines, and TLS.
foreach ($pluginGroup in @('platforms','styles','imageformats','iconengines','tls')) {
    $sourceGroup = Join-Path $pluginRoot $pluginGroup
    $targetGroup = Join-Path $build $pluginGroup
    if ((Test-Path $sourceGroup) -and ($pluginGroup -ne 'platforms' -or -not (Test-Path (Join-Path $targetGroup 'qwindows.dll')))) {
        New-Item -ItemType Directory -Force $targetGroup | Out-Null
        Copy-Item (Join-Path $sourceGroup '*') $targetGroup -Recurse -Force
    }
}

# Walk PE imports and copy remaining non-system MinGW/UCRT DLLs recursively.
$objdump = Join-Path $qtBin 'objdump.exe'
if (-not (Test-Path $objdump)) { throw "objdump.exe was not found in $qtBin." }
$systemDlls = @(
    'ADVAPI32.dll','BCRYPT.dll','BCRYPTPRIMITIVES.dll','COMDLG32.dll','CRYPT32.dll',
    'DWMAPI.dll','GDI32.dll','IMM32.dll','KERNEL32.dll','MSVCP_WIN.dll','MSVCRT.dll',
    'NCRYPT.dll','NORMALIZ.dll','NTDLL.dll','OLE32.dll','OLEAUT32.dll','RPCRT4.dll',
    'SHELL32.dll','SHLWAPI.dll','USER32.dll','VERSION.dll','WINMM.dll','WS2_32.dll',
    'WTSAPI32.dll','IPHLPAPI.dll','SETUPAPI.dll','DNSAPI.dll','UXTHEME.dll',
    'WINHTTP.dll','WININET.dll','COMBASE.dll','PROPSYS.dll','UCRTBASE.dll'
)
$queue = [System.Collections.Generic.Queue[string]]::new()
$seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$queue.Enqueue($exe)
$queue.Enqueue((Join-Path $packagedTool 'matricX-cowork.exe'))
Get-ChildItem $build -Recurse -File -Filter *.dll | ForEach-Object { $queue.Enqueue($_.FullName) }
while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    if (-not $seen.Add([IO.Path]::GetFullPath($file))) { continue }
    $imports = & $objdump -p $file 2>$null
    if ($LASTEXITCODE -ne 0) { throw "objdump could not inspect $file" }
    foreach ($line in $imports) {
        if ($line -match '^\s*DLL Name:\s*(.+?)\s*$') {
            $dll = $Matches[1]
            if ($dll -match '^(?i:api-ms-win-|ext-ms-win-)') { continue }
            $local = Join-Path $build $dll
            if (Test-Path $local) { $queue.Enqueue($local); continue }
            $candidate = Join-Path $qtBin $dll
            if (Test-Path $candidate) {
                Copy-Item $candidate $local -Force
                $queue.Enqueue($local)
                continue
            }
            if ($systemDlls -contains $dll) { continue }
            if ($dll -match '^(?i:api-ms-win-|ext-ms-win-)' -or $dll -match '^(?i:KERNELBASE|COMCTL32|SECUR32|SSPICLI|POWRPROF|CFGMGR32|DEVOBJ|WINTRUST|OLEACC|MPR|NETAPI32|USERENV|PSAPI|DBGHELP|CLB|MSIMG32|DINPUT8|DSOUND|AVRT|MFPLAT|MF|MFREADWRITE|WINSPOOL|WINSTA|WLDAP32|WEVTAPI|CRYPTBASE|PROFAPI|GPAPI|KtmW32|MSWSOCK|NSI|HID|Uxtheme)\.dll$') { continue }
            throw "Unresolved non-system DLL dependency '$dll' imported by '$file'. Add its runtime package or explicitly classify it as a Windows system DLL."
        }
    }
}

foreach ($required in @(
    (Join-Path $build 'Qt6Core.dll'),
    (Join-Path $build 'Qt6Gui.dll'),
    (Join-Path $build 'Qt6Widgets.dll'),
    (Join-Path $build 'Qt6Svg.dll'),
    (Join-Path $build 'platforms\qwindows.dll'),
    (Join-Path $build 'tools\matricX-cowork\matricX-cowork.exe')
)) {
    if (-not (Test-Path $required)) { throw "Deployment is incomplete; required file missing: $required" }
}
Write-Host "packaged $build"

# Produce a ready-to-ship archive after a successful deployment.
$zip = Join-Path $root 'matricx-cowork-windows.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $build '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "created  $zip"
