#!/usr/bin/env pwsh
#Requires -Version 5.1
# Manually refresh the vendored Ultimate ASI Loader. See AGENTS.md "Vendoring".
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir
$vendorDir = Join-Path $projectDir 'vendor/ultimate-asi-loader'

Import-Module (Join-Path $projectDir "cameraunlock-core/powershell/ModLoaderSetup.psm1") -Force

$zipPath = Join-Path $vendorDir 'Ultimate-ASI-Loader_x64.zip'
$dllPath = Join-Path $vendorDir 'dinput8.dll'
$readmePath = Join-Path $vendorDir 'README.md'
$licensePath = Join-Path $vendorDir 'LICENSE'

# Update-VendoredLoader's own idempotency check compares the vendored ASSET,
# and the asset here is a zip we do not keep, so it never fires and every run
# rewrites README.md with a fresh "Fetched at". The committed text is held here
# and put back when the extracted DLL turns out to be the one already vendored,
# so a run against unmoved upstream leaves no diff to mistake for a bump.
$priorReadme = if (Test-Path -LiteralPath $readmePath) { [IO.File]::ReadAllBytes($readmePath) } else { $null }
$priorLicense = if (Test-Path -LiteralPath $licensePath) { [IO.File]::ReadAllBytes($licensePath) } else { $null }
$priorDllHash = if (Test-Path -LiteralPath $dllPath) { (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }

# Dishonored 2 is a 64-bit game, so we need the x64 Ultimate ASI Loader.
# The upstream asset is a zip; install.cmd and deploy.ps1 both copy a bare
# dinput8.dll (renamed to winmm.dll at the game), so we extract the single
# proxy DLL the zip contains rather than vendoring the zip itself.
$meta = Update-VendoredLoader `
    -Name 'ultimate-asi-loader' `
    -OutputDir $vendorDir `
    -OutputFileName 'Ultimate-ASI-Loader_x64.zip' `
    -Owner 'ThirteenAG' -Repo 'Ultimate-ASI-Loader' `
    -VersionPrefix 'v9.' `
    -AssetPattern '^Ultimate-ASI-Loader_x64\.zip$'

# Extracted to a scratch file and validated THERE, and moved over the committed
# loader only once every check below has passed. Written straight into vendor/
# instead, a truncated download or an x86 asset replaces the good committed
# binary and only then throws, leaving the rejected file in the working tree
# where the next `git commit -a` picks it up.
$stagePath = Join-Path ([IO.Path]::GetTempPath()) ("ual-" + [Guid]::NewGuid().ToString('N') + '.dll')

# Every check below throws, and each one used to leave the rejected DLL sitting
# in %TEMP% under a name nothing ever collects: a run that refused a truncated
# or x86 asset left three and a half megabytes behind, and the next run left
# another under a fresh GUID. The finally removes whatever is still at the
# scratch path, which on the success path is nothing - the file has been moved
# over the committed loader by then.
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entry = $zip.Entries | Where-Object { $_.Name -ieq 'dinput8.dll' } | Select-Object -First 1
        if (-not $entry) { throw "x64 Ultimate ASI Loader zip did not contain dinput8.dll" }
        $out = [IO.File]::Create($stagePath)
        try {
            $in = $entry.Open()
            try { $in.CopyTo($out) } finally { $in.Dispose() }
        } finally { $out.Dispose() }
    } finally { $zip.Dispose() }

    # Fail fast if we somehow extracted the wrong architecture - copying an x86 (or
    # zip) proxy into an x64 game crashes the game on launch before our code runs.
    # The header is walked rather than indexed blind: a truncated or non-PE file
    # gives an e_lfanew that points anywhere, and two bytes read from there can read
    # 0x8664 by coincidence and vendor a file that is not a DLL at all.
    $bytes = [IO.File]::ReadAllBytes($stagePath)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Extracted ASI Loader is not a PE image (no MZ header); refusing to vendor it."
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or ($peOffset + 6) -gt $bytes.Length) {
        throw "Extracted ASI Loader has an out-of-range PE header offset ($peOffset); refusing to vendor it."
    }
    if ($bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) {
        throw "Extracted ASI Loader has no PE signature at offset $peOffset; refusing to vendor it."
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -ne 0x8664) {
        throw ("Extracted ASI Loader is not x64 (machine=0x{0:X4}); refusing to vendor it." -f $machine)
    }

    $stageHash = (Get-FileHash -LiteralPath $stagePath -Algorithm SHA256).Hash.ToLowerInvariant()

    # The zip is not shipped and not committed - install.cmd and the packager both
    # consume the bare DLL.
    Remove-Item $zipPath -Force

    $unchanged = ($null -ne $priorDllHash) -and ($priorDllHash -eq $stageHash) -and ($null -ne $priorReadme) -and ($null -ne $priorLicense)
    if ($unchanged) {
        [IO.File]::WriteAllBytes($readmePath, $priorReadme)
        [IO.File]::WriteAllBytes($licensePath, $priorLicense)
    } else {
        # Every check has passed, so the validated file becomes the committed one.
        Move-Item -LiteralPath $stagePath -Destination $dllPath -Force

        # Record the hash of the artifact we actually commit, so its provenance can
        # be checked without keeping the upstream zip around.
        Add-Content -Path $readmePath -Encoding utf8 -Value @"

## Committed artifact

Only ``dinput8.dll`` is committed; the upstream zip is a download intermediate
and is deleted after extraction.

- File: ``dinput8.dll`` (extracted from the asset above, unmodified)
- SHA-256: ``$stageHash``
"@

        if (-not (Test-Path $licensePath)) {
            throw "Upstream LICENSE was not written to $vendorDir - refusing to vendor a loader binary without its licence."
        }
    }
} finally {
    if (Test-Path -LiteralPath $stagePath) {
        Remove-Item -LiteralPath $stagePath -Force -ErrorAction SilentlyContinue
    }
}

# install.cmd states the loader version for the launcher's state file, and it is
# stamped on every run rather than only on a bump: written only when the DLL
# moves, a version that drifted for any other reason stays drifted, and the
# launcher reports a loader build that is not the one in the ZIP.
$bareVersion = $meta.Tag -replace '^v', ''
$installCmdPath = Join-Path $scriptDir 'install.cmd'
$installCmd = [IO.File]::ReadAllText($installCmdPath)
if ($installCmd -notmatch '(?m)^set "ASI_LOADER_VERSION=[0-9.]*"') {
    throw "No ASI_LOADER_VERSION line found in $installCmdPath"
}
# WriteAllText, not Set-Content: install.cmd is CRLF (.gitattributes pins it)
# and must keep the endings it was read with.
[IO.File]::WriteAllText($installCmdPath,
    ($installCmd -replace '(?m)^set "ASI_LOADER_VERSION=[0-9.]*"', "set `"ASI_LOADER_VERSION=$bareVersion`""))

if ($unchanged) {
    Write-Host "vendor/ultimate-asi-loader unchanged ($($meta.Tag), sha256=$($stageHash.Substring(0,12))...)" -ForegroundColor DarkGray
} else {
    Write-Host "Vendored x64 Ultimate ASI Loader ($($meta.Tag)) extracted to vendor/ultimate-asi-loader/dinput8.dll" -ForegroundColor Green
}
Write-Host "install.cmd ASI_LOADER_VERSION -> $bareVersion" -ForegroundColor DarkGray
