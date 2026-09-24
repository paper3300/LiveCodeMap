# Temppal 단일 TU 재생 작업 계획

작성: 2026-09-23, Asia/Seoul

상태: **단계 0–4 완료·승인**, **2026-09-25 단계 5의 두 추가 TU 검증 완료·한정 승인**. 단계 0–4 결과는 [기존 실행 결과](2026-09-23-Temppal-Replay-Result.md), 단계 5의 태스크/생성 헤더 클래스 각 2회 재생·원본 oracle·Sol/high 독립 검수는 [새 실행 기록](2026-09-25-Next-Stage-Execution.md)에 정리했다. 승인은 기존 입력 스냅샷과 선정한 사실에 한정하며 현재 UBT/UHT 최신성, 전체 UE reflection 또는 Phase 1/MCP 승인이 아니다. 아래 체크포인트는 이전 미승인 제출본을 포함한 이력이다.

## 목표와 범위

TemppalEditor / Win64 / Development / x64의 `TPGuildSystem.cpp` 하나를 실제 빌드 정보에서 출발해 공개 분석 API로 처리하고, 원본 소스와 대조 가능한 결과를 반복 생성한다. 현재 Development 산출물의 최신성은 미검증이므로, 기존 입력 스냅샷 재생 성공과 현재 프로젝트 빌드 컨텍스트 검증을 별도 결과로 기록한다.

이번 범위는 입력 연결, response-file 확장, 제한된 PCH 재생 정책, 확인된 인자 처리, 단일 TU 검증 실행 도구다. 전체 인덱싱, 저장소, MCP/UI, UE 리플렉션 전용 의미 분석, skipped-callee 수정은 별도 작업으로 유지한다. Temppal·Engine은 읽기 입력이며, UBT/UHT 실행이나 프로젝트 재빌드가 필요하면 작업 범위와 생성 위치를 먼저 구체화한다.

## 근거와 보완할 증거

- `build/vs2022-x64/Testing/Temporary/LastTest.log`: Release CTest 5/5 통과. Core 106/2,482, analyzer 28/2,759, smoke 8/67.
- `build/temppal-probe/RESULTS.md`: 원래 response 입력은 거부되고, 수동 적응을 거치면 response/PCH/인자/PCH 입력 문제가 순차적으로 드러남. 최종 진단 재생은 44.063초, 오류 0, 심볼 42,781, 관계 50,276.
- `build/temppal-probe/main.cpp`: 모든 재생에 `clang-cl.exe`를 직접 지정하며, 거부 인자와 SharedPCH 입력을 제거한 명령을 다시 분석한다. 이 결과는 원래 컴파일러 식별 및 컨텍스트 동등성을 증명하지 않는다.
- 현재 저장된 보고서에는 거부된 인자 3개의 실제 형태와 단계별 전체 원시 출력이 없다. 구현 전에 확보해야 한다. SharedPCH 입력의 실제 파일 종류와 참조 연결도 재확인한다.
- 기존 테스트는 response/PCH의 거부를 의도된 계약으로 검증한다. 지원 범위를 확장할 때 지원하는 경우와 계속 거부해야 하는 경우를 명시적으로 분리한다.

## Herdr 역할과 소유권

사용자 요청에 따라 새 세션을 구성했다. 조율자는 현재 대화의 에이전트이며, 구현자와 검수자는 새 세션이다. 단계 0 조사와 독립 경계 fixture 설계 프롬프트를 전송하고 두 작업자의 working 상태를 확인했다.

| 역할 | Herdr 대상 | 소유 범위 | 제출물 |
|---|---|---|---|
| 조정 | `lcm-coordinator` (`w8:pE`) | 계약, 입력 스냅샷, 작업 순서, 통합 판정, 계획/진행 기록 | 단계별 기준과 증거 목록 |
| 구현 | `lcm-implementer` (`w8:pF`, Claude) | 제품 코드, 테스트, 실행 도구, 구현 문서 | 변경 파일, 검증 명령, 결과 |
| 독립 검증 | `lcm-reviewer` (`w8:pG`, GPT-5.6-sol/high) | 고정된 코드 검토, 별도 fixture/oracle, 재현 검사 | 재현 가능한 결함 또는 범위가 명시된 승인 |

