# Shared safety helpers for the SDK script tests. Dot-source this file.
#
# Every recursive enumeration or deletion of a test work root is preceded by
# Assert-NoReparseChain: the project build directory and every existing path
# component from it down to the work root must not be a reparse point
# (junction/symlink). GetFullPath does not resolve junction ancestors, so the
# chain is checked component by component.

function Test-ReparsePoint([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) { return $false }
  $item = Get-Item -LiteralPath $path -Force
  return [bool]($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
}

function Assert-NoReparseChain([string]$buildDir, [string]$leaf) {
  $buildDir = [System.IO.Path]::GetFullPath($buildDir)
  $leaf = [System.IO.Path]::GetFullPath($leaf)
  if (-not $leaf.StartsWith($buildDir, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "path '$leaf' is outside the project build directory '$buildDir'"
  }
  if (Test-ReparsePoint $buildDir) { throw "refusing: '$buildDir' is a reparse point" }
  $rel = $leaf.Substring($buildDir.Length).TrimStart('\')
  $current = $buildDir
  if ($rel) {
    foreach ($part in $rel.Split('\')) {
      $current = Join-Path $current $part
      if (Test-ReparsePoint $current) {
        throw "refusing: '$current' is a reparse point (junction/symlink); no enumeration or deletion through it"
      }
    }
  }
}

# Resets a work root: chain check first (work root itself and all ancestors),
# then unlink descendant junctions as link objects (only if their targets are
# inside the work root), then remove the plain tree.
function Reset-WorkRoot([string]$buildDir, [string]$workRoot) {
  Assert-NoReparseChain $buildDir $workRoot
  if (Test-Path -LiteralPath $workRoot) {
    $links = @(Get-ChildItem -LiteralPath $workRoot -Recurse -Directory -Force |
      Where-Object { $_.Attributes -band [System.IO.FileAttributes]::ReparsePoint } |
      Sort-Object { $_.FullName.Length } -Descending)
    foreach ($link in $links) {
      $target = [string](Get-Item -LiteralPath $link.FullName -Force).Target
      if (-not $target.StartsWith($workRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to reset '$workRoot': junction '$($link.FullName)' targets '$target' outside the work root"
      }
      [System.IO.Directory]::Delete($link.FullName)   # removes the link only
    }
    Remove-Item -LiteralPath $workRoot -Recurse -Force
  }
  New-Item -ItemType Directory -Force $workRoot | Out-Null
}

# Self-test of the guard using canaries inside $guardRoot (which must already
# have passed the chain check). Returns the number of failed checks.
function Test-ReparseGuard([string]$buildDir, [string]$guardRoot, [scriptblock]$check) {
  New-Item -ItemType Directory -Force $guardRoot | Out-Null
  $canary1 = Join-Path $guardRoot 'canary-root'
  $canary2 = Join-Path $guardRoot 'canary-parent'
  New-Item -ItemType Directory -Force $canary1 | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $canary2 'child') | Out-Null
  Set-Content -LiteralPath (Join-Path $canary1 'keep.txt') -Value 'keep' -Encoding ASCII
  Set-Content -LiteralPath (Join-Path $canary2 'child\keep.txt') -Value 'keep' -Encoding ASCII

  # (a) the work root itself is a junction
  $linkRoot = Join-Path $guardRoot 'link-root'
  New-Item -ItemType Junction -Path $linkRoot -Target $canary1 | Out-Null
  $threw = $false
  try { Reset-WorkRoot $buildDir $linkRoot } catch { $threw = ($_.Exception.Message -match 'reparse point') }
  & $check $threw 'work root that is itself a junction is refused'
  & $check (Test-Path -LiteralPath (Join-Path $canary1 'keep.txt')) 'canary behind work-root junction survived'

  # (b) a parent of the work root is a junction
  $linkParent = Join-Path $guardRoot 'link-parent'
  New-Item -ItemType Junction -Path $linkParent -Target $canary2 | Out-Null
  $threw = $false
  try { Reset-WorkRoot $buildDir (Join-Path $linkParent 'child') } catch { $threw = ($_.Exception.Message -match 'reparse point') }
  & $check $threw 'work root under a junction parent is refused'
  & $check (Test-Path -LiteralPath (Join-Path $canary2 'child\keep.txt')) 'canary behind parent junction survived'

  # (c) plain directory passes
  $plain = Join-Path $guardRoot 'plain'
  New-Item -ItemType Directory -Force $plain | Out-Null
  $threw = $false
  try { Reset-WorkRoot $buildDir $plain } catch { $threw = $true }
  & $check (-not $threw) 'plain work root passes the chain check'

  # unlink the junction leaves created here (link objects only)
  foreach ($j in @($linkRoot, $linkParent)) { if (Test-Path -LiteralPath $j) { [System.IO.Directory]::Delete($j) } }
}
