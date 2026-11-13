# Runs one GPU stage of the environment HD pipeline under a run slot (BelowNormal), then releases the slot.
#   powershell -File scripts\hd\env_gpu.ps1 -Area hub -Stage esr|sd [-MaxMinutes 9]
param([string]$Area, [string]$Stage, [int]$MaxMinutes = 9)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $root 'host\locks.ps1')
$slot = Enter-RunSlots "env-hd $Area $Stage"
if ($null -eq $slot) { Write-Host 'no run slot'; exit 2 }
try {
    & (Join-Path $root 'host\gpu_share.ps1') -Samples 2 | Out-Host
    $tmp = Join-Path $env:TEMP "envgpu_$PID"; New-Item -ItemType Directory -Force $tmp | Out-Null
    if ($Stage -eq 'esr') {
        $p = Start-Process python -ArgumentList @('-E','-P',(Join-Path $root 'scripts\hd\env_batch.py'),'esr',$Area) -PassThru -NoNewWindow -RedirectStandardOutput "$tmp\o.txt" -RedirectStandardError "$tmp\e.txt"
    } else {
        $list = & python -E -P (Join-Path $root 'scripts\hd\env_batch.py') list $Area sd x4_sd
        if (-not $list) { Write-Host 'nothing to do'; exit 0 }
        $args2 = @('-I',(Join-Path $root 'scripts\hd\retexture_sd.py')) + @($list) + @('--max-seconds', ($MaxMinutes * 60 - 60))
        $p = Start-Process (Join-Path $root 'tools\venv-sd\Scripts\python.exe') -ArgumentList $args2 -PassThru -NoNewWindow -RedirectStandardOutput "$tmp\o.txt" -RedirectStandardError "$tmp\e.txt"
    }
    $p.PriorityClass = 'BelowNormal'
    if (-not $p.WaitForExit($MaxMinutes * 60 * 1000)) { taskkill /PID $p.Id /T /F | Out-Null; Write-Host 'timeout, killed' }
    Get-Content "$tmp\o.txt" -Tail 15; Get-Content "$tmp\e.txt" -Tail 5
    Write-Host "exit $($p.ExitCode)"
    Remove-Item -Recurse -Force $tmp
} finally { Exit-RunSlots }
