# 2026-09-18 작업 종료 및 재개 보고서

기준 시각: 2026-09-18 17:53 KST. 사용자 요청에 따라 오늘 작업을 종료하고 첫 릴리스 목표를 일시정지한다. 이유는 Claude 세션 사용량 제한이다. **릴리스 완료가 아니며, 현재 수정 중인 Phase 1C도 아직 승인되지 않았다.**

기준 문서는 [PRD v0.3](LiveCodeMap-PRD.md), [구현 계획](First-Release-Execution-Plan.md), [릴리스 검증 장부](First-Release-Acceptance-Ledger.md)다. 계획과 장부의 과거 진행 기록에 나오는 “active”, “now correcting”, “not yet”는 해당 시점의 기록이다. 중단·재개 상태는 이 보고서를 우선 참고하고, 제품 요구사항은 PRD를 따른다.

## 1. 종료 상태와 작업 보존

- Claude의 19:00 예약 자동 재개를 취소했다. 화면의 `Automatic continue cancelled`를 확인했다. 다음 사용자 요청 없이 재개하지 않는다.
- Herdr의 구현자와 검수자는 모두 idle 상태로 확인했다. 새 구현, 추가 검수, 다음 단계 착수는 중단했다.
- 소스와 테스트 수정은 그대로 보존했다. 커밋·스테이징·푸시·릴리스 발행은 하지 않았다.
- Git 기준 커밋은 `957f923`. 종료 시 `.gitignore`, `AGENTS.md`, `CLAUDE.md`, `CMakeLists.txt`, `CMakePresets.json`, `README.md`, `ROLE.md`, `Workspace/`, `cmake/`, `docs/`, `src/`, `tests/`, `tools/`가 모두 untracked다. **`git diff`만으로 현재 구현을 확인할 수 없다. `git clean` 또는 reset으로 정리하지 말 것.**
- `build/`의 SDK, 빌드 산출물, 공개 코퍼스, 독립 재현 도구와 상세 보고서는 ignored 상태다. 이 보고서는 주요 결과를 별도로 남기지만 해당 디렉터리의 백업을 대체하지 않는다. 재개 전까지 보존한다.
- 기존 `build/reports/phase1c-handoff.md`는 **17:19 초기 제출본**이다. 그 문서의 `FROZEN` 및 전체 통과 표시는 이후 수정 중인 소스의 최종 검증 결과가 아니다. 수정 완료 후 새 인계가 필요하다.

## 2. 역할과 재접속 정보

Herdr workspace `w8`, tab `w8:t1`; 동일 작업 디렉터리를 공유한다. 재접속 시 식별자가 여전히 유효한지 먼저 확인한다.

| 역할 | 설정 / pane | 책임과 종료 상태 |
| --- | --- | --- |
| agent1 | 현재 coordinator, `w8:p1` | 계획·요구사항·증거 통합·승인 판단; 이 보고서 작성 후 일시정지 |
| agent2 | `lcm-implementer`, Claude Fable 5.1 / High, `w8:p2` | 제품 코드·빌드·테스트·구현 문서 전담; 사용량 제한으로 중단, 자동 재개 취소 |
| agent3 | `lcm-reviewer`, Codex **gpt-5.6-sol / High**, `w8:p3` | 제품 소스 읽기 전용 독립 검수; 초기 Phase 1C 결함 보고 후 idle |

세션 참고: Claude `684db012-7e73-444e-adf4-a1ec858b6d54`, Codex 검수자 `01a0b264-e70b-7993-8fe4-3f163fe4e970`. 초기 요청의 `gpt-6-sol`은 사용자가 `gpt-5.6-sol`로 정정했다. 재개 시 잘못된 모델명으로 새 검수자를 만들지 않는다. 기존 세션을 사용할 수 있으면 우선 유지한다.

## 3. 완료·승인된 범위

아래 승인은 제한된 구현 단위에 대한 것이며, Phase 1 전체 또는 첫 릴리스 승인과 다르다.

