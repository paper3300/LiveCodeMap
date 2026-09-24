# Clang LibTooling SDK: acquisition, verification and integration

Updated 2026-09-18 (second bounded assignment). The pinned SDK is the official prebuilt LLVM 22.1.0
Windows distribution, bootstrapped project-locally under `build/deps/` (git-ignored). Nothing is
installed machine-wide, no PATH is modified, and LLVM is not built from source.

## Pin (`cmake/clang-sdk.json`)

| Field | Value |
| --- | --- |
| Release tag | `llvmorg-22.1.0` (published 2026-02-24) |
| Asset | `clang+llvm-22.1.0-x86_64-pc-windows-msvc.tar.xz` |
| URL | `https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.0/clang%2Bllvm-22.1.0-x86_64-pc-windows-msvc.tar.xz` |
| Size | 861,897,432 bytes |
| SHA-256 | `2dd6a6c990865f98766dec58cce1cbfba48e5affc0098e5b1fff96c2f1d23e81` (coordinator-supplied pin, independently matched) |
| Signature | `<asset>.sig`, OpenPGP, expected key `71046D1E9C6656BDD61171873E83BABF4A4F9E85` |
| Attestation | `<asset>.jsonl` (sigstore bundle v0.3, SLSA provenance v1) |
| Extracted root | `build/deps/clang+llvm-22.1.0-x86_64-pc-windows-msvc` |

## Verification performed on this machine

Three checks, reported separately because they establish different things:

1. **Hash verification (mandatory, passed).** Downloaded size equals the pinned size, and the
   SHA-256 computed locally (`certutil` and `Get-FileHash`) equals the pinned value. This proves the
   bytes on disk are the pinned bytes.
2. **OpenPGP signature verification (passed, limited trust anchor).** `gpg` from Git for Windows,
   using a project-local keyring (`build/deps/gnupg-local`) populated from
   `https://releases.llvm.org/release-keys.asc`, reports a good signature made 2026-02-24 by
   `71046D1E9C6656BDD61171873E83BABF4A4F9E85` (Cullen Rhodes, listed in the LLVM release keys).
   Trust in that key rests on the HTTPS fetch of the keys file, not on a web of trust; gpg prints
   the usual "not certified with a trusted signature" warning. The bootstrap script requires the
   signing fingerprint to equal the pinned one.
3. **Attestation (downloaded, NOT verified).** The sigstore bundle's in-toto statement lists the
   tarball with the same SHA-256 and names the builder
   `github.com/llvm/llvm-project/.github/workflows/release-binaries.yml@refs/tags/llvmorg-22.1.0`.
   Decoding the payload is a consistency check only; the sigstore signature and certificate chain
   were not verified because neither `gh attestation verify` nor `cosign` is available on the
   machine and installing them machine-wide is out of scope.

Archive inspection before extraction (`bsdtar -tf`): 4,741 entries, all under the single root
directory, no traversal, rooted or backslash entries. Present: `lib/cmake/clang/ClangConfig.cmake`,
`lib/cmake/llvm/LLVMConfig.cmake`, 258 `.lib` files including `clangTooling.lib`,
`clangFrontend.lib`, `clangAST.lib`, `clangBasic.lib`, `clangSerialization.lib`, `clangDriver.lib`,
`LLVMSupport.lib`; headers under `include/clang` and `include/llvm`; `bin/clang.exe`,
`bin/clang-cl.exe`, `bin/llvm-config.exe`; builtin resource headers under `lib/clang/22/include`.

## Repeatable bootstrap (`tools/bootstrap-clang-sdk.ps1`)

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\bootstrap-clang-sdk.ps1 [-Force] [-SkipSignature]
        [-MetadataPath <pin.json>] [-DepsRoot <dir under build\>] [-ValidateOnly]
