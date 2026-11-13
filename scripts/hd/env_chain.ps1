# Runs the queued GPU stages one after another (each under its own run slot via env_gpu.ps1).
param([int]$WaitPid = 0)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($WaitPid) { Wait-Process -Id $WaitPid -ErrorAction SilentlyContinue }
function Stage($a, $s) { & powershell -NoProfile -File (Join-Path $root 'scripts\hd\env_gpu.ps1') -Area $a -Stage $s }
Stage 'beach' 'esr'; Stage 'jungle' 'esr'
foreach ($a in 'hub', 'beach', 'jungle') {
    for ($i = 0; $i -lt 8; $i++) {
        $left = & python -E -P (Join-Path $root 'scripts\hd\env_batch.py') list $a sd x4_sd
        if (-not $left) { break }
        Stage $a 'sd'
    }
}
'chain done'
