param(
  [string]$BuildDir = "_Comp64DebugOptimized"
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot

if (-not (Test-Path $BuildDir)) {
  Write-Error "Build directory '$BuildDir' not found."
}

$vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vsWhere)) {
  Write-Error "vswhere.exe not found at '$vsWhere'."
}

$vsInfo = & $vsWhere -latest -products * -format json | ConvertFrom-Json
if ($null -eq $vsInfo -or $vsInfo.Count -eq 0) {
  Write-Error 'Failed to find a Visual Studio installation.'
}

$vsInfo = $vsInfo[0]
$vsInstall = $vsInfo.installationPath

$devShellModule = Join-Path $vsInstall 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
if (-not (Test-Path $devShellModule)) {
  Write-Error "Microsoft.VisualStudio.DevShell.dll not found at '$devShellModule'."
}

Write-Host "Using Visual Studio at $vsInstall"
Write-Host "Building targets in $BuildDir"

Import-Module $devShellModule
Enter-VsDevShell -VsInstanceId $vsInfo.instanceId -SkipAutomaticLocation -Arch amd64 -HostArch amd64 | Out-Null

meson compile -C $BuildDir dxgi d3d11
exit $LASTEXITCODE