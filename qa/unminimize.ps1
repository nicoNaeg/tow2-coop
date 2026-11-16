# Restore a game window without activating it and put it behind every other window: it renders at full speed
# (a minimized UE game throttles) but does not cover the owner's screen or take focus.
# Usage: unminimize.ps1 -ProcessId <pid>
param([Parameter(Mandatory)][int]$ProcessId)
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class UM {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
'@
$h = (Get-Process -Id $ProcessId).MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "no main window for pid $ProcessId" }
[void][UM]::ShowWindow($h, 4)                                # SW_SHOWNOACTIVATE
[void][UM]::SetWindowPos($h, [IntPtr]1, 0, 0, 0, 0, 0x0013)  # HWND_BOTTOM, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
"restored behind other windows: pid $ProcessId"
