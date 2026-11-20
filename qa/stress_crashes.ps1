# Crash hunt (runs 161 and 164: a game crash on arriving in 0605_OAHQ, one at a copy's start, each seen once):
# a joined pair travels to the Order's headquarters and back (stress_travel.py), then copies are started and closed
# several times. Every crash report folder and every first-chance access violation the test copies note is listed.
# Usage: stress_crashes.ps1 -Out <evidence folder> [-Rounds 5] [-Starts 5]
param([Parameter(Mandatory)][string]$Out, [int]$Rounds = 5, [int]$Starts = 5)
$qa = $PSScriptRoot; $hub = (Resolve-Path "$qa\..\..\..").Path; $cfg = "$qa\tow2.gamepool.json"
function Stop-Pair { & "$qa\close_copies.ps1" | Out-Null; Push-Location $hub; python -I tools/gamepool/gamepool.py --config $cfg down 2>&1 | Out-Null; Pop-Location }
function Get-Crashes { foreach ($i in 0, 1) { Get-ChildItem "$hub\local\gamepool\instances\tow2\$i\sandbox\LocalAppData\Arkansas\Saved\Crashes" -ErrorAction SilentlyContinue | ForEach-Object { "copy ${i}: $($_.Name)" } } }
$before = @(Get-Crashes)
& "$qa\run_quick.ps1" -Out "$Out\travel" -Stay *> "$Out-travel-console.txt"
Push-Location $qa; python -I stress_travel.py $Rounds; Pop-Location
Stop-Pair; New-Item -ItemType File -Force "$Out\travel\memguard.stop" | Out-Null
foreach ($k in 1..$Starts) {
    & "$qa\run_quick.ps1" -Out "$Out\start$k" -Bare *> "$Out-start$k-console.txt"
    Start-Sleep 20
    $up = @(Get-Process -Name TheOuterWorlds2-Win64-Shipping -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$hub\local\gamepool\instances\*" }).Count
    "start ${k}: $up of 2 copies running 20 s after both were ready"
    Stop-Pair; New-Item -ItemType File -Force "$Out\start$k\memguard.stop" | Out-Null
}
$new = @(Get-Crashes | Where-Object { $_ -notin $before })
"crash reports: $($new.Count) new"; $new
foreach ($i in 0, 1) { Select-String -Path "$hub\local\gamepool\instances\tow2\$i\sandbox\tow2coop.log" -Pattern 'fault: access violation' -ErrorAction SilentlyContinue | Select-Object -Last 4 | ForEach-Object { "fault note copy ${i}: $($_.Line)" } }
