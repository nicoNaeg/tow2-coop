# Shared-key test: (1) both copies hold the same key in tow2coop.ini -> the client auto-joins; (2) different keys ->
# the host ignores the client. Two pair starts (~15 min), memory guard on. Usage: run_key.ps1 -Out <evidence dir>
param([Parameter(Mandatory)][string]$Out, [string[]]$Cases = @('same key', 'different keys'))
$ErrorActionPreference = 'Continue'
$hub = 'C:\coop-anything'; $qa = "$hub\local\tow2-coop\qa"; $cfg = "$qa\tow2.gamepool.json"
New-Item -ItemType Directory -Force $Out | Out-Null
Remove-Item "$Out\memguard.stop" -ErrorAction SilentlyContinue
Set-Location $hub
python -I local/tow2-coop/qa/owner_guard.py snapshot "$Out\owner-before.json"
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-File', "$qa\memguard.ps1", '-Out', $Out
foreach ($case in @(@{ name = 'same key'; keys = 'k3yA1', 'k3yA1'; test = 'test_autojoin.py' }, @{ name = 'different keys'; keys = 'k3yA1', 'k3yB2'; test = 'test_key.py' })) {
    if ($Cases -notcontains $case.name) { continue }
    "=== $($case.name) ($(Get-Date -Format HH:mm:ss))"
    Set-Location $hub
    foreach ($i in 0, 1) {
        python -I local/tow2-coop/qa/deploy.py --config $cfg --instance $i --key $case.keys[$i] | Out-Null
        python -I local/tow2-coop/qa/prep_instance.py --config $cfg --instance $i | Out-Null
    }
    python -I tools/gamepool/gamepool.py --config $cfg up --pairs 1 --stagger 60 2>&1 | Select-Object -Last 2
    # restored behind the owner's windows: a minimized copy never finishes a save (run 32) and grew to 19 GB (run 34)
    Get-Process -Name 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$hub\local\gamepool\instances\*" } | ForEach-Object { & "$qa\unminimize.ps1" -ProcessId $_.Id | Out-Null }
    Set-Location $qa
    python -I session.py --instances 0 2>&1 | Select-Object -Last 1
    python -I $case.test 2>&1
    if ($case.name -eq 'same key') {  # the keyed pair also replays the run-31 melee sequence, with pictures
        Start-Sleep 15
        $p = foreach ($i in 0, 1) { (Get-Content "$hub\local\gamepool\instances\tow2\$i\sandbox\sandbox.log" | Select-String 'sandbox on: pid (\d+)' | Select-Object -Last 1).Matches[0].Groups[1].Value }
        python -I test_npc_sync.py 2>&1 | Select-Object -Last 1
        python -I test_melee.py $p[0] 0 $Out 2>&1
        python -I test_melee.py $p[1] 1 $Out 2>&1
    }
    & "$qa\close_copies.ps1"
    Set-Location $hub
    python -I tools/gamepool/gamepool.py --config $cfg down 2>&1 | Select-Object -Last 1
    Start-Sleep 5
}
foreach ($i in 0, 1) { python -I local/tow2-coop/qa/deploy.py --config $cfg --instance $i | Out-Null }  # back to no ini
New-Item -ItemType File -Force "$Out\memguard.stop" | Out-Null
if (Test-Path "$Out\memguard.log") { Get-Content "$Out\memguard.log" }
python -I local/tow2-coop/qa/owner_guard.py compare "$Out\owner-before.json"
