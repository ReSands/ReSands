# One-time setup of the Windows PC for PoP_Decomp. Run in an elevated PowerShell:
#   Set-ExecutionPolicy -Scope Process Bypass; .\setup.ps1
# Safe to re-run: winget skips what is already installed.
$ErrorActionPreference = 'Stop'

$Tools = 'C:\tools'
$Proj  = 'C:\dev\PoP_Decomp'

Write-Host '== Packages (winget)'
$packages = @(
    'Git.Git',
    'Kitware.CMake',
    'Ninja-build.Ninja',
    'EclipseAdoptium.Temurin.21.JDK',
    'Python.Python.3.12',
    'Tailscale.Tailscale'
)
foreach ($id in $packages) {
    winget install --id $id --exact --silent --accept-package-agreements --accept-source-agreements
}
# MSVC: Build Tools with the C++ workload (skip if full Visual Studio 2022 with it is installed).
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent --accept-package-agreements `
    --override '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'

Write-Host '== OpenSSH Server'
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0 | Out-Null
Set-Service -Name sshd -StartupType Automatic
Start-Service sshd

Write-Host '== Long paths (56k generated files with long names)'
New-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled `
    -Value 1 -PropertyType DWORD -Force | Out-Null
git config --system core.longpaths true

Write-Host '== Defender exclusion for the project and tools (scanning 56k files slows MSVC a lot)'
New-Item -ItemType Directory -Force -Path $Tools, (Split-Path $Proj) | Out-Null
Add-MpPreference -ExclusionPath $Tools, $Proj

Write-Host '== Ghidra 12.1.4 + Emotion Engine Reloaded v2.1.38 (checksums as verified on the Mac)'
$downloads = @(
    @{ Url  = 'https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip'
       Sha  = 'ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db' },
    @{ Url  = 'https://github.com/chaoticgd/ghidra-emotionengine-reloaded/releases/download/v2.1.38/ghidra_12.1.4_PUBLIC_20261004_ghidra-emotionengine-reloaded.zip'
       Sha  = 'e389ff3e921d9f7d4db2ece9906f808545ce4ff519d9e25a019b72ea509f240b' }
)
$dl = Join-Path $Tools 'downloads'
New-Item -ItemType Directory -Force -Path $dl | Out-Null
foreach ($d in $downloads) {
    $file = Join-Path $dl (Split-Path $d.Url -Leaf)
    if (-not (Test-Path $file)) { Invoke-WebRequest -Uri $d.Url -OutFile $file }
    $hash = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower()
    if ($hash -ne $d.Sha) { throw "Checksum mismatch for $file" }
}
$ghidra = Join-Path $Tools 'ghidra_12.1.4_PUBLIC'
if (-not (Test-Path $ghidra)) {
    Expand-Archive (Join-Path $dl 'ghidra_12.1.4_PUBLIC_20260921.zip') -DestinationPath $Tools
    Expand-Archive (Join-Path $dl 'ghidra_12.1.4_PUBLIC_20261004_ghidra-emotionengine-reloaded.zip') `
        -DestinationPath (Join-Path $ghidra 'Ghidra\Extensions')
}

Write-Host ''
Write-Host 'Done. Next steps are in tools\windows\SETUP.md (steps 3 onwards).'
