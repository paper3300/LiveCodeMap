<#
  Behaviour tests for cmake/ClangSdk.cmake.

  Fixture strategy (safety): rejection cases use small ISOLATED STUB CMake
  packages generated here (a few tiny files plus, where needed, a stub
  library compiled with /MD). No junctions, symlinks, hard links or copies of
  the real SDK are ever created. The genuine "compatible override accepted"
  case points LCM_CLANG_SDK_DIR at the real bootstrapped SDK read-only.
  Cleanup never traverses a reparse point: it refuses if one exists.

  Cases:
    A  required SDK missing is a configure error
    B  incomplete override (package files only) rejected, missing paths named
    C  stub declaring CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL rejected
    D  stub declaring LLVM_ENABLE_RTTI ON rejected
    E  stub declaring another version rejected
    H  stub with correct declarations but an incompatible /MD stub clangBasic.lib
       is rejected by the compile/link probe (declared metadata is not trusted)
    H2 stub that declares no CRT at all (would inherit ours) is rejected by the probe
    F  real SDK passed as override: accepted, ABI probe passed, flagged PROVENANCE UNVERIFIED
    G  default location: stub tree without marker / wrong marker not accepted,
       pinned marker accepted as verified (no probe on the default path)
#>
param(
  [Parameter(Mandatory = $true)][string]$WorkRoot,
  [string]$Generator = 'Visual Studio 17 2022',
  [string]$Platform = 'x64',
  [string]$SdkRoot = '',
  [string]$Compiler = ''   # cl.exe used to build the /MD stub library
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$probe = Join-Path $PSScriptRoot 'probe-project'
$WorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
$buildDir = Join-Path $repoRoot 'build'
if (-not $WorkRoot.StartsWith($buildDir + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "WorkRoot '$WorkRoot' must be inside '$buildDir'"
}

# Cleanup policy: the chain from the project build directory down to the
# work root (inclusive) must contain no reparse point before any enumeration
# or deletion; the fixtures created here contain no reparse points at all, and
# any found inside the work root with a target outside it stop the run.
. (Join-Path $PSScriptRoot 'common.ps1')
Reset-WorkRoot $buildDir $WorkRoot

$failures = 0
$script:lastOutput = ''
function Check([bool]$condition, [string]$what) {
  if ($condition) { Write-Host "  ok   $what" } else {
    Write-Host "  FAIL $what"
    $script:failures++
    if ($script:lastOutput) {
      $tail = $script:lastOutput
      if ($tail.Length -gt 3000) { $tail = $tail.Substring($tail.Length - 3000) }
      Write-Host '  ---- output tail ----'; Write-Host $tail; Write-Host '  ---------------------'
      $script:lastOutput = ''
    }
  }
}
function Configure([string]$name, [string[]]$defs) {
  $bin = Join-Path $WorkRoot "bin-$name"
  $args = @('-S', $probe, '-B', $bin, '-G', $Generator, "-DLCM_PROJECT_ROOT=$repoRoot") + $defs
  if ($Platform) { $args += @('-A', $Platform) }
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $out = & cmake.exe @args 2>&1 | Out-String
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $previous }
  $script:lastOutput = $out
  return @{ exit = $code; out = $out }
}

$meta = Get-Content -Raw -Encoding UTF8 (Join-Path $repoRoot 'cmake\clang-sdk.json') | ConvertFrom-Json

# --- Stub package builder ------------------------------------------------------
# Creates <WorkRoot>/<name> with every required path as a tiny dummy file, a
# fake clang/Basic/Version.h, and stub LLVMConfig.cmake / ClangConfig.cmake
# whose declared values come from $decl. When $stubLib is given it is placed
# as lib/clangBasic.lib and exported as the imported target clangBasic.
function New-StubSdk([string]$name, [hashtable]$decl, [string]$stubLib = '') {
  $root = Join-Path $WorkRoot $name
  foreach ($rel in $meta.required_paths) {
    $p = Join-Path $root ($rel -replace '/', '\')
    New-Item -ItemType Directory -Force (Split-Path -Parent $p) | Out-Null
    Set-Content -LiteralPath $p -Value '// stub' -Encoding ASCII
  }
  $resource = Join-Path $root "lib\clang\$($meta.version.Split('.')[0])\include"
  New-Item -ItemType Directory -Force $resource | Out-Null
  Set-Content -LiteralPath (Join-Path $resource 'stddef.h') -Value '// stub' -Encoding ASCII
  New-Item -ItemType Directory -Force (Join-Path $root 'include\clang\Basic') | Out-Null
  Set-Content -LiteralPath (Join-Path $root 'include\clang\Basic\Version.h') -Encoding ASCII -Value @'
#pragma once
#include <string>
namespace clang { std::string getClangFullVersion(); }
'@
  if ($stubLib) { Copy-Item -LiteralPath $stubLib -Destination (Join-Path $root 'lib\clangBasic.lib') -Force }
  $rootCMake = $root -replace '\\', '/'
  $lines = @()
  foreach ($k in $decl.Keys) { $lines += "set($k $($decl[$k]))" }
  $lines += "set(LLVM_INCLUDE_DIRS `"$rootCMake/include`")"
  $lines += 'set(LLVM_DEFINITIONS "")'
  $lines += 'set(LLVM_AVAILABLE_LIBS "")'
  Set-Content -LiteralPath (Join-Path $root 'lib\cmake\llvm\LLVMConfig.cmake') -Value ($lines -join "`n") -Encoding ASCII
  $clang = @(
    "set(CLANG_INCLUDE_DIRS `"$rootCMake/include`")",
    'if(NOT TARGET clangBasic)',
    '  add_library(clangBasic STATIC IMPORTED)',
    "  set_target_properties(clangBasic PROPERTIES IMPORTED_LOCATION `"$rootCMake/lib/clangBasic.lib`")",
    'endif()'
  )
  Set-Content -LiteralPath (Join-Path $root 'lib\cmake\clang\ClangConfig.cmake') -Value ($clang -join "`n") -Encoding ASCII
  return $root
}

$goodDecl = [ordered]@{
  LLVM_VERSION_MAJOR = $meta.version.Split('.')[0]
  LLVM_PACKAGE_VERSION = $meta.version
  LLVM_BUILD_TYPE = 'Release'
  CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreaded'
  LLVM_ENABLE_RTTI = 'OFF'
  LLVM_ENABLE_ASSERTIONS = 'OFF'
}
function With([hashtable]$base, [hashtable]$changes) {
  $copy = [ordered]@{}
  foreach ($k in $base.Keys) { $copy[$k] = $base[$k] }
  foreach ($k in $changes.Keys) { if ($null -eq $changes[$k]) { $copy.Remove($k) } else { $copy[$k] = $changes[$k] } }
  return $copy
}

# Build two stub clangBasic.lib variants implementing exactly the probed
# symbol: one with the static release CRT (/MT, compatible) and one with the
# dynamic CRT (/MD, incompatible). Built with the project's own generator so
# the toolchain environment is correct; isolated, nothing to do with the real SDK.
function Build-StubLib([string]$runtime) {
  $bin = Join-Path $WorkRoot "stub-$runtime"
  $src = Join-Path $PSScriptRoot 'stub-lib'
  $args = @('-S', $src, '-B', $bin, '-G', $Generator, "-DSTUB_RUNTIME=$runtime", "-DSTUB_VERSION=$($meta.version)")
  if ($Platform) { $args += @('-A', $Platform) }
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & cmake.exe @args 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { & cmake.exe --build $bin --config Release 2>&1 | Out-Null }
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $previous }
  $lib = Join-Path $bin 'out\clangBasic.lib'
  if ($code -eq 0 -and (Test-Path -LiteralPath $lib)) { return $lib }
  Write-Host "  note: stub library ($runtime) build failed (exit $code)"
  return ''
}
$stubMT = Build-StubLib 'MultiThreaded'
$stubMD = Build-StubLib 'MultiThreadedDLL'
$stubLib = $stubMD   # rejection cases below use the incompatible variant unless stated

