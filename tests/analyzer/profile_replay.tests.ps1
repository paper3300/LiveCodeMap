param(
  [Parameter(Mandatory)][string]$Tool,
  [Parameter(Mandatory)][string]$IndexTool,
  [Parameter(Mandatory)][string]$Compiler,
  [Parameter(Mandatory)][string]$ResourceDir,
  [Parameter(Mandatory)][string]$ScratchDir
)

$ErrorActionPreference = 'Stop'
$work = Join-Path $ScratchDir ([guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $work -Force | Out-Null
$source = Join-Path $work 'probe.cpp'
$configPath = Join-Path $work 'profiles.json'
$debugReport = Join-Path $work 'debuggame.json'
$developmentReport = Join-Path $work 'development.json'
$manualReport = Join-Path $work 'manual.json'
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($source, "#ifdef LCM_DEBUGGAME`nstruct DebugGameType {};`n#else`nstruct DevelopmentType {};`n#endif`n", $utf8)

$config = @{
  schema = 'lcm.replay-profiles.v1'
  active_profile = 'DebugGame'
  repository_root = $work
  resource_dir = $ResourceDir
  profiles = @{
    DebugGame = @{
      directory = $work
      units = @{
        probe = @{
          source = $source
          arguments = @($Compiler, '/c', '/std:c++20', '/DLCM_DEBUGGAME=1', $source)
          expect_type = 'DebugGameType'
        }
      }
    }
    Development = @{
      directory = $work
      units = @{
        probe = @{
          source = $source
          arguments = @($Compiler, '/c', '/std:c++20', $source)
          expect_type = 'DevelopmentType'
        }
      }
    }
  }
}
[IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 10), $utf8)

& $Tool --config $configPath --unit probe --out $debugReport | Out-Null
if ($LASTEXITCODE -ne 0) { throw "DebugGame replay failed: $LASTEXITCODE" }
& $Tool --config $configPath --profile Development --unit probe --out $developmentReport | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Development replay failed: $LASTEXITCODE" }
& $Tool --repo $work --resource-dir $ResourceDir --directory $work --source $source `
  --arg cl.exe --arg /c --arg /std:c++20 --arg $source --out $manualReport | Out-Null
if ($LASTEXITCODE -ne 0) { throw "manual replay failed: $LASTEXITCODE" }

$debug = Get-Content $debugReport -Raw | ConvertFrom-Json
$development = Get-Content $developmentReport -Raw | ConvertFrom-Json
$manual = Get-Content $manualReport -Raw | ConvertFrom-Json
if ($debug.profile -ne 'DebugGame' -or $development.profile -ne 'Development' -or
    $debug.selected_unit -ne 'probe' -or $development.selected_unit -ne 'probe' -or
    $debug.status -ne 'ok' -or $development.status -ne 'ok' -or
    $manual.status -ne 'ok' -or ($manual.PSObject.Properties.Name -contains 'profile') -or
    $debug.structure.passed -ne $true -or $development.structure.passed -ne $true -or
    $debug.consumer_command.command_id -eq $development.consumer_command.command_id) {
  throw 'profile selection did not keep the two compile contexts distinct'
}

$ErrorActionPreference = 'Continue'
& $Tool --config $configPath --profile Unknown --unit probe --out (Join-Path $work 'unknown.json') 2>&1 | Out-Null
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 3) { throw "unknown profile must fail before analysis: $LASTEXITCODE" }

$stateDir = Join-Path $work 'index-state'
$debugIndexPath = Join-Path $work 'debuggame-index.json'
$developmentIndexPath = Join-Path $work 'development-index.json'
$repeatIndexPath = Join-Path $work 'debuggame-repeat.json'
& $IndexTool --config $configPath --state-dir $stateDir --report $debugIndexPath | Out-Null
if ($LASTEXITCODE -ne 0) { throw "DebugGame indexing failed: $LASTEXITCODE" }
& $IndexTool --config $configPath --profile Development --state-dir $stateDir --report $developmentIndexPath | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Development indexing failed: $LASTEXITCODE" }
& $IndexTool --config $configPath --state-dir $stateDir --report $repeatIndexPath --jobs 4 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "unchanged DebugGame indexing failed: $LASTEXITCODE" }
$ErrorActionPreference = 'Continue'
& $IndexTool --config $configPath --state-dir $stateDir --report (Join-Path $work 'jobs0.json') --jobs 0 2>&1 | Out-Null
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 2) { throw "--jobs 0 must be refused as a usage error: $LASTEXITCODE" }

# Two consumers of one PCH producer: the generated database holds the producer entry once.
$pchDir = Join-Path $work 'pch'
New-Item -ItemType Directory -Path $pchDir -Force | Out-Null
$wrapper = Join-Path $pchDir 'Wrapper.h'
$pchSource = Join-Path $pchDir 'Wrapper.cpp'
$pchBinary = Join-Path $pchDir 'Wrapper.h.pch'
$one = Join-Path $work 'one.cpp'
$two = Join-Path $work 'two.cpp'
[IO.File]::WriteAllText($wrapper, "#pragma once`nstruct FromPch { int value; };`n", $utf8)
[IO.File]::WriteAllText($pchSource, "#include `"Wrapper.h`"`n", $utf8)
[IO.File]::WriteAllText($pchBinary, 'VCPCH0 not a clang pch', $utf8)
[IO.File]::WriteAllText($one, "int one() { FromPch v{1}; return v.value; }`n", $utf8)
[IO.File]::WriteAllText($two, "int two() { FromPch v{2}; return v.value; }`n", $utf8)
$producerArguments = @($Compiler, '/c', '/std:c++20', '/TP', '/YcWrapper.h', "/Fp$pchBinary", $pchSource)
$consumer = { param($file) @($Compiler, '/c', '/std:c++20', '/TP', "/FI$wrapper", "/Yu$wrapper", "/Fp$pchBinary", $file) }
$pchConfig = @{
  schema = 'lcm.replay-profiles.v1'
  active_profile = 'Pch'
  repository_root = $work
  resource_dir = $ResourceDir
  profiles = @{
    Pch = @{
      directory = $work
      units = @{
        one = @{ source = $one; arguments = (& $consumer $one); producer_source = $pchSource; producer_arguments = $producerArguments }
        two = @{ source = $two; arguments = (& $consumer $two); producer_source = $pchSource; producer_arguments = $producerArguments }
      }
    }
  }
}
$pchConfigPath = Join-Path $work 'pch-profiles.json'
[IO.File]::WriteAllText($pchConfigPath, ($pchConfig | ConvertTo-Json -Depth 10), $utf8)
$pchState = Join-Path $work 'pch-state'
$pchReport = Join-Path $work 'pch-index.json'
& $IndexTool --config $pchConfigPath --state-dir $pchState --report $pchReport --jobs 2 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "PCH indexing failed: $LASTEXITCODE" }
$pchIndex = Get-Content $pchReport -Raw | ConvertFrom-Json
# Windows PowerShell returns a JSON array as one pipeline object; enumerate it.
$generated = @((Get-Content (Join-Path $pchState 'Pch.compile_commands.json') -Raw | ConvertFrom-Json) | ForEach-Object { $_ })
$producerKey = ($pchSource -replace '\\', '/').ToLowerInvariant()
$producerEntries = @($generated | Where-Object { ($_.file -replace '\\', '/').ToLowerInvariant() -eq $producerKey })
if ($pchIndex.outcome -ne 'committed' -or $pchIndex.actual_analyses -ne 2 -or $generated.Count -ne 3 -or
    $producerEntries.Count -ne 1) {
  throw "the generated database must hold each producer entry once ($($generated.Count) entries, $($producerEntries.Count) producer)"
}

$debugIndex = Get-Content $debugIndexPath -Raw | ConvertFrom-Json
$developmentIndex = Get-Content $developmentIndexPath -Raw | ConvertFrom-Json
$repeatIndex = Get-Content $repeatIndexPath -Raw | ConvertFrom-Json
if ($debugIndex.selected_profile -ne 'DebugGame' -or $developmentIndex.selected_profile -ne 'Development' -or
    $debugIndex.outcome -ne 'committed' -or $developmentIndex.outcome -ne 'committed' -or
    $debugIndex.actual_analyses -ne 1 -or $developmentIndex.actual_analyses -ne 1 -or
    $debugIndex.generation_after -ne 1 -or $developmentIndex.generation_after -ne 1 -or
    $repeatIndex.outcome -ne 'unchanged' -or $repeatIndex.actual_analyses -ne 0 -or
    $repeatIndex.generation_after -ne 1) {
  throw 'the indexer mixed configurations or rebuilt an unchanged profile'
}
