# Memory guard for two-copy runs. Checks free RAM and free commit every second (a copy once grew 16 GB in 10 s) and
# stops the test copies (by PID, only processes under local\gamepool\instances) when either runs low, before Windows
# has to. Every 10 s it logs free RAM, free commit and each copy's memory to <Out>\memory.csv.
# Runs until <Out>\memguard.stop exists, or 3 hours.
# Usage: Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile','-File',<this>,'-Out',<dir>
param([Parameter(Mandatory)][string]$Out, [double]$MinCommitGB = 1.5, [double]$MinRamGB = 2.0, [int]$LogEvery = 10)
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class MemStat {
    [StructLayout(LayoutKind.Sequential)]
    public struct MEMORYSTATUSEX { public uint dwLength, dwMemoryLoad; public ulong ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual; }
    [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX m);
    public static double[] FreeGB() {  // free physical RAM, free commit (GB)
        var m = new MEMORYSTATUSEX(); m.dwLength = (uint)Marshal.SizeOf(m); GlobalMemoryStatusEx(ref m);
        return new double[] { m.ullAvailPhys / 1073741824.0, m.ullAvailPageFile / 1073741824.0 };
    }
}
'@
$root = 'C:\coop-anything\local\gamepool\instances\'
$inv = [cultureinfo]::InvariantCulture
New-Item -ItemType Directory -Force $Out | Out-Null
$csv = Join-Path $Out 'memory.csv'
if (-not (Test-Path $csv)) { 'time,freeRamGB,freeCommitGB,copies pid:workingSetMB/privateMB' | Set-Content $csv -Encoding utf8 }
$t0 = Get-Date; $n = 0
while (-not (Test-Path (Join-Path $Out 'memguard.stop')) -and ((Get-Date) - $t0).TotalHours -lt 3) {
    $ram, $commit = [MemStat]::FreeGB()
    $low = $commit -lt $MinCommitGB -or $ram -lt $MinRamGB
    if ($low -or $n % $LogEvery -eq 0) {
        $copies = @(Get-Process -Name 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" })
        $desc = ($copies | ForEach-Object { '{0}:{1}/{2}' -f $_.Id, [int]($_.WorkingSet64 / 1MB), [int]($_.PrivateMemorySize64 / 1MB) }) -join ' '
        [string]::Format($inv, '{0:HH:mm:ss},{1:F2},{2:F2},{3}', (Get-Date), $ram, $commit, $desc) | Add-Content $csv -Encoding utf8
        if ($low -and $copies.Count) {
            [string]::Format($inv, '{0:HH:mm:ss} LOW MEMORY (free RAM {1:F1} GB, free commit {2:F1} GB): stopping test copies {3}', (Get-Date), $ram, $commit, $desc) |
                Add-Content (Join-Path $Out 'memguard.log') -Encoding utf8
            $copies | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
        }
    }
    $n++
    Start-Sleep -Milliseconds 1000
}
