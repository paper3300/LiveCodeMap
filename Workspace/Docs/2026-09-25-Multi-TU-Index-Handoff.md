# 다음 구현 계약: TU 기여분 저장·교체와 최신성

2026-09-25. [이번 실행 기록](2026-09-25-Next-Stage-Execution.md)과 PRD FR-IDX-001~005, 기존 실행 계획의 Phase 2 검증표를 연결하는 다음 작업 계약이다. 저장소/검색/MCP가 구현됐다는 보고가 아니다. 실제 TU 검증 판정은 실행 기록을 따른다.

## 이번 증거가 요구하는 구분

- 분석 완료와 빌드 입력 최신성: 태스크의 Development 액션 존재, 위젯의 과거 개별 TU 오브젝트, 현재 unity 액션은 서로 다른 증거다. `status=ok`나 현재 파일 해시 일치만으로 compile/UHT context를 최신으로 표시하지 않는다.
- 명령과 재생 식별: 원본 argv의 command ID, 확장된 PCH 검증 명령의 ID, 응답 파일 바이트를 포함한 replay ID를 각 역할대로 저장한다. 같은 논리적 단위의 명령이 변경됐다고 이전 기여분을 남긴 채 새 ID 아래에 추가하지 않는다.
- 분석 입력과 공개 결과: 생성/외부/ignored 파일도 의존성 무효화에 필요하다. 원본 path/span/hash 및 TU별 증거 소유권을 유지하면서 공개 검색·관계 출력 범위를 별도로 적용한다.
- 강제 include: 비파일 전처리 문맥은 includer 경로가 비어 있을 수 있다. 실제 명령의 강제 입력과 연결하며, 임의의 소스 파일을 소유자로 만들어 넣지 않는다.
- 갱신 예산: 이번 두 TU의 첫 재생은 각각 약 343초와 359초다. 이 경로가 query의 작은 동기 갱신 예산에 들어온다고 가정하지 않는다. 큰 영향 범위에는 아래 PRD의 stale 반환 예산을 적용한다.

## 저장·교체 계약

1. SQLite 정규 테이블에 workspace/profile, 논리적 analysis unit, 원본 파일, symbol/location, relation/evidence, unresolved site, contribution, diagnostic 및 입력 관측을 저장한다. 자주 조회하는 필드는 column으로 두고 전체 graph/source 본문을 JSON으로 저장하지 않는다.
2. 교체 대상은 선택 profile의 논리적 compile action과 원본 파일/symbol 기여분이다. 원본/선택 명령 ID와 replay ID는 그 기여분의 provenance로 보존한다. 명령 변경 시 같은 논리적 단위의 과거 기여분을 조정하는 매핑을 먼저 정의한다.
3. 새 결과는 staging에 만들고 검증 성공 시 한 transaction으로 과거 기여분을 교체한다. commit 성공 때만 fact generation이 증가한다. 실패는 last attempt에 기록하고 마지막 committed generation과 last success를 보존한다.
4. 한 TU를 교체·제거해도 다른 TU의 같은 header/symbol, relation evidence, unresolved owner는 살아 있어야 한다. 마지막 참조가 사라졌을 때만 공통 노드를 정리한다.
5. OS writer lock과 committed generation reader를 사용한다. 순위 generation은 fact generation과 분리한다. 프로세스 종료·SQL 실패·reader 동시 실행에서 절반만 교체된 결과가 보이면 실패다.

## 최신성 계약

파일 내용, 선언/API, symbol body, compile action, retrieval document, deep input의 fingerprint를 구분한다. 이번 file/include 관측 목록만으로 이 전체 계약이 구현됐다고 판단하지 않는다.

| 변경 또는 확인 | 필요한 결과 |
|---|---|
| `.cpp` 변경 | 해당 compile action과 원본 기여분 무효화 |
| header/`.inl` 변경·삭제 | 선택 profile의 reverse include closure 무효화 |
| generated/ignored/external dependency 변경 | 공개 검색 포함 여부와 무관하게 관련 semantic 입력 무효화 |
| reflected header/UHT 입력 변경 | structure 갱신과 UHT context stale 표시 |
| rsp/toolchain/options/Build.cs/Target.cs/descriptor 변경 | compile context와 의존 semantic graph stale 표시 |
| 선택 명령 또는 unity 매핑 변경 | 원래 파일별 기여분을 조정하며 이전 명령의 증거를 중복 보존하지 않음 |
| 내용은 일치하지만 실제 build action/UHT 최신성 미검증 | 해당 계층 freshness unknown/stale를 유지; ready로 승격하지 않음 |
| 삭제 또는 새 ignore | 공개 결과에서 즉시 제외; 이전 semantic 결과를 그대로 노출하지 않음 |

query는 stat 및 변경 파일 hash를 확인하고 writer lock 획득 후 다시 검사한다. 큰 header fan-out은 PRD의 2초 예산 내 stale와 명시적 build 안내를 반환한다. `check_freshness`는 상태 검사이며 UBT/UHT 또는 대규모 rebuild를 실행하지 않는다. availability/freshness/last success/last attempt와 structure/compile/UHT/semantic/retrieval/deep 계층을 분리한다.

## 다음 Herdr 배정과 통과 기준

조율자는 논리적 단위와 명령 교체 매핑을 확정하고 fixture를 선정한다. Claude 구현자 한 명이 저장소·테스트를 편집한다. Sol/high 검수자는 요구사항과 고정 코드/입력만 받아 별도 DB와 프로세스로 검증한다.

첫 구현은 공통 header를 공유하는 작은 두 TU로 시작한다. 추가/교체/제거, 명령 변경, 분석 실패·SQL 실패 rollback, writer 강제 종료·OS lock, reader snapshot, ignored/generated/external 의존성 변경을 독립 검증한다. 이후 이번 실제 TU를 통합 자료로 사용하되 Temppal/Engine 원본을 테스트용으로 변경하지 않는다. 전체 Phase 2 승인은 기존 PRD와 실행 계획의 전체 변이 검증표를 따른다.

저장·교체·최신성 계약이 검증된 뒤 검색·관계 조회와 공통 응답 모델을 구현하고 CLI/MCP를 같은 조회 계층에 연결한다. `find_code`, `find_all`, `file_api`, `trace_calls`, `repo_map`, `check_freshness` 및 전체 출시 기준은 계속 별도 미완료 항목이다.
