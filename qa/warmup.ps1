# Start one test copy alone to the main menu and leave it there, so the graphics driver can fill its shader cache for
# that copy (runs 25-29: copies reached 13-19 GB right after start while the cache was full). Memory every 2 s.
# Usage: warmup.ps1 -Out <evidence dir> -Instance 0 [-Seconds 180] [-KeepIni (keep a tow2coop.ini placed in the copy)]
param([Parameter(Mandatory)][string]$Out, [int]$Instance = 0, [int]$Seconds = 180, [switch]$KeepIni)
$extra = @(); if ($KeepIni) { $extra += '--keep-ini' }
$hub = 'C:\coop-anything'; $qa = "$hub\local\tow2-coop\qa"; $cfg = "$qa\tow2.gamepool.json"
New-Item -ItemType Directory -Force $Out | Out-Null
Remove-Item "$Out\memguard.stop" -ErrorAction SilentlyContinue
Set-Location $hub
python -I local/tow2-coop/qa/deploy.py --config $cfg --instance $Instance @extra | Out-Null
python -I local/tow2-coop/qa/prep_instance.py --config $cfg --instance $Instance | Out-Null
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-File', "$qa\memguard.ps1", '-Out', $Out, '-LogEvery', 2
$t0 = Get-Date
python -I local/tow2-coop/qa/up_one.py $cfg $Instance 2>&1 | Select-Object -Last 2
Start-Sleep $Seconds
& "$qa\close_copies.ps1"
python -I tools/gamepool/gamepool.py --config $cfg down 2>&1 | Select-Object -Last 1
New-Item -ItemType File -Force "$Out\memguard.stop" | Out-Null
Start-Sleep 3
if (Test-Path "$Out\memguard.log") { Get-Content "$Out\memguard.log" }
python -I -c "import re,sys; rows=open(sys.argv[1],encoding='utf-8-sig').read().splitlines()[1:]; s=[(r.split(',')[0],int(m[2]),int(m[1])) for r in rows for m in re.findall(r'(\d+):(\d+)/(\d+)',r)]; print(len(s),'samples; peak (time, private MB, working set MB):',max(s,key=lambda x:x[1]) if s else None,'; last:',s[-1] if s else None,'; min free commit GB:',min(float(r.split(',')[2]) for r in rows),'; min free RAM GB:',min(float(r.split(',')[1]) for r in rows))" "$Out\memory.csv"
"started {0:HH:mm:ss}; newest NVIDIA cache files:" -f $t0
Get-ChildItem "$env:LOCALAPPDATA\NVIDIA\DXCache" -File | Sort-Object LastWriteTime -Descending | Select-Object -First 4 | ForEach-Object { '  {0,6:N0} MB {1:HH:mm:ss} {2}' -f ($_.Length / 1MB), $_.LastWriteTime, $_.Name }
