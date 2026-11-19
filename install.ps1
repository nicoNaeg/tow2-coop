# Install or remove the co-op mod in a player's own The Outer Worlds 2 (Steam) install.
# Over Steam (default; the two players must be Steam friends, no address or open port needed):
#   install.ps1                                    one install for both: host or join from the game's main menu
#   install.ps1 -Role host                         always host (no menu choice); a friend joins you
#   install.ps1 -Role client [-Friend <Steam name>]   always join a Steam friend who hosts (-Friend picks one of several)
# Direct IP instead (UDP 27960 + TCP 27961 open on the host):
#   install.ps1 -Role host -Transport udp          prints the key your friend needs (or pass -Key yourself)
#   install.ps1 -Role client -Peer 100.64.1.2 -Key <the host's key>
#   install.ps1 -Uninstall                         removes every file this script put there
#   install.ps1 -Find                              only shows the game folder it would use
# -GameDir <folder with Arkansas\Binaries\Win64> skips the Steam library search. Files placed: dsound.dll (loader),
# tow2coop.dll (the mod), tow2coop.ini (your settings). Nothing of the game is changed or overwritten.
param([ValidateSet('host', 'client', 'auto')][string]$Role, [ValidateSet('steam', 'udp')][string]$Transport, [string]$Friend, [string]$Peer,
      [string]$Key, [int]$Port = 27960, [string]$GameDir, [switch]$Uninstall, [switch]$Find)
$ErrorActionPreference = 'Stop'
$AppId = 1449110; $Build = '24225441'  # the game build this mod was made for (Steam build id)
$ours = @{ 'dsound.dll' = "$PSScriptRoot\bin\dsound.dll"; 'tow2coop.dll' = "$PSScriptRoot\bin\core\tow2coop.dll" }

function Find-Game {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { throw 'Steam not found; pass -GameDir' }
    $libs = @($steam -replace '/', '\') + (Select-String -Path "$steam\steamapps\libraryfolders.vdf" -Pattern '"path"\s+"(.+)"' |
        ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
    foreach ($lib in $libs | Select-Object -Unique) {
        $acf = "$lib\steamapps\appmanifest_$AppId.acf"
        if (Test-Path $acf) {
            $text = Get-Content $acf -Raw
            $dir = [regex]::Match($text, '"installdir"\s+"(.+)"').Groups[1].Value
            $b = [regex]::Match($text, '"buildid"\s+"(\d+)"').Groups[1].Value
            if ($b -ne $Build) { Write-Warning "game build $b, the mod was made for ${Build}: it will stay off (the game runs normally) until updated" }
            return "$lib\steamapps\common\$dir"
        }
    }
    throw "The Outer Worlds 2 (app $AppId) not found in the Steam libraries; pass -GameDir"
}

if (-not $GameDir) { $GameDir = Find-Game }
$win64 = Join-Path $GameDir 'Arkansas\Binaries\Win64'
if (-not (Test-Path (Join-Path $win64 'TheOuterWorlds2-Win64-Shipping.exe'))) { throw "no TheOuterWorlds2-Win64-Shipping.exe in $win64" }
if ($Find) { "game folder: $win64"; return }
if (Get-Process 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$win64\*" }) { throw 'close the game first' }

# A dsound.dll is ours when it is our current build or carries our marker (an older build of the mod).
function Test-Ours([string]$path) {
    if (-not (Test-Path $path)) { return $true }
    if ((Get-FileHash $path).Hash -eq (Get-FileHash $ours[(Split-Path $path -Leaf)]).Hash) { return $true }
    $b = [IO.File]::ReadAllBytes($path)  # the loader names it in UTF-16, the mod in ASCII
    return [Text.Encoding]::Unicode.GetString($b).Contains('TOW2_SANDBOX_DIR') -or [Text.Encoding]::ASCII.GetString($b).Contains('TOW2_SANDBOX_DIR')
}

if ($Uninstall) {
    foreach ($f in 'dsound.dll', 'tow2coop.dll') {
        if (-not (Test-Ours "$win64\$f")) { Write-Warning "$f is not the co-op mod's; left in place" } else { Remove-Item "$win64\$f" -ErrorAction SilentlyContinue }
    }
    Remove-Item "$win64\tow2coop.ini", "$win64\tow2coop.log" -ErrorAction SilentlyContinue
    "co-op mod removed from $win64"
    return
}
if (-not $Transport) { $Transport = if ($Peer) { 'udp' } else { 'steam' } }
if (-not $Role) { $Role = if ($Transport -eq 'steam') { 'auto' } else { throw 'direct IP: pass -Role host or -Role client' } }
if ($Role -eq 'auto' -and $Transport -ne 'steam') { throw 'direct IP: pass -Role host or -Role client (the main menu choice is for Steam)' }
if ($Transport -eq 'udp') {
    if ($Role -eq 'client' -and -not ($Peer -and $Key)) { throw 'a direct-IP client needs -Peer <host address> -Key <the key the host got>' }
    # The key keeps strangers who reach the open port out (a join password, not encryption).
    if (-not $Key) { $Key = -join ((48..57) + (97..122) | Get-Random -Count 10 | ForEach-Object { [char]$_ }) }
    if ($Key -notmatch '^[A-Za-z0-9_-]{4,64}$') { throw 'the key is 4-64 letters, digits, _ or -' }
    if ($Peer -and $Peer -notmatch ':\d+$') { $Peer = "${Peer}:$Port" }
}
if ($Friend -match '[\r\n]') { throw 'the friend name is one line' }
foreach ($f in $ours.Keys) {
    if (-not (Test-Path $ours[$f])) { throw "missing $($ours[$f]) (build first: build.ps1)" }
    if (-not (Test-Ours "$win64\$f")) { throw "$win64\$f belongs to another mod; remove it first (nothing was changed)" }
}
foreach ($f in $ours.Keys) { Copy-Item $ours[$f] "$win64\$f" -Force }
$ini = "[coop]`r`nrole=$Role`r`ntransport=$Transport`r`n"
if ($Transport -eq 'steam') {
    if ($Friend) { $ini += "host=$Friend`r`n" }
    [IO.File]::WriteAllText("$win64\tow2coop.ini", $ini, [Text.UTF8Encoding]::new($false))  # Steam names may have accents
    "co-op mod installed in $win64 as $Role over Steam. " + $(if ($Role -eq 'auto') {
        'In the main menu, pick the new co-op choice (6): host your game for your Steam friends, or join a friend who hosts.' } elseif ($Role -eq 'host') {
        'Start the game and load your campaign; a Steam friend who installed it as client joins you (no address or port needed).' } else {
        'Start the game and wait at the main menu while your Steam friend hosts; you join about a minute later.' })
    return
}
$ini += "; direct IP: both players use the same key`r`nport=$Port`r`nkey=$Key`r`n"
if ($Role -eq 'client') { $ini += "peer=$Peer`r`n" }
[IO.File]::WriteAllText("$win64\tow2coop.ini", $ini, [Text.Encoding]::ASCII)
"co-op mod installed in $win64 as $Role (direct IP)" + $(if ($Role -eq 'client') { " (host $Peer)" } else {
    " (UDP $Port and TCP $($Port + 1) must reach this PC). Your friend runs: install.ps1 -Role client -Peer <your address> -Key $Key" })
