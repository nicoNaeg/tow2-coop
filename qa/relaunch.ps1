# Start one test copy by hand with the same environment gamepool gives it (for disconnect/rejoin tests while the other
# copy keeps running). Usage: relaunch.ps1 -Instance 1 -Role client [-Port 27960]
param([Parameter(Mandatory)][int]$Instance, [Parameter(Mandatory)][ValidateSet('host', 'client', 'none')][string]$Role, [int]$Port = 27960)
$ErrorActionPreference = 'Stop'
$inst = "C:\coop-anything\local\gamepool\instances\tow2\$Instance"
$exe = "$inst\Arkansas\Binaries\Win64\TheOuterWorlds2-Win64-Shipping.exe"
if (-not (Test-Path "$inst\instance.json")) { throw "instance $Instance is not provisioned" }
$vars = @{ SteamAppId = '1449110'; SteamGameId = '1449110'; TOW2_SANDBOX_DIR = "$inst\sandbox"; TOW2_BRIDGE_PIPE = "tow2-$Instance";
           GAMEPOOL_ROLE = $Role; GAMEPOOL_PORT = "$Port"; GAMEPOOL_PAIR = '0' }
foreach ($k in $vars.Keys) { Set-Item "Env:$k" $vars[$k] }
try { $p = Start-Process -FilePath $exe -ArgumentList '-windowed', '-ResX=1280', '-ResY=720', '-nomovie' -WorkingDirectory (Split-Path $exe) -PassThru }
finally { foreach ($k in $vars.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue } }
"started instance $Instance as $Role, pid $($p.Id)"