실행 직전에 이름·pane·cwd·진행 상태를 다시 조회한다. 공유 작업 폴더의 제품 파일은 구현자 한 명이 편집한다. 리뷰어에는 조정자가 확정한 요구사항과 고정된 코드/테스트/입력만 전달하고 구현자의 설명·결론을 전달하지 않는다. 리뷰 중 제품 코드를 고정하고 독립 빌드·출력 경로를 사용한다.

## 단계별 작업과 통과 기준

### 0. 재현 입력과 계약 확정 — coordinator + implementer

- 선택 TU, 실제 원래 compiler 경로/종류, 작업 디렉터리, profile, 최상위·중첩 rsp, forced includes, 생성 헤더 및 PCH 참조를 수집한다.
- 실제 읽은 입력의 내용 해시와 출처를 기록한다. 타임스탬프만으로 최신성이나 같은 입력임을 판정하지 않는다.
- 단계별 원시 출력과 거부 인자 3개의 정확한 토큰·소비 관계를 보존한다. positional 토큰이 미인식 옵션의 값인지 별도 소스인지 확인한다.
- SharedPCH forced input과 `/Yu`, `/Fp`가 각각 무엇을 가리키는지 확인한다. 기존 probe의 nested-rsp 상대경로 기준도 원래 driver/빌드 생성 코드와 대조한다.
- 통과: 같은 입력으로 차단 원인을 재현하고 compiler/cwd/profile/인자/PCH의 출처가 설명된다. 확인되지 않은 값은 명시하며 추측으로 채우지 않는다.

### 1. response-file 확장과 입력 식별 — implementer

- raw command를 보존하고 제품 분석 경로에서 중첩 rsp를 확장한다. 토큰마다 원본 파일·순서와 변환 출처를 추적한다.
- 원래 driver에 맞는 인코딩·따옴표·상대경로 규칙을 적용한다. 지원하는 driver/형식을 좁게 명시한다.
- 순환, 누락, 비정상 인코딩/따옴표, 자원 한도를 넘어선 입력을 진단한다. 동일 파일의 합법적인 반복 참조와 순환을 구별한다.
- 명령 식별과 재생 컨텍스트 식별을 구분할지 확정하고, 중첩 rsp의 내용 변경이 재생 식별에 반영되도록 한다. 분석에 사용한 바이트와 해시가 일치해야 한다.
- 통과: 중첩·공백/한글 경로·인코딩·반복·순환·파일 누락·내용 변경 회귀를 통과한다. 확장된 위험 인자, 추가 입력, 중복 소스가 기존 gate를 우회하지 않는다. 실제 TU는 다음의 명시된 PCH 차단까지 진행한다.

### 2. 제한된 UE/MSVC PCH 재생 — implementer, 계약은 coordinator 확정

- 기본 후보는 원본 PCH 생성 입력을 확인한 뒤 텍스트 헤더를 재생하는 방식이다. 입력 파일 종류와 생성 규칙을 확인한 후 최종 방식을 선택한다.
- `/Yu`, `/Fp`, SharedPCH forced include를 함께 다루고, macro/undefine/include 순서 및 필요한 생성 헤더를 유지한다.
- 검증 가능한 경우에만 지원한다. 원본 헤더나 의미상 필요한 상태를 복원할 수 없으면 구체적인 이유와 함께 계속 거부한다.
- 통과: PCH가 제공하는 선언·매크로·조건부 include에 실제 의존하는 fixture에서 검증된 텍스트 기준과 구조적 결과가 일치한다. 필요한 입력 누락과 미지원 PCH는 거부된다. `degraded` 검사 제거만으로 통과시키지 않는다.

### 3. 확인된 인자 3개 처리 — implementer

- 단계 0에서 확인한 각 형태를 전달/정규화/파싱 의미에 무관하여 제거/계속 거부 중 하나로 분류하고 근거를 기록한다.
- 미인식 옵션의 분리된 값이면 옵션·값을 함께 처리한다. 알 수 없는 토큰을 일괄 제거하거나 옵션 family 전체를 허용하지 않는다.
- 통과: 실제 형태에 대한 양성 fixture와 유사 철자·잘못된 값·추가 소스·driver 차이에 대한 음성 fixture가 모두 통과한다.

단계 2와 3의 조사는 단계 1과 독립적으로 진행할 수 있다. 제품 편집은 순차 수행하며, reviewer는 확정된 요구사항으로 독립 fixture를 병행 준비할 수 있다.

