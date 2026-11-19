# Full co-op regression on one PC (two test copies). Writes every test's output to <Out>\suite.log and a summary.
# Usage: run_suite.ps1 -Out <evidence folder> [-Warm]
# -Warm: first start copy 0 alone and close it normally, so the graphics driver refills its shader cache (an emptied
# cache made each start reach 7-17 GB in run 49, which left no room for a map load with two copies).
param([Parameter(Mandatory)][string]$Out, [switch]$Warm)
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
function Pids {  # the running copy under each instance folder (the sandbox log's last pid can be an earlier copy's, run 114)
    foreach ($i in 0, 1) { (Get-Process -Name 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$hub\local\gamepool\instances\tow2\$i\*" } | Select-Object -First 1).Id }
}
function Up([switch]$KeepSaves) {
    Set-Location $hub
    foreach ($i in 0, 1) {
        python -I local/tow2-coop/qa/deploy.py --config $cfg --instance $i | Out-Null
        if ($KeepSaves) { python -I local/tow2-coop/qa/prep_instance.py --config $cfg --instance $i --keep-saves | Out-Null }
        else { python -I local/tow2-coop/qa/prep_instance.py --config $cfg --instance $i | Out-Null }
        $cr = "$hub\local\gamepool\instances\tow2\$i\sandbox\LocalAppData\Arkansas\Saved\Crashes"; if (Test-Path $cr) { Remove-Item $cr -Recurse -Force }
        $kept = "$hub\local\gamepool\instances\tow2\$i\sandbox\LocalAppData\tow2coop\characters"; if (Test-Path $kept) { Remove-Item $kept -Recurse -Force }  # a first join: no character kept from an earlier run
    }
    # --stagger: the client starts only after the host's start-up memory peak (runs 25-29), so the two never add up
    python -I tools/gamepool/gamepool.py --config $cfg up --pairs 1 --stagger 60 2>&1 | Select-Object -Last 2 | Out-Host
    Get-Process -Name 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$hub\local\gamepool\instances\*" } | ForEach-Object { & "$qa\unminimize.ps1" -ProcessId $_.Id | Out-Null }
    Set-Location $qa
}
function Down { & "$qa\close_copies.ps1" | Out-Host; Set-Location $hub; python -I tools/gamepool/gamepool.py --config $cfg down 2>&1 | Select-Object -Last 1 | Out-Host; Start-Sleep 5; Set-Location $qa }

if ($Warm) { & "$qa\warmup.ps1" -Out "$Out\warmup" -Instance 0 -Seconds 150 | Tee-Object -FilePath $log -Append | Out-Host }
Set-Location $hub
python -I local/tow2-coop/qa/owner_guard.py snapshot "$Out\owner-before.json" | Out-Host
# The copies' core logs grow across runs: remember where this run starts (the 'no caught faults' step reads from there).
$coreLogs = foreach ($i in 0, 1) { "$hub\local\gamepool\instances\tow2\$i\sandbox\tow2coop.log" }
$logStart = @{}; foreach ($f in $coreLogs) { $logStart[$f] = if (Test-Path $f) { (Get-Item $f).Length } else { 0 } }
# Stops the copies if free RAM or commit runs low (this PC: 2.8 GB fixed page file); log in <Out>\memory.csv
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-File', "$qa\memguard.ps1", '-Out', $Out
Up
Step 'host load' { python -I session.py --instances 0 }
Step 'auto-join' { python -I test_autojoin.py }
Step 'client saves blocked' { python -I test_client_saves.py "$hub\local\gamepool\instances\tow2\1\sandbox" }
Step 'players invulnerable' { python -I god.py }  # bots stand still in fights; a dead host cannot save for a hand-over
Start-Sleep 15
$p = Pids
Step 'npc authority' { python -I test_npc_sync.py }
Step 'melee host' { python -I test_melee.py $p[0] 0 $Out }
Step 'melee client' { python -I test_melee.py $p[1] 1 $Out }
Step 'story' { python -I test_story.py }
Step 'interact' { python -I test_interact.py }
Step 'outfit follows' { python -I test_look.py }
Step 'stand-in animation' { python -I test_anim.py $p[1] }
Step 'party XP' { python -I test_xp.py }
Step 'give items' { python -I test_give.py $p[0] $p[1] }
Step 'menus do not pause' { python -I test_nopause.py }
Step 'shared conversations' { python -I test_conversation.py }
Step 'companions follow the host' { python -I test_party.py }
Step 'minimized host saves' { python -I test_minimized.py }
Step 'public game (one account)' { python -I test_lobby.py }
Step 'co-op combat' { python -I test_combat.py ABCDE $p[0] }  # first (a calm start), before the reloads: they bring the NPCs back
Step 'weapon and gestures' { python -I test_gestures.py ABCDEFGHIJ }
Step 'spawned enemies' { python -I test_spawned.py ABCD }
Step 'death and reload' { python -I test_death.py ABCD $p[0] }
Step 'world sync' { python -I test_world.py $p[0] }
Step 'story both ways' { python -I test_resync.py AS }
Step 'resync map travel' { python -I test_resync.py B2C }  # host leads (followed), host leads (fallback), client leads
Step 'client saves (session)' { python -I test_client_saves.py "$hub\local\gamepool\instances\tow2\1\sandbox" session "$hub\local\the-outer-worlds-2\test-saves\TheOuterWorlds2" }
Step 'resume save' { python -I test_resume.py save "$Out\resume-facts.json" }
Down
Up -KeepSaves
Step 'host load CoopResume' { python -I session.py --instances 0 --save CoopResume }
Step 'auto-join after restart' { python -I test_autojoin.py }
Start-Sleep 20
Step 'resume check' { python -I test_resume.py check "$Out\resume-facts.json" }
foreach ($i in 0, 1) { Get-ChildItem "$hub\local\gamepool\instances\tow2\$i\sandbox\LocalAppData\Arkansas\Saved\Crashes" -ErrorAction SilentlyContinue | ForEach-Object { "crash report in copy ${i}: $($_.Name)" | Tee-Object -FilePath $log -Append; Copy-Item $_.FullName $Out -Recurse } }
Step 'no caught faults' {  # a fault the core's guard caught aborts that frame's sync (run 50: old world after a load)
    $bad = @(foreach ($f in $coreLogs) {
        if (-not (Test-Path $f)) { continue }
        $fs = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek([math]::Min($logStart[$f], $fs.Length), 'Begin')
        $text = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close()
        $text -split "`n" | Where-Object { $_ -match 'co-op frame error' } | ForEach-Object { "copy $(Split-Path -Leaf (Split-Path (Split-Path $f))): $_" }
    })
    $bad | Select-Object -First 8
    "RESULT: $(if ($bad.Count) { 'FAIL' } else { 'PASS' }) ($($bad.Count) failure(s))"
}
Down
New-Item -ItemType File -Force "$Out\memguard.stop" | Out-Null
if (Test-Path "$Out\memguard.log") { Get-Content "$Out\memguard.log" | Tee-Object -FilePath $log -Append }
Set-Location $hub
$owner =python -I local/tow2-coop/qa/owner_guard.py compare "$Out\owner-before.json" 2>&1 | Out-String
$owner | Tee-Object -FilePath $log -Append | Out-Host
"=== SUMMARY" | Tee-Object -FilePath $log -Append
foreach ($k in $results.Keys) { "{0,-28} {1}" -f $k, $results[$k] | Tee-Object -FilePath $log -Append }
