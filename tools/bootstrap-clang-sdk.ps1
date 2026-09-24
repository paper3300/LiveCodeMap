<#
.SYNOPSIS
  Repeatable, project-local bootstrap of the pinned Clang/LLVM LibTooling SDK.

.DESCRIPTION
  Reads a pin file (default cmake/clang-sdk.json), downloads the pinned
  official LLVM release archive into <deps>/downloads, verifies size and
  SHA-256 before touching the archive, optionally verifies the OpenPGP
  signature with the LLVM release keys in a project-local keyring, inspects
  the archive listing for the required paths, extracts under <deps> and writes
  a provenance marker. Nothing is installed machine-wide, no PATH is modified.

  Download integrity: a cached archive is trusted only after its SHA-256
  matches. On mismatch the cache is discarded and ONE fresh download (from
  byte zero, no resume) is attempted; a second mismatch stops with a recovery
  hint. -Force always discards the cache and downloads from zero.

  Destructive safety: the only recursive deletion is the previous extraction
  directory <deps>/<archive_root>, and only after every path component from
  the project build directory down to it has been checked not to be a
  reparse point (junction/symlink), because GetFullPath does not resolve
  junction ancestors.

  Hash verification and signature verification are reported separately.

.PARAMETER Force
  Discard the cached archive and any previous extraction, download from zero.

.PARAMETER SkipSignature
  Do not attempt the OpenPGP signature check (no signature/keys download).

.PARAMETER MetadataPath
  Pin file to use. Default: <repo>/cmake/clang-sdk.json.

.PARAMETER DepsRoot
  Directory that receives downloads/ and the extraction. Default: <repo>/build/deps.
  Must be inside <repo>/build.

.PARAMETER ValidateOnly
  Run metadata and path safety validation (including the reparse-ancestor
  check) and exit without downloading, extracting or deleting anything.
