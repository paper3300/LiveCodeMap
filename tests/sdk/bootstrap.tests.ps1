<#
  Behaviour tests for tools/bootstrap-clang-sdk.ps1 using a tiny local archive
  served over file://. Covers: same-size corrupt cache, partial+corrupt cache,
  -Force redownload from zero, persistent source corruption (bounded retry with
  recovery hint), idempotent rerun, and the reparse-ancestor deletion guard with
  junction fixtures. Everything lives under -WorkRoot (inside the ignored build
  tree). Exit code 0 on success.
#>
param([Parameter(Mandatory = $true)][string]$WorkRoot)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$script = Join-Path $repoRoot 'tools\bootstrap-clang-sdk.ps1'
$buildDir = Join-Path $repoRoot 'build'
$WorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
if (-not $WorkRoot.StartsWith($buildDir + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "WorkRoot '$WorkRoot' must be inside '$buildDir' (tests only touch the ignored build tree)"
}

. (Join-Path $PSScriptRoot 'common.ps1')

$failures = 0
function Check([bool]$condition, [string]$what) {
  if ($condition) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what"; $script:failures++ }
}
function Reset-Dir([string]$path) { Reset-WorkRoot $buildDir $path }
function Run-Bootstrap([string[]]$extra) {
  # Child stderr must not become a terminating error in PowerShell 5.1.
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script @extra 2>&1 | Out-String
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $previous }
  return @{ exit = $code; out = $out }
}

Reset-Dir $WorkRoot
$srcDir = Join-Path $WorkRoot 'source'
New-Item -ItemType Directory -Force $srcDir | Out-Null

# --- Build a tiny archive with the required layout --------------------------
$archiveRoot = 'tiny-sdk-1.0.0'
$tree = Join-Path $srcDir $archiveRoot
foreach ($rel in @('lib\cmake\clang', 'lib\cmake\llvm', 'include\clang\Tooling', 'lib\clang\1\include')) {
  New-Item -ItemType Directory -Force (Join-Path $tree $rel) | Out-Null
}
Set-Content -LiteralPath (Join-Path $tree 'lib\cmake\clang\ClangConfig.cmake') -Value '# fixture' -Encoding ASCII
Set-Content -LiteralPath (Join-Path $tree 'lib\cmake\llvm\LLVMConfig.cmake') -Value '# fixture' -Encoding ASCII
Set-Content -LiteralPath (Join-Path $tree 'lib\clangTooling.lib') -Value ('x' * 4096) -Encoding ASCII
Set-Content -LiteralPath (Join-Path $tree 'include\clang\Tooling\Tooling.h') -Value '// fixture' -Encoding ASCII
$asset = 'tiny-sdk-1.0.0-x86_64-pc-windows-msvc.tar.xz'
$archivePath = Join-Path $srcDir $asset
Push-Location $srcDir
try { & 'C:\Windows\System32\tar.exe' -cJf $asset $archiveRoot } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { throw 'fixture archive creation failed' }
$size = (Get-Item -LiteralPath $archivePath).Length
$sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()
$url = 'file:///' + ($archivePath -replace '\\', '/')

function Write-Meta([string]$path, [string]$sha256) {
  $meta = [ordered]@{
    name = 'tiny-sdk'; version = '1.0.0'; asset = $asset; url = $url
    size_bytes = $size; sha256 = $sha256
    signature_url = ''; attestation_url = ''; release_keys_url = ''
    expected_signing_key_fingerprint = ''
    archive_root = $archiveRoot
    required_paths = @('lib/cmake/clang/ClangConfig.cmake', 'lib/cmake/llvm/LLVMConfig.cmake', 'lib/clangTooling.lib', 'include/clang/Tooling/Tooling.h')
    install_relative_dir = "build/deps/$archiveRoot"
  }
  $meta | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath $path
}
$metaGood = Join-Path $WorkRoot 'pin-good.json'
$metaBad = Join-Path $WorkRoot 'pin-badhash.json'
Write-Meta $metaGood $sha
Write-Meta $metaBad ('0' * 64)

function Corrupt-SameSize([string]$file) {
  $bytes = [System.IO.File]::ReadAllBytes($file)
  $bytes[[int]($bytes.Length / 2)] = $bytes[[int]($bytes.Length / 2)] -bxor 0xFF
  [System.IO.File]::WriteAllBytes($file, $bytes)
}

