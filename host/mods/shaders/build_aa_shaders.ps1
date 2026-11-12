# Rebuilds runtime/nvn/nvn_aa_spv.h from smaa_aa.hlsl and cas.hlsl (dxc from the Vulkan SDK in tools\vulkan_sdk) and the SMAA area/search textures
# from the reference scripts (Scripts/AreaTex.py, Scripts/SearchTex.py of github.com/iryoku/smaa, MIT). Reference files go to build\smaa_ref.
$root = Split-Path (Split-Path (Split-Path $PSScriptRoot))
$ref = Join-Path $root 'build\smaa_ref'
New-Item -ItemType Directory -Force $ref | Out-Null
foreach ($f in 'Scripts/AreaTex.py','Scripts/SearchTex.py','SMAA.hlsl','LICENSE.txt') {
  $leaf = Split-Path $f -Leaf
  if (-not (Test-Path (Join-Path $ref $leaf))) { Invoke-WebRequest "https://raw.githubusercontent.com/iryoku/smaa/master/$f" -OutFile (Join-Path $ref $leaf) }
}
$py = Join-Path $root 'tools\venv-recomp\Scripts\python.exe'
Push-Location $ref
if (-not (Test-Path area_out.txt)) { & $py -I AreaTex.py | Out-File -Encoding ascii area_out.txt }
if (-not (Test-Path search_out.txt)) { & $py -I SearchTex.py | Out-File -Encoding ascii search_out.txt }
Pop-Location
& $py -I (Join-Path $PSScriptRoot 'gen_aa_header.py') $root
