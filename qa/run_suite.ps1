# Full co-op regression on one PC (two test copies). Writes every test's output to <Out>\suite.log and a summary.
# Usage: run_suite.ps1 -Out <evidence folder>
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Continue'
$hub = 'C:\coop-anything'; $qa = "$hub\local\tow2-coop\qa"; $cfg = "$qa\tow2.gamepool.json"
New-Item -ItemType Directory -Force $Out | Out-Null
$log = "$Out\suite.log"; $results = [ordered]@{}
function Step([string]$name, [scriptblock]$body) {
    "=== $name ($(Get-Date -Format HH:mm:ss))" | Tee-Object -FilePath $log -Append | Out-Host
    $text = & $body 2>&1 | Out-String
    $text | Tee-Object -FilePath $log -Append | Out-Host
    $m = [regex]::Match($text, 'RESULT: (PASS|FAIL)')
    if ($m.Success) { $results[$name] = $m.Groups[1].Value }
}
function Pids { foreach ($i in 0, 1) { (Get-Content "$hub\local\gamepool\instances\tow2\$i\sandbox\sandbox.log" | Select-String 'sandbox on: pid (\d+)' | Select-Object -Last 1).Matches[0].Groups[1].Value } }
function Up([switch]$KeepSaves) {
    Set-Location $hub
    foreach ($i in 0, 1) {
        python -I local/tow2-coop/qa/deploy.py --config $cfg --instance $i | Out-Null
        if ($KeepSaves) { python -I local/tow2-coop/qa/prep_instance.py --config $cfg --instance $i --keep-saves | Out-Null }
        else { python -I local/tow2-coop/qa/prep_instance.py --config $cfg --instance $i | Out-Null }
        $cr = "$hub\local\gamepool\instances\tow2\$i\sandbox\LocalAppData\Arkansas\Saved\Crashes"; if (Test-Path $cr) { Remove-Item $cr -Recurse -Force }
    }
    # --stagger: the client starts only after the host's start-up memory peak (runs 25-29), so the two never add up
    python -I tools/gamepool/gamepool.py --config $cfg up --pairs 1 --stagger 60 2>&1 | Select-Object -Last 2 | Out-Host
    Get-Process -Name 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$hub\local\gamepool\instances\*" } | ForEach-Object { & "$qa\unminimize.ps1" -ProcessId $_.Id | Out-Null }
    Set-Location $qa
}
function Down { & "$qa\close_copies.ps1" | Out-Host; Set-Location $hub; python -I tools/gamepool/gamepool.py --config $cfg down 2>&1 | Select-Object -Last 1 | Out-Host; Start-Sleep 5; Set-Location $qa }

Set-Location $hub
python -I local/tow2-coop/qa/owner_guard.py snapshot "$Out\owner-before.json" | Out-Host
# Stops the copies if free RAM or commit runs low (this PC: 2.8 GB fixed page file); log in <Out>\memory.csv
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-File', "$qa\memguard.ps1", '-Out', $Out
Up
Step 'host load' { python -I session.py --instances 0 }
Step 'auto-join' { python -I test_autojoin.py }
Start-Sleep 15
$p = Pids
Step 'npc authority' { python -I test_npc_sync.py }
Step 'melee host' { python -I test_melee.py $p[0] 0 $Out }
Step 'melee client' { python -I test_melee.py $p[1] 1 $Out }
Step 'story' { python -I test_story.py }
Step 'interact' { python -I test_interact.py }
Step 'resync quest outcome' { python -I test_resync.py A }
Step 'resync map travel' { python -I test_resync.py B }
Step 'resume save' { python -I test_resume.py save "$Out\resume-facts.json" }
Down
Up -KeepSaves
Step 'host load CoopResume' { python -I session.py --instances 0 --save CoopResume }
Step 'auto-join after restart' { python -I test_autojoin.py }
Start-Sleep 20
Step 'resume check' { python -I test_resume.py check "$Out\resume-facts.json" }
foreach ($i in 0, 1) { Get-ChildItem "$hub\local\gamepool\instances\tow2\$i\sandbox\LocalAppData\Arkansas\Saved\Crashes" -ErrorAction SilentlyContinue | ForEach-Object { "crash report in copy ${i}: $($_.Name)" | Tee-Object -FilePath $log -Append; Copy-Item $_.FullName $Out -Recurse } }
Down
New-Item -ItemType File -Force "$Out\memguard.stop" | Out-Null
if (Test-Path "$Out\memguard.log") { Get-Content "$Out\memguard.log" | Tee-Object -FilePath $log -Append }
Set-Location $hub
$owner =python -I local/tow2-coop/qa/owner_guard.py compare "$Out\owner-before.json" 2>&1 | Out-String
$owner | Tee-Object -FilePath $log -Append | Out-Host
"=== SUMMARY" | Tee-Object -FilePath $log -Append
foreach ($k in $results.Keys) { "{0,-28} {1}" -f $k, $results[$k] | Tee-Object -FilePath $log -Append }
