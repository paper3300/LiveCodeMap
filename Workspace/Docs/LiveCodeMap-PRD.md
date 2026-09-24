# LiveCodeMap 제품 요구사항 문서

> 문서 상태: 첫 릴리스 승인 기준선  
> 버전: 0.3  
> 작성일: 2026-09-17  
> 대상 릴리스: Windows 네이티브 첫 공개 릴리스  
> 요구사항 근거: `LiveCodeMap-Planning-Handoff.md`, `Graft-Technical-Analysis.md`
> 적용 우선순위: 근거 문서의 과거 제안과 상충하면 이 PRD 0.3의 요구사항과 출시 기준이 우선한다.

## 1. 문서 목적

이 문서는 Graft를 벤치마킹한 C++/Unreal Engine 특화 코드 탐색 도구 LiveCodeMap의 첫 공개 릴리스 범위, 사용자 경험, 기능 요구사항, 품질 목표와 출시 기준을 정의한다. 구현 세부를 자유롭게 바꿀 수 있더라도 이 문서의 사용자 계약, 정확도 경계와 검증 가능한 성공 기준은 유지해야 한다.

요구사항의 우선순위는 다음과 같다.

1. 코드 관계의 정확성과 불확실성의 정직한 표시
2. 외부 코딩 에이전트에 작고 유용한 컨텍스트 제공
3. 대규모 C++/Unreal 프로젝트에서의 freshness와 증분 성능
4. 로컬 구조 분석과 선택적 deep의 독립성
5. 재현 가능한 품질·비용 검증

## 2. 제품 요약

LiveCodeMap는 Graft의 핵심 기능과 사용자 경험을 벤치마킹해 C++ 및 Unreal Engine에 특화하는 코드 탐색 도구다. 대규모 코드베이스를 로컬에서 구조화하고, 코딩 에이전트가 문제 해결에 필요한 심볼, 파일, 호출, 참조와 작은 코드 컨텍스트를 빠르게 찾도록 돕는다.

LiveCodeMap 자체는 버그 원인을 단정하거나 수정하는 디버거가 아니다. 결정론적 구조 그래프, 선택적 의미 계층, 목적별 CLI/MCP 도구와 에이전트 통합을 제공하고, 실제 분석과 수정은 외부 코딩 에이전트가 수행한다.

첫 릴리스는 Windows 네이티브 환경에서 일반 C++ 프로젝트와 Unreal Engine C++ 프로젝트를 모두 지원한다. 공통 C++ 분석·검색 계층 위에 Unreal 전용 compile context adapter와 관계 처리를 추가하며, 일반 C++ 사용에는 Unreal Engine 설치가 필요하지 않아야 한다. Graft 0.18.0의 구조 인덱싱과 사용자 경험을 품질 하한으로 참고하지만, Graft를 확장하거나 포크하지 않고 독립적으로 구현한다.

기능과 구현 우선순위는 Graft의 탐색 경험을 C++/Unreal에 제공하는 데 기여하는지로 판단한다. 한국어 전용 예외 처리나 별도 다국어 검색 품질 보장은 첫 릴리스의 목표가 아니다.

## 3. 해결할 문제

대규모 C++ 프로젝트에서 한 기능은 여러 헤더와 구현 파일, template, overload와 가상 호출에 분산된다. Unreal 프로젝트에서는 UFUNCTION/RPC 쌍, delegate binding, UHT 생성 계층과 module 경계가 추가된다. 일반 텍스트 검색만 사용하는 에이전트는 다음 문제를 겪는다.

- 관련 파일과 sibling 구현을 누락한다.
- 이름이 같은 심볼을 실제 호출 대상으로 오인한다.
- 가상 호출과 Unreal event 경계를 확정 호출처럼 취급한다.
- unity, PCH와 UHT 생성 코드를 사용자 코드로 노출해 검색 결과를 오염시킨다.
- 오래된 index를 사용하거나 매 요청마다 과도한 재분석을 수행한다.
- 전체 파일을 반복해서 읽어 입력 토큰과 작업 시간을 낭비한다.
- 기능 설명과 C++ identifier·path를 연결해 관련 코드를 찾기 어렵다.

LiveCodeMap는 compiler/UBT 근거가 있는 구조 사실과 검색 랭킹을 분리하고, freshness와 confidence를 결과마다 표시해 이 문제를 해결한다.

## 4. 목표와 성공 정의

### 4.1 제품 목표

- C++/Unreal 프로젝트의 심볼과 관계를 결정론적으로 인덱싱한다.
- 확정 관계, 가능한 런타임 관계와 미해결 증거를 구분한다.
- 코드 변경 후 필요한 범위만 갱신하고 질의 시 최신 상태를 확인한다.
- Graft와 공통 비교 가능한 identifier, path와 자연어 질의에서 관련 코드를 높은 정밀도로 찾고 C++/Unreal 전용 관계 탐색을 보강한다.
- CLI와 MCP로 동일한 semantic response를 제공한다.
- Claude Desktop, Claude Code, Codex와 Codex CLI에 안전하게 연결한다.
- 선택적 `--deep` 의미 분석을 구조 그래프와 독립적으로 제공한다.
- 실제 동일 조건 A/B에서 정확도를 낮추지 않으면서 에이전트 입력 토큰을 줄인다.

### 4.2 첫 릴리스 성공 기준

첫 릴리스는 다음 조건을 모두 만족해야 성공으로 본다.

- synthetic fixture의 기대 심볼, 관계와 상태가 100% 일치한다.
- Unreal이 설치되지 않은 Windows 환경에서 일반 C++의 초기 index, 검색, 호출 탐색과 증분 갱신이 동작하고, Unreal 환경에서는 같은 공통 기능과 Unreal 전용 관계가 동작한다.
- confirmed call/reference precision이 99% 이상이다.
- direct call/reference recall이 95% 이상이다.
- possible virtual/runtime target recall이 90% 이상이다.
- 불확실한 대상을 `confirmed`로 승격한 사례가 0건이다.
- exact identifier/path Top-1이 98% 이상이다.
- 전체 질의의 Hit@5가 90% 이상이다.
- identifier 없는 자연어 질의의 Hit@5가 80% 이상이다.
- 전체 정답 회수율 Recall@5와 possible target의 precision·후보 수를 함께 보고하고 12절의 잡음 및 비교 기준을 통과한다.
- 동일 입력 3회 clean build의 fact hash가 100% 일치한다.
- agent A/B에서 baseline 대비 correctness가 낮아지지 않는다.
- 대표 agent task에서 양쪽 모두 성공한 paired run의 실제 input token 비율을 task별로 집계한 중앙값이 baseline 대비 0.70 이하다. 실패·timeout을 포함한 전체 run의 correctness는 별도로 통과해야 한다.
- wrong-file edit와 필수 sibling 누락이 baseline보다 증가하지 않는다.

## 5. 대상 사용자와 핵심 작업

### 5.1 대상 사용자

- 대규모 C++/Unreal 저장소를 탐색하는 소프트웨어 엔지니어
- 해당 저장소에서 작업하는 Claude 또는 Codex 계열 코딩 에이전트
- 인덱스 품질, freshness와 비용을 관리하는 프로젝트 담당자
- LiveCodeMap를 CI/release 환경에서 검증하는 유지관리자

### 5.2 핵심 사용자 작업

- 자연어 또는 identifier로 관련 클래스, 함수와 파일 찾기
- 특정 함수의 caller와 호출 경로 추적하기
- signature 또는 구현 변경의 구조적 영향 범위 확인하기
- 물리 파일의 public API와 split implementation 위치 확인하기
- module, target과 directory 구조 파악하기
- 가상 호출, RPC, delegate와 Blueprint 경계를 구분해 보기
- 변경 직후 index가 최신인지 확인하고 필요한 범위만 갱신하기
- 에이전트가 사용할 작은 context pack 얻기
- 선택한 graph view를 독립 HTML로 공유하기

대표 시나리오는 “Unreal UI에서 특정 아이콘이 표시되지 않는데 어떤 코드를 확인해야
하는가?” 같은 요청이다. LiveCodeMap는 에셋 상태를 진단하지 않고 관련 UI method,
resource 호출 경로, 외부 에셋 경계와 작은 source context를 제공한다.

## 6. 릴리스 범위

### 6.1 포함 범위

- Windows 네이티브 실행
- 일반 C++ 프로젝트의 compilation database 기반 구조·의미 분석과 compile context 없는 구조·텍스트 탐색
- Unreal Engine C++에 특화된 UBT/UHT, reflection, interface, RPC와 delegate 처리
- 구조 그래프와 선택적 의미 그래프
- SQLite 기반 결정론적 index와 증분 cache
- query-time freshness 확인
- CLI와 MCP 서버
- 에이전트 init/remove, 지시문, 지원되는 hook와 상태 표시
- 제한된 graph 시각화와 단일 offline HTML export
- monorepo, workspace, submodule, nested repository와 Git worktree 처리
- 명시적 version 확인과 사용자 주도 upgrade
- 실제 provider usage 기반 로컬 비용 통계
- 공개 benchmark harness와 저장소 밖의 선택적 로컬 검증

### 6.2 제외 범위

- LiveCodeMap가 직접 수행하는 버그 진단 또는 코드 수정
- Blueprint graph와 Blueprint 구현 분석
- `/Game/...` 에셋 경로, Content, redirector와 cook 상태 분석
- 일반 def-use, field read/write와 범용 dataflow graph
- Trail Brain
- GitHub App 기반 원격 서비스
- macOS와 Linux 첫 릴리스 지원
- Claude Desktop용 project lifecycle hook
- 자동 self-update 또는 query 중 DB 자동 migration
- background hook에 의한 `--deep`, UBT, UHT 또는 전체 project build 자동 실행

### 6.3 확장성 경계

공통 심볼·관계 모델과 언어별 분석기를 분리해 향후 Blueprint, C#, macOS와 Linux를 추가할 수 있어야 한다. 다만 첫 릴리스에서 미래 기능을 미리 노출하거나 불완전하게 구현하지 않는다. 플랫폼 종속 코드는 격리한다.

일반 C++와 Unreal C++는 같은 심볼 identity, fact store, freshness, retrieval 및 CLI/MCP 계약을 사용한다. Unreal 전용 9.4절 요구사항은 Unreal project에 적용하며 공통 C++ 경로가 UBT/UHT를 요구하지 않도록 분리한다. Unreal 지원 범위는 C++ 코드와 명시한 연결 관계이며 Blueprint 구현·asset 분석의 제외 범위는 유지한다.

## 7. 제품 원칙

### P-01. 정확도 우선

이름 유사성만으로 관계를 확정하지 않는다. 검색과 `ask`는 정밀도, `blast`는 재현율을 우선하되 모든 결과에서 confidence를 구분한다.

### P-02. 사실과 projection 분리

Compiler로 확인한 사실 그래프가 source of truth다. 검색 랭킹, class card, runtime 후보와 deep summary는 사실 그래프에서 재생성 가능한 projection 또는 query-time 결과다.

### P-03. 불확실성 보존

관계는 `confirmed`, `possible`, `unresolved`로 구분한다. 미해결 callsite에 가짜 대상 간선을 만들지 않고 원인과 위치를 남긴다.

