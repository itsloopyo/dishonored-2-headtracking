[CmdletBinding()]
param([switch]$AllowDirty)
$ErrorActionPreference = 'Stop'

$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
Import-Module (Join-Path $ProjectRoot 'cameraunlock-core\powershell\NightlyRelease.psm1') -Force

$constantsPath = Join-Path $ProjectRoot 'src\core\constants.h'
$match = Select-String -Path $constantsPath -Pattern 'D2HT_VERSION\s*=\s*"([^"]+)"'
if (-not $match) {
    throw "Could not extract D2HT_VERSION from $constantsPath"
}
$version = $match.Matches[0].Groups[1].Value

Publish-NightlyBuild `
    -ModId 'dishonored-2' `
    -ModName 'Dishonored2HeadTracking' `
    -Version $version `
    -ProjectRoot $ProjectRoot `
    -BuildCommand 'pixi run build' `
    -AllowDirty:$AllowDirty
