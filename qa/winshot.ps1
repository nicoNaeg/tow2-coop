# Picture of one process's main window via PrintWindow (the window paints itself; nothing else on the
# screen can end up in it). Usage: winshot.ps1 -ProcessId <pid> -Out <file.png>
param([Parameter(Mandatory)][int]$ProcessId, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[void][W]::SetProcessDPIAware()  # real pixel sizes; otherwise a scaled desktop crops the picture
$p = Get-Process -Id $ProcessId
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "process $ProcessId has no main window" }
$r = New-Object W+RECT
[void][W]::GetClientRect($h, [ref]$r)
$state = if ([W]::IsIconic($h)) { 'minimized' } else { 'normal' }
$w = [Math]::Max(1, $r.R - $r.L); $hh = [Math]::Max(1, $r.B - $r.T)
"window '{0}' state={1} client={2}x{3}" -f $p.MainWindowTitle, $state, $w, $hh
if ($state -eq 'minimized') { return }
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$ok = [W]::PrintWindow($h, $dc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
"PrintWindow ok=$ok -> $Out"