# --- Case 1: clean bootstrap ------------------------------------------------
Write-Host 'case 1: clean bootstrap from file:// source'
$deps1 = Join-Path $WorkRoot 'deps1'
$r = Run-Bootstrap @('-MetadataPath', $metaGood, '-DepsRoot', $deps1, '-SkipSignature')
Check ($r.exit -eq 0) 'exit 0'
Check (Test-Path -LiteralPath (Join-Path $deps1 "$archiveRoot\lib\clangTooling.lib")) 'extracted required path present'
Check ((Get-Content -Raw -LiteralPath (Join-Path $deps1 'tiny-sdk-1.0.0.ok')).Trim() -eq $sha) 'marker holds pinned hash'
$r2 = Run-Bootstrap @('-MetadataPath', $metaGood, '-DepsRoot', $deps1, '-SkipSignature')
Check ($r2.exit -eq 0 -and $r2.out -match 'already bootstrapped') 'second run is an idempotent no-op'

# --- Case 2: same-size corrupt cache is detected and redownloaded ----------
Write-Host 'case 2: same-size corrupt cache'
$deps2 = Join-Path $WorkRoot 'deps2'
New-Item -ItemType Directory -Force (Join-Path $deps2 'downloads') | Out-Null
Copy-Item -LiteralPath $archivePath -Destination (Join-Path $deps2 "downloads\$asset")
Corrupt-SameSize (Join-Path $deps2 "downloads\$asset")
$r = Run-Bootstrap @('-MetadataPath', $metaGood, '-DepsRoot', $deps2, '-SkipSignature')
Check ($r.exit -eq 0) 'exit 0 after fresh redownload'
Check ($r.out -match 'integrity mismatch' -and $r.out -match 'from zero') 'mismatch reported and fresh download taken'
Check ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $deps2 "downloads\$asset")).Hash.ToLowerInvariant() -eq $sha) 'cache now matches pin'
Check (Test-Path -LiteralPath (Join-Path $deps2 "$archiveRoot\lib\clangTooling.lib")) 'extraction happened'

# --- Case 3: partial + corrupt cache ----------------------------------------
Write-Host 'case 3: partial and corrupt cache (resume would not repair it)'
$deps3 = Join-Path $WorkRoot 'deps3'
New-Item -ItemType Directory -Force (Join-Path $deps3 'downloads') | Out-Null
$bytes = [System.IO.File]::ReadAllBytes($archivePath)
$half = $bytes[0..([int]($bytes.Length / 2))]
$half[3] = $half[3] -bxor 0xFF
[System.IO.File]::WriteAllBytes((Join-Path $deps3 "downloads\$asset"), $half)
$r = Run-Bootstrap @('-MetadataPath', $metaGood, '-DepsRoot', $deps3, '-SkipSignature')
Check ($r.exit -eq 0) 'exit 0'
Check ($r.out -match 'integrity mismatch') 'mismatch after resume reported'
Check ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $deps3 "downloads\$asset")).Hash.ToLowerInvariant() -eq $sha) 'fresh download repaired the cache'

# --- Case 4: -Force redownloads from zero even with a same-size corrupt cache
Write-Host 'case 4: -Force with same-size corrupt cache'
$deps4 = Join-Path $WorkRoot 'deps4'
New-Item -ItemType Directory -Force (Join-Path $deps4 'downloads') | Out-Null
Copy-Item -LiteralPath $archivePath -Destination (Join-Path $deps4 "downloads\$asset")
Corrupt-SameSize (Join-Path $deps4 "downloads\$asset")
$r = Run-Bootstrap @('-MetadataPath', $metaGood, '-DepsRoot', $deps4, '-SkipSignature', '-Force')
Check ($r.exit -eq 0) 'exit 0'
Check ($r.out -match '-Force: discarding cached archive') 'force discarded the cache'
Check (-not ($r.out -match 'integrity mismatch')) 'no mismatch: first download was already fresh'
Check ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $deps4 "downloads\$asset")).Hash.ToLowerInvariant() -eq $sha) 'cache matches pin'

