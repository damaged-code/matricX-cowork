# Drives matricX-cowork.exe over its JSON-lines protocol (safe commands only).
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'tools\matricX-cowork\matricX-cowork.exe'
if (-not (Test-Path $exe)) {
  & (Join-Path $PSScriptRoot 'build.ps1')
}
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $exe; $psi.Arguments = "--no-lock --no-cursor --glow off --idle-timeout 30 --enable-shell"
$psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.UseShellExecute = $false
$p = [Diagnostics.Process]::Start($psi)
$null = $p.StandardOutput.ReadLine()
foreach ($j in '{"id":1,"cmd":"screen_info"}','{"id":2,"cmd":"mouse_move","x":600,"y":400}','{"id":3,"cmd":"screenshot"}','{"id":4,"cmd":"end_session"}') {
  $p.StandardInput.WriteLine($j); $p.StandardInput.Flush(); $null = $p.StandardOutput.ReadLine()
}
# Filesystem tools inside the (always allowed) temp root.
$work = Join-Path ([IO.Path]::GetTempPath()) ("mcow-smoke-" + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force -Path $work | Out-Null
foreach ($j in @(
  ('{"id":10,"cmd":"fs_mkdir","path":"' + $work + '\sub"}'),
  ('{"id":11,"cmd":"fs_write","path":"' + $work + '\sub\a.txt","text":"hello smoke"}'),
  ('{"id":12,"cmd":"shell_exec","argv":["cmd","/c","exit","7"],"cwd":"' + $work + '"}'),
  ('{"id":13,"cmd":"fs_delete","path":"' + $work + '","recursive":true}'),
  '{"id":14,"cmd":"end_session"}')) {
  $p.StandardInput.WriteLine($j); $p.StandardInput.Flush(); $null = $p.StandardOutput.ReadLine()
}
$null = $p.StandardOutput.ReadLine(); $null = $p.WaitForExit(5000)
Write-Host 'All Windows smoke checks sent.'