### 4. 최소 실행 도구와 독립 통합 검증 — implementer → reviewer → coordinator

- 기존 probe의 단일 TU 실행 용도를 정식 재현 가능한 도구/스크립트로 정리한다. 공개 분석 API를 호출하고 원본 입력, 적용 변환, 식별값, 상태, 진단, 제한, 구조 검증 결과, 소요 시간을 저장한다.
- 입력별 수동 토큰 삭제와 임의의 compiler 이름 교체를 제거한다. 실패는 자동 검사에서 구별되는 종료 코드로 반환한다.
- 고정된 소스와 라이브러리의 식별을 기록하고 reviewer가 별도 빌드에서 검증한다. 조정자는 제출된 재현 명령으로 최종 결과를 확인한다.
- 통과: 동일 TU가 `status == ok`, `context_complete == true`, rejected arguments 0, error 0으로 완료된다. 이 상태와 별도로 원래 입력의 profile/latestness 검증 여부를 표시한다.
- 구조 기준: 대상 타입의 직접 기반 2개, virtual base 1개, 기반 식별·순서·접근 지정·원본 소스 위치를 확인한다. 대표 호출과 직접 include를 원본과 대조하고 관련 unresolved를 검토한다.
- 진단 재생의 42,781/50,276 등의 총량은 비교 자료이며 정확성의 고정 기대값이 아니다. PCH 문맥 복원으로 늘거나 줄 수 있고 그 차이를 설명해야 한다.
- 회귀: 영향받는 core/analyzer/normalizer 테스트와 Release CTest를 수행한다. core 인터페이스 변경 시 Clang-disabled 빌드를 확인하며, 다른 구성은 변경 위험이나 새 실패가 있을 때 추가한다.

### 5. 다음 검증 범위 — 단계 4 승인 후 별도 배정

- 시스템/태스크 호출 TU, 생성 헤더가 있는 reflected-class TU로 순차 확대한다. 일반 C++ AST 분석 성공과 UE reflection 의미 지원을 구별한다.
- 같은 고정 입력으로 첫 실행/반복 실행 시간과 peak memory를 측정한다. OS 캐시 조건을 기록하며 통제하지 않은 첫 실행을 cold로 단정하지 않는다.
- 최신 profile 재생이 필요하면 기존 산출물의 불일치를 먼저 구체화하고 필요한 UBT/UHT/빌드 범위를 정의한다.
- 전체 인덱싱·unity 매핑·증분 분석의 작업 계획은 이 증거를 받은 뒤 확정한다.

## 첫 배정

`lcm-implementer`에게 단계 0의 한 TU 입력 조사와 재현 증거 확보를 배정한다. 조정자가 원래 driver/identity/PCH 계약을 고정한 후 단계 1 구현으로 이어간다. Reviewer는 그 계약의 독립 음성·경계 fixture를 준비한다. 완료 기준은 진단 probe 성공이 아니라 단계 4의 제품 경로 승인이다.

## 2026-09-23 실행 체크포인트

