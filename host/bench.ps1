# Benchmark a fixed route: runs run_boot.ps1 -Route <route> with SNAP_PERF on and no frame dumps (-ShotMax 0), then writes
#   analysis\perf\bench_<route>_<stamp>.md  (avg fps, 1% / 0.1% low, max, percentiles, per-scene split, git hash + dirty, shader cache state)
#   analysis\perf\bench_<route>_<stamp>.csv (one row per scene, for comparing runs)
# Scenes come from '# scene: <name> <start_s> <end_s>' lines in the route file (seconds of game time).
#   powershell -File host\bench.ps1 -Route florio_photos [-Timeout 450]
# Extra arguments go to run_boot.ps1 (e.g. -Visible). The shader cache is the user one (%APPDATA%\SnapRecomp\shadercache) unless
# SNAP_SHADERCACHE_DIR / SNAP_CONFIG_DIR is set, so a second run right after the first is "warm".
[CmdletBinding(PositionalBinding=$false)]
param([Parameter(Mandatory)][string]$Route, [int]$Timeout = 450, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root
$rf = Join-Path $root "host\routes\$Route.txt"
if (-not (Test-Path $rf)) { Write-Host "no route file $rf"; exit 5 }
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$prefix = Join-Path $root "analysis\perf\bench_${Route}_$stamp"
$scd = if ($env:SNAP_SHADERCACHE_DIR) { $env:SNAP_SHADERCACHE_DIR } elseif ($env:SNAP_CONFIG_DIR) { Join-Path $env:SNAP_CONFIG_DIR 'shadercache' } else { Join-Path $env:APPDATA 'SnapRecomp\shadercache' }
$cf = Get-ChildItem $scd -Recurse -File -ErrorAction SilentlyContinue
$cache = if ($cf) { "warm ($(@($cf).Count) files, $([math]::Round((($cf | Measure-Object Length -Sum).Sum) / 1MB, 1)) MB in $scd)" } else { "cold (empty or missing $scd)" }
$env:SNAP_PERF = $prefix
try {
    & powershell -NoProfile -File (Join-Path $PSScriptRoot 'run_boot.ps1') -Route $Route -Timeout $Timeout -ShotMax 0 @Rest
} finally { Remove-Item Env:SNAP_PERF -ErrorAction SilentlyContinue }
$meta = @{ run = "bench_${Route}_$stamp"; route = $Route; stamp = $stamp; timeout = $Timeout; git = ((git rev-parse --short HEAD 2>$null) -join ''); dirty = [bool](git status --porcelain 2>$null); cache = $cache } | ConvertTo-Json
$mf = "$prefix.meta.json"; $meta | Set-Content $mf -Encoding utf8
if (Test-Path "${prefix}_frames.csv") {
    python -I (Join-Path $root 'analysis\perf\bench_report.py') $prefix $rf "$prefix.md" "$prefix.csv" $mf
} else { Write-Host "no frames log at ${prefix}_frames.csv (run too short, or this exe has no SNAP_PERF)" }
