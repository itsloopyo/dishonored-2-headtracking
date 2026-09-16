#!/usr/bin/env pwsh
#Requires -Version 5.1
# Removes the locally deployed ASI + ini. Mirrors deploy.ps1: an explicit path
# wins over detection. This one deletes files, so the argument being ignored
# meant deleting them out of an install the caller did not name.
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
    Write-Host "Could not detect Dishonored 2 install path. Pass it as an argument: pixi run uninstall <path>" -ForegroundColor Red
    exit 1
}

foreach ($f in @("Dishonored2HeadTracking.asi", "HeadTracking.ini")) {
    $p = Join-Path $gamePath $f
    if (Test-Path $p) { Remove-Item $p -Force; Write-Host "Removed $f" }
}
Write-Host "Local uninstall complete. winmm.dll left in place; use scripts/uninstall.cmd /force to remove the ASI loader." -ForegroundColor Cyan
