# 병렬 빌드와 추가 TU 검증 실행 기록

2026-09-25, Asia/Seoul. [인계 계획](2026-09-25-Next-Stage-Handoff.md)의 병렬 빌드 진단·우회 검증과 단계 5의 두 추가 TU 검증을 완료했다. 두 TU 각각의 첫/반복 실행은 Sol/high 독립 검수에서 **기존 입력 스냅샷 범위로 승인**됐다. 전체 Phase 1, UE reflection, 현재 UBT/UHT 최신성 또는 MCP 출시 승인을 뜻하지 않는다.

## 역할과 소유권

사용자가 요청한 최근 Herdr 방식으로 현재 탭 `w8:tA`에 새 팀을 구성했다.

| 역할 | 대상 | 소유 범위 |
|---|---|---|
| 조율 | `lcm-coordinator`, `w8:pJ` | 계약, 입력 선택, 해시 고정, 통합 재검증, 이 문서 |
| 구현 | `lcm-implementer`, Claude, `w8:pK` | 빌드 진단, 승인된 제품/테스트 변경, 기존 `build/vs2022-x64` |
| 독립 검수 | `lcm-reviewer`, Codex GPT-6-Sol/high, `w8:pM` | 원본 코드/입력 기반 독립 oracle 및 별도 검증 |

제품 편집자는 한 명이다. 검수자에게 구현자의 설명이나 이전 판정을 전달하지 않는다. 검수 동안 제품 해시를 고정한다. 기존 에이전트와 증거는 보존한다.

증거 디렉터리: `build/coordinator-next-20260925/`, `build/implementer-next-20260925/`, `build/reviewer-next-20260925/`. 시작 시 기존 승인 목록 78개 제품 파일의 SHA-256이 모두 일치했다. 대부분의 제품 파일은 Git 미추적 상태이므로 diff만으로 변경 여부를 판단하지 않는다.

## 순서와 통과 기준

1. 병렬 빌드 진단: 같은 소스에서 Release 전체 빌드의 작업자 1/2/4를 비교하고 실제 실패 위치와 환경을 기록한다. 필요한 최소 수정 또는 환경 우회 후 단일 및 병렬 4 빌드 종료 코드 0, CTest 5/5를 확인한다.
2. 진단 결과를 기록한 뒤 단계 5를 활성화한다. 원본 입력과 소스 oracle을 먼저 고정하고 시스템/태스크 TU, reflected-class TU를 순서대로 검증한다.
3. 각 TU의 원본 compiler/cwd/profile/rsp와 파일 해시, 호출/기반/include 출처와 위치, unresolved/진단/제한, 첫 실행 및 반복 시간과 peak memory를 기록한다. 독립 검수 후 조율자가 재실행한다.

선정 입력은 실제 per-file response가 존재하는 `TPLandEnterTask_ConnectSocket.cpp`와 `TPGuildMemberListPanelUWC.cpp`다. 후자는 생성 헤더와 세 직접 기반을 가진 클래스다. 최종 입력 계약과 결과는 아래에 기록했다.

기존 입력 스냅샷 재생과 현재 프로젝트 최신성은 구분한다. OS 캐시를 통제하지 않으며 cold benchmark로 부르지 않는다. 일반 C++ AST 성공을 UE reflection 의미 지원 또는 전체 Phase 1/MCP 승인으로 확대하지 않는다.

## 병렬 빌드 진단

구현자가 같은 소스에서 일반 실행 환경의 작업자 1/2/4 및 `/nr:false` 병렬 4를 모두 종료 코드 0으로 확인했다. `codex sandbox --permission-profile :workspace`에서는 단일 빌드가 0, 병렬 4가 1이다. MSBuild 통신 로그에는 기존 노드와 방금 생성한 자식 노드 모두에 대해 named pipe 연결 시 접근 거부가 기록됐다. 자식 노드는 연결 대기 중이므로 컴파일 오류나 `ZERO_CHECK` 오류로 단정하지 않는다. 정확한 pipe DACL 내부 원인은 조사하지 않았다.

