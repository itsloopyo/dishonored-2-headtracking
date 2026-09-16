#!/usr/bin/env pwsh
#Requires -Version 5.1
# Deploys the built ASI + seed ini into a local Dishonored 2 install.
# An explicit path wins over detection, as it does in install.cmd. The other way
# round it is not an override at all: with two installs present - a Steam copy
# and a test copy - the argument is silently ignored and the detected install is
# the one written to.
param(
    [Parameter(Position = 0)]
    [string]$GamePath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

Import-Module (Join-Path $projectDir "cameraunlock-core/powershell/GamePathDetection.psm1") -Force

if ($GamePath) {
    $gamePath = $GamePath
} else {
    $gamePath = Find-GamePath -GameId 'dishonored-2'
}
if (-not $gamePath -or -not (Test-Path $gamePath)) {
    Write-Host "Could not detect Dishonored 2 install path. Pass it as an argument: pixi run install <path>" -ForegroundColor Red
    exit 1
}
Write-Host "Game found at: $gamePath" -ForegroundColor Cyan

$asi = Join-Path $projectDir "bin/Release/Dishonored2HeadTracking.asi"
$ini = Join-Path $projectDir "HeadTracking.ini"
if (-not (Test-Path $asi)) {
    Write-Host "Build first: pixi run build" -ForegroundColor Red
    exit 1
}

# Ultimate ASI Loader ships as dinput8.dll and is renamed to the proxy the game
# exe already imports. winmm.dll is that proxy here, and install.cmd's
# ASI_LOADER_NAME must stay the same value.
$loaderPath = Join-Path $gamePath "winmm.dll"
if (-not (Test-Path $loaderPath)) {
    $vendor = Join-Path $projectDir "vendor/ultimate-asi-loader/dinput8.dll"
    if (-not (Test-Path $vendor)) {
        Write-Host "ASI Loader not vendored. Run: pixi run update-deps" -ForegroundColor Red
        exit 1
    }
    Copy-Item $vendor $loaderPath -Force
    Write-Host "Installed ASI Loader: $loaderPath" -ForegroundColor Green
}

Copy-Item $asi (Join-Path $gamePath "Dishonored2HeadTracking.asi") -Force
if (-not (Test-Path (Join-Path $gamePath "HeadTracking.ini"))) {
    Copy-Item $ini (Join-Path $gamePath "HeadTracking.ini") -Force
}
Write-Host "Deployed Dishonored2HeadTracking.asi to $gamePath" -ForegroundColor Green
