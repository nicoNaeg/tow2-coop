# Builds sandbox\dsound.dll (and its test) with llvm-mingw, then runs the checks.
# Usage: powershell -NoProfile -File build.ps1 [-NoTest]
param([switch]$NoTest)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$bin = Join-Path $PSScriptRoot '..\tools\llvm-mingw-20261020-ucrt-x86_64\bin'
$cc = Join-Path $bin 'clang.exe'
$readobj = Join-Path $bin 'llvm-readobj.exe'
if (-not (Test-Path $cc)) { throw "compiler not found: $cc" }
$out = Join-Path $root 'bin'
$twin = Join-Path $out 'twin'
New-Item -ItemType Directory -Force $out, $twin | Out-Null

function Build([string]$dir, [string[]]$extra) {
    & $cc -O2 -Wall -Wextra -Werror -shared @extra -o "$dir\dsound.dll" "$root\sandbox\dsound_proxy.c" "$root\sandbox\dsound.def" `
        -lole32 "-Wl,--out-implib,$dir\libdsound_proxy.a"
    if ($LASTEXITCODE) { throw "dsound.dll build failed ($dir)" }
    & $cc -O2 -Wall -Wextra -Werror -o "$dir\sandbox_test.exe" "$root\sandbox\sandbox_test.c" "$dir\libdsound_proxy.a" -lole32 -lshell32 -luuid
    if ($LASTEXITCODE) { throw "sandbox_test.exe build failed ($dir)" }
}
Build $out @()
$fakeDir = Join-Path $out 'fake'; New-Item -ItemType Directory -Force $fakeDir | Out-Null
& $cc -O2 -Wall -Wextra -Werror -shared -o "$fakeDir\steam_api64.dll" "$root\sandbox\fake_steam_api.c" "$root\sandbox\fake_steam_api.def"
if ($LASTEXITCODE) { throw "fake steam_api64.dll build failed" }
Build $twin @('-DTWIN_NO_PATCH', '-Wno-unused-function', '-Wno-unused-variable')
Write-Host "built $out\dsound.dll"
# The co-op core (C++17, static runtime, so the game folder needs no extra DLLs).
$cxx = Join-Path $bin 'clang++.exe'
$coreOut = Join-Path $out 'core'; New-Item -ItemType Directory -Force $coreOut | Out-Null
$bridgeInc = Join-Path $root '..\..\tools\bridge\include'
& $cxx -std=c++17 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -shared -static -I $bridgeInc -o "$coreOut\tow2coop.dll" `
    "$root\core\main.cpp" "$root\core\ue.cpp" "$root\core\hook.cpp" "$root\core\coop.cpp" "$root\core\join.cpp" "$root\core\story.cpp" "$root\core\steamnet.cpp" "$root\core\savefile.cpp" "$root\core\character.cpp" -lws2_32 -lole32 -luuid
if ($LASTEXITCODE) { throw "tow2coop.dll build failed" }
Write-Host "built $coreOut\tow2coop.dll"
# The players' installer: one exe with the two DLLs inside (installer\setup.cpp), its version from the mod's.
$ver = [regex]::Match((Get-Content "$root\core\coop.cpp" -Raw), 'kVersion = "([^"]+)"').Groups[1].Value
$num = (($ver -replace '[^0-9.].*$', '').Split('.') + @('0', '0', '0', '0'))[0..3] -join ','
Set-Content -Encoding ascii "$out\version.h" "#define TOW2_VERSION `"$ver`"`r`n#define TOW2_VERSION_NUM $num"
& (Join-Path $bin 'windres.exe') -I $root -I $out -O coff -o "$out\setup_res.o" "$root\installer\setup.rc"
if ($LASTEXITCODE) { throw "installer resources failed" }
& $cxx -std=c++17 -O2 -Wall -Wextra -Werror -municode -mwindows -static -I $out -o "$out\tow2coop-setup.exe" "$root\installer\setup.cpp" "$out\setup_res.o" `
    -lcomctl32 -lshell32 -lole32 -luuid -ladvapi32
if ($LASTEXITCODE) { throw "tow2coop-setup.exe build failed" }
Write-Host "built $out\tow2coop-setup.exe ($ver)"
if ($NoTest) { return }

$fail = 0
function Result([bool]$ok, [string]$what) { Write-Host ("{0} {1}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what); if (-not $ok) { $script:fail++ } }

# The proxy may import only system DLLs: it sits next to a game exe, nothing else is shipped with it.
$imports = (& $readobj --coff-imports "$out\dsound.dll" | Select-String 'Name: (\S+)').Matches | ForEach-Object { $_.Groups[1].Value }
$foreign = $imports | Where-Object { $_ -notmatch '^(KERNEL32|ole32|api-ms-win-crt-[a-z]+-l1-1-0)\.dll$' }
Result (-not $foreign) "dsound.dll imports only system DLLs ($($imports -join ', '))"

# Same name per ordinal as the system DLL: the game imports DSOUND by ordinal.
function ExportTable([string]$dll) {
    $lines = & $readobj --coff-exports $dll
    $pairs = @(); $ord = $null
    foreach ($l in $lines) {
        if ($l -match 'Ordinal: (\d+)') { $ord = $Matches[1] }
        elseif ($l -match 'Name: (\S+)' -and $ord) { $pairs += "$ord=$($Matches[1])"; $ord = $null }
    }
    ($pairs | Sort-Object) -join ' '
}
$sysTable = ExportTable "$env:WINDIR\System32\dsound.dll"
$myTable = ExportTable "$out\dsound.dll"
Result ($sysTable -eq $myTable -and $sysTable) "export table equals System32 dsound.dll ($myTable)"

$sbx = Join-Path $out 'test-sandbox'
$fake = Join-Path $out 'fake\steam_api64.dll'
function RunTest([string]$exe, [string]$mode, [string]$sandboxDir) {
    if (Test-Path $sbx) { Remove-Item -Recurse -Force $sbx }
    if ($sandboxDir) { $env:TOW2_SANDBOX_DIR = $sandboxDir } else { Remove-Item Env:TOW2_SANDBOX_DIR -ErrorAction SilentlyContinue }
    try { $text = & $exe $mode $fake 2>&1 | Out-String; $code = $LASTEXITCODE } finally { Remove-Item Env:TOW2_SANDBOX_DIR -ErrorAction SilentlyContinue }
    return @{ text = $text.Trim(); code = $code }
}
$r = RunTest "$out\sandbox_test.exe" 'passthrough' $null
Write-Host $r.text; Result ($r.code -eq 0) 'pass-through mode (no TOW2_SANDBOX_DIR): every folder is the real one'
$r = RunTest "$out\sandbox_test.exe" 'sandbox' $sbx
Write-Host $r.text; Result ($r.code -eq 0) 'sandbox mode: user folders redirected, others untouched'
$r = RunTest "$twin\sandbox_test.exe" 'sandbox' $sbx
Result ($r.code -gt 0) "twin without the import patch is caught by the sandbox test (failures: $($r.code))"
$r = RunTest "$out\sandbox_test.exe" 'steamfail' $sbx
Result ($r.code -eq 0x5AFE0010) ("unreadable Steam wrapper stops the process with 0x5AFE0010 (got 0x{0:X8})" -f $r.code)
$r = RunTest "$out\sandbox_test.exe" 'sandbox' 'relative\folder'
Result ($r.code -eq 0x5AFE0003) ("relative TOW2_SANDBOX_DIR stops the process with 0x5AFE0003 (got 0x{0:X8})" -f $r.code)
if (Test-Path $sbx) { Remove-Item -Recurse -Force $sbx }
if (-not $fail) {  # a player's package in the layout install.ps1 expects; no game files
    $stage = Join-Path $root 'dist\tow2coop'
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force "$stage\bin\core" | Out-Null
    Copy-Item "$root\install.ps1", "$root\README.md", "$root\LICENSE" $stage
    Copy-Item "$out\dsound.dll" "$stage\bin\"
    Copy-Item "$coreOut\tow2coop.dll" "$stage\bin\core\"
    Copy-Item "$out\tow2coop-setup.exe" $stage
    Copy-Item "$out\tow2coop-setup.exe" "$root\dist\tow2coop-setup.exe"  # the release's own download too
    # forward-slash entry names: Compress-Archive and ZipFile on Windows PowerShell 5.1 write '\', which other unzip tools misread
    python -I -c "import sys, zipfile, pathlib; s = pathlib.Path(sys.argv[1]); z = zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_DEFLATED); [z.write(f, f.relative_to(s).as_posix()) for f in sorted(s.rglob('*')) if f.is_file()]; z.close()" $stage "$root\dist\tow2coop.zip"
    Write-Host "package: $root\dist\tow2coop.zip"
}
Write-Host ("BUILD+TEST: {0} ({1} failure(s))" -f $(if ($fail) { 'FAIL' } else { 'PASS' }), $fail)
exit $fail
