# Selecting a replay profile

`lcm_replay` and `lcm_index` can select a named build configuration from a JSON file. The
`active_profile` is the default; `--profile` selects another profile for one
run. `DebugGame` and `Development` must have separate original compiler
arguments, response files and PCH producer inputs. Selecting a name never
rewrites one configuration's paths into the other configuration.

This workspace's private example lives under `Workspace/Private/` (ignored by
Git). Change its
`active_profile` to `Development` or `DebugGame` to choose the default.

```powershell
lcm_replay --config profiles.json --unit land_enter_task --out debuggame.json
lcm_replay --config profiles.json --profile Development --unit land_enter_task --out development.json
lcm_index --config profiles.json --state-dir index-state --report debuggame-index.json
lcm_index --config profiles.json --profile Development --state-dir index-state --report development-index.json
```

The config schema is `lcm.replay-profiles.v1`:

```json
{
  "schema": "lcm.replay-profiles.v1",
  "active_profile": "DebugGame",
  "repository_root": "D:/project",
  "resource_dir": "D:/clang/lib/clang/22",
  "profiles": {
    "DebugGame": {
      "profile_key": "Editor/Win64/DebugGame/x64",
      "uht_applicable": true,
      "directory": "D:/project/Engine/Source",
      "units": {
        "example": {
          "source": "D:/project/Source/example.cpp",
          "arguments": ["C:/path/to/cl.exe", "@D:/project/DebugGame/example.rsp"],
          "producer_source": "D:/project/DebugGame/SharedPCH.cpp",
          "producer_arguments": ["C:/path/to/cl.exe", "@D:/project/DebugGame/SharedPCH.rsp"]
        }
      }
    },
    "Development": {
      "profile_key": "Editor/Win64/Development/x64",
      "uht_applicable": true,
      "directory": "D:/project/Engine/Source",
      "units": {
        "example": {
          "source": "D:/project/Source/example.cpp",
          "arguments": ["C:/path/to/cl.exe", "@D:/project/Development/example.rsp"],
          "producer_source": "D:/project/Development/SharedPCH.cpp",
          "producer_arguments": ["C:/path/to/cl.exe", "@D:/project/Development/SharedPCH.rsp"]
        }
      }
    }
  }
}
```

All path fields and response-file paths in arguments are absolute. A unit may
omit both producer fields when it uses no PCH. Optional unit checks are
`expect_type`, `expect_direct_bases`, `expect_virtual_bases`,
`expect_direct_includes` and `evidence_limit` for replay. A unity action may
name its direct original files in `unity_members` for indexing. The replay
report records the selected profile and unit along with the actual raw commands.
An unknown profile or unit fails before analysis. The manual replay arguments
remain supported.

`lcm_index` treats all configured units of the chosen profile as the
authoritative selected set. It creates a profile-specific compilation database
in `--state-dir` (each distinct PCH producer entry is written once, however many
units name it) and stores contributions in its v3 `index.sqlite` (an older
database there is refused; use a new state directory). `--jobs <n>` analyses up
to n changed units at a time (default 1) and still publishes them in one batch.
The `profile_key` keeps DebugGame and Development facts separate in that database;
unchanged inputs do not start another analysis. The current private
config contains one selected task TU in each profile. Adding all current UBT
actions and validating their unity mapping remains separate work.

Neither tool discovers UBT actions, runs UBT/UHT or proves current build
currency. Query/CLI/MCP integration is still pending. The project's `Debug`
CMake build preset builds LiveCodeMap itself and is separate from Unreal's
`DebugGame` profile.
