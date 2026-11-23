# Waits until no other snap_boot.exe runs and the CPU load is under -MaxLoad percent (three samples in a row), or -MaxMin minutes
# pass. Prints the load it measured at the end ("quiet" or "timeout"), for the frame-time notes. Exit 0 quiet, 1 timeout.
param([int]$MaxLoad = 45, [int]$MaxMin = 60)
$t0 = Get-Date; $ok = 0
while (((Get-Date) - $t0).TotalMinutes -lt $MaxMin) {
    $games = @(Get-Process snap_boot -ErrorAction SilentlyContinue).Count
    $load = (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average
    if ($games -eq 0 -and $load -lt $MaxLoad) { $ok++ } else { $ok = 0 }
    if ($ok -ge 3) { "quiet: load $load%, no other game, after $([int]((Get-Date) - $t0).TotalSeconds) s"; exit 0 }
    Start-Sleep -Seconds 20
}
"timeout: load $load%, $games other game(s) after $MaxMin min"
exit 1
