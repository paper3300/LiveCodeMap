# Graft 0.18.0 기술 분석

> 분석일: 2026-09-17  
> 대상: [trailhq/Graft v0.18.0](https://github.com/trailhq/Graft/tree/de8456e892bad5aeee11403e47fb2227773eb27e) (`de8456e892bad5aeee11403e47fb2227773eb27e`)  
> 목적: LiveCodeMap 신규 프로젝트 설계를 위한 Graft 동작 방식·구현 세부·한계 분석

## 1. 결론

Graft에서 벤치마킹할 핵심은 단순한 "LLM 코드 요약"이 아니다. 실제 핵심은 다음 세 요소의 결합이다.

1. 결정론적인 심볼·관계 그래프를 로컬에서 저비용으로 유지한다.
2. 에이전트가 필요한 순간에만 작은 컨텍스트 팩을 검색한다.
3. MCP·훅·지시문을 통해 에이전트가 이 검색 계층을 실제 작업 흐름에서 사용하게 한다.

특히 구조 그래프, 정밀도 우선 관계 해석, 증분 캐시, query-time freshness, 작업별 검색 도구는 참고 가치가 높다. 반면 `--deep` 의미 계층, 다국어 검색, LSP 보강, 원격 규칙 주입, 자체 성능 측정 방식은 그대로 복제하면 안 된다.

LiveCodeMap는 Graft의 구조 인덱싱 원리를 참고하되 다음을 처음부터 개선해야 한다.

- 한국어·Unicode 검색
- 플랫폼 중립적인 LSP 탐색
- polyglot 저장소의 복수 LSP 실행
- 심볼 단위 LLM 입력 분할
- 배치 경계를 넘는 전역 개념 링크
- 구조·의미·Markdown projection의 명확한 freshness 모델
- 실제 provider usage를 이용한 비용 측정
- 원격 데이터와 에이전트 instruction 사이의 신뢰 경계

## 2. 분석 범위와 검증 수준

로컬 패키지 버전은 `0.18.0`이며 공개 저장소의 현재 `package.json`과 일치한다. 다만 로컬 사본에는 `.git` 메타데이터가 없으므로 공개 `main`의 특정 커밋과 바이트 단위로 동일한지는 확인할 수 없다.

정적 규모는 다음과 같다.

- TypeScript 소스: 140개, 약 29,198줄
- 테스트 파일: 127개, 약 22,962줄
- 런타임: Node.js 20 이상
- 모듈 형식: TypeScript ESM
- 라이선스: MIT

로컬에 `node_modules`가 없어 전체 테스트는 실행하지 않았다. 분석은 다음을 조합했다.

- 소스 정적 분석
- Windows 환경의 LSP 탐색 최소 재현
- Herdr를 통한 Claude Opus 독립 감사
- Codex와 Claude 결과의 코드 라인 단위 재검증

Claude는 68개의 읽기 전용 검사를 수행했다. 핵심 의심 사항에 대해서는 양쪽 분석이 일치했으며, Claude가 추가로 찾은 문제도 다시 소스와 대조한 뒤 이 문서에 포함했다.

## 3. 전체 아키텍처

Graft는 사실상 두 종류의 그래프를 유지한다.

| 계층 | 생성 방식 | 주요 결과 | 비용 |
| --- | --- | --- | --- |
| 구조 그래프 | tree-sitter, 정적 관계 해석, 선택적 LSP | 심볼, span, call/import/reference 관계 | 로컬, LLM 비용 없음 |
| 의미 그래프 | 파일 요약, 개념 합성, 심볼 crux 생성 | 개념 Markdown, 심볼 설명 | LLM API 비용 발생 |

전체 데이터 흐름은 다음과 같다.

```text
Git에 보이는 소스 파일
        │
        ├─ depth extractor: 언어별 전용 tree-sitter 해석
        ├─ breadth extractor: WASM grammar + tags.scm
        └─ container extractor: Vue <script> → TypeScript extractor
        │
        ▼
심볼 노드 + 미해결 raw edge
        │
        ▼
정적 관계 해석
같은 파일 → 명시적 import → 유일한 전역 후보
모호하면 연결하지 않음
        │
        ▼
graft/.graph/wiring.json
graft/.cache/ask-index.json
fingerprint / extraction cache
Markdown cards / INDEX
        │
        ▼
ask / grep / callers / map / blast / MCP
        │
        ▼
Claude·Codex·Cursor 등의 지시문과 훅에서 사용
```

구조 빌드의 중심은 [`src/graph/build.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/graph/build.ts#L151)이다.

## 4. 구조 그래프 빌드

### 4.1 파일 탐색

기본 파일 집합은 다음 Git 명령과 같은 의미를 가진다.

```text
git ls-files --cached --others --exclude-standard
```

추적 파일과 Git에 무시되지 않은 새 파일을 함께 처리한다. Git 탐색이 실패하면 파일시스템 순회로 폴백한다.

주요 제한은 다음과 같다.

- `node_modules`, `dist`, `build`, `target`, `vendor`, `coverage` 등 제외
- 1MB보다 큰 파일 제외
- 점으로 시작하는 디렉터리는 항상 제외
- 점 디렉터리는 `--include-dir`로도 되살릴 수 없음
- UTF-16LE는 지원하지만 UTF-16BE는 건너뜀
- 서브모듈과 중첩 저장소는 기본적으로 따라가지 않음

구현 근거: [`src/ingest/fs.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/ingest/fs.ts#L28)

### 4.2 세 종류의 extractor

Graft는 언어 지원을 세 단계로 나눈다.

#### Depth tier

언어별 전용 로직을 사용한다.

- TypeScript/JavaScript
- Python
- Go
- Java
- Kotlin
- PHP
- Swift
- R

함수·메서드·클래스·import·호출·상속을 비교적 정밀하게 추출한다.

#### Breadth tier

WASM tree-sitter grammar와 공통 `tags.scm` 방식으로 폭넓은 언어를 지원한다.

- Rust
- C/C++
- C#
- Ruby
- Scala
- Elixir
- Solidity
- OCaml
- Zig
- Dart
- Clojure
- Nix
- Lua

지원 폭은 넓지만 타입 및 binding 해석 정밀도는 depth tier보다 낮다.

#### Container tier

현재는 Vue SFC가 구현되어 있다. Wrapper를 일반 HTML로 해석하는 대신 `<script>` 위치를 찾은 뒤 내부 코드를 TypeScript extractor에 넘긴다. 추출 결과의 span은 원본 `.vue` 파일 위치로 다시 이동한다.

구현 근거: [`src/graph/container.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/graph/container.ts#L145)

### 4.3 노드 모델과 안정적인 ID

구조 그래프 노드는 다음과 같은 필드를 가진다.

- ID
- 이름과 kind
- 파일 경로
- line span
- signature
- export 여부
- 추출 origin
- `body_hash`
- 선택적 summary, crux, summary state

심볼 ID는 대략 다음 형태다.

```text
path/to/file.ts#OuterClass.method
```

같은 ID가 중복되면 `~2`, `~3`을 붙인다. 이는 빌드 간 심볼 identity와 의미 캐시 재사용을 가능하게 한다.

구현 근거: [`src/graph/extract.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/graph/extract.ts#L387)

### 4.4 관계 해석 정책

관계 후보는 다음 순서로 해석한다.

1. 같은 파일의 정확한 후보
2. import·경로·소유 타입으로 좁혀진 후보
3. 저장소 전체에서 이름이 유일한 후보
4. 여러 후보가 남으면 엣지를 버림

즉 "아마 이 메서드일 것"이라고 추측하지 않는다. 코드 주석에는 무리한 bare-method fallback이 호출 엣지 정밀도를 73%에서 37%로 떨어뜨렸다는 측정 결과도 기록돼 있다.

구현 근거: [`src/graph/resolve.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/graph/resolve.ts#L274)

언어 family도 분리한다. 예를 들어 Go의 `make`와 TypeScript의 `make`를 이름만 같다는 이유로 연결하지 않는다.

이 정책은 precision을 높이지만 다음 구조는 놓칠 수밖에 없다.

- dependency injection
- reflection
- 동적 dispatch
- 런타임 monkey patching
- 매크로·코드 생성 결과
- 리시버 타입을 추론할 수 없는 메서드 호출
- 외부 라이브러리로 나가는 호출

따라서 Graft 결과는 "완전한 call graph"가 아니라 "거짓 연결을 줄인 부분 그래프"로 해석해야 한다.

### 4.5 캐시와 저장

실제 빌드는 모든 파일을 다시 읽고 콘텐츠 해시를 계산한다. 해시가 같으면 이전 AST 추출 결과를 재사용해 parsing만 생략한다.

주요 캐시는 다음과 같다.

- extraction cache: 파일 해시 → 노드·raw edge
- `wiring.json`: 이전 심볼 summary/crux의 의미 캐시
- fingerprint: 크기·mtime·hash
- `ask-index.json`: 검색용 token bag과 document frequency
- deep summary/synthesis cache

`wiring.json`은 결정론적으로 정렬해 임시 파일에 쓴 뒤 rename한다. 검색용 `body_text`는 큰 그래프 JSON에 남기지 않고 `ask-index` sidecar로 옮긴다.

구현 근거: [`src/graph/write.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/graph/write.ts#L36)

## 5. `--deep` 의미 계층

README는 "두 번의 LLM 패스"라고 설명하지만 실제 `graft build --deep`에는 세 종류의 LLM workload가 있다.

### 5.1 파일별 요약

- 파일마다 한 번 호출
- 콘텐츠 해시 기반 캐시
- 기본 동시성 8
- 입력은 파일 앞의 24,000자만 전송
- 인증·할당량 문제 또는 연속 실패가 누적되면 이후 호출 중단

### 5.2 개념 합성

- 파일 요약을 48,000자 이하 배치로 묶음
- 배치마다 system/file/concept 노드와 링크 생성
- 배치도 파일 경로와 해시를 이용해 캐시
- 같은 slug의 노드는 후처리에서 병합
- 양 끝 노드가 모두 알려진 링크만 유지

구현 근거: [`src/context/build.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/context/build.ts#L146)

### 5.3 심볼 summary와 crux

구조 그래프 생성 뒤 변경된 파일 단위로 LLM을 호출한다. 한 호출에서 해당 파일의 여러 심볼에 대해 다음을 반환하게 한다.

- 한 문장 summary
- 핵심 구현을 보여주는 crux line range

심볼별 호출이 아니라 파일별 호출이므로 비용을 줄인다. 누락된 심볼이 있으면 한 번 더 재시도하며 crux는 최대 12줄로 제한한다.

구현 근거: [`src/graph/enrich.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/graph/enrich.ts#L74)

Cold deep build의 대략적인 호출 수는 다음과 같다.

```text
파일 요약 F회
+ 개념 합성 B회
+ 변경 파일 crux D회
+ 누락 응답 재시도
```

별도의 `graft blast --name`도 LLM을 사용할 수 있지만 `build --deep`의 일부는 아니다.

### 5.4 대형 파일 prefix clipping

입력을 토큰이나 심볼 경계가 아닌 파일 앞부분 기준으로 자른다.

- 파일 요약: 24,000자
- crux: 18,000자
- synth 입력: 60,000자

구현 근거:

- [`src/ai/summarize.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/ai/summarize.ts#L26)
- [`src/ai/crux.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/ai/crux.ts#L114)
- [`src/ai/synthesize.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/ai/synthesize.ts#L92)

대형 파일 뒤쪽 심볼도 응답 대상에는 포함되지만 모델이 받은 source에는 해당 심볼이 없을 수 있다. 이는 잘못된 crux, 환각 또는 반복 누락을 유도할 수 있다.

### 5.5 배치 간 개념 링크 손실

합성 프롬프트는 이번 응답에서 정의한 노드에만 링크하도록 요구한다. 따라서 서로 다른 48K 배치에 들어간 개념 사이의 관계는 모델이 의도적으로 만들 수 없다.

후처리의 전역 slug 병합과 name resolution이 일부 링크를 살릴 수는 있지만, 애초에 서로 다른 배치의 관계를 모델이 보지 못한 문제는 복구할 수 없다.

큰 저장소일수록 알파벳순 파일 배치 경계가 개념 그래프의 경계로 굳어질 위험이 있다. 이는 코드에 기반한 강한 추론이며 품질 실험을 통해 정량화해야 한다.

## 6. 검색과 랭킹

Graft는 embedding이나 vector DB를 사용하지 않는다.

### 6.1 `ask`

질의를 두 가지 경로로 나눈다.

- `who calls`, `callers`, `imports`, `depends on` 등의 영어 형태 → 구조 검색
- 나머지 → lexical 검색

Lexical 랭킹은 다음 신호를 조합한다.

- 이름 일치
- 경로 일치
- body BM25
- personalized PageRank
- 테스트 파일 de-ranking
- 파일 단위 결과 다양성
- 모노레포 scope 간 결과 fusion

Graph score는 lexical score에 약 0.5 비중으로 결합된다. 단순 키워드 일치만으로 helper나 test가 실제 구현보다 높게 올라오는 문제를 연결성으로 보정한다.

구현 근거: [`src/ask/ask.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/ask/ask.ts#L910)

`--source`와 `--full`은 코드를 인라인하지만 한 심볼당 최대 80줄이다. `ask` 결과는 top-N이므로 모든 발생 위치를 찾는 도구가 아니다.

### 6.2 작업별 도구

- `grep`: 모든 텍스트 발생 위치를 심볼별로 그룹화
- `skeleton`: 파일의 전체 API와 signature
- `callers`: incoming/outgoing BFS
- `map`: 디렉터리·hub·hotspot 요약
- `blast`: Git diff의 변경 line을 심볼에 매핑한 뒤 incoming graph 탐색

범용 raw graph API 대신 작업별 인터페이스를 제공하는 점은 좋은 설계다. 에이전트는 graph query language보다 `find code`, `trace calls`, `file API`를 더 안정적으로 사용한다.

### 6.3 한국어·Unicode 검색 결함

가장 중요한 결함이다. Tokenizer는 다음과 같이 ASCII 영숫자만 남긴다.

```ts
.split(/[^a-z0-9]+/)
```

구현 근거: [`src/ask/index-file.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/ask/index-file.ts#L38)

결과는 다음과 같다.

- 순수 한국어 질의는 검색 토큰이 모두 사라짐
- 영어 식별자가 섞인 질의는 영어 식별자만 남음
- 한국어 주석과 비 ASCII 식별자도 색인되지 않음
- stop-word 목록도 영어 전용
- 구조 의도 감지도 영어 정규식 전용

예를 들어 `Foo를 누가 호출하지?`는 `Foo`만 lexical token으로 남고 "호출자 탐색" 의도는 감지되지 않는다.

LiveCodeMap가 한국어 사용자를 대상으로 한다면 다음이 필요하다.

- Unicode `Letter`/`Number` 기반 tokenization
- CJK bigram 또는 형태소 분석 보완
- identifier와 자연어 필드 분리
- 구조 질의는 자연어 정규식보다 명시적 tool routing 우선

## 7. Freshness와 자동 동기화

주요 질의는 먼저 freshness를 확인한다.

1. Fingerprint의 `(size, mtime)`과 현재 stat 비교
2. 달라진 파일만 hash 확인
3. Drift가 있으면 `.sync.lock` 획득
4. 다른 프로세스가 먼저 갱신했을 수 있으므로 재검사
5. `graphOnly` 구조 리빌드
6. `wiring.json`, ask index, fingerprint 갱신

락 대기는 최대 2초다. 질의 경로의 자동 리빌드는 LLM을 호출하지 않는다.

구현 근거: [`src/graph/refresh.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/graph/refresh.ts#L148)

장점은 다음과 같다.

- 질의 시 구조 그래프가 현재 working tree를 반영
- 동시 접근 시 stampede 억제
- Git worktree에서 main checkout의 그래프를 seed로 사용
- 갱신 실패 시 기존 그래프로 답하면서 경고하는 fail-soft 정책

주의점은 다음과 같다.

- 동일 mtime tick 안에서 같은 길이로 변경된 파일은 놓칠 수 있음
- `GRAFT_REFRESH=hash`로 전 파일 해시 검증 가능
- query refresh는 Markdown card와 INDEX를 갱신하지 않음
- 구조 검색 결과와 Markdown projection의 freshness가 잠시 다를 수 있음
- lock release는 소유 PID를 확인하지 않고 파일을 삭제해 stale lock 회수 경쟁에서 다른 프로세스의 lock을 지울 가능성이 있음

## 8. 모노레포와 workspace

### 8.1 단일 그래프 내부 scope

다음 marker를 이용해 scope를 찾는다.

- `package.json`
- `go.mod`
- `pyproject.toml`, `setup.py`
- `Cargo.toml`
- `composer.json`
- `pom.xml`
- `build.gradle`, `build.gradle.kts`

기본 깊이는 2이며 JavaScript workspace glob은 예외다. 비파일 심볼이 5개보다 적은 작은 scope는 root로 병합한다.

### 8.2 여러 독립 저장소 workspace

부모 폴더 아래에 Git 저장소가 둘 이상 있으면 각 child를 독립 그래프로 빌드한다. 부모는 `workspace.json`만 유지하며 `ask` 결과는 child별 검색 결과를 reciprocal-rank 방식으로 결합한다.

장점은 큰 저장소 하나가 작은 저장소 결과를 압도하지 않는다는 점이다. 하지만 child 그래프 사이의 cross-repo edge는 만들어지지 않는다.

Cross-repo edge가 필요하면 `--follow-nested-repos`로 하나의 그래프로 빌드해야 한다. 따라서 공정한 저장소별 랭킹과 cross-repo 관계 사이의 trade-off가 있다.

## 9. 에이전트 통합

`graft init`은 단순한 설정 생성기가 아니다.

- `AGENTS.md`, `GEMINI.md`, Copilot instructions 등에 fenced block 삽입
- Claude/Cursor/Kiro/Windsurf 등에 전용 skill 또는 rule 생성
- MCP 설정 추가
- Claude statusline 설치
- SessionStart, UserPromptSubmit, PostToolUse, Stop hook 설치
- 선택에 따라 사용자 홈의 전역 설정 수정

Claude Code 흐름은 다음과 같다.

- SessionStart: 저장소 map과 freshness 상태 제공
- UserPromptSubmit: 일정 길이 이상의 prompt를 `graft ask`로 검색
- PostToolUse: 파일 편집 후 check와 blast radius 제공
- Stop: dirty 상태면 detached 구조 리빌드
- Statusline: 노드 수, enrichment, stale 상태, 추정 절감량 표시

MCP는 다음 도구를 제공한다.

- `graft_find_code`
- `graft_find_all`
- `graft_trace_calls`
- `graft_file_api`
- `graft_repo_map`
- `graft_check_freshness`

그래프가 없을 때 tool schema를 노출하지 않아 불필요한 컨텍스트 토큰을 피한다.

구현 근거: [`src/mcp/tools.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/mcp/tools.ts#L44)

Graft의 성능을 평가할 때는 다음 효과를 분리해야 한다.

1. 그래프와 랭커의 순수 retrieval 효과
2. 에이전트 skill/instruction 효과
3. Hook 기반 자동 컨텍스트 주입 효과

제품의 체감 성능에는 검색 알고리즘뿐 아니라 "Graft를 먼저 사용하라"는 행동 유도도 크게 기여한다.

## 10. 확인된 주요 결함과 한계

| 우선순위 | 문제 | 판정 |
| --- | --- | --- |
| P0 | 한국어/CJK lexical 검색이 사실상 동작하지 않음 | 확정 |
| P0 | Windows에서 `--lsp`가 서버를 찾지 못함 | 실행 재현 |
| P1 | Polyglot 저장소에서도 LSP 하나만 선택 | 확정 |
| P1 | 대형 파일 의미 분석이 prefix clipping | 확정 |
| P1 | 개념 그래프의 cross-batch 링크 손실 | 코드 기반 추론 |
| P1 | `-j`가 concept summary 동시성에 적용되지 않음 | 확정 |
| P1 | Workspace deep build가 일부 LLM 실패를 성공처럼 출력 | 확정 |
| P1 | 원격 Brain rule과 agent instruction 사이 신뢰 경계 | 보안 위험 |
| P2 | Workspace MCP에서 `depth: "all"`이 유실 | 확정 |
| P2 | `graft check`가 extraction cache 없이 전부 재파싱 | 확정 |

### 10.1 Windows LSP 탐색 실패

LSP binary를 다음 방식으로 찾는다.

```ts
execSync(`command -v ${cmd}`)
```

구현 근거: [`src/graph/lsp/registry.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/graph/lsp/registry.ts#L34)

Windows Node 기본 셸인 `cmd.exe`에는 `command`가 없다. 실제 재현 결과도 다음과 같았다.

```text
'command' is not recognized as an internal or external command
```

예외는 내부에서 `null`로 처리되므로 사용자는 LSP가 실패했다는 경고 없이 엣지 0개 결과를 받는다.

### 10.2 Polyglot 저장소에서 LSP 하나만 실행

`pickServer()`는 설치된 서버 중 첫 번째 하나만 반환한다. 우선순위는 대략 Rust, C/C++, Go, Python, TypeScript 순이다.

Rust와 TypeScript가 같이 있는 저장소에서 Rust server가 선택되면 TypeScript는 LSP 보강 대상에서 빠진다. 제외된 언어를 사용자에게 알리지도 않는다.

### 10.3 `--concurrency`가 concept pass에 적용되지 않음

CLI는 `-j`를 deep 파일 요약 병렬도라고 설명하지만 `engine.init()` 옵션에는 concurrency가 없다. Graph crux pass에는 전달되지만 concept file summary는 기본값 8을 사용한다.

구현 근거: [`src/engine.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/engine.ts#L28)

### 10.4 Workspace deep 실패 은폐

단일 저장소 경로는 concept/crux 실패를 검사해 불완전한 deep build를 exit code로 표시한다. Workspace path는 `engine.init()` 반환값을 버리고 graph error 일부만 출력한다.

따라서 할당량 부족, 인증 실패 등으로 의미 계층이 불완전해도 child build가 성공처럼 보일 수 있다.

구현 근거: [`src/graph/workspace-cli.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/graph/workspace-cli.ts#L65)

### 10.5 Workspace MCP의 `depth: "all"` 손실

일반 MCP 경로는 `all`과 `full`을 `Infinity`로 변환한다. Workspace 경로는 숫자인 경우만 depth를 전달한다. 결과적으로 workspace에서 `depth: "all"`은 기본 depth 1로 축소된다.

구현 근거: [`src/mcp/tools.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/mcp/tools.ts#L168)

### 10.6 `graft check`의 전체 재파싱

Query freshness는 빠른 fingerprint probe를 사용하지만 명시적 `graft check`는 extraction cache를 사용하지 않고 모든 source를 다시 parse한다.

구현 근거: [`src/graph/check.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/graph/check.ts#L100)

대형 저장소에서 `graft_check_freshness`를 자주 호출하면 별도의 확장성 병목이 될 수 있다.

## 11. Trail Brain과 GitHub App

### 11.1 Trail Brain

Brain은 다음 데이터를 로컬에서 읽어 원격 서비스로 보낸다.

- Commit message
- PR 본문과 댓글
- Decision docs
- Agent instructions
- CODEOWNERS
- 일부 lint/CI config
- Revert message
- Test 이름
- Symbol ID와 body hash

일반 소스 본문과 diff는 보내지 않는다. 하지만 commit·PR·docs·config도 사내 정보와 의사결정을 충분히 포함할 수 있으므로 별도 보안 검토가 필요하다.

구현 근거: [`src/brain/push.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/brain/push.ts#L1)

### 11.2 원격 rule의 instruction 삽입

Brain에서 받은 rule text를 별도 sanitization 없이 agent instruction file에 쓴다. 생성되는 문구도 이를 "이미 팀이 결정한 규칙이므로 따르라"고 강하게 지시한다.

구현 근거: [`src/brain/wire.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/brain/wire.ts#L39)

이는 확인된 원격 코드 실행 취약점은 아니다. 그러나 다음이 모두 agent instruction 신뢰 경계 안으로 들어온다.

- Brain 서버
- Brain 계정과 토큰
- Rule mining 데이터
- Repository history의 공격성 텍스트

Rule poisoning이나 서버 침해가 prompt injection으로 연결될 수 있으므로 LiveCodeMap에서는 원격 rule을 기본적으로 데이터로 취급하고, instruction 승격에 검토·승인 절차를 두는 편이 안전하다.

### 11.3 GitHub App

PR App 흐름은 다음과 같다.

```text
Webhook 검증
→ 임시 shallow checkout
→ 구조 graph build
→ git diff를 blast radius에 매핑
→ PR comment upsert
→ 선택적으로 interactive graph page 발행
```

대상 저장소의 패키지를 설치하거나 코드를 실행하지 않는다. Review는 별도 child process에서 실행되고 15분 timeout이 있으며 토큰은 오류 문자열에서 redaction한다.

구현 근거: [`src/app/review.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/app/review.ts#L45)

이 격리 방식은 LiveCodeMap의 서버형 분석 기능에 참고 가치가 높다.

## 12. Telemetry와 성능 수치 해석

Published build는 기본적으로 익명 telemetry를 사용한다.

- Install
- First run
- Init/build/query
- Session summary
- 추정 saved-token bucket

이벤트와 property는 코드 allowlist로 제한한다. CI, `DO_NOT_TRACK`, source build에서는 비활성화된다. 문서상의 약속이 아니라 코드에서 미등록 key를 버리는 구조는 좋은 설계다.

그러나 runtime의 `tokens saved`는 실제 모델 usage가 아니다.

```text
tokens ≈ characters / 4
baseline = 검색 결과가 가리킨 파일들을 전부 읽었을 경우
```

구현 근거: [`src/context/savings.ts`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/src/context/savings.ts#L25)

이는 에이전트가 해당 파일을 모두 읽었을 것이라는 반사실적 가정이다. 출력이 원본보다 큰 경우 절감 라인을 생략하므로 집계도 절감 방향으로 편향될 수 있다.

## 13. 공개 벤치마크 평가

공개 README의 주요 수치는 다음과 같다.

### 통제 실험

- 162 runs
- 저장소 2개
- 각 조건 3 trials
- 토큰 42% 절감
- Tool call 46% 절감
- Latency 60% 절감
- 비용 32% 절감
- Correctness 93%로 동일

### SWE-bench Verified

- 50 instances
- Cold Claude Code: 27/50
- Graft: 33/50
- Token 23% 절감
- Tool call 25% 절감
- Wall time 32% 절감

주의할 점은 다음과 같다.

- 통제 실험은 저장소 2개로 작음
- Graft 제작자가 구성한 harness와 task임
- Controlled benchmark correctness는 judge model에 의존
- SWE-bench도 전체가 아닌 50개 subset
- Retrieval, skill, hook 자동 주입 효과가 분리되지 않음
- 현재 저장소에는 benchmark harness가 없어 재현할 수 없음

관련 근거:

- [`README.md`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/README.md#benchmark)
- [`CHANGELOG.md`](https://github.com/trailhq/Graft/blob/de8456e892bad5aeee11403e47fb2227773eb27e/CHANGELOG.md#L626)

수치는 설계 가설로만 참고하고 LiveCodeMap용 평가를 독립적으로 구성해야 한다.

## 14. LiveCodeMap가 채택할 설계

### 14.1 구조 그래프와 의미 그래프 분리

LLM이 실패해도 구조 검색은 계속 동작해야 한다. 구조는 source of truth이고 의미는 선택적 augmentation이어야 한다.

### 14.2 정밀도 우선 resolver

모호한 edge를 만들어 잘못된 blast radius를 보여주는 것보다 edge를 생략하는 편이 안전하다. 모든 edge에 provenance와 confidence를 기록해야 한다.

### 14.3 콘텐츠 해시 기반 증분 처리

다음 캐시를 분리한다.

- Parse/extraction cache
- Relationship-resolution cache 또는 graph snapshot
- Meaning cache
- Retrieval index

### 14.4 Query-time freshness

빠른 stat probe, 필요한 파일의 hash 확인, lock 획득 후 재검사, fail-soft 정책은 그대로 참고할 가치가 있다.

### 14.5 작은 목적별 도구

범용 graph query 대신 다음과 같은 작업별 인터페이스가 적합하다.

- `find_code`
- `find_all`
- `file_api`
- `trace_calls`
- `repo_map`
- `blast_radius`
- `check_freshness`

### 14.6 Lexical seed + graph rank

Embedding 없이도 BM25와 personalized PageRank 조합은 좋은 baseline이 된다. 결과가 결정론적이고 로컬에서 저렴하게 실행된다.

### 14.7 Worktree seed와 atomic persistence

Worktree가 main checkout의 graph를 재사용하는 방식, 임시 파일 후 rename, 빌드 중간 checkpoint는 실전적인 설계다.

### 14.8 Telemetry allowlist

Telemetry가 필요하다면 자유 문자열을 보내지 않고 모든 event/property를 코드 enum과 allowlist로 제한해야 한다.

## 15. LiveCodeMap에서 개선할 설계

### 15.1 Unicode·CJK 우선

- `\p{L}`, `\p{N}` 기반 tokenization
- Identifier와 natural-language field 분리
- CJK bigram 또는 형태소 분석
- 언어별 stop word
- 구조 질의는 tool 호출을 우선

### 15.2 심볼 단위 LLM 입력

파일 prefix clipping 대신 심볼 span 중심 window를 구성한다.

- Target 심볼의 source는 반드시 포함
- Import와 인접 타입만 budget 안에서 추가
- 모델에게 보이지 않은 심볼은 target에서 제외
- Clipping 발생을 metadata와 error로 기록

### 15.3 개념 추출과 전역 링크 생성 분리

권장 흐름은 다음과 같다.

1. 배치별 개념 후보 추출
2. 전역 deduplication
3. 전역 이름·요약 목록을 이용한 링크 전용 pass
4. 구조 그래프 community를 이용한 보정

### 15.4 복수 LSP 실행

- `command -v` 대신 PATH/PATHEXT 직접 탐색
- 저장소에 해당하는 모든 LSP server 선택
- 언어별 병렬 실행과 timeout
- 제외된 언어와 실패 server를 사용자에게 보고

### 15.5 저장 포맷 확장성

대규모 저장소에서는 monolithic JSON parse/write가 병목이 될 수 있다. SQLite, sharded JSON 또는 columnar sidecar를 검토하고 Markdown은 human projection으로만 유지하는 편이 좋다.

### 15.6 명시적 consistency model

다음 상태를 별도로 표현해야 한다.

- Structural graph freshness
- Retrieval index freshness
- Semantic summary freshness
- Markdown card freshness
- Remote rule freshness

### 15.7 실제 비용 측정

`chars / 4` 추정값은 UI 참고 정보로만 사용한다. 제품 성능 평가는 provider가 반환한 실제 input/output/cache token과 wall-clock을 기록해야 한다.

### 15.8 원격 rule 격리

원격에서 받은 규칙은 기본적으로 검색 결과나 참고 데이터로 제공한다. Agent instruction으로 승격할 때는 다음을 적용한다.

- 명시적 사용자 승인
- Source attribution
- Version과 hash
- Review 가능한 diff
- Rollback
- Remote content quoting/escaping

## 16. 권장 구현 순서

### Phase 1: 구조 IR

- Stable symbol ID
- Path/span/signature/body hash
- Contains/calls/imports/references/extends
- Edge provenance/confidence
- Language plugin interface

### Phase 2: 증분 index와 freshness

- Content-hash extraction cache
- File stat fast path
- Atomic persistence
- Reader/writer lock
- Worktree seed
- Stale 결과 정책

### Phase 3: 검색 도구

- Find code
- Find all occurrences
- File API
- Call tracing
- Repository map
- Blast radius

### Phase 4: 한국어·다국어 retrieval

- Unicode tokenization
- CJK 처리
- Identifier-aware ranking
- Language-neutral intent routing
- Scope/workspace federation

### Phase 5: 선택적 의미 계층

- Symbol-window summarization
- Global concept linking
- Partial failure 표시
- Provider-independent cache key
- 의미 결과의 provenance

### Phase 6: Agent integration

- MCP 우선
- Project-local instruction 기본값
- Global config는 별도 승인
- Hook별 timeout과 failure isolation
- Retrieval, skill, hook 효과를 분리한 A/B 평가

## 17. LiveCodeMap 벤치마크 설계 제안

Graft 수치를 그대로 목표로 삼지 말고 다음 축을 분리해서 측정한다.

### 17.1 그래프 품질

- Call edge precision/recall
- Import/reference precision
- Dynamic dispatch 누락률
- 언어별 결과
- Polyglot cross-language 오탐

### 17.2 Retrieval 품질

- Top-1/Top-5 relevant symbol recall
- Exact-file recall
- 한국어·영어·혼합 질의
- Identifier가 없는 business concept 질의
- Test/helper 오랭킹 비율

### 17.3 Incremental 성능

- Cold build 시간
- 1파일 변경 rebuild 시간
- Query freshness probe 시간
- Peak memory
- Graph 크기
- 10K/50K/100K symbol 규모

### 17.4 Agent task 성능

각 task를 최소 다음 네 조건으로 실행한다.

1. Baseline file tools
2. Retrieval tool만 제공
3. Retrieval + skill instruction
4. Retrieval + skill + hook 자동 주입

측정값은 다음과 같다.

- Task correctness
- 실제 provider input/output/cache token
- Tool call 수
- Wall-clock
- 잘못된 파일 수정 수
- 필요한 sibling file 누락률
- 불필요한 context injection 양

### 17.5 재현성

- Harness를 제품 저장소 또는 별도 공개 저장소에 유지
- Task와 기준 commit 고정
- Model/version/temperature 기록
- Raw trace 보존
- 여러 언어와 저장소 규모 포함
- 최소 3회 이상 반복
- 평균뿐 아니라 분산과 실패 유형 공개

## 18. 최종 판단

Graft의 구조 계층은 상당히 좋은 레퍼런스다. 특히 다음은 직접 벤치마킹할 가치가 있다.

- 정밀도 우선 edge resolution
- 추출 캐시와 의미 캐시의 분리
- Query-time 자동 freshness
- PageRank 기반 lexical 보강
- 작은 목적별 도구
- Worktree와 모노레포 처리
- 원자적 persistence와 중간 checkpoint

하지만 제품 전체를 그대로 본뜨면 다음 문제가 따라온다.

- 한국어 검색 불능
- Windows LSP 무효
- Polyglot LSP 부분 적용
- 대형 파일의 의미 손실
- Batch-local 개념 그래프
- 추정 token savings의 과도한 활용
- 원격 rule과 로컬 instruction의 위험한 결합
- 구조·의미·Markdown freshness 차이

LiveCodeMap의 적절한 방향은 Graft 호환 복제가 아니다. Graft의 구조 인덱싱 원리를 가져오되 다국어, 플랫폼 중립성, 신뢰 경계, 대규모 저장소 확장성, 재현 가능한 평가를 처음부터 다시 설계하는 것이다.

## 19. 외부 참고 링크

- [Graft GitHub 저장소](https://github.com/trailhq/Graft)
- [공식 README](https://github.com/trailhq/Graft/blob/main/README.md)
- [공식 package.json](https://github.com/trailhq/Graft/blob/main/package.json)
- [Telemetry 명세](https://github.com/trailhq/Graft/blob/main/TELEMETRY.md)
- [Changelog](https://github.com/trailhq/Graft/blob/main/CHANGELOG.md)
