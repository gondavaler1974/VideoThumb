param(
    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'ffmpeg-sdk')
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$url = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip'
$tmp = Join-Path $env:TEMP 'videothumb-ffmpeg-shared.zip'

Write-Host "Downloading FFmpeg shared SDK..."
Invoke-WebRequest -Uri $url -OutFile $tmp

if (Test-Path $Destination) { Remove-Item $Destination -Recurse -Force }
New-Item -ItemType Directory -Path $Destination | Out-Null
Expand-Archive -Path $tmp -DestinationPath $Destination -Force
Remove-Item $tmp -Force

$root = Get-ChildItem $Destination -Directory | Where-Object {
    Test-Path (Join-Path $_.FullName 'include\libavcodec\avcodec.h')
} | Select-Object -First 1
if (-not $root) { throw 'Could not locate the extracted FFmpeg SDK root.' }

Write-Host ""
Write-Host "FFmpeg SDK ready:" -ForegroundColor Green
Write-Host $root.FullName
Write-Host ""
Write-Host "Now configure/build the CMake preset in VS Code."
