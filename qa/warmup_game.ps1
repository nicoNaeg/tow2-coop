# Start one test copy alone, load the test save into gameplay and stay there, so the graphics driver can fill its
# shader cache for the overland (after the cache was cleared, the first gameplay load reached 19 GB: warm-up 158).
# Memory every 2 s; the memory guard stops the copy as usual (-MinRamGB: its free-RAM floor, 2 by default). Usage: warmup_game.ps1 -Out <evidence dir> [-Instance 0] [-Seconds 90]
param([Parameter(Mandatory)][string]$Out, [int]$Instance = 0, [int]$Seconds = 90, [double]$MinRamGB = 2.0)
if (-not (Get-Process -Name steam -ErrorAction SilentlyContinue)) { throw 'Steam is not running: a copy started without it exits (code 53) and Steam then starts the real game install. Start Steam first.' }
$hub = 'C:\coop-anything'; $qa = "$hub\local\tow2-coop\qa"; $cfg = "$qa\tow2.gamepool.json"
New-Item -ItemType Directory -Force $Out | Out-Null
Remove-Item "$Out\memguard.stop" -ErrorAction SilentlyContinue
Set-Location $hub
python -I local/tow2-coop/qa/deploy.py --config $cfg --instance $Instance | Out-Null
python -I local/tow2-coop/qa/prep_instance.py --config $cfg --instance $Instance | Out-Null
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-File', "$qa\memguard.ps1", '-Out', $Out, '-LogEvery', 2, '-MinRamGB', $MinRamGB
python -I local/tow2-coop/qa/up_one.py $cfg $Instance 2>&1 | Select-Object -Last 1
Set-Location $qa
python -I session.py --instances $Instance 2>&1 | Select-Object -Last 1
Start-Sleep $Seconds
& "$qa\close_copies.ps1" | Out-Null
Set-Location $hub
python -I tools/gamepool/gamepool.py --config $cfg down 2>&1 | Select-Object -Last 1
New-Item -ItemType File -Force "$Out\memguard.stop" | Out-Null
Start-Sleep 3
if (Test-Path "$Out\memguard.log") { Get-Content "$Out\memguard.log" }
python -I -c "import re,sys; rows=open(sys.argv[1],encoding='utf-8-sig').read().splitlines()[1:]; s=[(int(m[2]),float(r.split(',')[1])) for r in rows for m in re.findall(r'(\d+):(\d+)/(\d+)',r)]; print('peak private MB', max(x[0] for x in s), 'min free RAM GB', min(x[1] for x in s))" "$Out\memory.csv"
Get-ChildItem "$env:LOCALAPPDATA\NVIDIA\DXCache" -File | Sort-Object LastWriteTime -Descending | Select-Object -First 2 | ForEach-Object { '  cache {0,6:N0} MB {1:HH:mm:ss}' -f ($_.Length / 1MB), $_.LastWriteTime }
