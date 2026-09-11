param(
    [string]$BuildDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\vs2022-x64\dist\Release'),
    [string]$TargetDir = 'C:\totalcmdx64\Plugins\wlx\VideoThumb'
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path (Join-Path $BuildDir 'VideoThumb.wlx64'))) {
    throw "VideoThumb.wlx64 not found in $BuildDir. Build Release first."
}

New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null

# Remove stale FFmpeg generations so the plugin directory has one deterministic set.
Get-ChildItem $TargetDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(avcodec|avformat|avutil|swresample|swscale)-\d+\.dll$' } |
    Remove-Item -Force

Copy-Item (Join-Path $BuildDir '*') $TargetDir -Recurse -Force
Write-Host "Installed VideoThumb v1.1 to $TargetDir" -ForegroundColor Green
Write-Host "Close/restart Total Commander before testing."