```

Steps, in order: validate metadata (archive root is a plain name, install dir is exactly
`build/deps/<archive_root>`, deps root inside `build\`); reparse-point check of every path component
from `<repo>\build` down to the install and download directories (junction ancestors are refused;
`GetFullPath` does not resolve them); download; integrity; optional signature; archive listing
check for required paths and unsafe entries; extraction; required-path check; provenance marker.

Download integrity rules (tested with a tiny local `file://` archive, no large download):

- a cached archive of the pinned size is trusted only after its SHA-256 matches;
- a mismatch (same-size corruption, or a partial+corrupt cache that resume cannot repair) discards
  the cache and performs **one** fresh download from byte zero; a second mismatch stops with a
  recovery hint; nothing is extracted and no marker is written;
- `-Force` always discards the cache and downloads from zero (never `-C -`);
- a cache larger than the pin is discarded.

Idempotency: a rerun is a no-op only when the marker holds the pinned hash **and** the required
paths exist. Destructive step: only the previous `<deps>/<archive_root>` is removed, only after the
reparse chain check and a resolved-path equality check; `-ValidateOnly` runs the validation without
touching anything. Behaviour tests: `tests/sdk/bootstrap.tests.ps1` (ctest `lcm_sdk_bootstrap_tests`),
whose junction fixtures point only at canary directories inside the test work root.

## Discovery trust model (`cmake/ClangSdk.cmake`)

- **Bootstrapped default** (`build/deps/<archive_root>`): accepted only with the provenance marker
  `build/deps/<name>-<version>.ok` equal to the pinned SHA-256 **and** all required paths present.
  The marker is provenance of the acquired archive, not an attestation of the extracted bytes.
- **`LCM_CLANG_SDK_DIR` override**: provenance is not verified and configuration says so
  (`PROVENANCE UNVERIFIED`, `LCM_CLANG_SDK_VERIFIED=FALSE`). It must be complete (required paths).
  Declarations in its `LLVMConfig.cmake` are checked **when present**: the pinned version, RTTI OFF,
  Release, and an explicitly declared `CMAKE_MSVC_RUNTIME_LIBRARY` must be `MultiThreaded` (an
  explicit incompatible declaration is rejected). The decisive check is a configure-time `try_run`
  probe that compiles, links (`/MT`, `/GR-`) and runs `clang::getClangFullVersion()` against the
  imported `clangBasic` target and checks the reported version: an override with incompatible
  libraries fails with `LNK2038 RuntimeLibrary` whether or not it declares a CRT, and an override
  that declares no CRT but ships compatible `/MT` libraries passes (the missing declaration is
  reported). The project's own CRT setting is cleared around the import so a missing declaration is
  never mistaken for an inherited one. Version equality never stands in for asset integrity.
- The DIA SDK path remap runs immediately after import so every link, including the probe, sees the
  corrected `LLVMDebugInfoPDB` dependency.
- Behaviour tests: `tests/sdk/discovery.tests.ps1` (ctest `lcm_sdk_discovery_tests`) use small
  isolated stub packages (wrong declared CRT, RTTI ON, wrong version, incomplete) and stub
  `clangBasic.lib` variants that implement exactly the probed symbol with `/MD` (rejected with
  `LNK2038 RuntimeLibrary`, not an unresolved symbol) and `/MT` (accepted, also when the CRT is
  undeclared), and test the genuine override read-only against the real SDK. Fixtures never create
  junctions, symlinks, hard links or copies of the real SDK. Both scripts reset their work root only
  after checking that no path component from the project build directory down to the work root is a
  reparse point, unlink descendant junctions as link objects only when their targets lie inside the
  work root, and self-test that guard with canaries.

## Machine inventory (corrected)