최소 재현은 `build/implementer-next-20260925/sandbox-repro.ps1`과 `comm-sbx-p4/MSBuild_CommTrace_PID_38508.txt`에 보존했다. 새 자식 노드 PID 10048의 연결도 거부된다. 병렬 실행 실패가 다수의 대기 노드를 남기므로 반복 실패 실행을 중단했다. 다른 프로세스를 종료하지 않았다.

운영 우회는 샌드박스에서 `--parallel 1`을 사용하거나 병렬 빌드를 승인된 호스트 환경에서 실행하는 것이다. 제품/CMake 변경은 필요하지 않았다. 구현자의 Release CTest는 5/5다. 조율자의 같은 소스 호스트 전체 빌드도 단일·병렬 4 모두 종료 코드 0이며, 별도 CTest도 5/5, 종료 코드 0(58.85초)을 확인했다. 현재 빌드는 기존 트리 증분 빌드이며 깨끗한 전체 재컴파일의 경쟁 조건까지 입증하지 않는다.

## 단계 5 활성화

빌드 진단과 우회 검증을 기록한 뒤 단계 5를 활성화했다. 선택 입력 13개의 초기 해시와 실제 compiler 파일 버전 `19.44.35227.0`은 `build/coordinator-next-20260925/selected-inputs-before.json`에 있다. 원래 compiler 설치 경로의 디렉터리 버전과 바이너리 버전은 구별한다.

기존 replay 보고서는 `FileObservation` 개수만 내보내므로 실제 컴파일러가 본 파일별 해시를 기록하는 최소 JSON 내보내기를 구현자에게 배정했다. 분석 의미 변경은 배정하지 않았다. 제품 수정 후 고정·독립 검수를 거쳐 실제 TU를 실행한다.

제품 수정은 `tools/lcm_replay/main.cpp`에 32줄을 추가한 것뿐이다. 기존 `FileObservation`와 `IncludeObservation`을 각각 내보내며 디스크를 다시 읽어 해시를 만들지 않는다. 후자는 클래스 헤더에서 생성 헤더로의 실제 해석 경로를 검증하기 위해 필요하다. 구현자 회귀 23개와 수정 후 Release CTest 5/5가 통과했다. 제품 78개 중 이 파일만 변경된 것을 조율자가 확인하고 `stage5-frozen-hashes.json`에 고정했다. 사용자 요청으로 실제 코드 검수는 GPT-6-Sol/high로 전환했으며 독립 승인됐다.

수정된 파일의 실제 재컴파일 중 노드를 재사용한 호스트 빌드에서 추가로 `MSB6001`, 환경 변수 키 `Path`/`PATH` 중복 오류가 발생했다. `/nr:false`로 새 노드를 사용한 병렬 4 빌드에서는 실제 컴파일과 링크까지 통과했다. 기존 샌드박스 노드의 환경이 원인이라는 설명은 아직 가설이다. 원시 실패와 성공 로그를 모두 보존하고, 이후 병렬 검증에는 호스트 실행 및 노드 재사용 금지를 사용한다.

독립 소스 oracle은 `build/reviewer-next-20260925/stage5-source-oracle.json`이다. 조율자는 원본 바이트/위치/해시 279개를 대조했다. 대상 직접 기반은 총 4개, 대표 호출은 13개이며, `.cpp` 직접 include는 태스크 13개·위젯 16개다. 이 수치는 소스 기대값이며 실제 replay 통과 결과가 아니다.

Sol/high 검수는 보고서 변경을 한정 승인했다. 별도 새 Release 빌드와 정상/누락 include/위험 인자 거부 사례가 통과했고 고정 해시 78개는 그대로다. 실제 재생에는 검수자의 별도 실행 파일(SHA-256 `5e0f7179dd2b7b05348ed7bdb635493b23b3c5310fd94cc90b5206a9980b264c`)을 사용한다. 상세: `build/reviewer-next-20260925/stage5-cli-review.md`.

## 입력 출처와 최신성 경계

