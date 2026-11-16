# Post a left click at client coordinates (x, y) to one process's main window through its own message queue:
# no focus change, the real cursor does not move. Usage: postclick.ps1 -ProcessId <pid> -X 325 -Y 225
param([Parameter(Mandatory)][int]$ProcessId, [Parameter(Mandatory)][int]$X, [Parameter(Mandatory)][int]$Y, [int]$HoldMs = 60)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class PC {
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetDpiForSystem();
}
'@
$p = Get-Process -Id $ProcessId
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "process $ProcessId has no main window" }
# X/Y are pixels of a winshot.ps1 picture; the game reads posted coordinates in DPI-scaled units, so divide by the
# system scale (measured: a click at y 225 hit the item drawn at y 336 on a 150% display).
$dpi = [PC]::GetDpiForSystem(); if ($dpi -le 0) { $dpi = 96 }
$X = [int][Math]::Round($X * 96 / $dpi); $Y = [int][Math]::Round($Y * 96 / $dpi)
$l = [IntPtr](($Y -shl 16) -bor ($X -band 0xFFFF))
[PC]::PostMessageW($h, 0x0200, [IntPtr]0, $l) | Out-Null   # WM_MOUSEMOVE
Start-Sleep -Milliseconds 50
[PC]::PostMessageW($h, 0x0201, [IntPtr]1, $l) | Out-Null   # WM_LBUTTONDOWN, MK_LBUTTON
Start-Sleep -Milliseconds $HoldMs
[PC]::PostMessageW($h, 0x0202, [IntPtr]0, $l) | Out-Null   # WM_LBUTTONUP
"posted click ($X,$Y) to pid $ProcessId"