# --- Case A --------------------------------------------------------------------
Write-Host 'case A: no SDK available'
$r = Configure 'none' @('-DLCM_CLANG_SDK_SEARCH_DEFAULT=OFF')
Check ($r.exit -ne 0 -and $r.out -match 'Clang SDK not found') 'required SDK missing is a configure error'

# --- Case B --------------------------------------------------------------------
Write-Host 'case B: incomplete override (package files only)'
$incomplete = Join-Path $WorkRoot 'sdk-incomplete'
New-Item -ItemType Directory -Force (Join-Path $incomplete 'lib\cmake\clang') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $incomplete 'lib\cmake\llvm') | Out-Null
Set-Content -LiteralPath (Join-Path $incomplete 'lib\cmake\clang\ClangConfig.cmake') -Value '# stub' -Encoding ASCII
Set-Content -LiteralPath (Join-Path $incomplete 'lib\cmake\llvm\LLVMConfig.cmake') -Value '# stub' -Encoding ASCII
$r = Configure 'incomplete' @("-DLCM_CLANG_SDK_DIR=$incomplete")
Check ($r.exit -ne 0 -and $r.out -match 'rejected: incomplete SDK') 'incomplete override rejected'
Check ($r.out -match 'clangTooling.lib') 'missing library is listed'

