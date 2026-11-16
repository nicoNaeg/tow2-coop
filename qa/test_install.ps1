# Installer check on a fake game folder in %TEMP% (never the real install): host and client settings, another mod's
# dsound.dll refused and left alone, uninstall removes only our files. Usage: test_install.ps1
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) "tow2-install-test-$PID"
$win64 = "$root\Arkansas\Binaries\Win64"
New-Item -ItemType Directory -Force $win64 | Out-Null
Set-Content "$win64\TheOuterWorlds2-Win64-Shipping.exe" 'fake'
$inst = Join-Path (Split-Path $PSScriptRoot) 'install.ps1'
$fails = 0
function Check($ok, $what) { if ($ok) { "PASS $what" } else { "FAIL $what"; $script:fails++ } }
& $inst -Role host -GameDir $root | Out-Null
$ini = Get-Content "$win64\tow2coop.ini"
Check ((Test-Path "$win64\dsound.dll") -and (Test-Path "$win64\tow2coop.dll") -and ($ini -contains 'role=host') -and ($ini -contains 'transport=steam')) 'host install places the loader, the mod, role=host over Steam'
$name = "Zo$([char]0xEB) $([char]0xDC)nder"  # built from code points: Windows PowerShell reads this file as ANSI
& $inst -Role client -Friend $name -GameDir $root | Out-Null
$raw = [IO.File]::ReadAllBytes("$win64\tow2coop.ini")
$ini = [Text.Encoding]::UTF8.GetString($raw) -split "`r`n"
Check (($ini -contains 'transport=steam') -and ($ini -contains "host=$name") -and $raw[0] -ne 0xEF -and -not ($ini -match '^key=')) 'Steam client install names the friend to join (UTF-8, no BOM), no key'
$said = & $inst -Role host -Transport udp -GameDir $root
$ini = Get-Content "$win64\tow2coop.ini"
$key = ($ini | Where-Object { $_ -match '^key=[a-z0-9]{10}$' }) -replace '^key='
Check ($key -and ($ini -contains 'transport=udp') -and "$said" -match "-Key $key") 'direct-IP host install makes a key and tells it for the friend'
$refused = $false
try { & $inst -Role client -Peer 10.0.0.5 -GameDir $root | Out-Null } catch { $refused = $true }
Check $refused 'direct-IP client install without the key is refused'
& $inst -Role client -Peer 10.0.0.5 -Key $key -GameDir $root | Out-Null
$ini = Get-Content "$win64\tow2coop.ini"
Check (($ini -contains 'peer=10.0.0.5:27960') -and ($ini -contains "key=$key") -and ($ini -contains 'transport=udp')) 'direct-IP client install writes the host address (default port) and the key'
& $inst -Uninstall -GameDir $root | Out-Null
Check (-not (Test-Path "$win64\dsound.dll") -and -not (Test-Path "$win64\tow2coop.dll") -and -not (Test-Path "$win64\tow2coop.ini")) 'uninstall removes every file it placed'
Check (Test-Path "$win64\TheOuterWorlds2-Win64-Shipping.exe") 'uninstall leaves the game alone'
Set-Content "$win64\dsound.dll" 'another mod'
$refused = $false
try { & $inst -Role host -GameDir $root | Out-Null } catch { $refused = $true }
Check ($refused -and (Get-Content "$win64\dsound.dll") -eq 'another mod' -and -not (Test-Path "$win64\tow2coop.dll")) "another mod's dsound.dll: install refused, nothing changed"
& $inst -Uninstall -GameDir $root 3>$null | Out-Null
Check ((Get-Content "$win64\dsound.dll") -eq 'another mod') "uninstall leaves another mod's dsound.dll"
Remove-Item $root -Recurse -Force
"RESULT: $(if ($fails) { 'FAIL' } else { 'PASS' }) ($fails failure(s))"
