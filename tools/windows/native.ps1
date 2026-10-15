# Builds the native renderer (tools/native: pop_native + pop_native_view) with MSVC + Ninja into
# game\native-build, and optionally renders a scene: -Scene <gltf> -Clip <file> -Out <png> [-Nearest]
param([string]$Scene = '', [string]$Clip = '', [string]$Out = '', [switch]$Nearest)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC not found (Visual Studio Build Tools with the C++ workload)' }
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:VSCMD_SKIP_SENDTELEMETRY = '1'
Set-Location $Root
$B = "$Root\game\native-build"
if (-not (Test-Path "$B\build.ninja")) {
    cmake -S tools\native -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | Select-String -Pattern 'error|Configuring done|Generating done'
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
}
cmake --build $B 2>&1 | Select-String -Pattern 'error|warning C|FAILED|Linking'
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
'build ok'
if ($Scene) {
    $a = @($Scene, $Clip, $Out); if ($Nearest) { $a += '--nearest' }
    & "$B\pop_native_view.exe" @a
    "exit $LASTEXITCODE"
}