| Item | Finding |
| --- | --- |
| VS 2022 Professional | MSVC 14.44.35207, Windows SDK 10.0.22621.0 / 10.0.26100.0 |
| VS "C++ Clang tools" component (`VC\Tools\Llvm`) | only `clang-format.exe` and `clang-tidy.exe`; no compiler, headers or libraries |
| vcpkg | present at `VC\vcpkg\vcpkg.exe` (bundled with VS), merely not on PATH. Earlier report said "not installed"; corrected. Not used: it would build LLVM from source. |
| gpg | `C:\Program Files\Git\usr\bin\gpg.exe` (Git for Windows) |
| gh / cosign | not available |
| bsdtar | `C:\Windows\System32\tar.exe` (libarchive 3.8.4 with liblzma) |
| Network | GitHub release assets and releases.llvm.org reachable |

## Integration constraints (verified while linking)

- **CRT: static `/MT`, not `/MD`.** The assignment assumed a `/MD` ABI; the linker's `LNK2038`
  metadata for every object in `clangTooling.lib`/`clangDriver.lib` reports
  `RuntimeLibrary=MT_StaticRelease`, and the SDK's own `LLVMConfig.cmake` (line 22) sets
  `CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded` for the importing project.
- **Actual state, corrected.** Because `find_package(LLVM)` sets that variable in the root scope,
  the SDK-enabled build had already been compiling `lcm_core` with `/MT` in every configuration
  (confirmed with `dumpbin /directives`: `RuntimeLibrary=MT_StaticRelease` for Debug and Release
  core objects). The earlier statement that the SDK-enabled core "remains `/MD`" was inaccurate;
  only the Clang-disabled build was `/MD`/`/MDd`. The root `CMakeLists.txt` now sets the policy
  explicitly for both builds, so core, smoke/analyzer and tests share one CRT and nothing depends
  on the SDK import's side effect. The smoke library still does not link `lcm_core`; that is a
  scope choice, not an ABI necessity any more.
- **Release CRT in every configuration.** `LLVM_BUILD_TYPE=Release`, `LLVM_ENABLE_ASSERTIONS=OFF`,
  `LLVM_ENABLE_RTTI=OFF`. All targets compile with `/MT` (Clang-based ones also `/GR-`) in all
  configurations, so `_ITERATOR_DEBUG_LEVEL=0` always holds. The Debug configuration builds own
  code unoptimized on the release CRT (`_DEBUG` not defined) and passes all tests. An earlier
  attempt to exclude Clang targets from Debug via `EXCLUDE_FROM_DEFAULT_BUILD_DEBUG` had no effect
  under `cmake --build` and was removed.
- **Baked build-machine path.** `LLVMExports.cmake` gives `LLVMDebugInfoPDB` the dependency
  `C:/Program Files/Microsoft Visual Studio/2022/Enterprise/DIA SDK/lib/amd64/diaguids.lib`. This
  machine has VS Professional, so `cmake/ClangSdk.cmake` remaps that entry to
  `${CMAKE_GENERATOR_INSTANCE}/DIA SDK/lib/amd64/diaguids.lib` (with `VSINSTALLDIR` as fallback) and
  warns when no local DIA SDK exists.
- **LibXml2.** `LLVMConfig.cmake` declares `LLVM_ENABLE_LIBXML2=1` and `LLVMWindowsManifest` links
  `LibXml2::LibXml2`, which the tarball does not ship (`find_package(LibXml2)` reports not found at
  configure). `clangTooling` and its dependencies do not use `LLVMWindowsManifest`, so this does not
  affect the analyzer; any future use of `lld`/manifest merging would.
- **Header warnings.** SDK headers are SYSTEM includes with `/external:W0`; C4702 is disabled for
  Clang targets because it is emitted at code generation and is not covered by `/external`.

`cmake/ClangSdk.cmake` applies all of this via `lcm_configure_clang_target()`. The core library
and its tests remain buildable in Debug and Release without the SDK.

## Alternatives considered

- **Source build** (LLVM from git, or vcpkg `llvm[clang,tools]`, or conan): gives Debug-compatible
  libraries with assertions but costs hours and tens of GB; excluded by coordinator instruction.
- **libclang C API only**: ships in the toolchain-only installer but lacks the resolved-AST fidelity
  the PRD's confirmed/possible/unresolved contract needs.