# --- Case 5: persistent mismatch (wrong pin) stops after one fresh retry ---
Write-Host 'case 5: source does not match the pin'
$deps5 = Join-Path $WorkRoot 'deps5'
$r = Run-Bootstrap @('-MetadataPath', $metaBad, '-DepsRoot', $deps5, '-SkipSignature')
Check ($r.exit -ne 0) 'non-zero exit'
Check ($r.out -match 'SHA-256 mismatch persists after a fresh download') 'bounded retry then clear failure'
Check ($r.out -match 'Recovery:') 'recovery hint present'
Check (-not (Test-Path -LiteralPath (Join-Path $deps5 $archiveRoot))) 'nothing extracted'
Check (-not (Test-Path -LiteralPath (Join-Path $deps5 'tiny-sdk-1.0.0.ok'))) 'no marker written'

# --- Case 6: reparse-point guard (non-destructive) --------------------------
Write-Host 'case 6: junction ancestors are refused before any destructive action'
$canaryDir = Join-Path $WorkRoot 'canary-target'
New-Item -ItemType Directory -Force $canaryDir | Out-Null
Set-Content -LiteralPath (Join-Path $canaryDir 'canary.txt') -Value 'do not delete' -Encoding ASCII

# 6a: install dir itself is a junction to the canary directory
$deps6a = Join-Path $WorkRoot 'deps6a'
New-Item -ItemType Directory -Force $deps6a | Out-Null
New-Item -ItemType Junction -Path (Join-Path $deps6a $archiveRoot) -Target $canaryDir | Out-Null
$r = Run-Bootstrap @('-MetadataPath', $metaGood, '-DepsRoot', $deps6a, '-SkipSignature', '-ValidateOnly')
Check ($r.exit -ne 0 -and $r.out -match 'reparse point') 'install dir junction refused in validate-only'
$r = Run-Bootstrap @('-MetadataPath', $metaGood, '-DepsRoot', $deps6a, '-SkipSignature', '-Force')
Check ($r.exit -ne 0 -and $r.out -match 'reparse point') 'install dir junction refused in a full -Force run'
Check (Test-Path -LiteralPath (Join-Path $canaryDir 'canary.txt')) 'canary survived'

# 6b: the deps dir itself is a junction (ancestor of the install dir)
$deps6bTarget = Join-Path $WorkRoot 'deps6b-real'
New-Item -ItemType Directory -Force (Join-Path $deps6bTarget $archiveRoot) | Out-Null
Set-Content -LiteralPath (Join-Path $deps6bTarget "$archiveRoot\canary.txt") -Value 'do not delete' -Encoding ASCII
$deps6b = Join-Path $WorkRoot 'deps6b'
New-Item -ItemType Junction -Path $deps6b -Target $deps6bTarget | Out-Null
$r = Run-Bootstrap @('-MetadataPath', $metaGood, '-DepsRoot', $deps6b, '-SkipSignature', '-Force')
Check ($r.exit -ne 0 -and $r.out -match 'reparse point') 'deps dir junction (ancestor) refused'
Check (Test-Path -LiteralPath (Join-Path $deps6bTarget "$archiveRoot\canary.txt")) 'canary behind ancestor junction survived'

# 6c: plain directories pass validation
$r = Run-Bootstrap @('-MetadataPath', $metaGood, '-DepsRoot', (Join-Path $WorkRoot 'deps6c'), '-SkipSignature', '-ValidateOnly')
Check ($r.exit -eq 0 -and $r.out -match 'path safety OK') 'plain directories pass validate-only'

# 6d: deps root outside build/ is refused
$r = Run-Bootstrap @('-MetadataPath', $metaGood, '-DepsRoot', (Join-Path $repoRoot 'deps-outside'), '-SkipSignature', '-ValidateOnly')
Check ($r.exit -ne 0 -and $r.out -match 'must be inside') 'deps root outside build/ refused'
Check (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'deps-outside'))) 'nothing created outside build/'

# Clean up junctions (remove the link objects only, never their targets).
foreach ($j in @((Join-Path $deps6a $archiveRoot), $deps6b)) {
  if (Test-Path -LiteralPath $j) { [System.IO.Directory]::Delete($j) }
}

# --- Case 7: the test harness's own reset guard --------------------------------
Write-Host 'case 7: work-root reset refuses junctions at the work root itself and at a parent'
Test-ReparseGuard $buildDir (Join-Path $WorkRoot 'guard') ${function:Check}

if ($failures -gt 0) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host 'all bootstrap checks passed'
exit 0
