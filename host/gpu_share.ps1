# Owner 2026-11-12: "Try not to use more than half my GPU on your instances for testing."
# Prints this project's share of 3D GPU time (snap_boot, our upscale/SD jobs under build\ or tools\) and the total. Exit 1 if ours > -MaxPct.
param([int]$MaxPct = 50, [int]$Samples = 3)
$ours = @{}; $all = 0.0
$procs = Get-CimInstance Win32_Process | Select-Object ProcessId, Name, CommandLine
$mine = $procs | Where-Object { $_.Name -eq 'snap_boot.exe' -or ($_.CommandLine -and $_.CommandLine -match 'new-pokemon-snap') } | ForEach-Object { [int]$_.ProcessId }
$cs = (Get-Counter '\GPU Engine(*engtype_3D)\Utilization Percentage' -SampleInterval 1 -MaxSamples $Samples -ErrorAction SilentlyContinue).CounterSamples   # engines of exiting processes give invalid samples
foreach ($s in $cs) {
    if ($s.InstanceName -notmatch 'pid_(\d+)') { continue }
    $v = $s.CookedValue / $Samples; $all += $v
    if ($mine -contains [int]$Matches[1]) { $ours[[int]$Matches[1]] += $v }
}
$sum = ($ours.Values | Measure-Object -Sum).Sum
"ours {0:N0}% (limit $MaxPct%), all processes {1:N0}%" -f $sum, $all
foreach ($k in $ours.Keys) { $n = ($procs | Where-Object ProcessId -eq $k).Name; "  $k $n {0:N1}%" -f $ours[$k] }
if ($sum -gt $MaxPct) { exit 1 }
