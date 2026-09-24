# 다음 작업 인계: 병렬 빌드 진단과 Temppal 검증 확대

작성: 2026-09-25, Asia/Seoul. 이 문서는 다음 Context의 작업 계획이다. 이 Context에서는 제품 코드를 수정하지 않았다.

## 확인된 출발점

- `cmake --build build/vs2022-x64 --config Release --parallel 1 -- /v:minimal` 통과. `ctest --test-dir build/vs2022-x64 -C Release --output-on-failure`는 5/5 통과했다.
- 같은 소스에서 `cmake --build build/vs2022-x64 --config Release --parallel 4 -- /v:minimal`은 종료 코드 1을 반복 반환했다. `/nr:false`를 붙여도 같았다. [병렬 빌드 로그](../../build/coordinator-replay-20260923/parallel-build-20260925.log)는 `ALL_BUILD.vcxproj`가 `ZERO_CHECK.vcxproj`를 성공시킨 직후, 경고·오류 0개로 실패했다고만 기록한다. 원인은 미확인이다. 첫 무제한 `--parallel` 시도도 종료 코드 1이었다.
- 새 `lcm_replay` 실행은 종료 코드 0, `status=ok`, `context_complete=true`, 거부 인자 0, 오류/fatal 0으로 완료됐다. [새 보고서](../../build/coordinator-replay-20260923/temppal-replay-current-20260925.json)의 원본 대조 366개가 통과했다. 이전 v3 승인 결과와 command/replay ID, PCH 이벤트·토큰·매크로·파일 digest, 사실 총량, 전체 구조 증거가 같다. 경고 395개와 note 407개는 기존 결과와 같다. 78개 고정 제품 파일의 해시 불일치는 0개다.
- Git에는 초기 커밋만 있고 현재 제품/문서 파일 대부분이 미추적 상태다. 공유 작업 폴더이므로 변경 전 소유권과 파일 상태를 다시 확인한다. 기존 빌드·증거 트리를 삭제하거나 덮어쓰지 않는다.

## 1. 병렬 빌드 실패를 분리하고 수정

1. 현재 프로세스/작업 폴더 상태와 빌드 입력을 기록한다. 위 단일 작업자 명령을 대조군으로 두고, `--parallel 2` 및 `--parallel 4`에서 첫 실패를 재현한다. 진단 출력은 새 로그 파일에 보존한다.
2. `ALL_BUILD`와 개별 타깃(`lcm_replay`, 세 테스트 실행 파일)의 병렬 빌드를 비교한다. MSBuild 상세 로그 또는 binary log에서 실패한 실제 프로젝트/타깃과 반환 코드를 찾는다. 로그가 여전히 조용하면 CMake가 아닌 MSBuild 직접 실행, 빌드 노드/프로세스 상태, 파일 잠금·자원 한도를 차례로 확인한다. 원인을 추정만으로 단정하지 않는다.
3. 기존 `build/vs2022-x64`의 상태 문제인지 판별해야 하면 새 `build/` 하위 디렉터리에 동일한 Visual Studio x64 구성을 만들어 비교한다. 다른 작업자의 프로세스나 기존 산출물을 일괄 종료·정리하지 않는다.
4. 확인된 원인에 필요한 최소 변경만 한다. 통과 기준은 같은 소스에서 단일 작업자와 **병렬 4 작업자 전체 Release 빌드가 모두 종료 코드 0**을 반환하고, Release CTest 5/5가 통과하는 것이다. 환경 문제라면 재현 조건과 운영상 우회책을 문서화한다. 빌드 실패를 테스트 통과로 덮지 않는다.

## 2. 계획 단계 5: 단일 TU 검증 확대

병렬 빌드 원인과 우회/수정을 기록한 뒤 [Temppal 재생 계획](2026-09-23-Temppal-Replay-Plan.md)의 단계 5를 시작한다.

1. 기존 실제 빌드 입력에서 시스템/태스크 호출 TU 하나와 생성 헤더가 있는 reflected-class TU 하나를 순서대로 선정한다. 원본 compiler, 작업 디렉터리, profile, consumer/producer 응답 파일, 읽은 파일 해시를 고정한다. 현재 입력 최신성은 별도로 표시한다.
2. 각 TU에 원본 소스 기반의 작은 구조 oracle을 먼저 만든다. 재생 성공 여부와 별개로 기반/호출/include의 위치·출처, 관련 unresolved, 진단과 제한을 확인한다. 일반 C++ AST 결과를 UE reflection 의미 지원으로 표현하지 않는다.
3. 기존 `lcm_replay` 경로로 실행하고 실패하면 최초 차단 지점을 최소 재현 사례로 분리한다. 수정이 필요한 경우 해당 범위의 회귀와 독립 대조 후 다시 실제 TU를 실행한다.
4. 고정 입력의 첫 실행과 반복 실행 시간 및 peak memory를 기록한다. OS 캐시를 통제하지 않은 실행을 cold benchmark로 부르지 않는다. UBT/UHT나 프로젝트 재빌드가 필요해지면 입력 불일치와 생성 위치, 변경 범위를 먼저 명시한다.

통과 기준은 두 추가 TU 각각의 결과·한계가 원본 입력으로 재현되고, 구조 oracle과 관련 회귀가 통과하며, 소스/입력/실행 파일 식별이 기록되는 것이다. 한 TU의 성공을 전체 UE 지원으로 확대하지 않는다.

## 3. MCP로 가는 다음 경계

단계 5 증거를 바탕으로 여러 TU의 기여분을 저장·교체하는 인덱스와 최신성 계약을 확정한다. 그 다음 검색·관계 조회와 공통 응답 모델을 만들고 CLI/MCP를 같은 조회 계층에 연결한다. [출시 기준](First-Release-Acceptance-Ledger.md)의 SQLite 인덱스, 검색, `find_code`·`find_all`·`file_api`·`trace_calls`·`repo_map`·`check_freshness`는 아직 구현되지 않았다. 단일 TU 보고서를 MCP로 노출하는 것만으로 이 기준을 충족했다고 판정하지 않는다.

## 다음 Context의 완료 보고

병렬 빌드의 최소 재현 명령·실패 지점·원인 증거·수정 파일·재검증 결과를 먼저 보고한다. 단계 5는 선정 TU와 입력 해시, oracle, 실제 재생 결과, 남은 한계를 별도로 보고한다. 전체 Phase 1 또는 MCP 출시 승인 여부를 이번 제한된 검증에서 추론하지 않는다.