| 범위 | 확인된 결과 |
| --- | --- |
| 기반 구조 | C++20/CMake 네이티브 프로젝트, 결정적 `cm1:` 식별자, 원본 소스 범위·해시, UTF-8 경로, contribution별 사실 보존, 계층별 상태 모델 |
| Clang 준비 | 공식 LLVM/Clang 22.1.0 Windows x64 SDK 고정·검증, 실제 LibTooling 분석/스모크 테스트 |
| Phase 1A/1B | 제한된 일반 C++ 심볼·직접 호출·상속/override·템플릿 패턴·unresolved 증거, 실제 생성/변환/연산자 호출과 lifetime/capture 관련 범위. 독립 검수 후 결함 수정·재검수 완료 |
| 컴파일 명령 어댑터 | 명시적 명령 선택·출처 보존, clang/clang-cl 경계, `--`/`-imsvc` 지원 및 안전한 인자 처리, 응답 파일·중복 입력 경계. 16:40 독립 승인 |

컴파일 DB의 원문 명령은 입력 데이터로만 다룬다. 원문을 shell 명령으로 실행하지 않는다. 실제 UE, 지속 저장소, 사용자용 CLI/MCP와 릴리스 패키지는 위 승인에 포함되지 않는다.

### 테스트 증거의 시점 구분

표의 수치는 `테스트 케이스 / assertion`이다.

| 스냅샷 | core | analyzer | Clang smoke | 증거·판정 |
| --- | --- | --- | --- | --- |
| 승인된 어댑터 시점 | 101 / 2,428 | 20 / 1,552 | 8 / 67 | Debug·Release·RelWithDebInfo 및 별도 core-only 확인; 독립 승인 |
| Phase 1C 초기 제출, 17:19 | 102 / 2,438 | 22 / 2,132 | 8 / 67 | coordinator가 세 구성의 9개 실행 파일을 직접 재실행, 모두 통과. 별도 core-only CTest 3/3. **독립 추가 재현에서 High 2건 발견 → 승인 안 됨** |
| 수정 중 초안, 약 17:47 | 102 / 2,438 | 23 / 2,228 | 8 / 67 | 구현자의 Debug 통과 보고. 이후 dependent qualifier 위험이 추가 확인됨. 최신 소스의 전체 구성 검증·최종 인계·독립 승인은 없음 |

구현자는 수정 초안의 Release·RelWithDebInfo·core-only 검증도 시작했으나, 종료 시점에 최신 수정까지 반영한 최종 증거가 취합되지 않았다. 빌드 파일의 존재나 과거 성공 로그로 완료 처리하지 않는다.

## 4. 현재 막힌 지점: Phase 1C 식별자 수정

Phase 1C는 실제 lambda call operator의 호출 접기, 숨겨진 closure 멤버, 익명 record 생성자, namespace 초기화 lambda anchor, local type/비타입 템플릿 인자(NTTP)의 안정적 식별을 다룬다. 초기 구현과 회귀 테스트는 존재하지만 다음 문제로 승인 보류 상태다.

### H1. 지원하지 않는 타입 wrapper에서 경로·행 번호가 식별자로 유출

독립 재현 입력:

```cpp
auto g = [] {};
void atomic_fn(_Atomic(decltype(g))* value) { (void)value; }
```

고정 Clang 22.1.0에서 유효한 입력이다. 초기 구현은 `_Atomic` 내부 local type을 인식하지 못해 raw printer로 넘어갔다. signature에 `(lambda at D:\...:1:10)`이 들어가고, 저장소 복사와 앞부분 줄 삽입 후 ID가 달라졌다. 분석 상태는 정상이고 limitation은 0이어서 지원하지 못한 사실도 드러나지 않았다. 검수자와 coordinator가 독립 probe에서 재현했다.

필요한 수정: 포함 타입을 재귀적으로 분류하고, 지원하지 않는 local composite는 명시적인 limitation으로 처리한다. 이번 수정에 atomic/vector/complex/non-prototype의 새 atom encoder까지 확장하지 않는다. 그 전체 지원 필요성은 이후 요구사항으로 남기며 면제하지 않는다. 일반 pointer 양성 대조군은 계속 통과해야 한다.

### H2. typed-null NTTP의 타입 소실로 다른 overload가 병합