- 태스크 TU: Development `Makefile.bin`의 원본 액션이 선택된 소스·응답 파일과 MSVC compiler FileItem 참조를 연결한다. `ActionHistory.bin`의 속성 해시도 compiler 경로·응답 경로·버전 조합과 일치한다. 하지만 Development 오브젝트/진단/의존성 파일이 없어 그 액션이 실제 컴파일을 완료한 이력은 입증하지 않는다. 작업 디렉터리는 UBT의 `WorkingDirectory => Unreal.EngineSourceDirectory` 계약으로 확인하며, 같은 TU의 DebugGame CodeView는 별도 프로필의 보조 증거로만 사용한다.
- 위젯 TU: 기존 Development 오브젝트의 CodeView에 compiler·cwd·원본 소스가 남아 있다. 현재 Development makefile에서는 이 소스가 `Module.Temppal.107.cpp` unity 입력으로 들어간다. 이번에 선택한 개별 TU 응답 파일은 과거 adaptive non-unity 입력이며 현재 빌드 액션이라고 부르지 않는다.
- 두 TU 모두 원래 compiler 경로와 Engine/Source cwd를 유지한다. 현재 shared rsp는 위젯 오브젝트보다 새롭고, 태스크 소스는 Development makefile보다 새롭다. 기존 입력 스냅샷 재생의 결과가 UBT/UHT 최신성이나 과거 오브젝트와의 동등성을 증명하지 않는다. UBT/UHT 또는 Temppal 재빌드는 실행하지 않았다.

원본 메타데이터 해시·필드 위치·추출 스크립트는 `build/implementer-next-20260925/provenance/`에 보존했다.

## 실제 재생 결과

태스크 첫 실행은 검수자 빌드로 종료 코드 0, `status=ok`, `context_complete=true`, 거부 인자 0, error/fatal 0이다. wall 343.194초, peak working set 2,498,879,488바이트, peak commit 2,683,428,864바이트를 Windows 프로세스 수명 peak 카운터로 측정했다. 종료 후 카운터 조회도 성공했다. OS 캐시는 통제하지 않았다.

기반 1개·선택 호출 7개·main include 13개의 원본 대조가 일치한다. 선택 main-file unresolved는 0이며, 전체 헤더를 포함한 TU unresolved는 25,316개, 제한 기록은 1,846개다. warning 375개와 note 372개를 보존한다. 전처리 prefix의 생성·소비·최종 비교에는 mismatch/drift가 없다.

파일 관측 항목은 2,545개이며 파일/응답/선정 입력의 현재 디스크 바이트 대조 2,560개가 통과했다. 같은 경로의 여러 관측이 있으므로 고유 파일 수와 구별한다. 비어 있는 synthetic PCH guard는 별도로 확인했다. 독립 verifier의 모든 includer가 절대 경로여야 한다는 가정 때문에 강제 include 3개가 표시됐다. 검수자의 별도 native-cl `/FI` fixture에서도 빈 includer/null owner가 재현됐다. 검수자는 정확한 명령의 강제 입력·해석 대상·null owner가 일치하는 경우만 허용하도록 verifier를 수정했다. 최초 실패 자료는 보존했으며 제품 수정은 없었다.

태스크 반복은 종료 코드 0, wall 343.584초, peak working set 2,501,632,000바이트, peak commit 2,683,490,304바이트다. 두 보고서의 `elapsed_ms`를 제외한 전체 필드(식별값·전체 구조 증거·파일/include 관측·진단·제한 포함)가 일치했다. 수정된 독립 oracle은 두 보고서 모두 실패 0이며, 반복 보고서의 디스크 바이트 대조 2,560개도 통과했다. 위젯 첫 실행을 같은 검수자 실행 파일로 시작했다.