# --- Cases C, D, E: declared-metadata rejections -------------------------------
Write-Host 'case C: stub declares MultiThreadedDLL'
$c = New-StubSdk 'sdk-wrong-crt' (With $goodDecl @{ CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreadedDLL' }) $stubLib
$r = Configure 'wrongcrt' @("-DLCM_CLANG_SDK_DIR=$c")
Check ($r.exit -ne 0 -and $r.out -match 'rejected' -and $r.out -match 'MultiThreadedDLL') 'wrong declared CRT rejected'

Write-Host 'case D: stub declares RTTI ON'
$d = New-StubSdk 'sdk-rtti' (With $goodDecl @{ LLVM_ENABLE_RTTI = 'ON' }) $stubLib
$r = Configure 'rtti' @("-DLCM_CLANG_SDK_DIR=$d")
Check ($r.exit -ne 0 -and $r.out -match 'rejected' -and $r.out -match 'RTTI') 'RTTI mismatch rejected'

Write-Host 'case E: stub declares another version'
$e = New-StubSdk 'sdk-version' (With $goodDecl @{ LLVM_PACKAGE_VERSION = '99.0.0' }) $stubLib
$r = Configure 'version' @("-DLCM_CLANG_SDK_DIR=$e")
Check ($r.exit -ne 0 -and $r.out -match 'rejected' -and $r.out -match '99.0.0') 'version mismatch rejected'

# --- Cases H..H4: the ABI probe is decisive; declarations are checked when present
if ($stubMT -and $stubMD) {
  Write-Host 'case H: correct declarations, /MD stub implementing the probed symbol'
  $h = New-StubSdk 'sdk-badlib' $goodDecl $stubMD
  $r = Configure 'badlib' @("-DLCM_CLANG_SDK_DIR=$h")
  Check ($r.exit -ne 0 -and $r.out -match 'compile/link probe against') 'ABI probe rejects the /MD stub library'
  Check ($r.out -match 'LNK2038' -and $r.out -match 'RuntimeLibrary') 'failure is specifically the CRT mismatch (LNK2038 RuntimeLibrary)'
  Check (-not ($r.out -match 'LNK2019')) 'failure is not an unresolved symbol'

  Write-Host 'case H2: no CRT declared, /MD stub library'
  $h2 = New-StubSdk 'sdk-nocrt-md' (With $goodDecl @{ CMAKE_MSVC_RUNTIME_LIBRARY = $null }) $stubMD
  $r = Configure 'nocrtmd' @("-DLCM_CLANG_SDK_DIR=$h2")
  Check ($r.exit -ne 0 -and $r.out -match 'compile/link probe against' -and $r.out -match 'LNK2038') 'undeclared CRT with /MD libraries fails by real ABI'

  Write-Host 'case H3: no CRT declared, /MT stub library (compatible in fact)'
  $h3 = New-StubSdk 'sdk-nocrt-mt' (With $goodDecl @{ CMAKE_MSVC_RUNTIME_LIBRARY = $null }) $stubMT
  $r = Configure 'nocrtmt' @("-DLCM_CLANG_SDK_DIR=$h3")
  Check ($r.exit -eq 0 -and $r.out -match 'override ABI probe passed') 'undeclared CRT with /MT libraries passes by real ABI'
  Check ($r.out -match 'does not declare a CRT') 'the missing declaration is reported'
  Check ($r.out -match 'PROVENANCE UNVERIFIED') 'still flagged provenance unverified'

  Write-Host 'case H4: declared MultiThreaded, /MT stub library (control)'
  $h4 = New-StubSdk 'sdk-goodlib' $goodDecl $stubMT
  $r = Configure 'goodlib' @("-DLCM_CLANG_SDK_DIR=$h4")
  Check ($r.exit -eq 0 -and $r.out -match 'override ABI probe passed') 'same probe passes with the compatible stub'
} else {
  Write-Host 'cases H..H4 skipped (stub libraries unavailable)'
  $failures++
}

# --- Case F: genuine override against the real SDK (read-only) -----------------
if ($SdkRoot -and (Test-Path -LiteralPath (Join-Path $SdkRoot 'lib\cmake\llvm\LLVMConfig.cmake'))) {
  Write-Host 'case F: real SDK passed as an explicit override'
  $r = Configure 'realoverride' @("-DLCM_CLANG_SDK_DIR=$SdkRoot")
  Check ($r.exit -eq 0) 'configure succeeds'
  Check ($r.out -match 'override ABI probe passed') 'the /MT link+run probe passed against the real libraries'
  Check ($r.out -match 'PROVENANCE UNVERIFIED') 'override is flagged as provenance unverified'
  Check ($r.out -match 'PROBE_RESULT: found=TRUE verified=FALSE') 'result variables report unverified'
} else {
  Write-Host 'case F skipped: real SDK root not available'
}

# --- Case G: default location marker logic with a stub tree ---------------------
Write-Host 'case G: default location present without / with provenance marker'
$fakeRoot = Join-Path $WorkRoot 'fake-project'
New-Item -ItemType Directory -Force (Join-Path $fakeRoot 'cmake') | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'cmake\ClangSdk.cmake') -Destination (Join-Path $fakeRoot 'cmake\ClangSdk.cmake')
Copy-Item -LiteralPath (Join-Path $repoRoot 'cmake\clang-sdk.json') -Destination (Join-Path $fakeRoot 'cmake\clang-sdk.json')
$defaultStub = New-StubSdk 'sdk-default-stub' $goodDecl $stubLib
$fakeDeps = Join-Path $fakeRoot 'build\deps'
New-Item -ItemType Directory -Force $fakeDeps | Out-Null
Copy-Item -LiteralPath $defaultStub -Destination (Join-Path $fakeDeps $meta.archive_root) -Recurse
$fakeMarker = Join-Path $fakeDeps "$($meta.name)-$($meta.version).ok"
$r = Configure 'nomarker' @("-DLCM_PROJECT_ROOT=$fakeRoot")
Check ($r.exit -ne 0 -and $r.out -match 'NOT accepted' -and $r.out -match 'provenance marker') 'default tree without marker is not accepted'
Set-Content -LiteralPath $fakeMarker -Value ('f' * 64) -Encoding ASCII -NoNewline
$r = Configure 'badmarker' @("-DLCM_PROJECT_ROOT=$fakeRoot")
Check ($r.exit -ne 0 -and $r.out -match 'NOT accepted') 'default tree with non-matching marker is not accepted'
Set-Content -LiteralPath $fakeMarker -Value ([string]$meta.sha256) -Encoding ASCII -NoNewline
$r = Configure 'goodmarker' @("-DLCM_PROJECT_ROOT=$fakeRoot")
Check ($r.exit -eq 0 -and $r.out -match 'PROBE_RESULT: found=TRUE verified=TRUE') 'default tree with pinned marker is accepted as verified'
Check (-not ($r.out -match 'override ABI probe')) 'default path relies on the provenance marker, not the override probe'

# Final safety assertion: the fixtures created no reparse points anywhere under WorkRoot.
$reparse = @(Get-ChildItem -LiteralPath $WorkRoot -Recurse -Directory -Force |
  Where-Object { $_.Attributes -band [System.IO.FileAttributes]::ReparsePoint })
Check ($reparse.Count -eq 0) 'no reparse points were created by the fixtures'

# --- Case I: the harness's own reset guard (canaries inside the work root) ------
Write-Host 'case I: work-root reset refuses junctions at the work root itself and at a parent'
Test-ReparseGuard $buildDir (Join-Path $WorkRoot 'guard') ${function:Check}

if ($failures -gt 0) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host 'all discovery checks passed'
exit 0