### P-04. 로컬 구조 분석과 선택적 deep

구조 build, 기본 검색과 graph traversal은 로컬에서 동작한다. LLM을 사용하는 deep은 선택적 기능이며 deep의 설정이나 실패가 구조 탐색을 막지 않는다.

### P-05. 빠른 최신성 확인

모든 검색 도구가 자체적으로 freshness를 검사한다. 작은 변경은 budget 안에서 증분 갱신하고, 큰 변경은 질의를 오래 막지 않은 채 stale 상태와 다음 행동을 반환한다.

### P-06. 표시량과 보존량 분리

성능이나 UI 제한 때문에 저장된 graph fact를 삭제하지 않는다. 질의의 traversal은 시간·방문 node budget 안에서 점진적으로 계산하고 page/limit과 이어서 탐색하는 cursor를 제공한다. 탐색 완료 여부와 반환량을 구분하며 전체 수를 아직 모르면 추정치를 정확한 `total`로 표시하지 않는다.

### P-07. 검증 가능한 주장

검색 품질, 비용과 Graft 대비 개선 주장은 고정 corpus, 동일 조건과 실제 provider usage를 사용한 재현 가능한 benchmark로만 한다.

## 8. 핵심 사용자 흐름

### 8.1 설치와 프로젝트 연결

1. 사용자는 npm, standalone ZIP 또는 source build로 LiveCodeMap를 설치한다.
2. `livecodemap init`으로 감지된 agent host를 확인한다.
3. 사용자가 선택한 host에만 project-local 설정을 설치한다.
4. `--dry-run`으로 변경 path, scope와 conflict를 사전에 확인할 수 있다.
5. init은 index나 build system을 실행하지 않고 project 종류에 맞는 compile context 설정과 필요한 `livecodemap build`를 안내한다.

### 8.2 초기 index 생성

1. 사용자는 일반 C++ 또는 Unreal project 종류와 분석 profile을 확인·선택한다.
2. 일반 C++의 `livecodemap build`는 선택한 `compile_commands.json`을 읽고, Unreal은 선택 target/configuration의 UBT compile action과 필요한 UHT context를 준비한다.
3. 구조, semantic, retrieval layer를 생성한다.
4. 부분 실패가 있어도 마지막 committed generation 또는 사용 가능한 구조 결과를 제공한다.
5. 사용자는 계층별 상태와 실패 unit을 확인한다.

### 8.3 탐색과 영향 분석

1. 사용자 또는 agent가 CLI/MCP로 질의한다.
2. LiveCodeMap가 freshness를 빠르게 검사하고 필요한 작은 증분 갱신을 수행한다.
3. lexical/identifier/path seed와 graph signal을 결합한다.
4. 결과는 source snippet, 포함 이유, confidence, freshness와 pagination 정보를 포함한다.
5. caller, trace 또는 blast가 확정 경로와 가능한 경계를 분리해 반환한다.

### 8.4 변경 후 작업

1. 지원 host의 write/edit hook가 변경을 debounce한다.
2. 구조·lexical refresh를 비동기 single-flight로 요청한다.
3. 작은 semantic 변경은 budget 안에서 갱신한다.
4. 큰 header fan-out은 2초 안에 stale을 반환하고 명시적 build를 안내한다.
5. 삭제되거나 새로 ignore된 파일은 이전 결과에 남지 않는다.

### 8.5 제거

1. `livecodemap remove`가 기본적으로 제거 plan만 표시한다.
2. 사용자가 `--yes`로 승인해야 변경을 반영한다.
3. LiveCodeMap가 소유한 block, MCP entry, hook와 helper만 제거한다.
4. 사용자가 수정한 managed 영역은 자동 삭제하지 않고 conflict로 보존한다.

## 9. 기능 요구사항

### 9.1 Build profile과 compile context

#### FR-BLD-001 — 명시적 index build

- `livecodemap build`는 binary compile/link 명령이 아니라 LiveCodeMap index 준비 명령이어야 한다.
- 전체 semantic build는 명시적 `livecodemap build`에서 수행하며, UBT 사용과 UHT 준비는 Unreal project의 명시적 build에만 적용해야 한다.
- 일반 query, hook와 `check_freshness`는 CMake configure/generate, UBT, UHT 재생성, project build 또는 `--deep`을 자동 실행하지 않아야 한다.
- 일반 C++ semantic build는 기존 compilation database를 입력으로 사용해야 한다. Database가 없으면 FR-BLD-005의 구조·텍스트 fallback을 제공한다. Database나 generated header 준비가 필요하면 사용자가 수행할 configure/generate/build 방법을 안내하고 project build를 자동 실행하지 않아야 한다.
- Engine version 때문에 metadata 준비에 실제 project build가 필요하면 이를 몰래 실행하지 않고 `project build required`를 반환해야 한다.

#### FR-BLD-002 — Profile 선택

- 프로젝트마다 일반 C++ 또는 Unreal 종류와 기본 분석 profile 하나를 선택할 수 있어야 한다. 명시적 설정을 자동 감지보다 우선하고 여러 `.uproject` 또는 compilation database 후보가 모호하면 선택지를 보고해야 한다.
- 일반 C++ profile은 compilation database 경로, compile command 선택 범위와 toolchain 정보를 식별해야 한다. Unreal profile은 target/platform/configuration과 Engine 정보를 식별해야 한다.
- Compile context 없는 profile도 source root와 project 종류로 식별하고 context가 missing임을 기록해야 한다. 존재하지 않는 database/toolchain 정보를 임의로 채우지 않아야 한다.
- Unreal profile의 target/platform/configuration은 사용자가 명시하며 저장소에 특정
  비공개 프로젝트의 기본값을 포함하지 않아야 한다.
- 정확한 semantic graph의 기본 범위는 active profile이어야 한다.
- profile 변경 시 compile context와 의존 계층을 stale로 표시해야 한다.
- inactive `#if` 코드는 구조·텍스트 검색에서 `[inactive]`로 표시할 수 있고, graph query에서는 `--include-inactive`로 명시해야 한다.

#### FR-BLD-003 — Compile context 우선순위

일반 C++ compile context는 다음 우선순위를 따라야 한다.

1. 사용자가 profile에 명시한 `compile_commands.json`
2. project root 또는 사용자가 지정한 build directory에서 유일하게 확인된 `compile_commands.json`
3. structural-only fallback

Unreal compile context는 다음 우선순위를 따라야 한다.

1. 선택 profile의 UBT action export
2. 같은 profile의 response/dependency artifact
3. UBT compilation database
4. structural-only fallback

- 최신 파일이라는 이유만으로 다른 profile artifact를 재사용하지 않아야 한다.
- 특정 undocumented UBT flag를 공개 제품 계약으로 삼지 않고 UE version adapter가 지원 방식을 선택해야 한다.
- raw compile argv와 analyzer-normalized argv, drop한 option과 이유, source, working directory, profile, module, toolchain, define, include, forced include, language standard, unity/PCH/UHT/dependency 정보를 보존해야 한다.
- MSVC PCH binary를 Clang analyzer에 강제로 넣지 않고 forced include source를 해석하며 `pch_emulated` 또는 degraded 상태를 표시해야 한다.
- 분석을 위해 실제 `Build.cs`, `Target.cs`, unity 또는 PCH 설정을 변경하거나 non-unity build를 강제하지 않아야 한다.

#### FR-BLD-004 — 일반 C++ 입력과 분석 범위

- 일반 C++는 Unreal 설치, `.uproject`, UBT, UHT와 `Build.cs` 없이 초기 build, 검색, 호출 탐색, 증분 갱신 및 CLI/MCP를 사용할 수 있어야 한다.
- Compilation database의 `directory`, `file`, `arguments` 또는 `command`, 선택적 `output`을 해석하고 relative path는 해당 working directory 기준으로 처리해야 한다. `command`는 compiler option 데이터로 읽고 임의 shell command로 실행하지 않아야 한다.
- 같은 file의 복수 compile command는 profile과 command identity로 구분해야 한다. 서로 다른 define/include context를 임의로 하나로 합치거나 첫 command를 조용히 선택하지 않아야 한다.
- Header와 header-only library는 실제로 포함하는 TU의 compile context로 분석해야 한다. 단독 header에 유효한 context가 없으면 구조 결과를 제공하고 semantic 분석을 위한 consumer/test TU 준비 방법을 안내해야 한다.
- CMake 프로젝트의 최초 설정 문서는 검증된 compilation database 생성 경로를 포함해야 한다. 다른 build system도 호환 database를 제공하면 같은 분석 경로를 사용할 수 있어야 한다. Database 입력 지원과 `.sln`/`.vcxproj` 등 build system 고유 설정의 직접 해석 지원을 구분해 문서화해야 한다.
- 검증한 compiler/toolchain, C++ standard와 compile option 호환 범위를 release에 명시해야 한다. 지원하지 않는 option이나 부족한 SDK/include/generated source는 해당 unit의 제한과 다음 행동으로 보고해야 한다.
- 일반 C++의 `repo_map`은 file/directory와 확인된 symbol 관계를 제공해야 한다. Compilation database에 없는 build target/module 소유 관계를 file명이나 output명으로 확정하지 않아야 한다.

