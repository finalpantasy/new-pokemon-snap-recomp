# Test runs are HIDDEN by default: snap_boot.exe goes through the ghost launcher (a never-shown "EmberGhost" desktop in a job object), so no
# window reaches the owner's screen. -Visible = old behavior (normal window, to watch). Launcher: $env:SNAP_GHOST_LAUNCHER or the default below.
# -SaveFixture <name> seeds the run's save dir from build\fixtures\saves\<name> (e.g. post_tutorial) instead of an empty save.
# -Route <name>: feeds host\routes\<name>.txt as SNAP_INPUT_SCRIPT, defaults the save fixture to the route's first-line
#   '# fixture: <name>' (else <name>), and saves checkpoint screenshots (first at -ShotAt s, then every -ShotEvery s, at most -ShotMax; NVN_FRAME_DUMP).
#   -ShotAt without -Route also dumps frames, to analysis\boot\run_<id>_NN.png.
# PARALLEL RUNS (owner exception to the one-heavy-process rule): takes one of 4 run slots (build\slots\run0..3.lock, host\locks.ps1) instead of
# build\heavy.lock (that stays the BUILD lock). -Bench / -Exclusive (or SNAP_PERF set) take all 4 slots and wait for them. While build\heavy.priority
# exists, slot 0 is reserved for the run whose $env:SNAP_LOCK_OWNER equals its content. -NoLock = the caller already holds a slot (scratch dir slotnl<pid>).
# Each run works in build\runs\slot<N>\ (copy of build\snap exe + DLLs + nvn_cache, own save dir, own copy of the shared shader cache), so a
# concurrent build can relink. Output carries a run id <slot>-<stamp>: analysis\boot\runs\<id>\boot_{trace,stderr,stdout}.log and
# route_<name>_<id>_NN.png / run_<id>_NN.png; the legacy names (boot_trace.log, route_<name>_NN.png) get a copy of the latest finished run.
# An explicit -Log keeps the old behavior (that path, stderr/stdout beside it). Runs the exe copy at BelowNormal priority with a hard timeout; kills it by PID if it hangs.
#   powershell -File host\run_boot.ps1 [-Route florio_photos] [-Timeout 300] [-Log analysis\boot\boot_trace.log] <snap_boot args...>   (default args: run)
[CmdletBinding(PositionalBinding=$false)]
param([int]$Timeout = 300, [switch]$KeepSave, [string]$SaveFixture, [switch]$Visible, [string]$Route, [string]$Bench, [int]$ShotAt = 20, [int]$ShotEvery = 30, [int]$ShotMax = 8, [switch]$NoLock, [switch]$Exclusive, [string]$BuildDir, [string]$Log ='analysis\boot\boot_trace.log', [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root
. (Join-Path $PSScriptRoot 'locks.ps1')
$logGiven = $PSBoundParameters.ContainsKey('Log')
# a game is being played from build\play (host\play.ps1): do not start a test boot meanwhile
if (Test-Path 'build\play.lock') {
    $pl = (Get-Content 'build\play.lock' -ErrorAction SilentlyContinue) -join ' '
    if ($pl -match 'pid=(\d+)' -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)) { Write-Host "play.lock held ($pl): not starting a boot run"; exit 3 }
}
# -Bench <route>: the same route with no screenshots or frame dumps and SNAP_PERF on; afterwards writes analysis\perf\bench_<route>_<stamp>.md
# and .csv (avg fps, 1% / 0.1% low, max, percentiles, per-scene split from '# scene: <name> <start_s> <end_s>' lines in the route file,
# git hash + dirty flag, shader cache cold/warm). Run it twice for a repeatability figure. Always exclusive.
$benchStamp = Get-Date -Format 'yyyyMMdd_HHmmss'; $benchPrefix = $null
if ($Bench) { $Route = $Bench; $benchPrefix = Join-Path $root "analysis\perf\bench_${Bench}_$benchStamp" }
if ($Route) {
    $rf = Join-Path $root "host\routes\$Route.txt"
    if (-not (Test-Path $rf)) { Write-Host "no route file $rf"; exit 5 }
    $env:SNAP_INPUT_SCRIPT = $rf
    if (-not $SaveFixture) {
        $m = Select-String -Path $rf -Pattern '^#\s*fixture:\s*(\S+)' | Select-Object -First 1
        $SaveFixture = if ($m) { $m.Matches[0].Groups[1].Value } else { $Route }
    }
}
$dumpOn = (-not $Bench) -and ($Route -or $PSBoundParameters.ContainsKey('ShotAt'))

# ---- slot: one of 6 concurrent run slots (all 6 for -Bench / -Exclusive / perf)
$excl = [bool]($Bench -or $Exclusive -or $env:SNAP_PERF)
$what = "run_boot $(if ($Route) { "route=$Route" } else { 'run' })$(if ($Bench) { ' bench' })"
$slots = @()
if ($NoLock) { $slotName = "slotnl$PID"; $sid = 'nl' }
else {
    $slots = Enter-RunSlots $what -Exclusive:$excl
    if ($null -eq $slots) { Write-Host 'could not get a run slot'; exit 6 }
    $slotName = "slot$($slots[0])"; $sid = "$($slots[0])"
}
$runId = "$sid-$(Get-Date -Format 'yyyyMMdd_HHmmss')"
Write-Host "run $runId on $slotName$(if ($excl) { ' (exclusive)' })"
$p = $null; $cacheBefore = $null
$bootDir = Join-Path $root 'analysis\boot'; $shotBase = $null; $err = $null; $so = $null
try {
    $runDir = Join-Path $root "build\runs\$slotName"; New-Item -ItemType Directory -Force $runDir | Out-Null
    # output locations
    if ($logGiven) { New-Item -ItemType Directory -Force (Split-Path $Log) | Out-Null }
    else { $runOut = Join-Path $bootDir "runs\$runId"; New-Item -ItemType Directory -Force $runOut | Out-Null; $Log = Join-Path $runOut 'boot_trace.log' }
    $err = Join-Path (Split-Path $Log) 'boot_stderr.log'; $so = Join-Path (Split-Path $Log) 'boot_stdout.log'
    $shotBase = if ($Route) { Join-Path $bootDir "route_${Route}_$runId" } else { Join-Path $bootDir "run_$runId" }
    if ($Bench) { $env:SNAP_PERF = $benchPrefix }
    elseif ($dumpOn) {
        # NVN_FRAME_DUMP works on the hidden desktop (the desktop capture SNAP_SHOT only sees black there); half-size BMPs
        $rawDir = "${shotBase}_raw"
        if (Test-Path $rawDir) { Remove-Item -Recurse -Force $rawDir }
        New-Item -ItemType Directory -Force $rawDir | Out-Null
        $env:NVN_FRAME_DUMP = $rawDir; $env:NVN_FRAME_DUMP_FROM = [string]$ShotAt; $env:NVN_FRAME_DUMP_EVERY = [string]$ShotEvery; $env:NVN_FRAME_DUMP_MAX = [string]$ShotMax
    }
    # isolated copy of the exe + DLLs + nvn shader cache (a build may relink build\snap meanwhile); retry if the source changes mid-copy
    # -BuildDir <dir> (or $env:SNAP_BUILD_DIR): run a worker's own build (host\build_snap.py --out <dir>) instead of build\snap
    $bd = if ($BuildDir) { $BuildDir } elseif ($env:SNAP_BUILD_DIR) { $env:SNAP_BUILD_DIR } else { 'build\snap' }
    $src = if ([IO.Path]::IsPathRooted($bd)) { $bd } else { Join-Path $root $bd }; $ok = $false
    for ($try = 0; $try -lt 15 -and -not $ok; $try++) {
        $ok = $true
        foreach ($f in (Get-ChildItem $src -File | Where-Object { $_.Name -in 'snap_boot.exe', 'snap_boot.pdb' -or $_.Extension -eq '.dll' })) {
            try { Copy-Item $f.FullName (Join-Path $runDir $f.Name) -Force -ErrorAction Stop } catch { $ok = $false; break }
            if ((Get-Item (Join-Path $runDir $f.Name)).Length -ne (Get-Item $f.FullName).Length) { $ok = $false; break }
        }
        if (-not $ok) { Start-Sleep -Seconds 2 }
    }
    if (-not $ok) { Write-Host 'could not copy a stable build\snap exe'; exit 7 }
    $warm = Join-Path $root 'build\snap\nvn_cache'   # own build dir without a warm translated-shader cache: use the main one
    if (-not (Test-Path "$src\nvn_cache") -and (Test-Path $warm)) { robocopy $warm "$runDir\nvn_cache" /MIR /MT:16 /NFL /NDL /NJH /NJS /NP | Out-Null }
    if (Test-Path "$src\nvn_cache") { robocopy "$src\nvn_cache" "$runDir\nvn_cache" /MIR /MT:16 /NFL /NDL /NJH /NJS /NP | Out-Null }
    # SNAP_XLATE_REV=<n> (translator revision, runtime\nvn\nvn_shader.cpp): the run reads nvn_cache_r<n>, seeded from build\xlate\r<n>\out (scripts\xlate_run.ps1)
    if ($env:SNAP_XLATE_REV -and $env:SNAP_XLATE_REV -ne '0') {
        $xr = Join-Path $root "build\xlate\r$($env:SNAP_XLATE_REV)\out"
        if (Test-Path $xr) { robocopy $xr "$runDir\nvn_cache_r$($env:SNAP_XLATE_REV)" /MIR /XF *.tsv /MT:16 /NFL /NDL /NJH /NJS /NP | Out-Null }
    }
    # per-slot pipeline cache seeded from the shared one (no concurrent writes to %APPDATA%); SNAP_SHADERCACHE_DIR in the caller's env is the seed if set
    $seed = if ($env:SNAP_SHADERCACHE_DIR) { $env:SNAP_SHADERCACHE_DIR } elseif ($env:SNAP_CONFIG_DIR) { Join-Path $env:SNAP_CONFIG_DIR 'shadercache' } else { Join-Path $env:APPDATA 'SnapRecomp\shadercache' }
    $scd = Join-Path $runDir 'shadercache'
    if ($Bench) {
        $cf = Get-ChildItem $seed -Recurse -File -ErrorAction SilentlyContinue
        $cacheBefore = if ($cf) { "warm ($(@($cf).Count) files, $([math]::Round((($cf | Measure-Object Length -Sum).Sum) / 1MB, 1)) MB in $seed)" } else { "cold (empty or missing $seed)" }
    }
    if (Test-Path $seed) { robocopy $seed $scd /MIR /NFL /NDL /NJH /NJS /NP | Out-Null } else { New-Item -ItemType Directory -Force $scd | Out-Null }
    $env:SNAP_SHADERCACHE_DIR = $scd
    # Test runs never touch the player's save (%APPDATA%\SnapRecomp\save): per-slot build\runs\slot<N>\save, wiped per run unless -KeepSave.
    $testSave = Join-Path $runDir 'save'
    if (-not $KeepSave -and (Test-Path $testSave)) { Remove-Item -Recurse -Force $testSave -ErrorAction SilentlyContinue }
    New-Item -ItemType Directory -Force $testSave | Out-Null
    if ($SaveFixture) {
        $fx = Join-Path $root "build\fixtures\saves\$SaveFixture"
        if (-not (Test-Path $fx)) { Write-Host "no save fixture $fx"; exit 4 }
        if ($KeepSave) { Write-Host "-SaveFixture with -KeepSave: not re-seeding $testSave" } else { Copy-Item -Recurse -Force (Join-Path $fx '*') $testSave }
    }
    if (-not $env:SNAP_SAVE_DIR) { $env:SNAP_SAVE_DIR = $testSave }
    # test runs load no mods unless asked (2026-11-12: the owner's HD packs in %APPDATA%\SnapRecomp\mods made hidden-run loading ~15x slower);
    # to test a pack set SNAP_MODS_DIR to its folder (or to "$env:APPDATA\SnapRecomp\mods" for the owner's set)
    if (-not $env:SNAP_MODS_DIR) { $noMods = Join-Path $runDir 'mods_none'; New-Item -ItemType Directory -Force $noMods | Out-Null; $env:SNAP_MODS_DIR = $noMods }
    if (-not $Rest) { $Rest = @('run') }
    $argl = @($Rest) + @('--trace', $Log, '--timeout', [string]($Timeout - 5))
    $exe = Join-Path $runDir 'snap_boot.exe'
    # the exe maps a fixed address (0x7300000000); on a busy PC that sometimes collides at start-up ('cannot map the thunk range'): retry, up to 3 starts
    $t0 = Get-Date
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        if ($Visible) {
            $p = Start-Process -FilePath $exe -ArgumentList $argl -NoNewWindow -PassThru -RedirectStandardError $err -RedirectStandardOutput $so
        } else {
            $gl = if ($env:SNAP_GHOST_LAUNCHER) { $env:SNAP_GHOST_LAUNCHER } else { Join-Path $env:USERPROFILE '.claude\skills\ghost-godot\ghost_launch.py' }
            if (-not (Test-Path $gl)) { Write-Host "ghost launcher not found ($gl); set SNAP_GHOST_LAUNCHER or use -Visible"; exit 97 }
            # the hidden desktop has no foreground window: the game must not gate (scripted) input on focus
            # ...and must not pause itself on focus loss (a hidden run once paused at 241 s)
            $env:SNAP_INPUT_BACKGROUND = '1'; $env:SNAP_HEADLESS = '1'; $env:SNAP_WINDOW_PAUSE = '0'
            # muted by default (owner, 2026-11-12): the timer-paced null audio backend keeps the game's audio timing and SNAP_AUDIO_CAPTURE, but nothing reaches the speakers; set SNAP_AUDIO_BACKEND=wasapi to hear it
            if (-not $env:SNAP_AUDIO_BACKEND) { $env:SNAP_AUDIO_BACKEND = 'null' }
            $gargs = @('-I', $gl, $exe) + @($argl)
            $p = Start-Process -FilePath 'python' -ArgumentList $gargs -NoNewWindow -PassThru -RedirectStandardError $err -RedirectStandardOutput $so
        }
        try { $p.PriorityClass = 'BelowNormal' } catch {}
        $left = [int]($Timeout - ((Get-Date) - $t0).TotalSeconds)
        if (-not $p.WaitForExit([math]::Max(1, $left) * 1000)) { taskkill /PID $p.Id /T /F | Out-Null; Write-Host "killed pid $($p.Id) after $Timeout s"; break }
        if (((Get-Date) - $t0).TotalSeconds -lt 30 -and (Select-String -Path $err -Pattern 'cannot map the thunk range' -Quiet)) { Write-Host "start-up address clash, retrying ($attempt)"; Start-Sleep -Seconds 2; continue }
        break
    }
    Write-Host "exit code $($p.ExitCode); run $runId; stderr in $err, trace in $Log"
    # keep the run's save before the slot is released (2026-11-12: the next run in this slot wiped it before a worker could copy it)
    if ((Test-Path $testSave) -and (Get-ChildItem $testSave -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1)) {
        $keep = Join-Path $root "build\runs\saves\$runId"; New-Item -ItemType Directory -Force $keep | Out-Null
        Copy-Item -Recurse -Force (Join-Path $testSave '*') $keep -ErrorAction SilentlyContinue; Write-Host "save kept in $keep"
        # owner 2026-11-12: these copies filled F: (195 runs x ~3.7 GB = 721 GB). Keep only the generation CURRENT names, and only the newest 20 runs.
        Get-ChildItem $keep -Recurse -Filter CURRENT -File -ErrorAction SilentlyContinue | ForEach-Object {
            $lines = @(Get-Content $_.FullName -ErrorAction SilentlyContinue)   # "SNAPSAVE1", current generation, checksum
            $gen = if ($lines.Count -ge 2 -and $lines[0] -eq 'SNAPSAVE1') { $lines[1].Trim() } else { '' }
            $gens = Join-Path $_.DirectoryName 'gens'
            if ($gen -and (Test-Path (Join-Path $gens $gen))) {
                Get-ChildItem $gens -Directory | Where-Object { $_.Name -ne $gen } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
        Get-ChildItem (Join-Path $root 'build\runs\saves') -Directory | Sort-Object LastWriteTime -Descending | Select-Object -Skip 20 |
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    }
    # owner 2026-11-12 "keep my drive a little clean": route screenshots in analysis\boot older than 24 h are deleted unless a doc names them
    # (AGENTS.md, host\README.md, docs\*.md, analysis\**\*.md); *_raw dump folders older than 24 h go too. Copy a shot you want to keep
    # into analysis\mods\compare\ or name it in a doc.
    try {
        $bootDir = Join-Path $root 'analysis\boot'; $cut = (Get-Date).AddHours(-24)
        $old = @(Get-ChildItem $bootDir -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -lt $cut -and $_.Extension -in '.png','.bmp','.jpg' })
        if ($old.Count) {
            $docs = @((Join-Path $root 'AGENTS.md'), (Join-Path $root 'host\README.md')) + @(Get-ChildItem (Join-Path $root 'docs'), (Join-Path $root 'analysis') -Recurse -Filter *.md -File -ErrorAction SilentlyContinue | ForEach-Object FullName)
            $refs = @{}; Select-String -Path $docs -Pattern '[A-Za-z0-9_.-]+\.(png|bmp|jpg)' -AllMatches -ErrorAction SilentlyContinue | ForEach-Object { $_.Matches | ForEach-Object { $refs[$_.Value] = 1 } }
            $old | Where-Object { -not $refs.ContainsKey($_.Name) } | Remove-Item -Force -ErrorAction SilentlyContinue
        }
        Get-ChildItem $bootDir -Directory -Filter '*_raw' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -lt $cut } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    } catch { Write-Host "screenshot cleanup skipped: $_" }
} finally {
    if ($p -and -not $p.HasExited) { taskkill /PID $p.Id /T /F | Out-Null }
    if (-not $NoLock) { Exit-RunSlots }
}
if (-not $logGiven -and $err -and (Test-Path $Log)) {   # legacy names = latest finished run
    Copy-Item $Log (Join-Path $bootDir 'boot_trace.log') -Force -ErrorAction SilentlyContinue
    Copy-Item $err (Join-Path $bootDir 'boot_stderr.log') -Force -ErrorAction SilentlyContinue
    Copy-Item $so (Join-Path $bootDir 'boot_stdout.log') -Force -ErrorAction SilentlyContinue
}
if ($NoLock) { Remove-Item -Recurse -Force $runDir -ErrorAction SilentlyContinue }
if ($Bench) {
    $cacheState = if ($cacheBefore) { $cacheBefore } else { 'unknown' }
    $dirty = [bool](git status --porcelain 2>$null)
    $meta = @{ run = "bench_${Bench}_$benchStamp"; route = $Bench; stamp = $benchStamp; timeout = $Timeout; git = ((git rev-parse --short HEAD 2>$null) -join ''); dirty = $dirty; cache = $cacheState } | ConvertTo-Json
    $mf = "$benchPrefix.meta.json"; $meta | Set-Content $mf -Encoding utf8
    if (Test-Path "${benchPrefix}_frames.csv") {
        python -I (Join-Path $root 'analysis\perf\bench_report.py') $benchPrefix (Join-Path $root "host\routes\$Bench.txt") "$benchPrefix.md" "$benchPrefix.csv" $mf
    } else { Write-Host "no frames log at ${benchPrefix}_frames.csv (run too short or SNAP_PERF unsupported by this exe)" }
} elseif ($dumpOn -and $shotBase) {   # raw BMPs -> <shotBase>_NN.png (route runs also refresh the legacy route_<name>_NN.png)
    $conv = "import sys,glob,shutil`nfrom PIL import Image`nd=sys.argv[1]`nfor n,f in enumerate(sorted(glob.glob(d+'_raw/*.bmp'))): Image.open(f).convert('RGB').save('%s_%02d.png'%(d,n))`nshutil.rmtree(d+'_raw',ignore_errors=True)"
    Get-ChildItem "${shotBase}_raw" -Filter *.bmp -ErrorAction SilentlyContinue | ForEach-Object Name | Set-Content "${shotBase}_dumps.txt"   # names carry present count and time: fps = delta p / delta t
    & python -c $conv $shotBase
    $shots = Get-ChildItem (Split-Path $shotBase) -Filter "$(Split-Path $shotBase -Leaf)_??.png" -ErrorAction SilentlyContinue
    if ($Route) {
        Get-ChildItem $bootDir -Filter "route_${Route}_??.png" -ErrorAction SilentlyContinue | Remove-Item -Force
        foreach ($f in $shots) { Copy-Item $f.FullName (Join-Path $bootDir ("route_${Route}_" + $f.Name.Substring($f.Name.Length - 6))) -Force }
    }
    Write-Host ("checkpoints: " + (($shots | ForEach-Object Name) -join ' '))
}
