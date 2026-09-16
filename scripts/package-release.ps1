#!/usr/bin/env pwsh
#Requires -Version 5.1
# Produces release/Dishonored2HeadTracking-v<ver>-installer.zip and -nexus.zip
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = 'SilentlyContinue'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

Import-Module (Join-Path $projectDir "cameraunlock-core/powershell/ReleaseWorkflow.psm1") -Force

$manifest = Get-Content (Join-Path $projectDir "manifest.json") -Raw | ConvertFrom-Json
$version = $manifest.version

# The version is written down in four places and only release.ps1 keeps them in
# step, so a hand-edit to one of them ships a package that disagrees with itself:
# the launcher badges whatever launcher-manifest.json says, install.cmd records
# MOD_VERSION in its state file, and the nightly resolves the ZIP name from
# constants.h. None of those disagreements is visible at build time, so they are
# compared here instead.
$launcherVersion = (Get-Content (Join-Path $projectDir "launcher-manifest.json") -Raw |
    ConvertFrom-Json).mod_info.version
$constantsMatch = Select-String -Path (Join-Path $projectDir "src/core/constants.h") `
    -Pattern 'D2HT_VERSION\s*=\s*"([^"]+)"'
if (-not $constantsMatch) { throw "Could not read D2HT_VERSION from src/core/constants.h" }
$constantsVersion = $constantsMatch.Matches[0].Groups[1].Value
$installMatch = Select-String -Path (Join-Path $projectDir "scripts/install.cmd") `
    -Pattern 'MOD_VERSION=([^"\s]+)'
if (-not $installMatch) { throw "Could not read MOD_VERSION from scripts/install.cmd" }
$installVersion = $installMatch.Matches[0].Groups[1].Value.Trim()

foreach ($other in @(
        @{ Name = 'launcher-manifest.json mod_info.version'; Value = $launcherVersion },
        @{ Name = 'src/core/constants.h D2HT_VERSION'; Value = $constantsVersion },
        @{ Name = 'scripts/install.cmd MOD_VERSION'; Value = $installVersion })) {
    if ($other.Value -ne $version) {
        throw ("Version mismatch: manifest.json says $version but $($other.Name) says " +
               "$($other.Value). Run scripts/release.ps1 rather than editing one by hand.")
    }
}

$releaseDir = Join-Path $projectDir "release"
if (-not (Test-Path $releaseDir)) { New-Item -ItemType Directory -Path $releaseDir | Out-Null }

$asi = Join-Path $projectDir "bin/Release/Dishonored2HeadTracking.asi"
$ini = Join-Path $projectDir "HeadTracking.ini"
$launcherManifest = Join-Path $projectDir "launcher-manifest.json"
if (-not (Test-Path $asi)) { throw "ASI not built. Run: pixi run build" }
if (-not (Test-Path $ini)) { throw "HeadTracking.ini missing" }
if (-not (Test-Path $launcherManifest)) { throw "launcher-manifest.json missing" }

# Both ZIPs are binary distributions of MIT and BSD-2-Clause code, so the notice
# and disclaimer have to travel with them. A missing licence file fails the
# build rather than being skipped - a silent skip turns a compliance failure
# into a green build.
$license = Join-Path $projectDir "LICENSE"
$notices = Join-Path $projectDir "THIRD-PARTY-NOTICES.md"
$readme = Join-Path $projectDir "README.md"
$changelog = Join-Path $projectDir "CHANGELOG.md"
foreach ($f in @($license, $notices, $readme, $changelog)) {
    if (-not (Test-Path $f)) { throw "Required licence/doc file missing: $f" }
}

$vendorDir = Join-Path $projectDir "vendor/ultimate-asi-loader"
$vendorDll = Join-Path $vendorDir "dinput8.dll"

# ---- Installer ZIP (GitHub Release) ----
$installerStage = Join-Path $releaseDir "stage-installer"
if (Test-Path $installerStage) { Remove-Item $installerStage -Recurse -Force }
New-Item -ItemType Directory -Path $installerStage | Out-Null
New-Item -ItemType Directory -Path (Join-Path $installerStage "plugins") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $installerStage "vendor/ultimate-asi-loader") | Out-Null

Copy-Item $asi (Join-Path $installerStage "plugins/Dishonored2HeadTracking.asi")
Copy-Item $ini (Join-Path $installerStage "plugins/HeadTracking.ini")
Copy-Item (Join-Path $projectDir "scripts/install.cmd") $installerStage
Copy-Item (Join-Path $projectDir "scripts/uninstall.cmd") $installerStage
# The committed manifest is the authoritative copy of the seed: reviewable,
# diffable and in git. Re-stamping it from disk here would ship a correct ZIP
# over a stale committed file and nobody would ever see the drift, so drift
# fails the build instead and gets re-stamped in a commit. The check normalises
# line endings, so a CRLF working copy and an LF one compare equal.
#
# The seed is what delivers HeadTracking.ini on the launcher route at all: in
# manifest mode install.cmd never runs, so without it the player never sees the
# commented default and the file sits outside the launcher's receipt where
# uninstall cannot reach it.
Assert-ManifestSeedsMatchShipped -ManifestPath $launcherManifest -ProjectRoot $projectDir
Copy-Item $launcherManifest $installerStage
Copy-Item $readme $installerStage
Copy-Item $license $installerStage
Copy-Item $changelog $installerStage
Copy-Item $notices $installerStage
# find-game.ps1 imports GamePathDetection.psm1 from beside itself, so the shared
# bundle has to be staged as a unit. Hand-copying a subset ships an install.cmd
# that dies with "Installer ZIP is corrupt" on every run.
Copy-SharedBundle -StagingDir $installerStage