- 단계 0 차단 재현을 확보하고 단계 1 및 단계 3의 제한된 구현을 배정했다. 제품 PCH 지원은 별도 계약 확정 전까지 기존 거부를 유지한다.
- 원본 compiler 경로와 cwd는 기존 오브젝트의 CodeView 기록에서 확보했다. 현재 중첩 응답 파일에는 과거 오브젝트 기록보다 include 경로 7개가 더 있어, 현재 스냅샷 재생과 역사적 빌드 동등성을 구분한다. 컴파일러 설치 디렉터리명은 실제 바이너리 버전을 증명하지 않는다.
- 독립 native-cl 실험에 따라 응답 파일 지원은 native cl, cwd 기준 중첩 경로, BOM 없는 ASCII / BOM 있는 UTF-8 / BOM 있는 UTF-16LE로 한정한다. BOM 없는 비ASCII 입력은 코드페이지를 추측하지 않고 거부한다.
- 원본 command_id와 응답 입력 바이트·확장 순서·출처를 반영한 replay identity를 구분한다. 동일 요청의 반복 참조는 같은 입력 스냅샷을 사용한다. 누락·순환·인코딩 오류·자원 한도와 기존 소스/안전 검사를 유지한다.
- 확인된 거부 토큰은 `/experimental:log`, 그 SARIF 출력 경로, `/d2ExtendedWarningInfo`다. native-cl의 정확한 형식만 출력/진단 인자로 처리하고 유사 옵션 family는 허용하지 않는다.
- 조율 증거: `build/coordinator-replay-20260923/`; 구현 조사: `build/implementer-replay-20260923/`; 독립 검증: `build/reviewer-replay-20260923/`. 아직 제품 경로 승인이나 단계 4 완료 판정은 없다.
- 단계 1/3 최초 구현은 Release CTest 5/5, Clang-disabled core를 통과했고 조율자 재실행도 통과했다. 73개 파일을 고정한 독립 검수는 empty-command-id 재생 식별 충돌, slash-only 예외의 dash 표기 허용, 토큰 한도 0 우회를 재현했다. Windows case-sensitive 경로 혼동은 정적 결함이며 이 환경에서 실행 재현은 불가능했다. 검수 보고서: `build/reviewer-replay-20260923/stage1-3-independent-review.md`. 최초 구현은 미승인이다.
- 검수 종료 후 위 결함 수정 및 단계 2/4 구현을 배정했다. 리뷰어는 제품 코드를 읽지 않고 독립 PCH fixture를 준비한다. 다음 제품 검수는 최종 고정 스냅샷으로 수행한다.
- PCH의 생성·소비 include 목록은 다르지만 독립 Clang 실험의 전처리/매크로/include·pack 결과가 같은 바이트로 나왔다. 이는 진단 증거일 뿐 제품 승인이 아니다. 제품 계약은 명시적 producer 명령, `/Yc`·wrapper-only source·`/Fp`·첫 `/FI == /Yu`의 대응, 동일 semantic options, 고정 입력 버퍼, 순서 있는 PP 이벤트/토큰/매크로/counter/include 비교를 요구한다. 최종 매크로 표만 같은 숨은 스택 반례를 거부해야 한다. 단일 TU 범위이며 cross-TU cache는 구현하지 않는다.
- 단계 1/3의 네 결함 수정과 단계 2/4 최초 구현을 고정하고 독립 최종 검수를 시작했다. 고정 목록은 `build/coordinator-replay-20260923/pch-submitted-hashes.json`이다. 조율자의 별도 identity probe에서 빈 command_id와 서로 다른 cwd의 식별이 수정됐음을 확인했다.
- 최초 실제 제품 실행은 340,075ms이며 **미승인**이다. 생성/소비 prefix는 전처리 이벤트 368,531개, 진입 파일 2,232개, counter 307 및 비교 digest가 일치했지만 최종 분석의 prefix 종료 추적 결함으로 두 번째 forced include를 잘못 거부했다. 동결 중 작은 입력으로 재현했고, 최종 prefix 상태 재검증 및 파일 존재 조회의 스냅샷 보장도 수정 대기 항목이다. 보고서에는 원본 소스 대조에 필요한 기반/호출/include의 상세 증거가 추가로 필요하다.
- 독립 단계 2/4 검수는 최초 제출본을 미승인으로 판정했다. 상대 `/Yu` include 검색 거부, 잘못된 `/Yc` 허용, wrapper 앞 pragma 상태 허용을 재현했고 짧은 의미 옵션·중복 forced include·고정 producer 바이트·producer provenance·CLI 검증 누락도 지적했다. 보고서: `build/reviewer-replay-20260923/final-code-fixture-review.md`. 단계 1/3 기존 네 결함은 독립 재검증을 통과했고 case-sensitive fixture도 실제 조건에서 통과했다. 검수 종료 후 동결을 해제하고 위 결함과 최종 prefix 일관성 및 증거 보완을 구현자에게 배정했다.
- 수정본은 Release CTest 5/5, core 130/2,744, analyzer 45/2,931을 통과했고 다시 고정했다. `build/coordinator-replay-20260923/pch-corrected-hashes.json`의 78개 파일을 대상으로 독립 재검수를 시작했다. 최종 관측기 도입 후 실제 TU에서 발생한 접근 위반은 원시 출력과 함께 보존했으며, 토큰 처리 수정 후 실제 TU 재실행은 아직 판정 중이다.
- 이후 독립 12개 사례에서 unguarded wrapper의 재진입 결함을 재현했다. 실제 TU는 파서 전용 pragma 인자 확장 때문에 최종 이벤트가 54개 더 발생했다. 비교 검사를 제거하는 방향은 승인하지 않았고, 생성·소비 capture도 파서 기반으로 맞춰 일반 토큰 및 모든 이벤트/매크로/파일/counter 비교를 유지했다. annotation은 세 실행에서 동일하게 제외하고 pragma 이벤트는 보존한다. 관측 구간은 첫 wrapper 경계에서 영구 종료한다.
- v3 고정본(`build/coordinator-replay-20260923/pch-v3-hashes.json`, 78개 파일)은 독립 코드/fixture 승인을 받았다. 보고서: `build/reviewer-replay-20260923/v3-independent-approval.md`. 독립 replay 12/12, CLI 실패 10/10, PCH 17 cases/133 assertions, 실제 case-sensitive PCH 12 assertions 및 별도 Release CTest 5/5를 통과했다. 제품 전체 analyzer는 47 cases/2,954 assertions이며 core는 130/2,744다. 이전 Clang-disabled 검증 이후 core 파일은 변경되지 않았다.
- 구현자의 실제 TU v3 실행은 395,731ms, native exit 0, status ok, context complete, 거부 인자 0, 오류 0으로 완료했다. 조율자는 원본 바이트에서 만든 oracle로 366개 검사를 통과했다. 두 기반의 식별·순서·접근·virtual 여부·전체 span, Init의 세 호출과 대상 정의, 24개 include의 원문·순서·span·해결 출처가 일치하며 main-file unresolved는 0이다. 최종 조율자 별도 빌드 재실행은 아직 진행 중이다.
- **최종 완료:** 조율자가 검수자의 별도 Release 실행 파일로 원본 명령을 재실행해 native exit 0, status ok, context complete, 거부 인자 0, 오류/fatal 0 및 원본 대조 366개 통과를 확인했다. 392,503ms 소요. 생성/소비/최종 prefix 이벤트 368,585개, 파일 2,232개, counter 307과 모든 digest가 일치했다. 두 실제 실행의 command/replay/prefix 식별값과 전체 내보낸 구조 증거가 같고, 고정 제품 78개·실행 파일/라이브러리·대상 원본 소스의 해시 변경은 0개다. 보고서: `build/coordinator-replay-20260923/temppal-replay-v3.json`, `final-oracle-verification.json`, `final-reproduction-comparison.json`. 단계 0–4 승인, 단계 5 미착수.