태스크 쌍은 Sol/high 검수자의 `task-pair-assessment.md`에서 기존 입력 스냅샷 범위로 승인됐다. 위젯 첫 실행은 종료 코드 0, wall 358.759초, peak working set 2,577,174,528바이트, peak commit 2,756,378,624바이트다. 기반 3개·대표 호출 6개·main include 16개와 생성 헤더 관측이 oracle을 통과했고, 디스크 바이트 대조 2,633개도 통과했다. 위젯 반복도 종료 코드 0, oracle 실패 0, 디스크 바이트 대조 2,633개 통과이며, 시간 외 전체 JSON 필드가 첫 실행과 같다. [최종 독립 검수](../../build/reviewer-next-20260925/stage5-final-review.md)는 두 TU 쌍 모두를 같은 한정 범위로 승인했다.

위젯 첫 보고서는 warning 373개, note 380개, main unresolved 0개, 전체 TU unresolved 26,159개, 제한 1,800개를 기록한다. 선택 생성 헤더의 `#ifdef`(offset 445)와 `#endif`(619)가 dependency scanner 지원 범위 밖이라는 제한 2개가 있다. 이 생성 헤더 내부의 lexical direct-include facts는 게시되지 않는다. 클래스 헤더에서 생성 헤더로의 실제 include 해석과 관측 바이트 검증이 통과한 것과 구별한다.

| 실행 | 종료 코드 | wall (초) | peak working set (GiB) | peak commit (GiB) |
|---|---:|---:|---:|---:|
| 태스크 첫 실행 | 0 | 343.194 | 2.327 | 2.499 |
| 태스크 반복 | 0 | 343.584 | 2.330 | 2.499 |
| 위젯 첫 실행 | 0 | 358.759 | 2.400 | 2.567 |
| 위젯 반복 | 0 | 359.229 | 2.401 | 2.571 |

네 실행 모두 동일한 검수자 binary를 사용했으며 실행 전후 binary/선정 소스/헤더 해시는 같다. 최종 제품 78개와 source oracle 해시도 변하지 않았다. 비교·최종 고정 증거: `task-reproduction-comparison.json`, `widget-reproduction-comparison.json`, `final-freeze-check.json`. process wall과 CLI의 분석 전용 `elapsed_ms`를 혼동하지 않는다.

## 재현 명령과 증거

병렬 빌드는 호스트 환경에서 노드 재사용을 끈다. process 범위 설정이며 전역 환경을 수정하지 않는다.

```powershell
$env:MSBUILDDISABLENODEREUSE='1'
cmake --build build/vs2022-x64 --config Release --parallel 1 -- /v:minimal /nr:false
cmake --build build/vs2022-x64 --config Release --parallel 4 -- /v:minimal /nr:false
ctest --test-dir build/vs2022-x64 -C Release --output-on-failure
```

실제 replay는 다음 스크립트가 고정 manifest를 검사하고 원본 compiler/cwd/consumer·producer rsp를 전달한다. `--unit 0`은 태스크, `--unit 1`은 위젯이다. 태그는 새 값을 사용하며 기존 보고서를 덮어쓰지 않는다. 같은 단위의 반복 실행에는 같은 binary와 입력을 사용한다.

```powershell
python build/coordinator-next-20260925/measure-replay.py --unit 0 --tag task-check-01 --tool D:/Git/LiveCodeMap/build/reviewer-next-20260925/release-independent/tools/lcm_replay/Release/lcm_replay.exe
python build/coordinator-next-20260925/check-inputs.py build/coordinator-next-20260925/task-check-01.json --out build/coordinator-next-20260925/task-check-01-inputs.json
python build/reviewer-next-20260925/verify_report.py --unit task --report build/coordinator-next-20260925/task-check-01.json --output build/reviewer-next-20260925/task-check-01-oracle.json
```

전체 응답·파일 해시와 argv는 각 replay JSON 및 `*-metrics.json`에 있다. 첫/반복 결과 비교는 `compare-replays.py`로 `elapsed_ms`를 제외한 모든 필드를 확인한다. source oracle 검증 성공과 별도로 diagnostics/limits 및 현재 build context의 최신성을 검토해야 한다.

다음 저장·교체·최신성 구현 계약은 [Multi-TU 인계](2026-09-25-Multi-TU-Index-Handoff.md)에 정리했다. 이번에는 SQLite 저장소/검색/MCP를 구현하지 않았다.