```cpp
template<auto V> struct Wrap {};
struct A {}; struct B {};
void consume_named_null(Wrap<static_cast<A*>(nullptr)>) {}
void consume_named_null(Wrap<static_cast<B*>(nullptr)>) {}
```

컴파일러에는 서로 다른 함수 2개가 있지만 초기 구현에는 `(Wrap<nullptr>)` 심볼 1개와 위치 2개로 합쳐졌다. 서로 다른 closure pointer의 null 인자도 같은 문제를 재현했다. local type인 경우뿐 아니라 **모든 typed-null 인자의 canonical type**을 보존해야 한다.

추가 회귀 범위: block-local enum을 값으로 쓰는 integral NTTP의 local type atom. 해당 enum 재현에서는 lambda ordinal 때문에 최종 ID 충돌까지 입증되지는 않았지만, 타입 인코딩 누락은 확인했다. 기존 short/int/long long 및 signed/unsigned 구분과 golden ID는 유지한다. overload 2개씩의 존재뿐 아니라 실제 call target도 확인한다.

### R3. 종료 직전 발견한 dependent qualifier 분류 위험 — 미해결

현재 `src/lcm/analyzer/facts_builder.cpp` 약 1281행은 `TemplateTypeParmType`, `DependentNameType`, `UnresolvedUsingType`을 함께 non-local leaf로 처리한다. 그러나 `DependentNameType`은 qualifier를 보유하고, 그 안에 구체적인 local type이 들어갈 수 있다.

coordinator의 컴파일러 검증 입력:

```cpp
auto g = [] {};
template<class A, class B> struct Box {};
template<class T> void deferred_fn(typename Box<decltype(g), T>::type*) {}
```

`build/coordinator-type-classifier/dependent.cpp`에 보존돼 있다. 고정 SDK `clang/AST/TypeBase.h`의 `DependentNameType::getQualifier()`와 실제 구문 통과를 확인했다. **이는 소스/API와 컴파일러 입력으로 뒷받침된 위험이며, 별도 제품 canonical-key 실패 재현까지 완료한 것은 아니다.**

구현자는 위험을 인정하고 `NestedNameSpecifier` API를 확인하던 중 사용량 제한에 도달했다. 종료 시 whitelist는 여전히 남아 있다. 재개 첫 작업은 실제 key 동작을 재현하고, qualifier 내부를 안전하게 검사하거나 unknown/명시적 limitation으로 처리하는 것이다. 진짜 template-parameter leaf의 정상 입력은 유지한다.

### 재검수 시 주의

- 기존 `build/reviewer-phase1c/out-escalated/Release/phase1c_probe.exe`는 초기 결함 라이브러리를 정적으로 링크한 실행 파일이다. 최신 소스로 **재링크한 후** 판정해야 한다.
- 관련 probe/입력: `build/reviewer-phase1c/phase1c_probe.cpp`, `atomic.cpp`, `copied/atomic.cpp`, `null_nttp.cpp`, `iile.cpp`.
- 초기 검수의 정상 대조군: 명시적/괄호/generic 즉시 lambda 호출, 숨겨진 closure 멤버 8개, contribution 제거·최종 정리, namespace/comma anchor. 이 결과도 회귀 확인한다.
- aggregate 암시적 생성의 원본 닫는 `}` 범위는 컴파일러의 실제 위치로 확인됐다. 결함으로 바꾸지 말고 span/hash 회귀 테스트로 보존한다.

## 5. 공개 코퍼스·비교 기준 준비 결과

### fmt 12.2.0

