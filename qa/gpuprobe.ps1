# Which GPU does a starting test copy use, and how much of its memory is graphics memory? Every 2 s for -Seconds:
# the copy's private bytes, and its dedicated/shared GPU memory per adapter (Windows "GPU Process Memory" counters).
# Usage: gpuprobe.ps1 [-Seconds 40]   (start the copy first; the probe waits up to 60 s for it)
param([int]$Seconds = 40)
$root = 'C:\coop-anything\local\gamepool\instances\'
$adapters = @{}
Get-CimInstance Win32_VideoController | ForEach-Object { "adapter: $($_.Name) ($($_.PNPDeviceID.Split('\')[1]))" }
$p = $null
for ($k = 0; $k -lt 60 -and -not $p; $k++) {
    $p = Get-Process -Name 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Select-Object -First 1
    if (-not $p) { Start-Sleep 1 }
}
if (-not $p) { 'no test copy running'; exit 1 }
$t0 = $p.StartTime
for ($k = 0; $k -lt $Seconds / 2; $k++) {
    $q = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if (-not $q) { 'copy gone'; break }
    $gpu = @{}
    foreach ($c in 'Dedicated Usage', 'Shared Usage') {
        try {
            (Get-Counter "\GPU Process Memory(pid_$($p.Id)_*)\$c" -ErrorAction Stop).CounterSamples | ForEach-Object {
                $luid = ($_.InstanceName -split '_luid_')[1] -replace '_phys.*', ''
                $gpu["$luid $($c.Split(' ')[0])"] = [int]($_.CookedValue / 1MB)
            }
        } catch {}
    }
    $g = ($gpu.GetEnumerator() | Where-Object { $_.Value -gt 0 } | Sort-Object Name | ForEach-Object { "$($_.Name) $($_.Value) MB" }) -join '; '
    '{0,4:N0}s private {1,6:N0} MB | {2}' -f ((Get-Date) - $t0).TotalSeconds, ($q.PrivateMemorySize64 / 1MB), $g
    Start-Sleep 2
}
