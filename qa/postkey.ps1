# Post a key press (down + up) to one process's main window through its own message queue: no focus change,
# no OS-level input, other windows unaffected. Usage: postkey.ps1 -ProcessId <pid> -Key Space|Enter|Escape|Down|Up
param([Parameter(Mandatory)][int]$ProcessId, [Parameter(Mandatory)][string]$Key, [int]$HoldMs = 80)
$ErrorActionPreference = 'Stop'
$vk = @{ Space = 0x20; Enter = 0x0D; Escape = 0x1B; Down = 0x28; Up = 0x26; Left = 0x25; Right = 0x27; Tab = 0x09 }[$Key]
if (-not $vk -and $Key -match '^[A-Z0-9]$') { $vk = [int][char]$Key }  # letters and digits: VK code = ASCII
if (-not $vk) { throw "unknown key $Key" }
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class PK {
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint type);
}
'@
$h = (Get-Process -Id $ProcessId).MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "no main window for pid $ProcessId" }
$scan = [PK]::MapVirtualKeyW([uint32]$vk, 0)
$down = [IntPtr](1 -bor ($scan -shl 16))
$up = [IntPtr]([int64](1 -bor ($scan -shl 16) -bor (1 -shl 30)) -bor 0x80000000L)
[void][PK]::PostMessageW($h, 0x0100, [IntPtr]$vk, $down)   # WM_KEYDOWN
Start-Sleep -Milliseconds $HoldMs
[void][PK]::PostMessageW($h, 0x0101, [IntPtr]$vk, $up)     # WM_KEYUP
"posted $Key to pid $ProcessId"
