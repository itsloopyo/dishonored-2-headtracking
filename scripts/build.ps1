#!/usr/bin/env pwsh
#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir
Set-Location $projectDir

if (-not (Test-Path "cameraunlock-core/cpp/CMakeLists.txt")) {
    Write-Host "Submodule cameraunlock-core not initialized; running git submodule update..." -ForegroundColor Yellow
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "Submodule init failed" }
}

cmake -S . -B build -A x64 -DD2HT_BUILD_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$asi = Join-Path $projectDir "bin/Release/Dishonored2HeadTracking.asi"
if (-not (Test-Path $asi)) { throw "Build succeeded but ASI not found at $asi" }

Write-Host "Built: $asi" -ForegroundColor Green