- 고정 커밋: `1be298e1bd68957e4cd352e1f676f00e07dcfb57`.
- 공개 원본과 빌드/입력 snapshot 준비. compilation DB 51개 명령 중 C++ 50개(30개 파일), C 1개. 대안 profile은 병합하지 않고 각각 보존했다.
- upstream 빌드는 성공. upstream CTest는 **19/21**이며 `chrono_test.locale`, `unicode_test.legacy_locale`이 실패했다. 정확한 원인은 아직 입증하지 않았다. 제외·skip·locale patch로 성공 처리하지 않았다.
- 명시적으로 선택한 C++ 50개 frontend 진단은 모두 오류·timeout 없이 완료했고, 검수자가 16:47 coverage를 확인했다. 경고 10개, limitation 종류 29개, unresolved 증거가 남아 있다.
- 이는 입력/프런트엔드 준비 증거다. graph precision/recall, 검색 품질, 성능 또는 릴리스 통과가 아니다.
- 상세: `build/reports/fmt-frontend-readiness.md`, `fmt-input-snapshot.json`, `fmt-upstream-ctest.log`; 고정 실행 결과 `build/coordinator-fmt-frontend/runs/20260918-074238-803`.

### Graft 0.18.0

- 고정 커밋: `de8456e892bad5aeee11403e47fb2227773eb27e`. 검증한 공개 archive/lockfile로 ignored 로컬 트리에 준비했다. upstream 알고리즘·소스는 수정하지 않았다.
- lifecycle script를 자동 실행하지 않는 설치 후 필요한 빌드 단계를 개별 확인했다. TypeScript/viewer 빌드, C++ WASM 추출, native extractor 로딩, 격리된 CLI 시작 검사를 통과했다.
- 새 격리 디렉터리에서 **실제 structural CLI toy build 통과**: `main.cpp` 1개, node 3개, resolved call edge 1개, card 1개. provider key/deep/LSP 없이 실행했다.
- native Kotlin binding 초기 누락 및 Windows 긴 경로 FileTracker 실패는 명시적 빌드와 `TrackFileAccess=false` 재빌드로 해결했다. upstream 소스 변경 없음.
- 이것은 비교 도구 실행 준비일 뿐, 공통 코퍼스 비교·A/B·토큰 절감·검색 품질의 증거가 아니다. 전체 네트워크 무통신을 trace로 입증한 것도 아니다.
- CLI는 telemetry opt-out과 별개로 updater 동작이 있어 격리 HOME/APPDATA와 update-check 상태를 통제했다. ignored 코퍼스에 대한 `git ls-files` 영향도 피해야 한다. 전역 설정이나 프로젝트 `.gitignore`를 바꾸지 않는다.
- 상세: `build/reports/graft-baseline-prerequisites.md`; 실행기 `build/baselines/graft-cli-build-probe.cjs`. 이 실행기는 기존 output을 덮어쓰지 않도록 재실행을 거부한다. 다음 실행은 새 통제 경로를 사용하고 기존 증거를 삭제하지 않는다.

Cesium Unreal `v2.29.1`은 커밋 `29ea626a7a5c76f12e49fd22e9628c870740598d`의 원격 식별만 확인했다. 실제 checkout/UE 빌드·검증 완료가 아니다.

## 6. 남은 릴리스 범위와 외부 결정

**어떤 전체 릴리스 gate도 아직 통과하지 않았다.** 단순 진행률 숫자를 산출하지 않는다.

| 단계 | 남은 주요 범위 |
| --- | --- |
| Phase 1 | 현재 Phase 1C 수정·재검수, direct base/include, 전체 타입·템플릿·alias·allocation·no-context 및 실제 UE 분석 |
| Phase 2 | SQLite 지속 저장(의존성 준비만 됨), atomic generation, crash/rollback, writer lock, invalidation/freshness, worktree |
| Phase 3 | 사용자 CLI/MCP 공통 계약과 모든 명령, 페이지/탐색/소스 검증, early A/B |
| Phase 4 | 검색/ranking, 고정 개발·holdout 평가, 동등 조건 Graft 비교 |
| Phase 5 | 명시적 deep/provider 실행, 실제 usage·비용·cache 및 무변경 재실행 0 호출 |
| Phase 6 | host init/remove/hooks, viewer/export, npm/Windows ZIP/mcpb, CI·라이선스·SBOM·clean-machine·실제 UE lane·발행 |

미정인 사용자/외부 입력은 다음 작업에서 필요한 시점에 확인한다. 이번 종료를 위해 답할 필요는 없다.