입력 형식과 설정 문서는 [Clang compilation database 명세](https://clang.llvm.org/docs/JSONCompilationDatabase.html)와 [CMake export 문서](https://cmake.org/cmake/help/latest/variable/CMAKE_EXPORT_COMPILE_COMMANDS.html)를 참고한다.

#### FR-BLD-005 — Compile context 없는 동작

- 두 project 종류 모두 compile context가 없거나 일부 unit을 해석하지 못해도 가능한 file/path, 구문 기반 symbol과 source text 검색을 제공해야 한다.
- Compiler로 검증하지 못한 overload resolution, call target, override와 활성 전처리 분기는 확정 사실로 표시하지 않아야 한다. 제공할 수 없는 graph 범위는 missing/partial과 원인으로 표시해야 한다.
- Fallback 결과에는 분석 근거를 표시해야 한다. 이후 유효 context로 semantic 분석을 완료하면 임시 구조 결과와 논리 symbol의 중복 없이 통합·교체해야 한다.
- 일반 C++에는 compilation database 또는 의존성 준비를, Unreal에는 필요한 UBT/UHT context 준비를 안내해야 한다. 일반 C++에 Unreal 설치를 해결 방법으로 제시하지 않아야 한다.

### 9.2 공통 심볼과 관계 그래프

#### FR-GPH-001 — 논리 심볼 identity

- 선언과 정의를 별도 공개 심볼로 복제하지 않고 하나의 논리 심볼에 여러 위치를 연결해야 한다.
- 클래스 소유자는 파일명이 아니라 compiler가 확인한 qualified owner로 결정해야 한다.
- 여러 `.cpp`에 나뉜 구현은 같은 class method로 연결하고 free/static helper는 이름이나 파일 유사성만으로 class에 귀속하지 않아야 한다.

#### FR-GPH-002 — Stable ID

- 공개 stable ID는 schema version을 포함한 `cm1:<hash>` 형식이어야 한다.
- canonical key는 repository member, language, symbol kind, semantic owner chain, canonical name, normalized signature와 linkage discriminator를 포함해야 한다.
- 외부 linkage named symbol ID에는 절대 경로, 행 번호, body hash, active profile과 UHT generated path를 넣지 않아야 한다.
- internal linkage와 anonymous namespace는 repository-relative file scope를 포함해야 한다.
- lambda, anonymous type과 local symbol은 enclosing symbol과 lexical anchor/ordinal을 사용하며 앞선 anonymous symbol 삽입에 따른 ID 변경을 허용한다.
- body/comment 수정과 외부 linkage named symbol의 파일 이동은 ID를 유지하고, 이름·owner·signature 변경은 새 ID를 생성해야 한다.
- rename을 Git history나 heuristic만으로 같은 identity로 승격하지 않아야 한다.
- Clang USR은 provider evidence/진단 key로 저장할 수 있으나 영구 공개 ID로 사용하지 않아야 한다.

#### FR-GPH-003 — Scope, overload, template와 alias

- namespace를 scope/node로 모델링하고 named reopened namespace를 하나의 논리 scope로 합쳐야 한다.
- anonymous namespace는 repository-relative file scope에 종속되어야 한다.
- overload는 normalized signature로 구분해야 한다.
- template primary, explicit/partial specialization과 explicit instantiation을 구분해야 한다.
- 모든 implicit instantiation을 공개 node로 만들지 않되 사용 지점의 template type argument를 보존해야 한다.
- type alias는 별도 node이며 원문 alias와 해석된 대상을 모두 보존해야 한다.

#### FR-GPH-004 — Include와 외부 경계

- 직접 include만 저장하고 spelling, resolved target, 조건식과 원본 위치를 보존해야 한다.
- transitive include는 query 시 계산해야 한다.
- 내부 직접 include는 약한 ranking signal로 사용할 수 있으나 PCH와 외부 include는 ranking signal에서 제외해야 한다.
- 분석 root 밖 Engine/Plugin 선언은 resolution에 사용할 수 있지만 실제로 참조된 심볼만 `indexed: false` placeholder로 보존해야 한다.
- 외부 placeholder의 본문, 내부 호출, 파일 경로와 전체 API를 인덱싱하거나 일반 검색 랭킹 대상으로 삼지 않아야 한다.

#### FR-GPH-005 — 사실, 후보와 미해결 증거

- relation은 confirmed fact, candidate relation, unresolved site와 query-time runtime expansion으로 분리해야 한다.
- relation에는 복수 evidence를 연결할 수 있어야 하며 한 contribution이 사라져도 다른 evidence가 남으면 relation을 유지해야 한다.
- compiler semantic resolution을 최우선으로 사용해야 한다.
- 애매한 candidate는 possible로 보존하고 unresolved callsite는 expression, 위치와 원인을 남겨야 한다.
- 이름만 같은 심볼을 target으로 확정하지 않아야 한다.

### 9.3 C++ 호출·타입 의미

#### FR-CPP-001 — 상속과 override

- 직접 base edge만 저장하고 access, virtual 여부와 선언 순서를 보존해야 한다.
- transitive base 관계는 query 시 계산해야 한다.
- override는 compiler가 확인한 경우만 confirmed여야 한다.
- 일반 C++ type을 `I` prefix 또는 pure virtual 여부만으로 interface로 분류하지 않아야 한다.

#### FR-CPP-002 — Virtual dispatch

- virtual callsite에서 compile-time selected base method/virtual slot까지 confirmed로 기록해야 한다.
- 실제 override 구현 후보는 possible runtime target으로 query 시 계산해야 한다.
- 모든 override 구현으로 직접 edge를 미리 생성하지 않아야 한다.
- `final`, explicit `Base::Method()`와 non-virtual call은 구현까지 confirmed로 연결할 수 있어야 한다.
- callers, trace와 blast는 직접/확정 경로와 possible dispatch를 분리해야 한다.

#### FR-CPP-003 — Callable

- 사용자 선언 constructor, destructor, conversion function과 operator를 독립 callable로 보존해야 한다.
- `= default`, `= delete` 상태를 표시해야 한다.
- compiler가 암시적으로 합성한 special member는 기본 검색에서 제외하되 실제 참조되면 hidden symbol로 보존해야 한다.
- compiler가 확인한 생성, 복사, 이동, 변환, base/member initializer와 사용자 정의 operator 호출을 confirmed relation으로 저장해야 한다.
- 자동 객체 파괴는 `implicit lifetime` 성격으로 저장하고 도구별로 표시 여부를 제어해야 한다.
- built-in operator는 callable로 만들지 않아야 한다.

#### FR-CPP-004 — Lambda와 간접 호출

- lambda는 독립 callable이고 compiler closure의 `operator()`는 lambda symbol로 접어야 한다.
- lambda body 내부 호출을 enclosing function의 직접 호출로 귀속하지 않아야 한다.
- capture 대상, 값/참조, 명시/암시, `this`/`*this`와 init-capture를 보존하되 일반 def-use로 확장하지 않아야 한다.
- init-capture initializer의 호출은 enclosing callable, lambda body 호출은 lambda에 귀속해야 한다.
- 즉시 실행 lambda는 `enclosing callable -> lambda -> inner call`로 표현하고 `[immediately invoked]`로 표시해야 한다.
- 함수·method 주소는 정확한 `references` relation이어야 한다.
- callsite가 직접 증명하는 즉시 간접 호출만 confirmed로 연결해야 한다.
- pointer variable, `std::invoke`, `TFunction` 또는 callback container를 통한 target은 dataflow 없이 추측하지 않아야 한다.

#### FR-CPP-005 — Type wrapper

- 일반 C++의 raw pointer, `std::unique_ptr`, `std::shared_ptr`, `std::weak_ptr`와 nested container 내부 type에 일반 `references` relation을 만들어야 한다. Unreal에서는 `TObjectPtr`, `TWeakObjectPtr`, `TSoftObjectPtr`, `TSubclassOf`, `TScriptInterface`, `TSharedPtr`와 `TUniquePtr`에도 같은 규칙을 적용해야 한다.
- wrapper expression은 metadata로 보존하되 wrapper에서 owns 또는 inherits relation을 만들지 않아야 한다.
- 흔한 wrapper type reference는 약한 ranking signal로 사용하거나 기본 ranking에서 제외해야 한다.

### 9.4 Unreal 전용 의미

#### FR-UE-001 — UHT 계층

- `.generated.h`와 `.gen.cpp`는 compile/bridge 계층으로 사용할 수 있으나 기본 검색, class card, 일반 graph와 ranking에 노출하지 않아야 한다.
- UHT generated 사실은 원본 사용자 source 위치에 다시 매핑해야 한다.
- generated 판정은 파일명/함수명 heuristic이 아니라 선택 profile의 UBT/UHT provenance에 근거해야 한다.
- 사용자가 작성한 `CustomThunk` 등은 이름이 `exec*`라는 이유로 제거하지 않아야 한다.
- `execFoo`, `StaticRegisterNatives*`, `Z_Construct_*`, `EmptyLinkFunction*`, registration array와 UHT generated constructor/destructor를 공개 call graph와 ranking에서 제외해야 한다.

#### FR-UE-002 — Event, RPC와 interface

- Unreal `UINTERFACE`의 U/I pair를 명시적으로 모델링해야 한다.
- 사용자 선언 `Foo()`와 UHT generated `Foo()` 정의는 하나의 논리 symbol로 합치고 표시 위치는 사용자 source여야 한다.
- `Foo_Implementation()`과 RPC `Foo_Validate()`는 별도 callable이며 원본과 Unreal-specific relation으로 연결해야 한다.
- interface `Execute_Foo()`는 internal bridge로 보존하되 기본 결과에서 원본 `Foo`로 접고 callsite의 실제 spelling과 generated provenance를 남겨야 한다.
- `BlueprintImplementableEvent`는 native 구현을 만들지 않고 Blueprint external boundary로 표시해야 한다.
- `BlueprintNativeEvent`의 fallback과 RPC base-to-implementation 연결은 일반 confirmed `calls`가 아니라 전용 relation이어야 한다.
- `callers(Foo)`는 사용자 `Foo()`와 `Execute_Foo()` callsite를 포함하되 generated thunk/registration function은 제외해야 한다.

#### FR-UE-003 — Reflection metadata

- `UCLASS`, `USTRUCT`, `UENUM`, `UPROPERTY`, `UFUNCTION`의 원문 macro text를 보존해야 한다.
- 다음 항목은 코드 연결 relation으로 구조화해야 한다: Blueprint event, Getter/Setter, `ReplicatedUsing`, Server/Client/NetMulticast, RPC `_Implementation`/`_Validate`, `BindWidget`/`BindWidgetOptional`, `BlueprintAssignable`, Unreal interface와 delegate binding.
- Category, tooltip, edit flag와 clamp는 속성으로만 보존해야 한다.
- `StaticClass`, `IsA`, `Cast`, `FindFunction`, `ProcessEvent`, `NewObject`, `SpawnActor`는 특별 runtime edge가 아니라 일반 call/type reference로 처리해야 한다.
- Blueprint 구현과 BindWidget 대상은 external code boundary로 표시해야 한다.

#### FR-UE-004 — Delegate와 callback

- delegate declaration을 구조화해야 한다.
- compiler/구문으로 직접 확인되는 binding/subscription에서 owner class와 handler relation을 기록해야 한다.
- `Broadcast`, `Execute`, `ExecuteIfBound`를 특별 실행 edge로 만들지 않아야 한다.
- `Remove`, `Unbind`, `Clear`와 lifecycle을 특별 relation으로 만들지 않아야 한다.
- callback의 미래 실행을 dataflow 없이 생성하지 않아야 한다.

#### FR-UE-005 — Module과 target

- Target이 module을 포함하고 module이 file을 포함하는 구조를 모델링해야 한다.
- module dependency의 public/private와 runtime/editor 성격을 보존해야 한다.
- module 정보를 `Build.cs` 정규식이 아니라 선택 UBT profile의 해석 결과에서 얻어야 한다.
- 외부 Engine/Plugin module은 이름만 가진 placeholder여야 한다.

#### FR-UE-006 — Unity, PCH, `.inl`과 split implementation

- unity file과 PCH는 compile context로만 사용하고 검색, graph와 class card에 노출하지 않아야 한다.
- compiler source location을 사용해 모든 사실을 원본 `.h`, `.cpp`, `.inl`에 다시 귀속해야 한다.
- unity action 내 여러 `.cpp` 결과를 원본별로 분리하고 canonical identity로 중복 제거해야 한다.
- `.inl`은 고정 header/implementation이 아니라 source fragment로 취급해야 한다.
- `.inl`의 물리 위치, semantic owner와 inclusion context를 보존하고 여러 context를 허용해야 한다.
- `.inl` 변경 시 관련 TU를 무효화해야 한다.
- 하나의 class 구현이 여러 `.cpp`에 나뉜 split implementation을 정상 지원해야 한다.

### 9.5 저장, 증분 갱신과 freshness

#### FR-IDX-001 — SQLite fact store

- local SQLite를 사용하고 graph database나 monolithic JSON을 primary store로 사용하지 않아야 한다.
- 외부 stable ID와 별도로 join/traversal용 integer surrogate key를 사용할 수 있어야 한다.
- workspace/profile, analysis unit, file, entity/symbol, location, relation, evidence, unresolved site, contribution과 diagnostic을 저장해야 한다.
- 자주 질의하는 필드는 정규 column에 두고 드문 언어별 metadata만 JSON payload로 허용해야 한다.
- 전체 source 본문을 DB에 복제하지 않고 path/span/hash, 검색 token과 작은 파생 정보만 저장해야 한다.
- WAL을 선호하되 지원되지 않는 filesystem에서는 rollback journal로 fallback해야 한다.

#### FR-IDX-002 — Atomic generation

- 새 분석은 staging contribution에 기록하고 성공 시 transaction으로 이전 contribution과 atomic 교체해야 한다.
- 실패 시 마지막 committed generation을 유지하고 stale/failed 상태를 별도로 기록해야 한다.
- fact generation과 ranking generation을 분리하고 ranking 실패와 무관하게 구조 도구가 동작해야 한다.
- 한 writer와 마지막 committed generation을 읽는 reader 구조를 사용해야 한다.
- PID lock file 삭제 방식 대신 OS lock을 사용해야 한다.

#### FR-IDX-003 — Invalidation

- 분석 실행 단위는 compile action/TU, 저장·교체 단위는 원본 file 및 symbol contribution이어야 한다.
- TU별 evidence를 보존해 한 TU 무효화가 다른 TU의 유효 evidence를 제거하지 않아야 한다.
- `.cpp` 변경은 관련 compile action, `.h`/`.inl` 변경은 선택 profile의 reverse include closure를 무효화해야 한다.
- unity action은 전체 재분석하되 원본별 contribution을 교체해야 한다.
- reflected header 변경 시 structure를 갱신하고 UHT context를 stale로 표시해야 한다.
- body-only 변경은 해당 symbol의 outgoing relation, retrieval과 deep input을 갱신해야 한다.
- signature 변경은 이전 symbol 제거, 새 symbol 생성과 dependent TU/resolver bucket 갱신을 유발해야 한다.
- file content, declaration/API, symbol body, retrieval document, deep input과 compile action fingerprint를 분리해야 한다.
- 검색에서 제외한 generated/ignored header와 분석 root 밖 dependency도 선택 compile action의 입력이면 변경 감시 대상이어야 한다. 변경·삭제 시 관련 semantic evidence를 무효화하고 UHT 입력이면 UHT context도 stale로 표시해야 한다.
- `Build.cs`, `Target.cs`, project/plugin descriptor, response file, toolchain과 compile option의 변경은 관련 compile context와 의존 semantic graph를 stale로 표시해야 한다. query에서는 기존 실행 제한을 유지하고 필요한 명시적 build를 안내해야 한다.
- 일반 C++에서는 compilation database의 변경·교체·삭제, 선택한 build directory/profile, response file과 참조 dependency의 변경을 감시해야 한다. Source 추가 후 compile command가 없으면 구조 결과를 제공하고 semantic context가 missing임을 표시해야 한다.
- 알려진 CMake configure 입력이 변경되면 compile context를 stale로 표시하고 database 재생성을 안내해야 한다. 다른 build system은 제공된 database·dependency의 검증 범위와 재생성이 필요한 조건을 명시해야 한다.

#### FR-IDX-004 — Query-time freshness

- query는 stat probe, 변경 file hash, writer lock 획득 후 재검사를 수행해야 한다.
- structural/lexical 변경은 작은 budget에서 증분 갱신해야 한다.
- 유효 compile context가 있고 범위가 budget 안이면 semantic 갱신을 수행할 수 있어야 한다.
- 큰 header fan-out은 2초 안에 stale semantic 상태와 `livecodemap build` 안내를 반환해야 한다.
- 삭제되거나 새로 ignore된 file을 stale 결과로 계속 노출하지 않아야 한다.
- 존재하는 변경 file의 이전 semantic evidence는 명확히 stale로 표시한 경우에만 유지할 수 있다.
- source location/evidence는 분석 당시 file content hash와 연결해야 한다. 현재 file에서 읽는 snippet의 정합성은 FR-RET-004에 따라 별도로 검증해야 한다.
- `check_freshness`는 상태만 검사하고 UBT나 대규모 rebuild를 실행하지 않아야 한다.

#### FR-IDX-005 — 계층별 상태

- 사용자 상태는 `ready`, `partial`, `stale`, `missing`, `failed`로 요약해야 한다.
- 내부적으로 availability, freshness, last success와 last attempt를 분리해야 한다.
- structure, compile context, UHT context, semantic graph, retrieval과 deep 상태를 독립적으로 제공해야 한다.
- 각 계층에 적용 여부를 제공해야 한다. 일반 C++의 UHT context는 `applicable: false`로 표시하고 missing/failed나 전체 상태 저하의 원인으로 집계하지 않아야 한다.
- total/success/failed/stale/ignored unit 수를 제공해야 한다.

#### FR-IDX-006 — Ignore

- 사용자가 project별 folder/file pattern으로 ignore를 지정할 수 있어야 한다.
- 파일명이나 내용만으로 generated code를 임의 판정하지 않아야 한다.
- ignore는 공개 structure, deep 입력, retrieval, graph와 visualization의 포함 범위에 일관되게 적용해야 한다.
- 검색·출력 제외와 분석 의존성 변경 감시는 분리해야 한다. ignored file도 다른 포함 file의 의미 해석에 영향을 주면 dependency fingerprint와 reverse invalidation에 유지해야 한다.
- 적용 rule과 포함·제외·실패 file 수를 보여줘야 한다.
- ignore 대상이 compile context에 나타나도 node/relation/search/deep에서 제외하고 참조는 ignored/unresolved boundary로 남겨야 한다.
- 의존성 검증을 위한 내부 metadata가 ignored/external file의 본문이나 API를 공개 검색 결과로 노출하는 근거가 되어서는 안 된다.

#### FR-IDX-007 — Workspace와 worktree

- worktree마다 `.livecodemap/` 아래 별도 mutable SQLite DB와 OS lock을 사용해야 한다.
- worktree 간 live DB를 공유하지 않고 immutable content-addressed cache만 Windows user-local cache로 공유해야 한다.
- 하나의 Git monorepo는 하나의 index로 처리해야 한다.
- nested repository와 submodule은 감지한 뒤 사용자가 workspace member로 명시적으로 선택해야 한다.
- 독립 repository workspace는 member별 index와 workspace catalog를 사용해야 한다.
- cross-member relation은 compiler가 실제 확인한 참조만 overlay relation으로 제공해야 한다.
- worktree는 compatible main snapshot으로 seed할 수 있지만 현재 worktree hash를 다시 검사하고 다른 file을 무효화해야 한다.

### 9.6 Retrieval과 context pack

#### FR-RET-001 — Ranking projection

- 검색 document 단위는 사용자 source symbol, 물리 file, 검색용 class aggregate, module/target과 선택적 deep summary여야 한다.
- UHT/unity/PCH, ignored source, compiler implicit symbol, external placeholder, 흔한 wrapper type reference와 unconfirmed candidate edge는 기본 ranking projection에서 제외해야 한다.
- identifier, qualified name, path, signature/type, natural language, body/comment와 deep summary field를 분리해야 한다.
- lexical seed, 이름·경로 가중치, BM25/IDF, graph rank, file diversity, test/helper 감점과 scope fusion을 결합해야 한다.
- Unreal relation 증가가 기본 검색 품질을 낮추지 않도록 fact graph와 ranking graph를 분리해야 한다.

#### FR-RET-002 — 일반 tokenization

- Graft의 lexical 검색을 기준으로 identifier, path와 자연어 token을 처리하고 C++/Unreal corpus의 비교 결과로 개선을 검증해야 한다.
- 원문 identifier를 보존하고 search normalization을 compiler identity에 사용하지 않아야 한다.
- Unicode source와 path의 원문을 보존하고 tokenizer 및 normalization의 version을 retrieval fingerprint에 포함해야 한다.
- 한국어 전용 lexicon, 조사 처리, CJK bigram이나 별도 다국어 모델을 필수 구현으로 요구하지 않는다. 언어별 특수 처리의 필요성은 공통 benchmark와 실제 사용 근거로 판단한다.

#### FR-RET-003 — Identifier 분해

- 원본 전체, qualified component, snake/kebab/camel/Pascal/acronym/digit 경계 variant를 생성해야 한다.
- Unreal의 `U`, `A`, `F`, `S`, `E`, `T`, `I` 단일 type prefix 제거 variant를 추가할 수 있어야 한다.
- project-specific prefix를 hard-code하지 않아야 한다.
- 초기 ranking 우선순위는 exact identifier, qualified/suffix, identifier component, path, natural-language BM25, graph, body/comment/string 순이어야 한다.
- 구조 탐색 의도는 목적별 도구와 graph signal로 지원하고 특정 자연어의 예외 처리를 필수 경로로 두지 않아야 한다.

#### FR-RET-004 — Class card와 source context

- 기본 결과는 symbol/file 중심이고 file diversity를 유지해야 한다.
- class명 질의에는 declaration/definition 위치, inheritance, interface, reflection, subscription/binding과 주요 method를 합친 파생 class card를 제공할 수 있어야 한다.
- class card는 graph에서 파생하며 별도 LLM summary를 필수로 하지 않아야 한다.
- 기본 snippet은 관련 부분만 제공해야 한다.
- snippet은 실제로 읽은 file bytes의 hash와 해당 span을 산출한 source hash가 일치할 때만 제공해야 한다. 응답에는 graph generation과 snippet source hash를 함께 기록해야 한다.
- hash가 다르면 현재 source를 재분석해 stable symbol과 span을 확인하거나 snippet을 보류하고 `source_changed` diagnostic과 재조회 방법을 반환해야 한다. 이전 span으로 현재 file의 다른 코드를 인용하지 않아야 한다.
- 현재 source로 재매핑한 snippet과 이전 semantic evidence를 함께 제공할 때는 각각의 source hash와 freshness를 구분해야 한다. 재분석·읽기 중 재변경도 같은 규칙을 적용한다.
- `--full`도 결과당 약 80줄을 넘지 않고 clipping과 원래 범위를 표시해야 한다.

### 9.7 CLI, MCP와 응답 계약

#### FR-API-001 — CLI surface

첫 릴리스 CLI는 다음 command를 제공해야 한다.

- `build`
- `ask`
- `skeleton`
- `grep`
- `callers`
- `map`
- `blast`
- `check`
- `viz`
- `init`
- `remove`

#### FR-API-002 — MCP surface

첫 릴리스 MCP server는 다음 tool을 제공해야 한다.

- `find_code`
- `find_all`
- `file_api`
- `trace_calls`
- `repo_map`
- `check_freshness`

#### FR-API-003 — 공통 semantic response

- CLI와 MCP는 같은 semantic response model을 사용하고 CLI만 사람 친화적으로 render해야 한다.
- 응답은 선택된 project 종류(`cpp` 또는 `unreal`)와 계층별 적용 여부를 포함해야 한다. 일반 C++에 적용되지 않는 Unreal metadata는 비적용으로 표시해야 한다.
- 모든 도구는 profile, graph generation, 계층별 freshness, `success|partial|stale|error`, stable ID, `confirmed|possible|unresolved`, diagnostic과 pagination 정보를 반환해야 한다.
- pagination은 `complete`, `total`, `shown`, `returned_count`, `discovered`, `omitted`, `next_cursor`와 제한 이유를 포함해야 한다. `complete`는 요청한 scope의 탐색 완료 여부, `shown`은 현재 page의 결과 수, `returned_count`는 cursor 연속 조회에서 중복 없이 반환한 누적 수, `discovered`는 지금까지 발견한 고유 결과 수다.
- `complete: false`이면 `total`과 `omitted`는 `null`이며 발견된 수를 전체 수로 단정하지 않아야 한다. 탐색 완료 시 `total`은 정확한 고유 결과 수, `omitted`는 `total - returned_count`여야 한다.
- cursor는 query, profile, graph generation과 결과에 사용된 ranking generation·ignore scope에 종속되어야 한다. 관련 snapshot이 바뀌면 만료를 알리고 재조회를 안내해야 한다.
- traversal budget 소진 시 `partial`과 이어서 탐색할 cursor를 반환해야 한다. cursor는 방문 집합과 남은 탐색 상태를 유지하거나 재구성해 동일 snapshot에서 중복·누락 없이 진행하고 완료할 수 있어야 한다. 탐색 완료 후에도 미반환 page가 있으면 cursor를 제공해야 한다.
- 모호한 symbol selector에서 임의의 하나를 선택하지 않고 후보와 signature를 반환해야 한다.

#### FR-API-004 — 도구별 기본값

- CLI `ask`는 기본 8개 결과를 반환해야 한다.
- MCP `find_code`는 기본 5개 결과를 반환해야 한다.
- `ask`는 외부 LLM 답변 생성기가 아니라 identifier, natural language, path와 graph signal을 결합한 context pack이어야 한다.
- `callers`는 기본 1 hop, 20 caller를 제공하고 caller별 callsite를 묶으며 unique caller 수와 callsite 수를 분리해야 한다. 미완료 탐색의 수치는 발견된 수임을 표시해야 한다.
- `trace_calls`는 기본 1 hop, 20 record를 제공하고 confirmed path와 possible dispatch branch를 분리해야 한다.
- `--all`은 reachable closure 전체를 요청하는 option이며 시간·방문 node budget을 해제하지 않아야 한다. cursor로 탐색을 이어갈 수 있어야 하며 cyclic graph의 모든 simple path 조합을 열거하지 않아야 한다.
- `blast`는 기본 2 hop, 50 impact를 제공하고 `directly_changed`, `confirmed_dependency_path`, `possible_runtime_path`, `unresolved_boundary`, `external_boundary`를 분리해야 한다.
- `blast`의 confirmed는 동작 변화가 확정됐다는 뜻이 아니라 구조적 dependency path가 확정됐다는 뜻이어야 한다.
- `file_api`는 실제 물리 file 기준 기본 100 entry를 반환하고 module/profile/freshness, direct include, 선언·정의 symbol, 다른 owner 위치 link와 `.inl` context를 포함해야 한다.
- 일반 C++에서 확인할 수 없는 build target/module metadata는 `null`과 미확인 사유를 제공하고, 해당 metadata 부재만으로 공통 file/symbol 탐색을 실패 처리하지 않아야 한다.
- split implementation을 `file_api`가 임의 병합하지 않고 logical symbol/class card link를 제공해야 한다.

### 9.8 선택적 `--deep`

#### FR-DEEP-001 — 명시적 실행과 provider 선택

- deep은 사용자가 명시적으로 `--deep`을 실행할 때 수행해야 한다.
- 사용자는 외부, 사내 또는 local LLM provider를 선택할 수 있어야 한다.
- deep의 입력 범위는 공통 ignore와 symbol-window 규칙을 따라야 한다.
- hook는 `--deep`을 자동 실행하지 않아야 한다.

#### FR-DEEP-002 — 독립 실패와 cache

- deep 실패와 무관하게 structure graph가 동작해야 한다.
- 성공, 실패, 누락과 cache reuse 상태를 분리해야 한다.
- deep 결과는 content-addressed cache에 보존하고 compatible index 재생성에서 재사용해야 한다.
- 변경 없는 deep 재실행의 provider token은 0이어야 한다.
- symbol body 변경은 해당 symbol과 명시적 dependent aggregate만 무효화해야 한다.
- cache hit/miss, 미처리·실패 unit과 중단 이유를 모두 보고해야 한다.

#### FR-DEEP-003 — 입력 단위와 비용

- file prefix가 아니라 target symbol span 중심 window를 사용해야 한다.
- target source를 포함하고 import와 인접 type은 budget 안에서만 추가해야 한다.
- symbol workload의 기본 input 상한은 8K token, output 상한은 512 token이어야 한다.
- provider가 반환한 실제 input/output/cache token, 비용과 p50/p95 시간을 기록해야 한다.
- 고정 달러 금액을 release gate로 사용하지 않아야 한다.

### 9.9 Agent 통합

#### FR-AGT-001 — 지원 host와 설치 위치

- 지원 대상은 Claude Desktop, Claude Code CLI, Codex와 Codex CLI다.
- Codex는 `AGENTS.md`, project-local `.codex/config.toml`과 `.codex/hooks.json`을 사용해야 한다.
- Claude Code는 `CLAUDE.md`, `.mcp.json`과 `.claude/settings.json`을 사용해야 한다.
- Claude Desktop은 `.mcpb` Desktop Extension과 앱 연결 상태만 사용하고 project hook를 설치하지 않아야 한다.
- 기본 init은 project-local 설정만 수정하고 user home/machine-wide 설정은 별도 명시적 선택 없이는 수정하지 않아야 한다.

#### FR-AGT-002 — 안전한 init

- `livecodemap init`은 감지한 host를 보여주고 사용자가 선택한 host만 연결해야 한다.
- non-interactive init은 `--host`를 요구해야 한다.
- `--dry-run`은 수정 path, repo/user scope, 생성·merge·conflict 여부를 보여줘야 한다.
- instruction file에는 짧은 managed block만 삽입하고 기존 내용을 덮어쓰거나 중복 block을 만들지 않아야 한다.
- JSON/TOML은 LiveCodeMap 소유 entry만 merge해야 한다.
- parse 실패나 같은 이름의 다른 MCP entry conflict가 있으면 수정하지 않고 보고해야 한다.
- 가능한 경우 여러 file 변경을 staging 후 일괄 반영해야 한다.
- install manifest와 managed block은 host, component, LiveCodeMap version과 ownership을 기록해야 한다.
- 같은 option의 재실행은 idempotent해야 한다.

#### FR-AGT-003 — Instruction 내용

상시 managed instruction은 다음만 짧게 포함해야 한다.

- LiveCodeMap 우선 탐색
- freshness와 confidence 구분
- `--deep`은 명시적으로만 실행
- 일반 file tool로 중요 source를 최종 검증

긴 설명은 MCP tool description과 선택적 host-native skill에 둔다.

#### FR-AGT-004 — Lifecycle hook

- `SessionStart`는 selected profile, index 존재 여부와 계층별 freshness만 짧게 제공해야 한다.
- `UserPromptSubmit`은 identifier, path 또는 code exploration 의도가 명확할 때만 결정론적 local `ask`를 실행해야 한다.
- `UserPromptSubmit` context는 최대 5개 결과, 약 1,500 token 이하, 초기 foreground budget 2초여야 한다.
- `PostToolUse`는 실제 write/edit tool에만 반응해 변경을 debounce하고 structural/lexical refresh를 async single-flight로 요청해야 한다.
- `PostToolUse`는 매번 blast 결과를 대화에 주입하지 않아야 한다.
- `Stop`은 dirty/freshness를 검사하고 필요할 때 background refresh를 요청할 수 있으나 turn 종료를 막거나 continuation/test/build를 강제하지 않아야 한다.
- 모든 hook는 fail-open이어야 하며 한 줄 diagnostic, timeout, recursion guard와 기존 writer lock을 사용해야 한다.

#### FR-AGT-005 — 상태 표시

- 공통 상태는 profile, graph generation, structure/compile context/UHT/semantic/retrieval/deep, unit 수, last success와 last attempt를 제공해야 한다.
- Claude Code statusline은 사용자가 명시적으로 선택하고 기존 custom statusline이 없을 때만 설치해야 한다.
- 기존 statusline을 덮어쓰거나 임의 합성하지 않아야 한다.
- Codex는 공식 hook `statusMessage`와 MCP 결과를 사용해야 한다.
- Claude Desktop은 Extensions/Connectors 상태와 `check_freshness`를 사용해야 한다.
- 추정 token 절감량을 표시하지 않고 실제 provider usage가 있을 때만 통계를 표시해야 한다.

#### FR-AGT-006 — 안전한 remove

- `livecodemap remove`는 기본적으로 plan만 보여주고 `--yes`가 있어야 반영해야 한다.
- 특정 host 제거와 `--keep-index`를 지원해야 한다.
- LiveCodeMap 소유 block, MCP entry, hook와 helper만 제거해야 한다.
- 사용자 내용이 남은 file 자체를 삭제하지 않아야 한다.
- 설치 후 managed 영역이 수정됐다면 자동 삭제하지 않고 conflict로 보존해야 한다.
- Claude Desktop extension 제거는 앱 UI에서 사용자가 승인해야 한다.
- 결과를 `removed`, `unchanged`, `modified-preserved`, `missing`, `manual-action-required`로 구분해야 한다.

### 9.10 시각화와 HTML export

#### FR-VIZ-001 — 질의 중심 viewer

- `livecodemap viz`는 전체 graph 전시가 아니라 질의 결과 탐색 도구여야 한다.
- 기본 화면은 `map` 기반 target/module/directory 개요여야 한다.
- symbol/file 선택 시 기본 1-hop relation을 보여주고 사용자가 방향별로 한 단계씩 확장할 수 있어야 한다.
- 전체 저장소의 모든 node/edge를 browser에 한꺼번에 보내지 않아야 한다.
- 항상 shown, 탐색 완료 여부와 제한 이유를 표시해야 한다. 전체 수를 모르면 omitted를 미확정으로 표시하고 이어서 탐색할 수 있어야 한다.

#### FR-VIZ-002 — 표현 계약

- symbol search, node kind/relation filter, 방향, signature, repository-relative 위치, 작은 snippet, 포함 이유, profile, generation과 계층별 freshness를 제공해야 한다.
- confirmed relation은 실선, possible relation은 점선으로 구분해야 한다.
- unresolved call은 가짜 target node가 아니라 callsite warning/evidence로 표시해야 한다.
- stale relation을 숨기지 않아야 한다.
- graph와 같은 범위를 나타내는 list/outline view를 제공해야 한다.
- blast view는 changed symbol, confirmed dependency path, possible runtime path, unresolved boundary와 external boundary를 구분해야 한다.

#### FR-VIZ-003 — Local viewer 보안

- `127.0.0.1`에만 bind해야 한다.
- read-only이며 임의 session token을 사용해야 한다.
- 외부 network 요청과 telemetry를 수행하지 않아야 한다.
- `--no-open`을 지원해야 한다.
- 조회와 확장은 일반 freshness 계약을 따르고 UBT/UHT/build/deep을 자동 실행하지 않아야 한다.
- generation이 바뀌면 이전 응답을 섞지 않아야 한다.

#### FR-VIZ-004 — 독립 HTML

- CSS, JavaScript와 선택 graph data를 포함한 단일 offline file이어야 한다.
- `file://`에서 동작하고 CDN이나 server를 요구하지 않아야 한다.
- 현재의 제한된 view/query snapshot만 포함하고 SQLite DB나 전체 저장소 graph를 넣지 않아야 한다.
- profile, generation, freshness, 포함 수와 탐색 완료 여부를 기록해야 한다. 전체 수가 확정된 경우에만 생략 수를 기록하고 미완료 snapshot은 부분 결과임을 표시해야 한다.
- 절대 경로 대신 repository-relative path만 포함해야 한다.
- snippet 제한을 적용하고 code가 포함될 수 있음을 export 전에 경고해야 한다.
- 첫 릴리스에는 별도 LLM context graph tab을 제공하지 않으며 cached deep summary는 detail panel에서만 보여줄 수 있다.

### 9.11 배포, version과 upgrade

#### FR-DST-001 — 배포 artifact

- 공개 Git 저장소가 원본이어야 한다.
- npm organization `@livecodemap`를 확보하고 launcher를 `@livecodemap/cli`로 배포해야 한다.
- package scope와 무관하게 실행 command는 `livecodemap`여야 한다.
- 첫 platform package는 `@livecodemap/win32-x64`여야 하고 launcher와 exact version dependency로 lockstep을 유지해야 한다.
- install 중 GitHub나 별도 server에서 binary를 내려받는 `postinstall`을 사용하지 않아야 한다.
- platform package 자체가 검증된 binary와 필요한 runtime을 포함해야 한다.
- 지원하지 않는 OS/architecture를 install 또는 첫 실행에서 명확히 보고해야 한다.

#### FR-DST-002 — Release 구성

하나의 `vX.Y.Z` source tag에서 다음 artifact를 생성해야 한다.

- npm launcher와 Windows platform package
- Windows standalone ZIP
- Claude Desktop `.mcpb`
- SHA-256 checksum
- SBOM
- `THIRD_PARTY_NOTICES`

- Source tag를 checkout하고 문서화한 CMake preset으로 build하는 설치 경로를 지원해야 한다.
- Source build와 배포 binary는 동일 CLI/MCP 동작과 version을 제공해야 한다.
- `.mcpb` install/update/remove는 첫 릴리스에서 Claude Desktop UI를 통한 명시적 동작이어야 한다.

#### FR-DST-003 — Publish 보안

- tag 기반 CI만 release를 생성해야 한다.
- source build, test, clean-machine npm install, `livecodemap --version`, MCP start와 init/remove smoke test가 통과해야 publish할 수 있다.
- npm publish는 OIDC trusted publishing과 provenance를 사용해야 한다.
- package allowlist로 예상하지 않은 source, fixture, credential과 absolute path 포함을 차단해야 한다.
- 첫 릴리스에서 Authenticode는 필수가 아니지만 unsigned binary임을 숨기지 않아야 한다.

#### FR-DST-004 — Version과 명시적 upgrade

- SemVer를 사용해야 한다.
- 1.0 전 breaking change는 minor를 올리고 patch는 compatible fix만 포함해야 한다.
- launcher, platform package, `.mcpb`와 ZIP은 같은 version이어야 한다.
- CLI version과 별도로 SQLite schema, MCP response schema와 stable ID schema version을 유지해야 한다.
- npm latest 확인은 사용자가 `livecodemap check`를 실행할 때만 수행해야 한다.
- 공식 npm registry 외에 별도 update server를 운영하거나 요구하지 않아야 한다.
- hook, MCP query와 일반 검색은 version 확인을 위해 network에 접속하지 않아야 한다.
- LiveCodeMap는 자체 executable을 교체하지 않아야 한다.
- npm 사용자는 안내된 `npm install -g`, source 사용자는 tag checkout/rebuild로 명시적으로 upgrade해야 한다.
- package upgrade 후 agent wiring을 자동 수정하지 않아야 한다.
- `livecodemap check`는 version 차이가 있으면 `livecodemap init --dry-run`과 명시적 refresh를 안내해야 한다.

#### FR-DST-005 — Schema 호환성

- incompatible DB를 query 중 자동 migration하거나 삭제하지 않아야 한다.
- `livecodemap build`가 새 generation을 만들고 성공한 경우에만 교체해야 한다.
- compatible content-addressed deep cache는 재사용하고 incompatible entry는 무시해야 한다.
- downgrade도 기존 DB를 파괴하지 않고 `incompatible index`와 필요한 version/build를 안내해야 한다.

#### FR-DST-006 — License

- LiveCodeMap source, documentation과 공개 synthetic fixture는 Apache-2.0으로 배포해야 한다.
- bundled LLVM/Clang 등 third-party component는 원 license를 유지하고 필요한 attribution을 artifact와 `THIRD_PARTY_NOTICES`에 포함해야 한다.
- Graft source를 복사하지 않아야 한다.
- 비공개 로컬 source와 그로부터 파생된 공개 불가 정보는 모든 공개 artifact에서 제외해야 한다.
- 초기에는 CLA를 요구하지 않고 contribution이 project license로 제공됨을 `CONTRIBUTING.md`에 명시해야 한다.

## 10. 데이터·보안 요구사항

### SEC-001 — 분석 입력 범위

- 분석과 deep 입력은 선택한 project/profile 및 공통 ignore 규칙을 따라야 한다.
- 검색에서 제외된 의존성의 변경 감시는 FR-IDX-003/006을 따라야 한다.
- deep의 대상/실패/cache 상태를 사용자에게 보여줘야 한다.

### SEC-002 — Asset 제외

- `/Game/...` path를 node로 모델링하거나 검증하지 않아야 한다.
- Content, redirector와 cook 상태를 분석하지 않아야 한다.
- `LoadObject`, `LoadIcon` 등은 일반 code call로 취급해야 한다.
- `TSoftObjectPtr`은 type reference이며 외부 asset node를 생성하지 않아야 한다.

### SEC-003 — Telemetry와 local statistics

- 첫 릴리스 제품 동작은 telemetry를 요구하지 않아야 한다.
- 비용/성능 통계는 local에 저장하고 provider가 반환한 실제 usage만 사용해야 한다.

### SEC-004 — 공개/로컬 benchmark 분리

- 공개 release gate는 synthetic 및 open-source corpus만으로 재현할 수 있어야 한다.
- 선택적 비공개 검증의 identity, source, 경로, 설정, snippet, raw trace, 측정치와
  파생 사실을 repository, npm package, fixture와 공개 report에 포함하지 않아야 한다.
- 비공개 검증 입력과 결과는 저장소 밖 또는 명시적으로 ignore된 local-only 경로에 둔다.

## 11. 비기능 요구사항

### 11.1 성능 목표

공통 기준 환경은 Windows 11, 16 logical core 이상, 32GiB RAM과 NVMe SSD다. 일반 C++ track은 Unreal 미설치 환경의 고정 compilation database/profile을, Unreal track은 release에서 명시한 지원 UE profile을 사용한다. Benchmark 결과에는 실제 hardware, toolchain과 filesystem cache 상태를 기록한다. 공통 query/freshness 목표는 두 공개 track에 적용한다.

| 항목 | 첫 릴리스 목표 |
| --- | ---: |
| 변경 없음 freshness p95 | 100ms 이하 |
| Warm search p95 | 500ms 이하 |
| 기본 1~2 hop traversal p95 | 1초 이하 |
| 단일 file structural/retrieval 갱신 후 응답 p95 | 2초 이하 |
| 단일 `.cpp` semantic 갱신 p95 | 10초 이하 |
| 큰 header fan-out stale 반환 | 2초 이하 |
| Worktree seed 및 검증 | 1분 이하 |

- CLI startup과 freshness 확인 시간을 query latency에 포함해야 한다.
- traversal 성능은 고정 query별 완전한 결과까지의 시간·방문 node 수와 첫 page 응답 시간을 각각 보고해야 한다. 기본 traversal p95 목표는 고정 benchmark의 기본 1~2 hop query가 완전히 계산되는 시간을 기준으로 하며, budget에 걸린 partial 응답을 완료 시간으로 집계하지 않아야 한다.
- 대규모 traversal의 budget 중단·재개는 별도로 검증하고 실제 시간·node budget과 `complete` 상태를 결과에 기록해야 한다.
- `project build required` 시간은 LiveCodeMap build 시간과 분리해야 한다.
- 10K/50K/100K synthetic symbol 규모에서 build throughput 저하는 30% 이내여야 한다.
- 목표를 맞추기 위해 graph fact를 삭제하지 않아야 한다. 초과 시 원인을 공개하고 저장/조회 구조를 개선해야 한다.

### 11.2 결정성과 복원력

- 같은 input/profile/toolchain의 clean build는 동일 fact hash를 생성해야 한다.
- deep, ranking 또는 일부 TU 실패가 valid committed structure를 손상시키지 않아야 한다.
- process crash 또는 취소 후 partial staging data가 committed generation으로 노출되지 않아야 한다.
- query와 hook failure는 사용자 편집과 agent turn을 막지 않아야 한다.

### 11.3 사용성

- 모든 상태와 error는 다음 행동을 제안해야 한다.
- partial/stale/degraded 상태를 success로 숨기지 않아야 한다.
- absolute path는 local CLI에 필요할 때만 사용하고 export와 공개 trace에는 repository-relative path를 사용해야 한다.
- default output은 작아야 하며 요청 범위, 반환량과 탐색 완료 여부를 항상 알 수 있어야 한다. 전체 수가 미확정이면 그 사실과 후속 탐색 방법을 표시해야 한다.

## 12. Benchmark와 검증 계획

### 12.1 Corpus

| Corpus | 용도 | 공개 여부 |
| --- | --- | --- |
| Synthetic C++ fixture | Compilation database, 다중 TU/profile, template, overload, virtual dispatch, header-only, fallback과 증분 갱신 gold data | 공개 |
| Synthetic Unreal fixture | UHT, RPC, delegate, virtual dispatch, `.inl`, unity/PCH, ignore, workspace의 graph/mutation gold data | 공개 |
| `{fmt}` 12.2.0 고정 commit | Unreal 미설치 환경의 일반 C++ semantic/retrieval, template, overload, alias, header-only, Unicode | 공개 |
| Cesium for Unreal v2.29.1 고정 commit | 실제 공개 Unreal code | 공개 |

- Cesium은 engine-independent public structural lane과 UE가 설치된 Windows runner의 full semantic lane으로 나눈다.
- 일반 C++ track은 Synthetic C++ fixture와 `{fmt}`의 consumer/test TU를 포함한 compilation database로 검증한다. Unreal track은 Synthetic Unreal fixture와 Cesium으로 검증하며 한 track의 성공으로 다른 track의 검증을 대체하지 않는다.
- 공개 benchmark query set은 identifier, 영어 기능 설명, acronym과 path를 포함하고 튜닝 전에 크기와 manifest를 고정한다.
- Query는 Graft와 공통 비교 가능한 identifier, path, 영어 기능 설명과 acronym, C++/Unreal 전용 탐색 과제를 포함한다. 한국어 전용 query quota나 한영 동등성 gate는 두지 않는다.
- 공개 query를 개발·튜닝 set과 최종 평가용 holdout으로 분리하고 각 subset의 query 수와 manifest를 튜닝 전에 고정한다. 분할은 query 유형별로 층화하며 같은 의도의 표현 변형은 같은 split에 둔다.
- 튜닝에 사용한 query와 holdout의 결과를 분리하고 release retrieval gate는 holdout에 적용한다. holdout 결과를 보고 튜닝했다면 해당 set은 개발용으로 전환하고 새 holdout을 고정해야 한다.
- Graft 비교 시 version/commit, snapshot, query, scope, context budget과 deep 사용 여부를 맞추고 실행 설정을 기록한다. 공통 기능 비교와 Graft에 없는 Unreal 전용 기능 검증은 별도 결과로 보고한다.

### 12.2 Graph 품질 gate

| Metric | Gate |
| --- | ---: |
| Synthetic expected symbol/relation/state | 100% |
| Confirmed call/reference precision | 99% 이상 |
| Direct call/reference recall | 95% 이상 |
| Possible virtual/runtime target recall | 90% 이상 |
| Uncertain target의 confirmed 오승격 | 0건 |
| 기본 결과의 UHT/unity/PCH 내부 노출 | 0건 |
| 삭제·ignore file 잔존 | 0건 |
| 3회 clean build fact hash 일치 | 100% |

- 평가 전에 profile·분석 scope와 expected relation/target set을 고정한다. precision은 올바른 예측 수/전체 예측 수, recall은 회수한 정답 수/전체 정답 수로 계산하고 micro 집계와 relation 종류별 결과를 함께 보고한다.
- 분석 실패·미해결 때문에 회수하지 못한 in-scope 정답을 recall 분모에서 빼지 않는다. 외부·비활성·제외 범위는 사전에 고정하고 boundary coverage를 별도로 보고한다.
- Possible target은 callsite별 precision, recall과 후보 수의 중앙값/p95를 함께 보고한다. Synthetic fixture는 기대 candidate set과 정확히 일치해야 하며, 실제 corpus에서는 불필요 후보와 누락 유형을 함께 검토해야 한다.
- Graft와 공통 비교 가능한 candidate 조회에서는 같은 scope와 output budget에서 precision과 recall이 모두 Graft보다 낮지 않아야 한다. Unreal 전용 candidate 조회는 고정 gold set과 blind rubric으로 평가하며 recall 수치만으로 통과시키지 않는다.

### 12.3 Retrieval 품질 gate

| Metric | Gate |
| --- | ---: |
| Exact identifier/path Top-1 | 98% 이상 |
| 전체 질의 Hit@5 | 90% 이상 |
| Identifier 없는 자연어 질의 Hit@5 | 80% 이상 |
| 무관한 test/helper Top-5 진입률 | 5% 이하 |

- Graft 0.18.0과 공통 실행 가능한 C++ 질의에서는 LiveCodeMap 품질이 낮지 않아야 한다.
- Graft보다 낫다는 표현은 같은 snapshot/query/평가 기준의 결과가 있을 때만 사용한다.
- Hit@K는 상위 K개 안에 정답이 하나 이상 있는 query 수/전체 query 수다. Exact identifier/path Top-1은 해당 query subset의 Hit@1이며, 무관한 test/helper Top-5 진입률은 그러한 결과가 하나 이상 포함된 query의 비율이다.
- Recall@5는 query별로 상위 5개에서 회수한 고유 정답 수/해당 query의 전체 고유 정답 수를 계산한 뒤 macro 평균한다. Hit@5와 별도로 전체 정답 회수율을 보고하며 Graft 공통 query에서 Recall@5도 낮지 않아야 한다.
- 정답은 query별 symbol/file 단위를 미리 지정한다. class card나 한 결과에 포함된 여러 symbol을 임의로 복수 정답으로 세지 않고 고정된 매핑 규칙을 사용한다.
- 정답 없는 query의 거짓 양성률은 별도 평가하고 Hit/Recall의 정답 있는 query 분모에 섞지 않는다. 각 metric은 분자·분모, query 유형별 결과와 95% 신뢰구간을 함께 보고한다.
- 0.1의 모호한 Top-5 recall 표기는 0.2에서 사용자 검토에 따라 Hit@5 gate와 별도 Recall@5 보고·비교로 변경했다. 두 지표를 동일한 의미로 보고하지 않는다.

### 12.4 Incremental mutation set

다음 mutation을 고정하고 invalidated TU, 교체 contribution, 유지 evidence와 freshness를 gold data와 비교한다.

- body-only 수정
- `.cpp` 추가와 삭제
- signature 변경
- 작은 header fan-out
- 큰 header fan-out
- reflected header 변경과 UHT stale
- ignore 변경
- profile 변경
- worktree divergence
- ignored/generated header와 외부 dependency의 변경·삭제
- `Build.cs`, `Target.cs`, project/plugin descriptor, response file과 toolchain/compile option 변경
- 일반 C++ compilation database의 변경·삭제·복수 후보, build profile 전환과 source 추가 후 compile command 누락
- 일반 C++의 알려진 CMake configure 입력 변경과 header-only consumer context 변경
- 함수 앞 줄 삽입·삭제 후 이전 semantic evidence와 현재 snippet의 불일치
- snippet 재분석·읽기 중 source 재변경

Traversal은 별도로 high fan-out/cycle fixture에서 budget 중단과 cursor 재개를 검증한다. 동일 snapshot에서 모든 page를 합친 고유 결과 집합은 완전 탐색 gold set과 일치해야 하며, 중복·누락, 미완료 결과의 거짓 total, generation 변경 후 cursor 혼용이 없어야 한다.

### 12.5 Deep 비용 검증

- 실제 provider input/output/cache token, 비용과 p50/p95 시간을 기록한다.
- 변경 없는 재실행은 provider token 0이어야 한다.
- local/in-house provider도 같은 workload와 output 검증을 사용한다.
- cache hit/miss, 미처리·실패 unit과 중단 이유를 함께 보고한다.

### 12.6 Agent A/B

총 12개 task를 사용한다.

Phase 3에서 baseline file tool과 LiveCodeMap retrieval only를 비교하는 최소 harness를 먼저 실행하고, 공통 실행 가능한 task에는 Graft retrieval 조건도 포함한다. Navigation, Impact, Patch 각 1개 이상의 대표 개발 task로 정확도·토큰·시간·누락을 검토한 뒤 검색 개선과 deep 구현을 진행한다. 최종 release A/B는 아래 전체 조건으로 수행하며 초기 실험과 구분한다.

- Navigation 4개: class/file, call path, virtual/Unreal event boundary, delegate/BindWidget boundary
- Impact 4개: signature, module/include dependency, split implementation sibling, 공개 Unreal UI code path
- Patch 4개: pinned historical defect, base/override signature, UFUNCTION/RPC pair, 여러 sibling file 변경

일반 C++와 Unreal 공개 corpus에 걸쳐 12개 task를 구성하고 각 task에 expected
symbol/file set, 금지 file과 verification test 또는 manual rubric을 둔다.

Public task에는 Unreal 없이 수행하는 일반 C++ Navigation/Impact/Patch를 각 1개 이상 포함한다. 초기 Phase 3 A/B와 최종 결과에서 일반 C++ 및 Unreal track의 성공률·토큰·시간·누락을 각각 보고한다.

각 task는 다음 네 조건으로 실행한다.

1. Baseline file tool
2. Retrieval only
3. Retrieval + managed instruction
4. Retrieval + instruction + hook

- 공통 실행 가능한 task에는 Graft 0.18.0 retrieval 비교 조건을 추가한다. 동일 file tool 접근권, model, prompt, snapshot, scope, context budget과 deep 설정을 사용한다. Graft 실행 불가 또는 미지원 Unreal 기능은 사유와 적용 범위를 보고하고 공통 비교 결과에 섞지 않는다.
- 같은 model/version, reasoning setting, prompt와 snapshot을 사용한다.
- 독립 clean worktree와 새 session을 사용하고 조건 순서를 무작위화한다.
- 최소 3회 실행하고 결과가 갈리거나 변동계수가 20%를 넘으면 5회로 늘린다.
- task, 반복 번호와 조건을 사전에 대응시켜 paired run을 구성한다. 반복 실행을 독립 task 표본처럼 세지 않고 task별 결과와 task 단위 집계의 신뢰구간을 보고한다.
- correctness와 wrong-file edit·필수 sibling 누락은 실패·timeout을 포함한 전체 run에서 집계한다. Baseline 대비 correctness 비열화 gate는 이 고정 평가 set의 관측 성공률 비교이며 일반적인 동등성의 통계적 증명으로 표현하지 않는다. task별 회귀와 불확실성도 함께 보고한다.
- input token은 provider의 실제 usage에서 cached/uncached 입력을 누락·중복 없이 합산하고 tool schema, instruction과 hook context를 포함한다. 양쪽 모두 성공한 paired run의 `treatment input / baseline input` 비율을 task별 중앙값으로 집계한 뒤 task 간 중앙값을 사용한다.
- 최종 선택한 구성은 평가하는 모든 task에 최소 하나의 양쪽 성공 pair가 있어야 token gate를 판정할 수 있다. pair가 없는 task를 제외해 통과시키지 않으며, paired sample 수와 제외된 실패 run은 별도 공개한다.
- Cold indexing 비용은 agent task 시간과 분리한다.
- 작업별 input/output/cache token, 실제 비용과 wall-clock을 함께 비교한다. Cold indexing과 증분 갱신 비용을 포함한 반복 사용 시나리오도 별도 보고한다.
- Deep 효과는 일부 task의 secondary experiment로 측정한다.
- Test, expected symbol set과 blinded human review를 우선하고 LLM judge만으로 성공을 판정하지 않는다.
- Correctness, 실제 token, tool call, wall-clock, wrong-file edit, sibling 누락과 불필요한 hook context를 기록한다.

## 13. 출시 gate

첫 공개 릴리스는 다음 gate를 모두 통과해야 한다.

### 13.1 기능 gate

- 필수 CLI와 MCP surface가 구현되어 있다.
- Windows C++/Unreal synthetic fixture의 필수 관계가 동작한다.
- Unreal이 설치되지 않은 Windows runner에서 일반 C++의 compilation database 기반 초기 build, CLI/MCP 검색, 호출 탐색, 증분 갱신과 init/remove가 통과한다.
- 일반 C++의 compile context 누락 시 구조·텍스트 fallback과 제한 표시가 동작하며 UBT/UHT를 실행하거나 Unreal 설치를 요구하지 않는다.
- Unreal runner에서는 동일 공통 기능과 UBT/UHT·reflection·RPC·delegate 전용 검증이 통과한다.
- init/remove와 지원 host 연결이 idempotent하고 사용자 설정을 보존한다.
- visualization과 offline HTML이 scope/security 계약을 지킨다.
- 구조 graph는 deep/provider 없이 완전히 사용할 수 있다.

### 13.2 품질 gate

- 12.2~12.6절의 graph, retrieval, incremental, deep와 agent 검증 기준을 통과한다.
- Graft 공통 C++ query 품질이 Graft보다 낮지 않다.
- 동일 input 3회 fact hash가 일치한다.
- 불확실한 target의 confirmed 오승격이 없다.

### 13.3 Agent 가치 gate

- Baseline 대비 correctness가 낮아지지 않는다.
- 12.6절의 paired 성공 run 집계에서 대표 task의 실제 input token 비율 중앙값이 0.70 이하다. 전체 run correctness와 task별 paired sample 조건도 충족한다.
- 출시 기본 구성은 A/B의 retrieval only, instruction 추가, hook 추가 결과를 비교해 선택하고 token 수치와 함께 실제 비용·wall-clock 및 task별 회귀를 검토한다.
- Wrong-file edit와 필수 sibling 누락이 증가하지 않는다.

### 13.4 배포 gate

- `@livecodemap` npm organization scope와 소유권이 확보되어 있다.
- Tag 기반 CI의 build/test/install/MCP/init/remove smoke test가 통과한다.
- npm provenance, checksum, SBOM과 third-party notice가 생성된다.
- Package allowlist 검사에서 private source, credential과 absolute path가 발견되지 않는다.
- 비공개 로컬 검증 정보가 어떤 공개 artifact에도 포함되지 않는다.
- Apache-2.0, contribution policy와 third-party attribution이 준비되어 있다.

## 14. 구현 단계

단계는 기능 의존성을 나타내며 일정 약속은 아니다.

### Phase 1 — 구조 IR

- 일반 C++ compilation database와 Unreal UBT/UHT 입력을 공통 compile context로 연결하는 adapter
- Stable symbol ID
- Path/span/signature/body hash
- Contains/calls/imports/references/extends와 Unreal 최소 relation
- Evidence, confidence와 unresolved site
- Language analyzer interface

완료 조건: Unreal 미설치 환경의 Synthetic C++ fixture와 Unreal 환경의 Synthetic Unreal fixture에서 공통 C++ graph·stable identity 및 입력 adapter의 기본 경로를 검증한다.

### Phase 2 — 증분 index와 freshness

- SQLite fact store와 atomic contribution 교체
- Content hash, stat fast path와 reverse include invalidation
- Reader/writer locking과 worktree seed
- 계층별 freshness와 stale 정책

완료 조건: mutation gold set, crash safety와 freshness latency 검증.

### Phase 3 — 검색과 목적별 도구

- Ranking projection
- CLI/MCP common response
- Find/file/call/map/blast 도구
- Pagination, snippet과 class card
- 최소 Agent A/B harness, baseline/retrieval only와 공통 task의 Graft 비교

완료 조건: 개발 set 기준 identifier/path retrieval 검증과 tool contract test 통과. Navigation/Impact/Patch 대표 개발 task의 초기 A/B에서 정확도, input token, wall-clock과 누락을 검토하고 후속 개선 우선순위를 정한다.

### Phase 4 — Graft 대비 C++/Unreal 검색 품질

- Graft 공통 query와 C++/Unreal 전용 query의 분리 평가
- Identifier/path-aware field/ranking과 일반 자연어 검색
- 초기 Agent A/B에서 확인한 검색 실패·context 누락 개선
- Workspace scope fusion

완료 조건: 개발 set에서 retrieval 기준과 Graft 비교 검증을 수행하고 초기 Agent A/B를 재평가한다. 최종 holdout 평가는 Phase 6에서 수행한다.

### Phase 5 — 선택적 의미 계층

- Symbol-window deep input
- Provider-independent content cache
- Partial failure와 usage accounting
- Cached summary ranking/detail integration

완료 조건: zero-token unchanged rerun과 deep 비용/상태 검증.

### Phase 6 — Agent·visualization·배포

- Project-local init/remove와 host adapter
- Fail-open hook와 status
- Local viewer/offline HTML
- npm/ZIP/`.mcpb` release pipeline
- Agent A/B harness의 instruction/hook 조건 확장과 전체 task·holdout 평가

완료 조건: agent 가치 gate와 전체 release gate 통과.

## 15. 위험과 대응

| 위험 | 영향 | 대응 |
| --- | --- | --- |
| 일반 C++ compilation database/option 불일치 | 잘못된 profile 분석 또는 일부 TU 누락 | 명시적 database/profile 선택, toolchain 호환 범위와 fallback 검증 |
| Unreal/UBT version별 compile context 차이 | Semantic 분석 누락 또는 build 실패 | Version adapter, artifact 우선순위, degraded/required 상태를 명시 |
| Large header fan-out | Query 지연 | 2초 budget 후 stale 반환, explicit build 안내 |
| UHT/unity generated noise | 검색 정밀도 저하 | Provenance 기반 hidden layer와 원본 source 재귀속 |
| Virtual/delegate over-approximation | 잘못된 impact | Confirmed/possible/unresolved 분리, query-time expansion |
| 대규모 graph 저장/조회 병목 | Build와 traversal 목표 실패 | SQLite normalized store, projection 분리, budget과 재개 가능한 traversal |
| 검색 제외 dependency 변경 누락 | 오래된 의미 분석을 최신으로 오인 | 출력 ignore와 의존성 변경 감시 분리 |
| 이전 span과 현재 source 불일치 | 다른 코드를 snippet으로 제공 | Source hash 대조, 검증된 재매핑 또는 snippet 보류 |
| 실제 agent 가치 검증 지연 | 유용성이 낮은 기능에 구현 비용 집중 | Phase 3 최소 A/B와 단계별 재평가 |
| Hook context 과주입 | Token 절감 상쇄 | 명확한 intent gate, 1,500-token/2초 budget, A/B 측정 |
| Deep provider 실패/비용 | 부분 기능 중단 | 구조 독립성, content cache, 부분 상태, 실제 usage 기록 |
| Agent 설정 손상 | 사용자 신뢰 손실 | Project-local 기본값, dry-run, ownership manifest, conflict preserve |
| 비공개 로컬 검증 정보 유출 | 보안/법적 문제 | 공개/local-only 경계, ignore 규칙, package allowlist와 release 검사 |
| Unsigned Windows binary 경고 | 설치 마찰 | 상태 공개, checksum/provenance 제공, 추후 signing identity 도입 |

## 16. 의존성과 선행조건

- Windows 11 지원 환경
- C++ compiler semantic 분석을 위한 LLVM/Clang 계층
- 일반 C++ semantic 분석 시 선택 profile의 compilation database와 해당 toolchain의 SDK/include/generated source 접근
- Unreal semantic 분석 시 대상 Unreal Engine 및 UBT/UHT 접근, 선택 profile의 response/dependency artifact 또는 UBT action 획득 가능성
- 일반 C++는 Unreal Engine에 의존하지 않으며 compile context 없는 구조·텍스트 탐색은 FR-BLD-005를 따른다.
- SQLite
- Node/npm은 npm launcher 설치 경로에서 필요
- Claude Desktop `.mcpb` 형식과 각 agent host의 현재 project-local 설정/hook 계약
- Deep 사용 시 사용자가 선택한 외부, 사내 또는 local LLM provider
- Release 전에 `@livecodemap` npm organization 확보

## 17. 향후 검토 항목

다음은 첫 릴리스 요구사항이 아니며 별도 승인 없이 scope에 추가하지 않는다.

- macOS와 Linux binary
- Blueprint graph/implementation 분석
- C# 또는 다른 언어 analyzer
- Asset/Content/cook 상태 분석
- 일반 dataflow와 field read/write graph
- Claude Desktop official extension directory 등록
- Windows Authenticode signing
- GitHub App 또는 원격 hosted service
- 별도 LLM context graph tab
- 제품 telemetry

## 18. 용어

- **Fact graph**: compiler, parser 또는 UBT/UHT provenance로 확인된 구조 사실의 원본 저장 계층.
- **Ranking projection**: 검색을 위해 fact graph에서 파생한 document와 가중 graph. Source of truth가 아니다.
- **Confirmed**: compiler/구문/명시 설정으로 대상이나 dependency가 확인된 관계.
- **Possible**: virtual dispatch나 runtime binding처럼 실행 가능하지만 특정 실행이 확정되지 않은 관계.
- **Unresolved**: call/reference evidence는 있으나 target을 안전하게 결정할 수 없는 상태.
- **Contribution**: 특정 analysis unit이 fact store에 제공한 교체 가능한 evidence 묶음.
- **Graph generation**: atomic commit된 fact snapshot의 식별자.
- **Class card**: 하나의 논리 class에 대한 여러 물리 위치와 주요 관계를 합친 파생 view.
- **Deep**: 사용자가 명시적으로 실행하는 선택적 LLM 의미 분석.
- **Freshness**: 현재 source/profile과 각 index 계층이 일치하는 정도.
- **Traversal complete**: 요청한 scope의 탐색이 끝났는지 여부. Source freshness 및 모든 결과 page의 반환 완료와는 별개다.
- **External boundary**: 분석 root 밖 dependency, Blueprint 구현 또는 asset처럼 LiveCodeMap가 내부 graph를 제공하지 않는 경계.

## 19. 요구사항 변경 원칙

- 이 PRD의 수치 gate를 완화하려면 benchmark 근거와 명시적 승인이 필요하다.
- First-release scope를 넓히는 기능은 핵심 코드 탐색 가치와 기존 성능/정확도 gate에 미치는 영향을 먼저 검토해야 한다.
- 구현 중 발견한 engine/toolchain 제약은 기능을 조용히 생략하는 근거가 될 수 없다. `partial`, `stale`, `missing`, `failed` 중 적절한 상태와 원인을 사용자에게 표시해야 한다.
- 상충하는 요구사항이 발견되면 Graft 벤치마킹과 C++/Unreal 특화라는 제품 목적 안에서 정확성, 사용자 명시성, 성능 순으로 판단하되 문서를 갱신하기 전에 충돌을 명시적으로 검토한다.

### 19.1 0.2 검토 반영 — 2026-09-17

- 사용자 검토에 따라 Graft를 벤치마킹한 C++/Unreal 특화 도구라는 제품 목적을 우선 기준으로 명시했다.
- 한국어 전용 구현·품질 gate와 외부 전송 규약을 제거했다.
- Agent A/B를 Phase 3으로 앞당기고 후속 단계에서 확대한다.
- Ignore와 의존성 변경 감시를 분리하고 source hash 기반 snippet 정합성을 요구한다.
- Graph fact 보존을 유지하면서 budget·cursor 기반 점진적 traversal을 허용한다.
- Hit@5/Recall@5, candidate 잡음, holdout과 paired Agent A/B 집계 기준을 분리했다. 이 절의 변경은 기존 0.1 기준에 대한 사용자의 수정 지시를 반영한다.

### 19.2 0.3 검토 반영 — 2026-09-17

- 사용자 요청에 따라 일반 C++와 Unreal Engine C++를 모두 첫 릴리스의 지원 대상으로 명시했다.
- 일반 C++ compilation database 경로와 Unreal UBT/UHT 경로를 분리하고 공통 분석·검색 계층을 공유한다.
- Compile context 없는 fallback, 일반 C++의 UHT 비적용 상태와 database/profile 변경 감시를 명시했다.
- Unreal 미설치 Windows의 일반 C++ 검증과 Unreal 설치 환경의 전용 검증을 각각 출시 기준에 포함했다.