#>
[CmdletBinding()]
param(
  [switch]$Force,
  [switch]$SkipSignature,
  [string]$MetadataPath = '',
  [string]$DepsRoot = '',
  [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

function Write-Step($text) { Write-Host "[clang-sdk] $text" }

# Native tools write progress to stderr; under $ErrorActionPreference='Stop'
# PowerShell 5.1 would turn that into a terminating error. Run them with
# 'Continue' and rely on $LASTEXITCODE instead.
function Invoke-Native([scriptblock]$block) {
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & $block } finally { $ErrorActionPreference = $previous }
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $MetadataPath) { $MetadataPath = Join-Path $repoRoot 'cmake\clang-sdk.json' }
$meta = Get-Content -Raw -Encoding UTF8 -LiteralPath $MetadataPath | ConvertFrom-Json

# --- Validate metadata that feeds filesystem paths -------------------------
$archiveRoot = [string]$meta.archive_root
if ($archiveRoot -notmatch '^[A-Za-z0-9][A-Za-z0-9._+\-]*$') {
  throw "metadata archive_root '$archiveRoot' is not a plain directory name"
}
if ([string]$meta.install_relative_dir -ne "build/deps/$archiveRoot") {
  throw "metadata install_relative_dir '$($meta.install_relative_dir)' must equal 'build/deps/$archiveRoot'"
}
$assetName = [string]$meta.asset
if ($assetName -notmatch '^[A-Za-z0-9][A-Za-z0-9._+\-]*$') { throw "metadata asset '$assetName' is not a plain file name" }
if ([string]$meta.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw "metadata sha256 is not a 64-digit hex string" }
$pinnedSha = ([string]$meta.sha256).ToLowerInvariant()
$pinnedSize = [int64]$meta.size_bytes

$buildDir = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'build'))
if (-not $DepsRoot) { $DepsRoot = Join-Path $buildDir 'deps' }
$depsDir = [System.IO.Path]::GetFullPath($DepsRoot)
if (-not $depsDir.StartsWith($buildDir + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "deps root '$depsDir' must be inside '$buildDir'"
}
$downloadDir = Join-Path $depsDir 'downloads'
$installDir = [System.IO.Path]::GetFullPath((Join-Path $depsDir $archiveRoot))
$marker = Join-Path $depsDir ("{0}-{1}.ok" -f $meta.name, $meta.version)
$archive = Join-Path $downloadDir $assetName
$sigFile = "$archive.sig"
$attFile = "$archive.jsonl"
$keysFile = Join-Path $downloadDir 'release-keys.asc'
$gnupgHome = Join-Path $depsDir 'gnupg-local'

if ($installDir.TrimEnd('\') -eq $depsDir.TrimEnd('\')) { throw "install dir must not be the deps root" }
if (-not $installDir.StartsWith($depsDir + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "install dir '$installDir' is not inside '$depsDir'"
}

# --- Reparse-point safety: every component from <repo>\build down ----------
function Test-ReparsePoint([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) { return $false }
  $item = Get-Item -LiteralPath $path -Force
  return [bool]($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
}

function Assert-NoReparseChain([string]$leaf) {
  # Check $buildDir, then each component below it up to and including $leaf.
  if (-not $leaf.StartsWith($buildDir, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "path '$leaf' is outside the project build directory"
  }
  if (Test-ReparsePoint $buildDir) { throw "refusing: '$buildDir' is a reparse point" }
  $rel = $leaf.Substring($buildDir.Length).TrimStart('\')
  $current = $buildDir
  if ($rel) {
    foreach ($part in $rel.Split('\')) {
      $current = Join-Path $current $part
      if (Test-ReparsePoint $current) {
        throw "refusing: '$current' is a reparse point (junction/symlink); no destructive action through it"
      }
    }
  }
}

Assert-NoReparseChain $installDir
Assert-NoReparseChain $downloadDir
Write-Step "path safety OK: no reparse points from $buildDir down to $installDir"

if ($ValidateOnly) {
  Write-Step "validate-only: metadata and path checks passed; nothing downloaded, extracted or deleted"
  exit 0
}

New-Item -ItemType Directory -Force $downloadDir | Out-Null

function Test-RequiredPaths($root) {
  foreach ($rel in $meta.required_paths) {
    if (-not (Test-Path -LiteralPath (Join-Path $root ($rel -replace '/', '\')))) { return $false }
  }
  return $true
}

# --- Idempotency: marker AND files must both be present --------------------
if (-not $Force -and (Test-Path -LiteralPath $marker)) {
  $recorded = (Get-Content -Raw -LiteralPath $marker).Trim()
  if ($recorded -eq $pinnedSha -and (Test-Path -LiteralPath $installDir) -and (Test-RequiredPaths $installDir)) {
    Write-Step "already bootstrapped: $installDir (marker matches pinned SHA-256, required paths present)"
    exit 0
  }
  Write-Step "marker present but SDK incomplete or pin changed; re-bootstrapping"
}

# --- 1. Download with integrity-driven retry -------------------------------
function Invoke-Download($url, $target, [bool]$resume) {
  $mode = if ($resume) { 'resuming' } else { 'from zero' }
  Write-Step "downloading ($mode) $url"
  if ($resume) {
    Invoke-Native { & curl.exe -L --fail --retry 8 --retry-delay 5 --retry-all-errors -C - --max-time 3600 -sS -o $target $url }
  } else {
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
    Invoke-Native { & curl.exe -L --fail --retry 8 --retry-delay 5 --retry-all-errors --max-time 3600 -sS -o $target $url }
  }
  if ($LASTEXITCODE -ne 0) { throw "download failed with curl exit code $LASTEXITCODE for $url" }
}

function Get-ArchiveHash { return (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant() }

$freshAttempted = $false
$attempt = 0
while ($true) {
  $attempt++
  $exists = Test-Path -LiteralPath $archive
  $size = if ($exists) { (Get-Item -LiteralPath $archive).Length } else { 0 }
  if ($Force -and -not $freshAttempted) {
    Write-Step "-Force: discarding cached archive"
    Invoke-Download $meta.url $archive $false
    $freshAttempted = $true
  } elseif ($exists -and $size -eq $pinnedSize) {
    Write-Step "cached archive has the pinned size; verifying hash"
  } elseif ($exists -and $size -gt $pinnedSize) {
    Write-Step "cached archive is larger than the pin ($size > $pinnedSize); discarding"
    Invoke-Download $meta.url $archive $false
    $freshAttempted = $true
  } else {
    Invoke-Download $meta.url $archive $true
  }

  $actualSize = (Get-Item -LiteralPath $archive).Length
  $actualHash = Get-ArchiveHash
  if ($actualSize -eq $pinnedSize -and $actualHash -eq $pinnedSha) {
    Write-Step "size OK ($actualSize bytes)"
    Write-Step "SHA-256 OK ($actualHash)"
    break
  }
  Write-Step "integrity mismatch: size $actualSize (pinned $pinnedSize), SHA-256 $actualHash (pinned $pinnedSha)"
  if ($freshAttempted) {
    throw "SHA-256 mismatch persists after a fresh download from zero. Recovery: check the pin in $MetadataPath and the network path, then delete '$archive' and rerun."
  }
  Write-Step "discarding cached archive and downloading fresh from zero (bounded to one retry)"
  Remove-Item -LiteralPath $archive -Force
  Invoke-Download $meta.url $archive $false
  $freshAttempted = $true
  $actualSize = (Get-Item -LiteralPath $archive).Length
  $actualHash = Get-ArchiveHash
  if ($actualSize -eq $pinnedSize -and $actualHash -eq $pinnedSha) {
    Write-Step "size OK ($actualSize bytes)"
    Write-Step "SHA-256 OK ($actualHash) after fresh download"
    break
  }
  throw "SHA-256 mismatch persists after a fresh download from zero (got $actualHash, pinned $pinnedSha). Recovery: check the pin in $MetadataPath and the network path, then delete '$archive' and rerun."
}

# --- 2. Signature verification (reported separately from the hash check) ---
$signatureStatus = 'not attempted'
if (-not $SkipSignature) {
  if (-not [string]$meta.signature_url -or -not [string]$meta.release_keys_url) {
    $signatureStatus = 'skipped: pin has no signature/keys URL'
  } else {
    $gpgExe = $null
    $cmd = Get-Command gpg.exe -ErrorAction SilentlyContinue
    if ($cmd) { $gpgExe = $cmd.Source }
    elseif (Test-Path -LiteralPath 'C:\Program Files\Git\usr\bin\gpg.exe') { $gpgExe = 'C:\Program Files\Git\usr\bin\gpg.exe' }
    if ($gpgExe) {
      Invoke-Download $meta.signature_url $sigFile $false
      Invoke-Download $meta.release_keys_url $keysFile $false
      New-Item -ItemType Directory -Force $gnupgHome | Out-Null
      $env:GNUPGHOME = $gnupgHome
      $gpgLog = Join-Path $gnupgHome 'gpg.log'
      Invoke-Native { & $gpgExe --batch --quiet --no-tty --logger-file $gpgLog --import $keysFile }
      $verify = Invoke-Native { & $gpgExe --batch --no-tty --status-fd 1 --logger-file $gpgLog --verify $sigFile $archive }
      $good = @($verify) | Select-String -Pattern '^\[GNUPG:\] VALIDSIG ([0-9A-F]+)'
      if ($good) {
        $fpr = $good.Matches[0].Groups[1].Value
        if ($fpr -eq [string]$meta.expected_signing_key_fingerprint) {
          $signatureStatus = "valid OpenPGP signature by pinned key $fpr (keyring fetched from $($meta.release_keys_url) over HTTPS; no web-of-trust)"
        } else {
          throw "signature valid but signed by unexpected key $fpr (pinned $($meta.expected_signing_key_fingerprint))"
        }
      } else {
        throw "OpenPGP signature verification failed"
      }
    } else {
      $signatureStatus = 'skipped: gpg.exe not available'
    }
  }
}
Write-Step "signature: $signatureStatus"
if ([string]$meta.attestation_url) {
  Invoke-Download $meta.attestation_url $attFile $false
  Write-Step "attestation: $(Split-Path -Leaf $attFile) downloaded; sigstore bundle NOT verified (no gh/cosign tooling)"
}

# --- 3. Inspect archive listing before extraction --------------------------
$tar = 'C:\Windows\System32\tar.exe'
if (-not (Test-Path -LiteralPath $tar)) { throw "bsdtar not found at $tar" }
Write-Step "listing archive (this decompresses the whole archive once)"
$listing = Invoke-Native { & $tar -tf $archive }
if ($LASTEXITCODE -ne 0) { throw "archive listing failed" }
$missing = @()
foreach ($rel in $meta.required_paths) {
  if (-not ($listing -contains "$archiveRoot/$rel")) { $missing += $rel }
}
if ($missing.Count -gt 0) { throw "archive lacks required paths: $($missing -join ', ')" }
$outside = @($listing | Where-Object { -not $_.StartsWith("$archiveRoot/") -and $_ -ne $archiveRoot -and $_ -ne "$archiveRoot/" })
if ($outside.Count -gt 0) { throw "archive contains entries outside '$archiveRoot/': $($outside | Select-Object -First 5)" }
$traversal = @($listing | Where-Object { $_ -match '(^|/)\.\.(/|$)' -or $_ -match '^[A-Za-z]:' -or $_.StartsWith('/') -or $_.Contains('\') })
if ($traversal.Count -gt 0) { throw "archive contains traversal/rooted/backslash entries: $($traversal | Select-Object -First 5)" }
Write-Step "listing OK: $($listing.Count) entries, all under $archiveRoot/, required paths present"

# --- 4. Extract (the only destructive step, re-guarded) --------------------
if (Test-Path -LiteralPath $installDir) {
  Assert-NoReparseChain $installDir
  $resolved = [System.IO.Path]::GetFullPath((Get-Item -LiteralPath $installDir -Force).FullName)
  if ($resolved -ne $installDir) { throw "refusing to delete unexpected path $resolved" }
  Write-Step "removing previous extraction $resolved"
  Remove-Item -LiteralPath $resolved -Recurse -Force
}
Write-Step "extracting to $depsDir"
Invoke-Native { & $tar -xf $archive -C $depsDir }
if ($LASTEXITCODE -ne 0) { throw "extraction failed" }
if (-not (Test-RequiredPaths $installDir)) { throw "extracted tree lacks required paths" }
Set-Content -Encoding ASCII -NoNewline -LiteralPath $marker $pinnedSha
Write-Step "done: $installDir"
Write-Step "configure with: cmake --preset vs2022-x64   (the preset searches $installDir by default)"
