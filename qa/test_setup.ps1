# The players' installer (bin\tow2coop-setup.exe, its command line) on a fake game folder in %TEMP% (never the real
# install): Steam and direct-IP settings, another mod's dsound.dll refused and left alone, an older build of the mod
# replaced, uninstall removes only our files, the game's Win64 folder accepted too. Usage: test_setup.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$exe = Join-Path $repo 'bin\tow2coop-setup.exe'
$root = Join-Path ([IO.Path]::GetTempPath()) "tow2-setup-test-$PID"
$win64 = "$root\Arkansas\Binaries\Win64"
New-Item -ItemType Directory -Force $win64 | Out-Null
Set-Content "$win64\TheOuterWorlds2-Win64-Shipping.exe" 'fake'
$fails = 0
function Check($ok, $what, $detail = '') { if ($ok) { "PASS $what $detail" } else { "FAIL $what $detail"; $script:fails++ } }
function Setup { $o = & $exe @args 2>&1 | Out-String; @{ text = $o.Trim(); code = $LASTEXITCODE } }  # the pipe waits for it
function Ini { [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes("$win64\tow2coop.ini")) -split "`r`n" }

$r = Setup --install --game $root
$ini = Ini
Check ($r.code -eq 0 -and (Test-Path "$win64\dsound.dll") -and (Test-Path "$win64\tow2coop.dll") -and ($ini -contains 'role=auto') -and ($ini -contains 'transport=steam')) `
    'install over Steam: the loader, the mod, role=auto (host or join from the main menu)' "($($r.code)) $($r.text)"
$r = Setup --status --game $root
Check ($r.code -eq 0 -and $r.text -match 'this version|cette version') 'status: installed, this version' $r.text
$r = Setup --install --udp --game $root
Check ($r.code -ne 0) 'direct IP without a role is refused' "($($r.code)) $($r.text)"
$name = "Zo$([char]0xEB) $([char]0xDC)nder"
$r = Setup --install --friend $name --game $root
$raw = [IO.File]::ReadAllBytes("$win64\tow2coop.ini")
$ini = Ini
Check ($r.code -eq 0 -and ($ini -contains "host=$name") -and $raw[0] -ne 0xEF -and -not ($ini -match '^key=')) 'a friend to join by Steam name (UTF-8, no BOM), no key'
$r = Setup --install --udp --role host --game $root
$ini = Ini
$key = ($ini | Where-Object { $_ -match '^key=[a-z0-9]{10}$' }) -replace '^key='
Check ($r.code -eq 0 -and $key -and ($ini -contains 'transport=udp') -and ($ini -contains 'role=host') -and $r.text.Contains($key)) 'direct-IP host: a key is made and told for the friend' $r.text
$r = Setup --install --udp --role client --peer 10.0.0.5 --game $root
Check ($r.code -ne 0) 'direct-IP join without the key is refused' $r.text
$r = Setup --install --udp --role client --peer 10.0.0.5 --key $key --game $root
$ini = Ini
Check ($r.code -eq 0 -and ($ini -contains 'peer=10.0.0.5:27960') -and ($ini -contains "key=$key") -and ($ini -contains 'role=client')) 'direct-IP join writes the host address (default port) and the key'
$r = Setup --install --udp --role host --key 'no spaces!' --game $root
Check ($r.code -ne 0) 'a bad key is refused' $r.text
foreach ($f in 'dsound.dll', 'tow2coop.dll') { $s = [IO.File]::Open("$win64\$f", 'Append'); $s.WriteByte(0); $s.Close() }  # an older build: other bytes, same marker
$r = Setup --status --game $root
Check ($r.text -match 'older|ancienne') 'status: an older version' $r.text
$r = Setup --install --game $win64  # the game's Win64 folder given instead of its own
$same = foreach ($f in @{ 'dsound.dll' = 'bin\dsound.dll'; 'tow2coop.dll' = 'bin\core\tow2coop.dll' }.GetEnumerator()) { (Get-FileHash "$win64\$($f.Key)").Hash -eq (Get-FileHash (Join-Path $repo $f.Value)).Hash }
Check ($r.code -eq 0 -and -not ($same -contains $false)) 'install over an older build replaces it (the Win64 folder given)' $r.text
$r = Setup --uninstall --game $root
Check ($r.code -eq 0 -and -not (Test-Path "$win64\dsound.dll") -and -not (Test-Path "$win64\tow2coop.dll") -and -not (Test-Path "$win64\tow2coop.ini")) 'uninstall removes every file it placed' $r.text
Check (Test-Path "$win64\TheOuterWorlds2-Win64-Shipping.exe") 'uninstall leaves the game alone'
Set-Content "$win64\dsound.dll" 'another mod'
$r = Setup --install --game $root
Check ($r.code -ne 0 -and (Get-Content "$win64\dsound.dll") -eq 'another mod' -and -not (Test-Path "$win64\tow2coop.dll")) "another mod's dsound.dll: install refused, nothing changed" $r.text
$r = Setup --uninstall --game $root
Check ((Get-Content "$win64\dsound.dll") -eq 'another mod') "uninstall leaves another mod's dsound.dll" $r.text
$r = Setup --install --game "$root\nothing-here"
Check ($r.code -ne 0) 'a folder without the game is refused' $r.text
Remove-Item $root -Recurse -Force
"RESULT: $(if ($fails) { 'FAIL' } else { 'PASS' }) ($fails failure(s))"
