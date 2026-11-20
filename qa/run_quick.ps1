# Targeted regression on one pair for the two-PC fixes (stand-in look and animation, interactions incl. dead-NPC loot,
# map-travel hand-over), plus the owner-state and caught-fault checks. The whole suite is run_suite.ps1.
# Usage: run_quick.ps1 -Out <evidence folder> [-NoRamCheck] [-Only 'interact and loot','...'] [-Menu] (the join steps always run;
# -NewGame: menu mode, copy 1 without saves of its own, then only the new-game-together test; -Bare: only start the copies; -Stay: leave the joined pair running after god mode, for probes; -Menu: the copies start in menu mode and the players host and join through the game's main menu instead)
param([Parameter(Mandatory)][string]$Out, [switch]$NoRamCheck, [string[]]$Only, [switch]$Warm, [switch]$Menu, [switch]$Stay, [switch]$Bare, [switch]$NewGame)
if (-not (Get-Process -Name steam -ErrorAction SilentlyContinue)) { throw 'Steam is not running: a copy started without it exits (code 53) and Steam then starts the real game install. Start Steam first.' }
$ErrorActionPreference = 'Continue'
$hub = 'C:\coop-anything'; $qa = "$hub\local\tow2-coop\qa"; $cfg = "$qa\tow2.gamepool.json"
New-Item -ItemType Directory -Force $Out | Out-Null
$log = "$Out\suite.log"; $results = [ordered]@{}
function Step([string]$name, [scriptblock]$body) {
    if ($Only -and $name -notin $Only -and $name -notin 'host load', 'auto-join', 'co-op menu', 'players invulnerable', 'no caught faults') { return }
    if ($NewGame -and $name -notin 'new game together', 'no caught faults') { return }  # its own run: the rest needs the usual start
    "=== $name ($(Get-Date -Format HH:mm:ss))" | Tee-Object -FilePath $log -Append | Out-Host
    $text = & $body 2>&1 | Out-String
    $text | Tee-Object -FilePath $log -Append | Out-Host
    $m = [regex]::Match($text, 'RESULT: (PASS|FAIL)')
    if ($m.Success) { $results[$name] = $m.Groups[1].Value }
}
function Pids {  # the running copy under each instance folder (the sandbox log's last pid can be an earlier copy's, run 114)
    foreach ($i in 0, 1) { (Get-Process -Name 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$hub\local\gamepool\instances\tow2\$i\*" } | Select-Object -First 1).Id }
}
if ($Warm) { foreach ($w in 0, 1) { & "$qa\warmup.ps1" -Out "$Out\warmup$w" -Instance $w -Seconds 150 | Tee-Object -FilePath $log -Append | Out-Host } }  # refill the driver's shader cache (run 67: 6.5 GB at the menu)
Set-Location $hub
python -I local/tow2-coop/qa/owner_guard.py snapshot "$Out\owner-before.json" | Out-Host
$coreLogs = foreach ($i in 0, 1) { "$hub\local\gamepool\instances\tow2\$i\sandbox\tow2coop.log" }
$logStart = @{}; foreach ($f in $coreLogs) { $logStart[$f] = if (Test-Path $f) { (Get-Item $f).Length } else { 0 } }
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-File', "$qa\memguard.ps1", '-Out', $Out
if ($NewGame) { $Menu = $true; New-Item -ItemType Directory -Force "$Out\nosaves\TheOuterWorlds2" | Out-Null }
foreach ($i in 0, 1) {
    python -I local/tow2-coop/qa/deploy.py --config $cfg --instance $i | Out-Null
    $saves = @(); if ($NewGame -and $i -eq 1) { $saves = @('--saves', "$Out\nosaves\TheOuterWorlds2") }  # a player with no character yet
    python -I local/tow2-coop/qa/prep_instance.py --config $cfg --instance $i @saves | Out-Null
    $cr = "$hub\local\gamepool\instances\tow2\$i\sandbox\LocalAppData\Arkansas\Saved\Crashes"; if (Test-Path $cr) { Remove-Item $cr -Recurse -Force }
    $kept = "$hub\local\gamepool\instances\tow2\$i\sandbox\LocalAppData\tow2coop\characters"; if (Test-Path $kept) { Remove-Item $kept -Recurse -Force }  # a first join: no character kept from an earlier run
}
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })  # -File passes 'a,b' as one string
$extra = @(); if ($NoRamCheck) { $extra += '--no-ram-check' }
if ($Menu) { $env:TOW2_COOP_MENU = '1' } else { Remove-Item Env:TOW2_COOP_MENU -ErrorAction SilentlyContinue }  # the copies inherit it
python -I tools/gamepool/gamepool.py --config $cfg up --pairs 1 --stagger 60 @extra 2>&1 | Select-Object -Last 2 | Out-Host
Get-Process -Name 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$hub\local\gamepool\instances\*" } | ForEach-Object { & "$qa\unminimize.ps1" -ProcessId $_.Id | Out-Null }
Set-Location $qa
if ($Bare) { "copies started, no steps (-Bare); stop them: close_copies.ps1, gamepool down, then $Out\memguard.stop"; return }
if ($NewGame) { $p = Pids; Step 'new game together' { python -I test_newgame.py $p[0] $p[1] } }
elseif ($Menu) { $p = Pids; Step 'co-op menu' { python -I test_menu.py $p[0] $p[1] } }
else { Step 'host load' { python -I session.py --instances 0 }; Step 'auto-join' { python -I test_autojoin.py } }
Step 'players invulnerable' { python -I god.py }
if ($Stay) { "the pair stays up for probes; stop it: close_copies.ps1, gamepool down, then $Out\memguard.stop"; return }
Start-Sleep 10
$p = Pids
Step 'outfit and hair' { python -I test_look.py }
Step 'stand-in animation' { python -I test_anim.py $p[1] }
Step 'interact and loot' { python -I test_interact.py }
Step 'party XP' { python -I test_xp.py }
Step 'story both ways' { python -I test_resync.py AS }
Step 'give items' { python -I test_give.py $p[0] $p[1] }
Step 'own character' { python -I test_character.py }
Step 'menus do not pause' { python -I test_nopause.py }
Step 'shared conversations' { python -I test_conversation.py }
Step 'companions follow the host' { python -I test_party.py }
Step 'minimized host saves' { python -I test_minimized.py }
Step 'public game (one account)' { python -I test_lobby.py }
Step 'co-op combat' { python -I test_combat.py ABCDE $p[0] }  # first (a calm start), before the reloads: they bring the NPCs back
python -I god.py | Out-Null  # the steps before may leave a player down or vulnerable (runs 168-169)
Step 'weapon and gestures' { python -I test_gestures.py ABCDEFGHIJ }
python -I god.py | Out-Null  # the steps before may leave a player down or vulnerable (runs 168-169)
Step 'spawned enemies' { python -I test_spawned.py ABCD }
python -I god.py | Out-Null
Step 'hive waves' { python -I test_waves.py }
python -I god.py | Out-Null  # the steps before may leave a player down or vulnerable (runs 168-169)
Step 'death and reload' { python -I test_death.py ABCD $p[0] }
python -I god.py | Out-Null  # the steps before may leave a player down or vulnerable (runs 168-169)
Step 'world sync' { python -I test_world.py $p[0] }
python -I god.py | Out-Null  # the steps before may leave a player down or vulnerable (runs 168-169)
Step 'resync map travel' { python -I test_resync.py B2C }  # host leads (followed), host leads (fallback), client leads
foreach ($i in 0, 1) { Get-ChildItem "$hub\local\gamepool\instances\tow2\$i\sandbox\LocalAppData\Arkansas\Saved\Crashes" -ErrorAction SilentlyContinue | ForEach-Object { "crash report in copy ${i}: $($_.Name)" | Tee-Object -FilePath $log -Append } }
Step 'no caught faults' {
    $bad = @(foreach ($f in $coreLogs) {
        if (-not (Test-Path $f)) { continue }
        $fs = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek([math]::Min($logStart[$f], $fs.Length), 'Begin')
        $text = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close()
        $text -split "`n" | Where-Object { $_ -match 'co-op frame error|join error' } | ForEach-Object { "copy $(Split-Path -Leaf (Split-Path (Split-Path $f))): $_" }
    })
    $bad | Select-Object -First 8
    "RESULT: $(if ($bad.Count) { 'FAIL' } else { 'PASS' }) ($($bad.Count) failure(s))"
}
foreach ($f in $coreLogs) { if (Test-Path $f) { Select-String -Path $f -Pattern 'fault: access violation' | Select-Object -Last 6 | ForEach-Object { "fault note: $($_.Line)" | Tee-Object -FilePath $log -Append } } }  # first-chance, test copies only
& "$qa\close_copies.ps1" | Out-Host
Set-Location $hub; python -I tools/gamepool/gamepool.py --config $cfg down 2>&1 | Select-Object -Last 1 | Out-Host
New-Item -ItemType File -Force "$Out\memguard.stop" | Out-Null
if (Test-Path "$Out\memguard.log") { Get-Content "$Out\memguard.log" | Tee-Object -FilePath $log -Append }
python -I local/tow2-coop/qa/owner_guard.py compare "$Out\owner-before.json" 2>&1 | Out-String | Tee-Object -FilePath $log -Append | Out-Host
"=== SUMMARY" | Tee-Object -FilePath $log -Append
foreach ($k in $results.Keys) { "{0,-24} {1}" -f $k, $results[$k] | Tee-Object -FilePath $log -Append }
