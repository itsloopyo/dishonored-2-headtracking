#!/usr/bin/env pwsh
#Requires -Version 5.1
<#
.SYNOPSIS
    Release workflow for Dishonored 2 Head Tracking.

.DESCRIPTION
    1. Validate semver + git state.
    2. Regenerate CHANGELOG.md from conventional commits (via
       cameraunlock-core/powershell/ReleaseWorkflow.psm1).
    3. Bump version in manifest.json, pixi.toml, CMakeLists.txt,
       src/core/constants.h, scripts/install.cmd, launcher-manifest.json.
    4. Build the release.
    5. Commit the version + changelog as "Release v<version>".
    6. Create annotated tag v<version> and push it; CI picks up the tag and
       produces the GitHub release artifacts.

.PARAMETER Version
    Semver string (e.g. "1.0.0") or major|minor|patch|nightly.

.EXAMPLE
    pixi run release 1.0.0
#>
param(
    [Parameter(Position = 0)]
    [string]$Version = '',
    # Ship a release even when there are no user-facing commits since the
    # last tag (writes a maintenance changelog entry instead of aborting).
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir
$manifestPath = Join-Path $projectDir 'manifest.json'
$pixiPath = Join-Path $projectDir 'pixi.toml'
$cmakePath = Join-Path $projectDir 'CMakeLists.txt'
$constantsPath = Join-Path $projectDir 'src/core/constants.h'
$installCmdPath = Join-Path $projectDir 'scripts/install.cmd'
$launcherManifestPath = Join-Path $projectDir 'launcher-manifest.json'
$changelogPath = Join-Path $projectDir 'CHANGELOG.md'

Import-Module (Join-Path $projectDir 'cameraunlock-core/powershell/ReleaseWorkflow.psm1') -Force

# Mirrors New-ChangelogFromCommits' insertion so a -Force maintenance entry
# lands in the same place with the same shape.
function Add-MaintenanceChangelogEntry {
    param([string]$Path, [string]$NewVersion)
    $date = Get-Date -Format 'yyyy-MM-dd'
    $entry = "## [$NewVersion] - $date`n`n### Changed`n`n- Maintenance release (no user-facing changes).`n`n"
    $changelog = Get-Content $Path -Raw
    if ($changelog -match '(?s)(# Changelog.*?)(## \[)') {
        $changelog = $changelog -replace '(?s)(# Changelog.*?\n\n)', "`$1$entry"
    } else {
        $changelog = $changelog -replace '(?s)(# Changelog.*?\n)', "`$1$entry"
    }
    $changelog = $changelog.TrimEnd() + "`n"
    Set-Content $Path $changelog -NoNewline
}

function Get-ConstantsVersion {
    $content = Get-Content $constantsPath -Raw
    if ($content -match 'D2HT_VERSION\s*=\s*"([^"]+)"') {
        return $Matches[1]
    }
    throw "Could not read D2HT_VERSION from $constantsPath"
}

Write-Host ''
Write-Host '=== Dishonored 2 Head Tracking Release ===' -ForegroundColor Cyan
Write-Host ''

$current = Get-ConstantsVersion

if ([string]::IsNullOrWhiteSpace($Version)) {
    Write-Host "Current version: $current" -ForegroundColor Yellow
    Write-Host 'Usage: pixi run release <major|minor|patch|nightly|X.Y.Z>'
    exit 0
}

if ($Version -eq 'nightly') {
    & (Join-Path $scriptDir 'release-nightly.ps1')
    # Under Set-StrictMode, reading $LASTEXITCODE throws when no native command
    # has run yet, and a nightly that completes without one would die here
    # instead of exiting clean.
    $nightlyCode = if (Test-Path variable:LASTEXITCODE) { $LASTEXITCODE } else { 0 }
    exit $nightlyCode
}

try {
    $Version = Resolve-ReleaseVersion -Argument $Version -CurrentVersion $current
} catch {
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$tag = "v$Version"

$branch = git rev-parse --abbrev-ref HEAD
if ($branch -ne 'main') {
    Write-Host "Must be on main branch to release (currently on '$branch')" -ForegroundColor Red
    exit 1
}
if (-not (Test-CleanGitStatus)) {
    Write-Host 'Working tree has uncommitted changes - commit or stash first.' -ForegroundColor Red
    git status --short
    exit 1
}
if (Test-GitTagExists -Tag $tag) {
    Write-Host "Tag '$tag' already exists." -ForegroundColor Red
    exit 1
}

# THIRD-PARTY-NOTICES.md names the cameraunlock-core commit compiled into the
# release ZIPs, and bumping the submodule does not touch it. Packaging refuses
# to ship that mismatch, so a bump with no notices edit stopped the release
# here, or in CI once the tag had already been pushed. Re-sync it and let this
# release carry the correction. It runs after the branch, clean-tree and tag
# gates above so an aborted release never leaves a commit behind.
$noticesRoot = Split-Path -Parent $PSScriptRoot
& git -C $noticesRoot diff --quiet -- THIRD-PARTY-NOTICES.md
if ($LASTEXITCODE -ne 0) { throw "THIRD-PARTY-NOTICES.md has uncommitted edits. Commit or discard them, then re-run." }
& (Join-Path $noticesRoot 'cameraunlock-core\scripts\sync-core-notices.ps1') -Repo $noticesRoot
if ($LASTEXITCODE -ne 0) { throw "sync-core-notices.ps1 exited $LASTEXITCODE - fix THIRD-PARTY-NOTICES.md before releasing." }
& git -C $noticesRoot diff --quiet -- THIRD-PARTY-NOTICES.md
if ($LASTEXITCODE -ne 0) {
    & git -C $noticesRoot commit -q -m 'chore: record the cameraunlock-core commit this build compiles' -- THIRD-PARTY-NOTICES.md
    if ($LASTEXITCODE -ne 0) { throw "Could not commit the re-synced THIRD-PARTY-NOTICES.md." }
    Write-Host 'THIRD-PARTY-NOTICES.md re-synced to the pinned cameraunlock-core commit.' -ForegroundColor Yellow
}

Write-Host "Current version: $current" -ForegroundColor Gray
Write-Host "New version:     $Version" -ForegroundColor Green
Write-Host ''

# Step 1 - changelog (the gate that can fail). Generate it BEFORE mutating
# any version files so an abort here leaves the working tree clean instead
# of stranding a half-applied version bump with no tag.
Write-Host 'Generating CHANGELOG from commits...' -ForegroundColor Cyan
$hasTags = git tag -l 2>$null
if (-not $hasTags) {
    $date = Get-Date -Format 'yyyy-MM-dd'
    Set-Content $changelogPath "# Changelog`n`n## [$Version] - $date`n`nFirst release.`n"
} else {
    try {
        $changelogArgs = @{
            ChangelogPath = $changelogPath
            Version       = $Version
            ArtifactPaths = @('src/', 'cameraunlock-core', 'scripts/install.cmd', 'scripts/uninstall.cmd')
        }
        New-ChangelogFromCommits @changelogArgs
    } catch {
        if (-not $Force) {
            Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host 'No user-facing changes to release. Re-run with -Force for a maintenance release.' -ForegroundColor Yellow
            exit 1
        }
        Write-Host 'No user-facing commits since last tag - writing maintenance entry (-Force).' -ForegroundColor Yellow
        Add-MaintenanceChangelogEntry -Path $changelogPath -NewVersion $Version
    }
}

# Step 2 - bump version across all version files
#
# [regex]::new(...).Replace(text, replacement, 1) throughout, NOT the static
# [regex]::Replace(text, pattern, replacement, 1). The static overload's fourth
# argument is a RegexOptions, so a trailing 1 there means IgnoreCase and every
# match is rewritten - which reads exactly like a count and is not one. The
# instance method's third argument really is the match limit.
Write-Host "Updating manifest.json to $Version..." -ForegroundColor Cyan
$manifestRaw = Get-Content $manifestPath -Raw
if ($manifestRaw -notmatch '"version"\s*:\s*"[^"]+"') { throw "version field not found in $manifestPath" }
$manifestRaw = [regex]::new('"version"\s*:\s*"[^"]+"').Replace($manifestRaw, "`"version`": `"$Version`"", 1)
Set-Content -Path $manifestPath -Value $manifestRaw -NoNewline

Write-Host "Updating pixi.toml to $Version..." -ForegroundColor Cyan
$pixiRaw = Get-Content $pixiPath -Raw
if ($pixiRaw -notmatch '(?m)^version\s*=\s*"[^"]+"') { throw "version key not found in $pixiPath" }
$pixiRaw = [regex]::new('(?m)^version\s*=\s*"[^"]+"').Replace($pixiRaw, "version = `"$Version`"", 1)
Set-Content -Path $pixiPath -Value $pixiRaw -NoNewline

Write-Host "Updating CMakeLists.txt to $Version..." -ForegroundColor Cyan
$cmakeRaw = Get-Content $cmakePath -Raw
# Anchored to project(): an unanchored 'VERSION x.y.z' also matches
# cmake_minimum_required the moment its floor is written with three components,
# and would quietly rewrite the CMake floor to the mod version instead.
if ($cmakeRaw -notmatch '(?m)^\s*project\([^)]*VERSION\s+\d+\.\d+\.\d+') { throw "project(... VERSION x.y.z ...) not found in $cmakePath" }
$cmakeRaw = [regex]::new('(?m)(^\s*project\([^)]*VERSION\s+)\d+\.\d+\.\d+').Replace($cmakeRaw, "`${1}$Version", 1)
Set-Content -Path $cmakePath -Value $cmakeRaw -NoNewline

Write-Host "Updating src/core/constants.h to $Version..." -ForegroundColor Cyan
$constantsRaw = Get-Content $constantsPath -Raw
if ($constantsRaw -notmatch 'D2HT_VERSION\s*=\s*"[^"]+"') { throw "D2HT_VERSION not found in $constantsPath" }
$constantsRaw = [regex]::new('D2HT_VERSION\s*=\s*"[^"]+"').Replace($constantsRaw, "D2HT_VERSION = `"$Version`"", 1)
Set-Content -Path $constantsPath -Value $constantsRaw -NoNewline

# Keep install.cmd's MOD_VERSION in lockstep - it's what the installer
# writes into the user's .headtracking-state.json. ReadAllText/WriteAllText
# preserve the file's CRLF line endings.
Write-Host "Updating scripts/install.cmd MOD_VERSION to $Version..." -ForegroundColor Cyan
$installRaw = [System.IO.File]::ReadAllText($installCmdPath)
if ($installRaw -notmatch 'set "MOD_VERSION=[^"]+"') {
    throw "MOD_VERSION line not found in $installCmdPath"
}
$installRaw = [regex]::new('set "MOD_VERSION=[^"]+"').Replace($installRaw, "set `"MOD_VERSION=$Version`"", 1)
[System.IO.File]::WriteAllText($installCmdPath, $installRaw)

# launcher-manifest.json is the file lopari reads; keep its mod_info.version
# in lockstep. JSON-aware update so the rest of the manifest is untouched.
if (Test-Path $launcherManifestPath) {
    Write-Host "Updating launcher-manifest.json to $Version..." -ForegroundColor Cyan
    $launcherJson = Get-Content $launcherManifestPath -Raw | ConvertFrom-Json
    $launcherJson.mod_info.version = $Version
    $launcherJson | ConvertTo-Json -Depth 10 | Set-Content $launcherManifestPath -NoNewline
}

# Step 3 - build, test and package
# package rather than build: it depends on both, and it is the only path that
# runs the version cross-check, the manifest-seed comparison, the vendored
# licence checks and the two node validators. Building alone leaves every one of
# those to fire for the first time in CI, after the tag is public and the only
# recovery is deleting the tag.
Write-Host 'Building and packaging release (pixi run package)...' -ForegroundColor Cyan
Push-Location $projectDir
try {
    pixi run package
    if ($LASTEXITCODE -ne 0) { throw 'Build/package failed' }
} finally {
    Pop-Location
}

# Step 4 - commit specific files only (avoid git add -A sweeping in build artifacts)
Write-Host 'Committing version + changelog...' -ForegroundColor Cyan
git add $manifestPath $pixiPath $cmakePath $constantsPath $installCmdPath $changelogPath
if (Test-Path $launcherManifestPath) { git add $launcherManifestPath }
# Skip commit if everything is already at this version (re-running release
# for the same version, e.g. after deleting a tag / release on GitHub to
# republish). The tag still gets recreated below at the current HEAD.
git diff --cached --quiet
if ($LASTEXITCODE -eq 0) {
    Write-Host 'No version/changelog changes - tagging existing HEAD.' -ForegroundColor Yellow
} else {
    git commit -m "Release v$Version"
    if ($LASTEXITCODE -ne 0) { throw 'Commit failed' }
}

# Step 5 - push, then tag, then push the tag.
#
# In that order, and every step checked. $ErrorActionPreference does not catch a
# native command's exit code, so an unchecked `git push origin main` that is
# rejected - a protected branch, a remote that moved, expired credentials - used
# to be followed by the tag pushing anyway. release.yml fires on the tag, so a
# public GitHub Release would be built and published from a commit that is not
# on main, while this script printed that everything had worked.
Write-Host 'Pushing main...' -ForegroundColor Cyan
git push origin main
if ($LASTEXITCODE -ne 0) { throw 'git push origin main failed; nothing has been tagged or published.' }

Write-Host "Creating tag $tag..." -ForegroundColor Cyan
git tag -a $tag -m "Release $tag"
if ($LASTEXITCODE -ne 0) { throw "git tag $tag failed" }

git push origin $tag
if ($LASTEXITCODE -ne 0) { throw "git push origin $tag failed; main is pushed but no release will be built." }

Write-Host ''
Write-Host "Release $tag pushed - CI will build and publish artifacts." -ForegroundColor Green
