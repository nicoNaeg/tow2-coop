# Full soak: start a pair, load the save in both, drive both players for N minutes with checks, stop, compare owner state.
# Usage: soak_run.ps1 -Minutes 30 -Out <evidence folder>
param([double]$Minutes = 30, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Continue'
$hub = 'C:\coop-anything'; $qa = "$hub\local\tow2-coop\qa"; $cfg = "$qa\tow2.gamepool.json"
New-Item -ItemType Directory -Force $Out | Out-Null
Set-Location $hub
python -I local/tow2-coop/qa/owner_guard.py snapshot "$Out\owner-before.json"
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-File', "$qa\memguard.ps1", '-Out', $Out  # see run_suite.ps1
foreach ($i in 0, 1) {
    python -I local/tow2-coop/qa/deploy.py --config $cfg --instance $i | Out-Null  # also restores steam_appid.txt
    python -I local/tow2-coop/qa/prep_instance.py --config $cfg --instance $i | Out-Null
}
python -I tools/gamepool/gamepool.py --config $cfg up --pairs 1 --stagger 60 2>&1 | Select-Object -Last 3  # see run_suite.ps1
Get-Process -Name 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$hub\local\gamepool\instances\*" } | ForEach-Object { & "$qa\unminimize.ps1" -ProcessId $_.Id | Out-Null }
Set-Location $qa
python -I session.py 2>&1 | Select-Object -Last 2
Start-Sleep 15
$pids = foreach ($i in 0, 1) { (Get-Content "$hub\local\gamepool\instances\tow2\$i\sandbox\sandbox.log" | Select-String 'sandbox on: pid (\d+)' | Select-Object -Last 1).Matches[0].Groups[1].Value }
"host pid $($pids[0]) client pid $($pids[1])"
python -I soak.py $pids[0] $pids[1] "$Out\soak.jsonl" $Minutes 2>&1 | Select-Object -Last 1
foreach ($i in 0, 1) { Get-ChildItem "$hub\local\gamepool\instances\tow2\$i\sandbox\LocalAppData\Arkansas\Saved\Crashes" -ErrorAction SilentlyContinue | ForEach-Object { "crash report in copy ${i}: $($_.Name)"; Copy-Item $_.FullName $Out -Recurse } }
& "$qa\close_copies.ps1"
Set-Location $hub
python -I tools/gamepool/gamepool.py --config $cfg down 2>&1 | Select-Object -Last 1
New-Item -ItemType File -Force "$Out\memguard.stop" | Out-Null
if (Test-Path "$Out\memguard.log") { Get-Content "$Out\memguard.log" }
Start-Sleep 5
python -I local/tow2-coop/qa/owner_guard.py compare "$Out\owner-before.json"