## 원본 입력을 보존한 재현 명령

저장소 루트의 PowerShell에서 실행한다. 아래 경로는 조사한 기존 로컬 profile의 정확한 입력이며, 현재 프로젝트의 최신 빌드를 뜻하지 않는다. 결과 JSON의 상태·context·오류·구조 증거와 프로세스 종료 코드를 함께 확인한다.

```powershell
$compiler = 'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe'
$projectRoot = 'D:/ProjectT_Main/Program/Client'
$producer = "$projectRoot/Temppal/Intermediate/Build/Win64/x64/TemppalEditor/Development/UnrealEd/SharedPCH.UnrealEd.Project.RTTI.ValApi.ValExpApi.Cpp20"
$replayArgs = @(
    '--repo', $projectRoot,
    '--resource-dir', 'D:/Git/LiveCodeMap/build/deps/clang+llvm-22.1.0-x86_64-pc-windows-msvc/lib/clang/22',
    '--directory', "$projectRoot/Engine/Source",
    '--source', "$projectRoot/Temppal/Source/Temppal/Game/System/Guild/TPGuildSystem.cpp",
    '--arg', $compiler,
    '--arg', "@$projectRoot/Temppal/Intermediate/Build/Win64/x64/UnrealEditor/Development/Temppal/TPGuildSystem.cpp.obj.rsp",
    '--producer-source', "$producer.cpp",
    '--producer-arg', $compiler,
    '--producer-arg', "@$producer.h.obj.rsp",
    '--expect-type', 'FTPGuildSystem', '--expect-direct-bases', '2', '--expect-virtual-bases', '1',
    '--expect-direct-includes', '24', '--evidence-limit', '0',
    '--out', 'build/temppal-replay-result.json'
)
& .\build\vs2022-x64\tools\lcm_replay\Release\lcm_replay.exe @replayArgs
$replayExit = $LASTEXITCODE
Write-Output "replay_exit_code=$replayExit"
```