if (-not (Test-Path $vendorDll)) {
    throw "vendor/ultimate-asi-loader/dinput8.dll missing. Run: pixi run update-deps."
}
Copy-Item $vendorDll (Join-Path $installerStage "vendor/ultimate-asi-loader/dinput8.dll")
# MIT clause: the loader binary must not ship without ThirteenAG's notice.
foreach ($f in @("LICENSE", "README.md")) {
    $p = Join-Path $vendorDir $f
    if (-not (Test-Path $p)) { throw "vendor/ultimate-asi-loader/$f missing - the bundled loader must ship with its licence." }
    Copy-Item $p (Join-Path $installerStage "vendor/ultimate-asi-loader/$f")
}

$installerZip = Join-Path $releaseDir "Dishonored2HeadTracking-v$version-installer.zip"
if (Test-Path $installerZip) { Remove-Item $installerZip -Force }
# CreateFromDirectory rather than Compress-Archive, whose -Path is a wildcard
# PATTERN: under a checkout path containing [ or ] it matches nothing and
# packages nothing. Entering the staging directory first does not rescue it -
# the glob fails either way - and -LiteralPath cannot take a wildcard at all.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $installerStage, $installerZip,
    [System.IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item $installerStage -Recurse -Force

# ---- Nexus ZIP (extract to game folder) ----
$nexusStage = Join-Path $releaseDir "stage-nexus"
if (Test-Path $nexusStage) { Remove-Item $nexusStage -Recurse -Force }
New-Item -ItemType Directory -Path $nexusStage | Out-Null
Copy-Item $asi (Join-Path $nexusStage "Dishonored2HeadTracking.asi")
Copy-Item $ini (Join-Path $nexusStage "HeadTracking.ini")
# LICENSE, THIRD-PARTY-NOTICES.md and README.md are staged by the checked
# loop below, which is the one that enforces their presence.

$nexusZip = Join-Path $releaseDir "Dishonored2HeadTracking-v$version-nexus.zip"
if (Test-Path $nexusZip) { Remove-Item $nexusZip -Force }
# The Nexus ZIP is a binary distribution too: the licences of everything
# compiled into or bundled with the payload require their notices to travel
# with it, so LICENSE and THIRD-PARTY-NOTICES.md ship at its root.
foreach ($noticeDoc in @('LICENSE', 'THIRD-PARTY-NOTICES.md', 'README.md')) {
    $noticeSrc = Join-Path $projectDir $noticeDoc
    if (-not (Test-Path $noticeSrc)) {
        throw "Required notice file not found: $noticeDoc. Every published ZIP is a binary distribution and must carry it."
    }
    Copy-Item $noticeSrc -Destination $nexusStage -Force
    Write-Host "  $noticeDoc" -ForegroundColor Green
}
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $nexusStage, $nexusZip,
    [System.IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item $nexusStage -Recurse -Force

# ---- Validate ----
#
# The only check on what actually came out. validate-manifest.mjs opens the ZIP
# and confirms every files[] source is in it, that mod_info.version matches the
# ZIP name, and that nothing is carried which no manifest row deploys;
# validate-notices.mjs checks the licence set. A manifest naming a source that
# is not in the package installs a mod that does not run, and nothing else in
# the build would say so.
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    throw "node is required to validate the packaged ZIPs. Install Node.js, or run pixi run package on a machine that has it."
}
$coreScripts = Join-Path $projectDir "cameraunlock-core/scripts"
& node (Join-Path $coreScripts "validate-manifest.mjs") $installerZip
if ($LASTEXITCODE -ne 0) { throw "validate-manifest.mjs rejected $installerZip" }
# validate-notices.mjs takes the REPO, not a ZIP: it checks that
# THIRD-PARTY-NOTICES.md reproduces the licence text of everything compiled into
# or shipped beside the payload, which is the content half of the licence gate.
& node (Join-Path $coreScripts "validate-notices.mjs") $projectDir
if ($LASTEXITCODE -ne 0) { throw "validate-notices.mjs rejected THIRD-PARTY-NOTICES.md" }

Write-Host "Packaged:" -ForegroundColor Green
Write-Host "  $installerZip"
Write-Host "  $nexusZip"
