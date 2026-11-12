# Builds a ShaderHarness copy against a translator source tree into build\xlate\<Name> (own project copy and output, so no build lock
# and the in-use tools\shader_harness build is never touched).
#   powershell -File scripts\xlate_build.ps1 -Name base     unpatched Ryubing tree (tools\ref\ryubing-ryujinx)
#   powershell -File scripts\xlate_build.ps1 -Name r2       build\xlate\r2\ryujinx (patched by scripts\xlate_patch.ps1)
# Output: build\xlate\<Name>\h\bin\Release\net10.0\ShaderHarness.dll
param([Parameter(Mandatory)][string]$Name)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ryu = if ($Name -eq 'base') { Join-Path $root 'tools\ref\ryubing-ryujinx' } else { Join-Path $root "build\xlate\$Name\ryujinx" }
$h = Join-Path $root "build\xlate\$Name\h"
New-Item -ItemType Directory -Force $h | Out-Null
Copy-Item (Join-Path $root 'scripts\shader_harness\Program.cs'), (Join-Path $root 'scripts\shader_harness\Directory.Packages.props') $h -Force
$c = Get-Content (Join-Path $root 'scripts\shader_harness\ShaderHarness.csproj') -Raw
$c = $c -replace '(?m)^\s*<BaseOutputPath.*\r?\n', '' -replace '(?m)^\s*<BaseIntermediateOutputPath.*\r?\n', ''
Set-Content (Join-Path $h 'ShaderHarness.csproj') $c -NoNewline
& dotnet build (Join-Path $h 'ShaderHarness.csproj') -c Release --nologo -v q "-p:RyujinxDir=$ryu"
if ($LASTEXITCODE -ne 0) { throw "dotnet build failed ($LASTEXITCODE)" }
