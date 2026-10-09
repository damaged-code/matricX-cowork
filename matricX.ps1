Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class NativeWin {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int nIndex);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern int SetWindowLong(IntPtr h, int i, int v);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
}
"@

# Screen size straight from Win32 (no dependency on WinForms Screen discovery)
$X = 0
$Y = 0
$W = [int][NativeWin]::GetSystemMetrics(0)   # SM_CXSCREEN
$H = [int][NativeWin]::GetSystemMetrics(1)   # SM_CYSCREEN
if ($W -le 0 -or $H -le 0) { throw "Could not determine screen size." }
$T = 10          # border thickness
$EDGE = 6        # how close counts as "hitting" the border

# ---- four solid lime strips that form the screen border ----
$topY = $Y
$botY = $Y + $H - $T
$rigX = $X + $W - $T
$rects = @(
  (New-Object System.Drawing.Rectangle($X, $topY, $W, $T)),
  (New-Object System.Drawing.Rectangle($X, $botY, $W, $T)),
  (New-Object System.Drawing.Rectangle($X, $Y, $T, $H)),
  (New-Object System.Drawing.Rectangle($rigX, $Y, $T, $H))
)

$forms = New-Object System.Collections.ArrayList
foreach ($r in $rects) {
  $f = New-Object System.Windows.Forms.Form
  $f.FormBorderStyle = 'None'
  $f.StartPosition = 'Manual'
  $f.Bounds = $r
  $f.TopMost = $true
  $f.ShowInTaskbar = $false
  $f.ControlBox = $false
  $f.BackColor = [System.Drawing.Color]::Lime
  $f.add_HandleCreated({
    param($s, $e)
    # WS_EX_TRANSPARENT = click-through. Never add WS_EX_NOACTIVATE (it stops rendering).
    $ex = [NativeWin]::GetWindowLong($s.Handle, -20)
    [NativeWin]::SetWindowLong($s.Handle, -20, $ex -bor 0x20) | Out-Null
  })
  $f.Show()
  $f.Visible = $false        # start hidden
  [void]$forms.Add($f)
}

# ---- movement state ----
$rnd = New-Object System.Random
$p = New-Object NativeWin+POINT
[NativeWin]::GetCursorPos([ref]$p) | Out-Null
$cx = $p.X; $cy = $p.Y
$sx = $cx; $sy = $cy
$tx = $cx; $ty = $cy
$step = 0
$steps = 0
$dwell = 0
$edgePending = $false
$sw = [System.Diagnostics.Stopwatch]::StartNew()

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 10
$timer.add_Tick({
  if (([NativeWin]::GetAsyncKeyState(0x1B) -band 0x8000) -ne 0) {
    $timer.Stop(); foreach ($f in $forms) { $f.Close() }; [System.Windows.Forms.Application]::Exit(); return
  }
  if ($sw.Elapsed.TotalSeconds -ge 60) {
    $timer.Stop(); foreach ($f in $forms) { $f.Close() }; [System.Windows.Forms.Application]::Exit(); return
  }

  # --- movement / dwell ---
  $move = $true
  if ($dwell -gt 0) {
    $dwell--; $move = $false
  } elseif ($step -ge $steps) {
    if ($edgePending) {
      $edgePending = $false; $dwell = 75; $move = $false
    } else {
      $sx = $cx; $sy = $cy
      $edgePending = $false
      if ($rnd.NextDouble() -lt 0.45) {
        $edgePending = $true
        switch ($rnd.Next(0, 4)) {
          0 { $tx = $X;          $ty = $rnd.Next($Y, $Y + $H) }
          1 { $tx = $X + $W - 1; $ty = $rnd.Next($Y, $Y + $H) }
          2 { $ty = $Y;          $tx = $rnd.Next($X, $X + $W) }
          3 { $ty = $Y + $H - 1; $tx = $rnd.Next($X, $X + $W) }
        }
      } else {
        $tx = $rnd.Next($X, $X + $W)
        $ty = $rnd.Next($Y, $Y + $H)
      }
      $steps = $rnd.Next(15, 40)
      $step = 0
    }
  }

  if ($move) {
    $step++
    $t = $step / $steps
    $e = $t * $t * (3 - 2 * $t)
    $cx = [int][math]::Round($sx + ($tx - $sx) * $e)
    $cy = [int][math]::Round($sy + ($ty - $sy) * $e)
    [NativeWin]::SetCursorPos($cx, $cy) | Out-Null
  }

  # --- show the green border only while the cursor is at an edge ---
  $lx = $cx - $X
  $ly = $cy - $Y
  $near = ($lx -le $EDGE) -or ($ly -le $EDGE) -or
          ($lx -ge $W - 1 - $EDGE) -or ($ly -ge $H - 1 - $EDGE)
  foreach ($f in $forms) { if ($f.Visible -ne $near) { $f.Visible = $near } }
})

$timer.Start()
[System.Windows.Forms.Application]::EnableVisualStyles()
[System.Windows.Forms.Application]::Run()
"Animation finished."