1. 지원 UE 버전, 엔진 설치 위치, 공개 fixture target/configuration, 실제 UE runner.
2. 반복 A/B 및 deep 실험에 사용할 provider/model과 사용량·비용 한도. 현재 구현 요청을 임의의 유료 호출 승인으로 해석하지 않는다.
3. npm 조직/패키지 소유권, CI/OIDC 환경 및 실제 공개 발행 권한. 아직 어떤 산출물도 배포하지 않았다.

## 7. 다음 작업의 정확한 시작 순서

1. 사용자 재개 요청 후 이 보고서 → `AGENTS.md` → PRD → 계획·장부를 읽고, `git status --short`와 Herdr 세션 상태를 확인한다. dirty/untracked 작업을 보존한다. 사용량이 회복됐다는 이유만으로 자동 시작하지 않는다.
2. 기존 역할을 유지해 agent2에 **Phase 1C identity correction만** 다시 맡긴다. 계획의 동일 제목 절에 수정 범위와 승인 조건이 있다. 먼저 R3 실제 key 재현·수정, 그다음 H1/H2와 정상 대조군을 확인한다.
3. 구현자가 최신 소스를 Debug·Release·RelWithDebInfo에서 빌드/테스트하고 Clang-disabled core-only도 검증한다. 정확한 케이스/assertion, 변경 파일, 남은 limitation을 담은 **새 수정 인계 보고서**를 작성한 후 소스를 동결한다.
4. agent3가 최신 라이브러리로 독립 probe를 재링크하여 H1/H2/R3, 복사 경로·줄 삽입, 실제 overload target 및 회귀 대조군을 검수한다. coordinator도 세 구성의 실행 파일과 core-only 결과를 확인한다. 통과 전 Phase 1C를 승인하지 않는다.
5. Phase 1C 승인 후에만 계획의 **Prepared direct-base/include increment (not activated)**를 활성화한다. 지금은 설계와 별도 컴파일러 API probe만 준비됐고 제품 구현은 시작하지 않았다.
6. 이후 PRD 전체 단계와 gate를 순서대로 진행한다. fmt/Graft 준비 성공을 제품 품질 또는 릴리스 성공으로 대체하지 않는다.

다음 direct-base/include 설계는 `build/reports/base-include-design.md`에 있다. 직접 base의 access/virtual/순서/원본 증거, contribution 소유 direct include, 원본 조건 분기/활성·해결 상태, 반복 방문과 외부 경로 비공개를 다룬다. standalone `build/coordinator-include-scanner/` probe는 API 준비 증거이며 제품 기능이 아니다.

### 빌드 재개 참고

- 프로젝트: `D:\Git\LiveCodeMap`; CMake 3.30, VS2022 Professional MSVC 14.44, Node 24.16.0, Python 3.11.
- LLVM SDK: `build/deps/clang+llvm-22.1.0-x86_64-pc-windows-msvc`; resource `lib/clang/22`. 현재 CMake cache와 `docs/clang-sdk-acquisition.md`를 먼저 확인한다. 재다운로드나 전역 설치부터 하지 않는다.
- 기존 제품 build: `build/vs2022-x64`; coordinator core-only: `build/coordinator-coreonly`.
- 각 구성은 `cmake --build --preset debug` / `release` / `relwithdebinfo`, 이어 `ctest --preset debug` / `release` / `relwithdebinfo`. CMake preset 자체만으로 SDK 선택이 모두 기록된다고 가정하지 않는다.
- 직접 실행 파일은 각 구성의 `tests/<cfg>/lcm_core_tests.exe`, `tests/analyzer/<cfg>/lcm_analyzer_tests.exe`, `tests/clang/<cfg>/lcm_clang_smoke_tests.exe`.
- Windows sandbox의 PATH/Path 중복 또는 child-process 제한 때문에 동일 명령이 sandbox에서만 실패한 사례가 있었다. 먼저 실제 원인을 구분하고, 필요한 승인 절차를 따르며 전역 환경을 수정하지 않는다.

재개 요청 예시: “herdr 스킬로 2026-09-18 보고서부터 확인하고 기존 3-agent 역할을 유지해 Phase 1C 미완료 수정과 독립 검수부터 이어가자. 모호한 부분은 질문해줘.”
