# Close every test copy normally (window close, like the player clicking X), waiting up to -Seconds; a forced stop
# mid-load seemed to leave the graphics driver's shader cache cold (run 25: next starts reached 13-19 GB).
# Only processes under local\gamepool\instances. Run gamepool down afterwards for anything still running.
param([int]$Seconds = 40)
$copies = @(Get-Process -Name 'TheOuterWorlds2-Win64-Shipping' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'C:\coop-anything\local\gamepool\instances\*' })
foreach ($c in $copies) { taskkill /pid $c.Id | Out-Null }
$t0 = Get-Date
while ((Get-Date) - $t0 -lt [timespan]::FromSeconds($Seconds)) {
    if (-not ($copies | Where-Object { Get-Process -Id $_.Id -ErrorAction SilentlyContinue })) { break }
    Start-Sleep 1
}
$left = @($copies | Where-Object { Get-Process -Id $_.Id -ErrorAction SilentlyContinue })
"closed $($copies.Count - $left.Count) of $($copies.Count) copies normally" + $(if ($left.Count) { "; still running: $($left.Id -join ',')" } else { '' })
