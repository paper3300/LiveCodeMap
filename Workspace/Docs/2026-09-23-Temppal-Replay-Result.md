# Temppal 단일 TU 재생 결과

2026-09-23, Asia/Seoul. **계획 단계 0–4 완료·승인.**

사용자가 기존 에이전트를 종료한 뒤 Herdr에 새 팀을 구성했다. 조율자는 `lcm-coordinator` (`w8:pE`), 제품 구현자는 `lcm-implementer` (`w8:pF`, Claude), 독립 검수자는 `lcm-reviewer` (`w8:pG`, Codex)다. 구현자 한 명이 제품 파일을 편집했고, 검수 중에는 제품 해시를 고정했다. 검수에서 발견한 결함을 수정한 뒤 v3가 독립 승인을 받았으며, 조율자가 검수자의 별도 빌드로 실제 명령을 재실행했다.

## 구현

- native MSVC 응답 파일의 중첩 확장, 검증된 인코딩/경로 규칙, 입력 제한과 출처 추적. 원본 command identity와 실제 응답 바이트를 반영한 replay identity를 구분한다.
- 확인된 `/experimental:log`와 분리된 출력 경로, `/d2ExtendedWarningInfo`만 제한적으로 처리한다. 유사 옵션과 위험 입력은 계속 거부한다.
- 명시적인 PCH producer 명령을 검증하는 텍스트 스냅샷 재생. 생성/소비/최종 파서가 고정된 입력을 사용하며, 전처리 이벤트·일반 토큰·매크로·파일·counter와 경계를 대조한다. 파서 annotation은 동일한 규칙으로 제외하고 pragma 이벤트는 보존한다.
- 공개 API를 호출하는 `lcm_replay` 실행 도구. 원본 명령, 실제 소비한 양쪽 응답 입력, 변환, 식별값, 진단, 구조 증거, 시간과 구분된 실패 종료 코드를 JSON으로 기록한다.

## 검증

| 확인 | 결과 |
|---|---|
| 독립 PCH/응답 재생 사례 | 12/12 통과 |
| 독립 CLI 실패 사례 | 10/10 통과 |
| 독립 PCH 회귀 | 17 cases / 133 assertions 통과 |
| 실제 case-sensitive PCH 검사 | 12 assertions 통과, skip 없음 |
| 별도 Release CTest | 5/5 통과 |
| 전체 Core / analyzer | 130/2,744 및 47/2,954 통과 |
| Clang-disabled | 3/3 통과; 이후 core 변경 없음 |
| 원본 소스 대조 | 366개 검사 통과 |
| 검수 전후 제품 78개 해시 | 변경 0개 |

독립 검수 보고서: `build/reviewer-replay-20260923/v3-independent-approval.md`.

## 실제 TPGuildSystem.cpp

원본 compiler 경로, Engine/Source 작업 디렉터리, consumer rsp와 producer rsp를 사용했다. 수동 인자 삭제나 compiler 교체 없이 두 별도 빌드 실행에서 모두 다음 결과를 얻었다.

- native exit 0, `status=ok`, `context_complete=true`, 거부 인자 0, 오류/fatal 0.
- 생성/소비/최종 prefix: 이벤트 368,585개, 파일 2,232개, counter 307 및 모든 비교 digest 일치.
- `FTPGuildSystem`: 순서대로 public `ITPPacketHandler`, public virtual `ITPToJson`. 식별·순서·접근·전체 원본 span 일치.
- `Init`의 `CleanUp`, `InitData`, `RegisterEvents` 호출과 정의 위치 일치. 직접 include 24개와 원본 위치/해결 출처 일치. 대상 main-file unresolved 0개.
- 구현자 실행 395,731ms, 조율자 실행 392,503ms. OS 캐시를 통제한 cold benchmark가 아니며 일부 실행 시간이 다른 검증 작업과 겹쳤다.
- 경고 395개, note 407개는 보고서에 보존했다. 전체 TU 사실은 symbols 208,255, relations 281,663이다. 과거 진단 probe가 제외했던 SharedPCH 문맥을 복원했으므로 과거 총량을 고정 기대값으로 사용하지 않았다. 전체 헤더를 포함한 unresolved 25,529개를 모두 해결했다는 주장은 하지 않는다.

두 실제 실행의 command/replay/prefix identity, 사실 총량, 내보낸 상세 구조 증거가 동일하다. 조율자 실행 전후 실행 파일·라이브러리 및 대상 원본 소스 해시도 동일하다.

최종 증거:

- `build/coordinator-replay-20260923/temppal-replay-v3.json`
- `build/coordinator-replay-20260923/final-oracle-verification.json`
- `build/coordinator-replay-20260923/final-reproduction-comparison.json`
- `build/coordinator-replay-20260923/pch-v3-hashes.json`
- `build/coordinator-replay-20260923/product-changes.json`

## 범위와 재현

승인은 **기존 입력의 텍스트 스냅샷 재생**에 한정한다. 과거 binary `.pch`와의 동등성 또는 현재 Intermediate 최신성을 증명하지 않는다. 기존 object 기록의 include 경로 322개와 현재 응답 파일의 329개 사이에는 차이가 있다.

Temppal·Engine은 읽기 입력으로만 사용했고 UBT/UHT, 프로젝트 재빌드, 전체 인덱싱, cross-TU cache, MCP/UI, skipped-callee 수정은 수행하지 않았다. 단계 5는 별도 범위다.

전체 재현 명령은 [계획 문서](2026-09-23-Temppal-Replay-Plan.md#원본-입력을-보존한-재현-명령)에 있다. 로컬 검증 스크립트는 `build/coordinator-replay-20260923/run-temppal-replay.ps1`과 `verify-temppal-report.ps1`이다.
