# LiveCodeMap 기획 논의 인계

> 상태: 아래 16.1~16.6과 17.1~17.5는 모두 확정됐고, 사용자의 명시적 요청에 따라 `Workspace/Docs/LiveCodeMap-PRD.md`로 정리됐다.
> 기록일: 2026-09-17
> 중요: 이 문서는 논의 근거를 보존하는 인계 기록이며, 제품 요구사항 기준선은 `Workspace/Docs/LiveCodeMap-PRD.md`다.
> 후속 변경: 2026-09-17 사용자 검토를 반영한 PRD 0.3이 우선한다. Graft의 C++/Unreal 특화 벤치마킹을 제품 목적으로 재강조하고, 한국어 전용 요구사항과 외부 전송 규약을 제거했다. 조기 Agent A/B, 의존성 변경 감시, snippet 정합성, 점진적 traversal과 평가 지표 변경은 PRD 19.1절을 따른다. 19.2절은 일반 C++의 compilation database 경로와 Unreal UBT/UHT 경로, 공통 분석 계층 및 각 환경의 독립 출시 검증을 명시한다. 아래 과거 확정 기록 중 상충하는 내용은 현재 요구사항으로 적용하지 않는다.

## 1. 다음 세션 작업 지침

1. 작업 전에 저장소 루트의 `ROLE.md`를 전부 읽고 따른다.
2. 이 문서와 `Workspace/Docs/Graft-Technical-Analysis.md`를 참고한다.
3. 이미 확정된 항목을 다시 선택지로 묻지 않는다.
4. 아래의 "다음 논의 시작점"부터 사용자와 요구사항을 구체화한다.
5. 제품 요구사항과 출시 기준은 `Workspace/Docs/LiveCodeMap-PRD.md`를 기준으로 한다.
6. 비공개 로컬 검증 자료는 저장소 밖 또는 명시적으로 ignore된 경로에서만 사용한다.

## 2. 프로젝트 정의

- 프로젝트명은 LiveCodeMap다.
- Graft를 확장하거나 포크하지 않고 독립적으로 구현한다.
- 작업 공간의 Graft 0.18.0 메커니즘과 사용자 경험을 벤치마킹한다.
- 최종 목표는 Graft의 핵심 기능 대부분을 C++ 및 Unreal Engine 프로젝트에 제공하는 것이다.
- LiveCodeMap는 버그를 직접 해결하는 디버거가 아니다.
- 외부 코딩 에이전트가 원인을 분석할 수 있도록 관련 클래스, 파일, 호출, 참조와 작은 코드 컨텍스트를 제공한다.
- 예시 요청: "Unreal UI에서 특정 아이콘이 표시되지 않는데 관련 코드를 찾아줘."

## 3. 범위와 플랫폼

- 첫 릴리스: Windows 네이티브 환경
- 첫 분석 언어: C++
- 초기 최적화 대상: Unreal Engine C++ 프로젝트
- 향후 Blueprint, C# 등 다른 언어를 지원할 수 있게 공통 심볼·관계 모델과 언어별 분석기를 분리한다.
- macOS와 Linux는 첫 릴리스 범위가 아니지만 플랫폼 종속 코드는 격리한다.

### 기준 corpus

- 공개 synthetic C++/Unreal fixture와 고정된 open-source corpus를 기준으로 사용한다.
- Unreal Engine 소스, `Plugins`, `Content`, `Intermediate` 등은 각 benchmark manifest에서
  포함·제외 범위를 명시한다.
- 비공개 로컬 corpus의 identity, path, version, profile, 규모와 측정 결과는 이 저장소에
  기록하지 않는다.

## 4. 제품 기능 범위

### 목표 기능

- 구조 그래프와 선택적 의미 그래프
- 결정론적 인덱싱과 증분 캐시
- 질의 시 자동 freshness 확인
- CLI: `build`, `ask`, `skeleton`, `grep`, `callers`, `map`, `blast`, `check`, `viz`
- MCP 서버
- MCP 도구: `find_code`, `find_all`, `file_api`, `trace_calls`, `repo_map`, `check_freshness`
- 에이전트 초기화와 제거
- 지시문, MCP, 지원되는 호스트의 생명주기 훅과 상태 표시
- 시각화와 독립 HTML 내보내기
- monorepo, workspace, submodule, nested repository, worktree 처리
- 버전 확인과 사용자가 실행하는 명시적 업그레이드
- 실제 provider usage 기반 로컬 통계

### 제외 기능

- Trail Brain
- GitHub App 기반 원격 서비스
- 에셋 경로·Content 분석
- LiveCodeMap가 직접 수행하는 버그 디버깅

## 5. 배포와 라이선스

- 공개 오픈소스로 배포한다.
- 공개 Git 저장소가 원본이다.
- Git clone 설치와 npm 설치를 모두 지원한다.
- npm 공식 레지스트리를 사용하고 별도 업데이트 서버는 두지 않는다.
- `livecodemap` npm 이름의 사용 가능 여부는 출시 전에 확인한다. launcher와 platform package를 하나의 package family로 관리하기 위해 scoped package를 사용한다.
- 패키지가 scoped여도 실행 명령은 `livecodemap`로 유지한다.
- 라이선스는 아직 미확정이다. MIT를 추천했지만 사용자가 승인하지 않았다.

## 6. 분석 및 개인정보 경계

### ignore

- LiveCodeMap가 생성 코드 여부를 파일명이나 내용으로 임의 판단하지 않는다.
- 사용자가 프로젝트별 폴더·파일 패턴을 직접 ignore한다.
- ignore는 구조 분석, `--deep`, 검색, 그래프, 시각화와 freshness에 일관되게 적용한다.
- 적용된 제외 규칙과 포함·제외·실패 파일 수를 보여준다.
- 명시적 ignore 대상은 컴파일 컨텍스트에 나타나더라도 노드·관계·검색·deep에서 제외한다.
- ignore 대상 참조는 ignored/unresolved 경계로 남긴다.

### 분석 루트 밖 의존성

- Engine이나 Plugin처럼 분석 루트 밖의 선언은 해석 과정에서 일시적으로 사용할 수 있다.
- 실제로 참조된 외부 심볼만 최소 placeholder로 보존한다.
- placeholder에는 canonical name, kind, provider, `indexed: false` 정도만 둔다.
- 외부 본문, 내부 호출, 파일 경로와 전체 API는 인덱싱하지 않는다.
- 외부 심볼은 일반 `find_code`나 `repo_map`의 검색·랭킹 대상이 아니다.
- `skeleton`이나 호출 추적의 경계로는 표시할 수 있다.

### 에셋 제외

- `/Game/...` 같은 에셋 경로를 모델링하거나 검증하지 않는다.
- Content, redirector, cook 상태를 분석하지 않는다.
- `LoadObject`, `LoadIcon` 등은 일반 코드 호출로 취급한다.
- `TSoftObjectPtr`은 타입 참조이며 외부 에셋 노드를 만들지 않는다.

## 7. `--deep`

- 일반 `build`는 로컬 구조 분석이며 외부 전송이 없다.
- 사용자가 명시적으로 `--deep`을 실행할 때만 외부 LLM 전송을 기본 허용한다.
- 사용자는 외부 전송을 금지하거나 사내·로컬 LLM을 지정할 수 있다.
- ignore 대상은 LLM으로 전송하지 않는다.
- 자동 훅은 `--deep`을 실행하지 않는다.
- deep 실패와 관계없이 구조 그래프는 동작해야 한다.
- 성공, 실패, 누락, 캐시 재사용 상태를 분리해서 표시한다.

## 8. 에이전트 연동

- 대상: Claude Desktop, Claude Code CLI, Codex, Codex CLI
- 공통 기반은 CLI, MCP와 지시문이다.
- 호스트가 지원할 때만 심화 훅과 상태 표시를 사용한다.
- 모든 에이전트의 자동화 수준을 억지로 동일하게 만들지 않는다.
- Claude Code와 Codex 계열은 가능하면 `SessionStart`, `UserPromptSubmit`, `PostToolUse`, `Stop` 흐름을 사용한다.
- Claude Desktop에 전용 훅이 없으면 MCP 수준으로 대응한다.
- 각 검색 도구가 자체적으로 freshness를 확인한다.

## 9. Graft 벤치마크에서 확인한 기준

- Graft 0.18.0의 C++ 구조 그래프는 `contains`, `calls`, `imports`, `references`, `implements`, `extends` 중심이다.
- C++ 분석은 전용 심층 분석기보다 broad tier에 가깝고, 선택적 clangd 보강도 제한적이다.
- Unreal 전용 관계, 일반적인 def-use/dataflow, 완전한 override 모델은 제공하지 않는다.
- Graft 품질은 그래프만으로 나오지 않는다. IDF, BM25, 이름·경로 가중치, personalized PageRank, 파일 다양성, test 감점, scope fusion, deep summary/crux와 source pack이 함께 작동한다.
- 기본 결과 수와 깊이는 Graft 사용자 경험을 기준으로 유지한다.
  - CLI `ask`: 기본 8개
  - MCP `find_code`: 기본 5개
  - `callers`/`trace_calls`: 기본 1 hop
  - `blast`: 기본 2 hop
  - full source: 결과당 최대 약 80줄
- Graft tokenizer는 ASCII 영숫자 중심이라 한국어 질의 품질을 위해 Unicode/CJK 처리가 필요하다.
- Graft 0.18.0의 retrieval/context 품질을 첫 릴리스의 하한으로 삼는다.
- Graft와 구현이 달라도 된다. LiveCodeMap가 더 좋다는 주장은 동일 조건 벤치마크로만 한다.
- 풍부한 Unreal 관계 때문에 검색 품질이 나빠지지 않도록 사실 그래프와 랭킹 그래프를 분리한다.

## 10. 정확도 우선 설계 원칙

- 사실 그래프와 추론 결과를 분리한다.
- 컴파일러로 확인된 사실은 보존하고, 런타임 후보처럼 상황 의존적인 관계는 질의 시 계산한다.
- 성능 때문에 사실을 버리지 않는다. 저장 구조, 캐시와 표시량을 최적화한다.
- 사용자에게는 관계를 다음 세 범주로 표시한다.
  - 확정 관계
  - 가능한 관계
  - 미해결 호출 또는 참조
- 미해결 호출에 가짜 대상 간선을 만들지 않는다.
- 이름이 비슷하다는 이유만으로 관계를 확정하지 않는다.
- 검색과 `ask`는 정밀도 우선으로 잡음을 줄인다.
- `blast`와 영향 분석은 재현율 우선으로 누락을 줄이되 확정과 가능 영향을 분리한다.
- 분석 실패, 외부 경계, 비활성 코드, 오래된 의미 분석을 숨기지 않는다.

## 11. C++·Unreal 심볼 및 관계 모델: 확정 사항

### 11.1 심볼 정체성

- 선언과 구현은 별도 심볼로 복제하지 않는다.
- 하나의 논리 심볼에 여러 선언·정의 위치를 연결한다.
- 클래스 소속은 파일명이 아니라 컴파일러가 확인한 qualified owner로 결정한다.
- 하나의 헤더에 선언된 클래스가 여러 `.cpp`에 나뉘어 구현되어도 모두 같은 클래스의 메서드다.
- free/static helper는 파일명이 비슷하다는 이유로 클래스에 붙이지 않는다.

### 11.2 클래스 카드와 파일 API

- 기본 source/search 결과는 Graft처럼 심볼·파일 중심이고 파일 다양성을 유지한다.
- 클래스명 질의에는 파생된 logical class card를 제공한다.
- 클래스 카드는 모든 선언·정의 위치, 상속, interface, reflection, subscription/binding, 주요 메서드를 합쳐 보여준다.
- `file_api`는 실제 물리 파일을 기준으로 유지한다.
- 클래스 카드는 그래프에서 파생하며 중복 데이터나 별도 LLM 요약을 필수로 하지 않는다.

### 11.3 include

- 직접 include만 저장한다.
- include spelling, 해석된 대상, 조건식, 원본 위치를 보존한다.
- transitive include는 질의 시 계산한다.
- 프로젝트 내부 직접 include는 랭킹에서 약한 신호로 사용한다.
- PCH와 외부 include는 랭킹 신호에서 제외한다.
- 파일명만으로 generated code를 판단하지 않는다.

### 11.4 namespace, overload, template, alias

- namespace를 scope/node로 모델링한다.
- 이름 있는 재개방 namespace는 하나의 논리 scope로 합친다.
- anonymous namespace는 파일 경로에 종속된 scope로 둔다.
- overload는 정규화된 signature로 구별한다.
- template primary, explicit/partial specialization, explicit instantiation을 구별한다.
- 모든 implicit instantiation을 독립 노드로 만들지는 않는다.
- 사용 지점의 template type argument는 보존한다.
- type alias는 별도 노드로 두고 원문 alias와 해석된 대상을 모두 보존한다.

### 11.5 상속과 override

- 직접 base edge만 저장하며 access, virtual 여부와 선언 순서를 보존한다.
- transitive base 관계는 질의 시 계산한다.
- override 관계는 컴파일러로 확인된 경우만 확정한다.
- 일반 C++ 타입을 `I` 접두어나 pure virtual만으로 interface라고 추론하지 않는다.
- Unreal `UINTERFACE`의 U/I 쌍은 명시적으로 모델링한다.
- `Foo`와 `Foo_Implementation`은 별도 심볼이며 Unreal 전용 관계로 연결한다.

### 11.6 가상 호출

- 가상 호출 지점에서 컴파일 시 선택된 base method/virtual slot까지는 확정 관계다.
- 실제 실행될 수 있는 override 구현은 가능한 런타임 대상으로 질의 시 계산한다.
- 호출 지점에서 모든 override 구현으로 직접 간선을 미리 만들지 않는다.
- `final` 메서드·클래스, 명시적 `Base::Method()`와 비가상 호출은 구현까지 확정할 수 있다.
- 특정 override의 `callers`는 직접 호출자와 base slot을 통한 가능한 호출자를 분리해 보여준다.
- `trace_calls`는 확정 경로를 우선하고 관련 가능한 후보를 함께 표시한다.
- `blast`는 프로젝트 내부의 가능한 override를 넓게 확장하되 확정/가능 영향을 분리한다.

### 11.7 일반 호출 해석

- compiler semantic resolution 결과를 최우선으로 사용한다.
- 애매한 후보는 가능한 관계로 보존한다.
- 해석하지 못한 callsite는 표현식, 위치와 원인을 포함한 미해결 증거로 남긴다.
- 이름만 같은 함수나 메서드를 호출 대상으로 확정하지 않는다.
- 전용 def-use, 필드 read/write, 일반 데이터 흐름 그래프는 첫 릴리스에 만들지 않는다.
- 필요한 값 흐름은 관련 심볼과 작은 source snippet을 외부 에이전트에 제공해 분석하게 한다.

### 11.8 람다와 간접 호출

- 람다는 독립 callable 심볼로 만든다.
- 람다 내부 호출을 둘러싼 함수의 직접 호출로 잘못 귀속하지 않는다.
- 함수·메서드 주소는 정확한 `references` 관계로 기록한다.
- compiler와 구문으로 직접 확인되는 callback binding만 binding 관계로 기록한다.
- `TFunction` 변수나 callback container를 통한 대상은 데이터 흐름 없이 추측하지 않는다.
- 확인할 수 없는 실행은 간접 호출로 남긴다.
- delegate, timer와 callback이 나중에 실행된다는 실행 간선은 만들지 않는다.

### 11.9 delegate

- Graft에는 delegate 전용 관계가 없지만 Unreal 코드 탐색을 위해 최소 구조를 둔다.
- delegate 선언을 구조화한다.
- binding/subscription에서 소유 클래스와 handler 관계를 기록한다.
- `Broadcast`, `Execute`, `ExecuteIfBound`를 특별한 실행 관계로 만들지 않는다.
- `Remove`, `Unbind`, `Clear`와 lifecycle을 특별 관계로 만들지 않는다.
- 실행과 해제는 일반 호출 및 텍스트 검색으로 찾을 수 있다.

### 11.10 Unreal macro와 reflection

- `UCLASS`, `USTRUCT`, `UENUM`, `UPROPERTY`, `UFUNCTION` 등의 원문 macro text를 보존한다.
- 다음 항목만 코드 연결 관계로 구조화한다.
  - `BlueprintNativeEvent`, `BlueprintImplementableEvent`
  - Getter/Setter
  - `ReplicatedUsing`
  - `Server`, `Client`, `NetMulticast`
  - RPC `_Implementation`, `_Validate`
  - `BindWidget`, `BindWidgetOptional`
  - `BlueprintAssignable`
  - Unreal interface와 delegate binding
- Category, tooltip, edit flag, clamp 등의 metadata는 속성으로만 보존한다.
- `StaticClass`, `IsA`, `Cast`, `FindFunction`, `ProcessEvent`, `NewObject`, `SpawnActor` 등의 runtime reflection API를 특별히 연결하지 않는다.
- 위 API는 일반 호출과 타입 참조로 처리한다.
- Blueprint 구현과 BindWidget 대상은 첫 릴리스에서 외부 코드 경계다.

### 11.11 Unreal 포인터와 컨테이너 타입

- wrapper 내부 타입에 일반 `references` 관계를 만든다.
- wrapper 자체는 type expression/metadata로 보존한다.
- 포인터 wrapper에서 owns 또는 inherits 관계를 만들지 않는다.
- raw pointer, `TObjectPtr`, `TWeakObjectPtr`, `TSoftObjectPtr`, `TSubclassOf`, `TScriptInterface`, `TSharedPtr`, `TUniquePtr`와 중첩 컨테이너를 같은 원칙으로 처리한다.
- 흔한 타입 허브가 검색 순위를 오염하지 않도록 type reference는 약한 랭킹 신호로 사용하거나 제외한다.

### 11.12 module과 target

- Target이 module을 포함하고 module이 파일을 포함하는 구조로 모델링한다.
- module dependency는 public/private, runtime/editor 등의 성격을 보존한다.
- `Build.cs` 텍스트 정규식이 아니라 선택된 UBT profile의 해석 결과를 사용한다.
- 외부 Plugin/Engine module은 이름만 가진 placeholder로 둔다.

### 11.13 `.inl`과 분할 구현 파일

- `.inl`을 고정적으로 header 또는 implementation으로 분류하지 않고 source fragment로 취급한다.
- 물리 위치와 semantic owner/inclusion context를 함께 보존한다.
- 여러 include context를 허용하고 compiler identity로 중복 제거한다.
- `.inl` 변경 시 관련 translation unit을 무효화한다.
- 공개 synthetic fixture는 X-macro fragment, class body 안에 들어가는 선언 fragment,
  `.cpp`에서 include되는 implementation fragment를 각각 포함한다.
- 하나의 논리 class 구현이 여러 `.cpp`와 `.inl`에 나뉜 형태를 정상 지원한다.

### 11.14 UBT unity build와 PCH

- unity file과 PCH는 컴파일 해석을 위한 임시 컨텍스트로만 사용한다.
- 검색, 그래프와 클래스 카드에 unity/PCH 파일 자체를 노출하지 않는다.
- compiler source location을 이용해 모든 사실을 원본 `.h`, `.cpp`, `.inl`에 다시 귀속한다.
- 하나의 unity translation unit에 여러 `.cpp`가 있어도 사실을 원본 파일별로 분리한다.
- canonical symbol identity로 중복 제거한다.
- 원본 파일이 ignore 대상이면 unity를 통해 발견되어도 제외한다.
- 원본 변경 시 관련 unity 분석 단위를 다시 처리하되 원본별 사실을 교체할 수 있게 한다.
- PCH가 없거나 오래되면 구조 분석은 계속하고 정밀 의미 분석의 저하 상태를 표시한다.
- 분석 목적으로 프로젝트 `Build.cs`/`Target.cs`나 실제 unity 설정을 변경하지 않는다.
- 강제로 non-unity build를 만들지 않는다.
- generated/unity 분류는 파일명 추측이 아니라 UBT build action과 분석 루트 정보에 근거한다.

## 12. build profile과 활성 코드

- 프로젝트마다 하나의 기본 target/configuration profile을 선택한다.
- Unreal target/platform/configuration은 사용자가 명시하며 특정 비공개 프로젝트의
  profile을 repository default로 두지 않는다.
- 정밀 의미 분석은 활성 profile을 기준으로 한다.
- 사용자가 실행한 명시적 `livecodemap build`는 로컬 UBT를 호출해 compile information을 준비할 수 있다.
- 질의와 freshness 확인이 전체 UBT를 자동 실행하지는 않는다.
- build configuration이 바뀌면 compile context를 stale로 표시한다.
- inactive `#if` 코드도 구조와 텍스트 검색에서는 `[inactive]`로 볼 수 있다.
- 정확한 call graph의 기본값은 active profile만 포함한다.
- 필요할 때 `--include-inactive`로 비활성 코드를 포함한다.
- macro 사실은 생성 헤더가 아니라 원본 사용자 코드 위치에 매핑한다.

## 13. 질의 결과와 출력 제한

- 요청한 traversal 자체는 완전히 계산하고 표현 단계에서만 결과량을 제한한다.
- 항상 total, shown, omitted와 cursor를 표시한다.
- `--all`은 실제 closure를 계산한다.
- CLI 텍스트와 MCP 응답은 페이지네이션한다.
- 기본 결과 수와 hop은 Graft 0.18.0 사용자 경험을 기준으로 한다.

## 14. 성공 기준과 벤치마크

### 검증 축

1. 인덱싱 완전성과 결정성
2. `--deep` 처리 완전성과 부분 실패 표시
3. 코드 연결 관계와 검색 정확도
4. LiveCodeMap 미사용 대비 실제 에이전트 토큰 감소

### A/B 조건

- 동일 코드 상태
- 동일 요청
- 동일 에이전트와 모델
- 동일 설정
- 각 작업 최소 3회 반복
- 실제 provider input/output/cache token 사용
- 도구 호출 수, 완료 시간과 정확성도 함께 측정

### 첫 정량 목표

- 정확도를 낮추지 않으면서 대표 작업 중앙값 기준 실제 입력 토큰 30% 이상 감소

### 아직 수치가 필요한 항목

- cold indexing 시간
- warm query 응답 시간
- 단일 파일 증분 갱신 시간
- 최대 메모리
- 인덱스 크기
- deep 처리 시간과 비용

## 15. 공개 fixture가 다뤄야 할 Unreal 근거

- `UCLASS`, `UPROPERTY`, `UFUNCTION`, `UINTERFACE`와 pointer wrapper를 포함한다.
- delegate 선언, dynamic/native/lambda binding과 broadcast를 포함한다.
- `BindWidget`, `BindWidgetOptional`, `BlueprintImplementableEvent`와
  `BlueprintNativeEvent` 경계를 포함한다.
- namespace, template, virtual/override와 member-function pointer를 포함한다.
- asset graph를 구현하지 않아도 공개 Unreal UI fixture의 code call/reference와 작은
  source context를 제공하는 탐색 과제를 포함한다.

## 16. 이번 세션에서 확정한 설계

### 16.1 UHT `.generated.h`와 생성 함수

- UHT 산출물은 일반 사용자 source가 아니라 숨은 compile/bridge 계층이다.
- `.generated.h`와 `.gen.cpp`는 compiler 해석에 사용하지만 기본 검색, 클래스 카드, 일반 그래프와 랭킹에는 노출하지 않는다.
- 사용자 선언 `Foo()`와 UHT가 만든 `Foo()` 정의는 하나의 논리 심볼로 합친다. 표시 위치는 사용자 source다.
- `Foo_Implementation()`과 RPC의 `Foo_Validate()`는 각각 별도 callable이며 원본 `Foo`와 Unreal 전용 관계로 연결한다.
- interface의 `Execute_Foo()`는 내부 bridge로 보존하되 기본 결과에서는 원본 `Foo`로 접는다. 호출 위치에는 실제 spelling과 generated bridge provenance를 남긴다.
- `execFoo`, `StaticRegisterNatives*`, `Z_Construct_*`, `EmptyLinkFunction*`, 등록 배열과 UHT 생성 constructor/destructor는 공개 호출 그래프와 랭킹에서 제외한다.
- `BlueprintImplementableEvent`에는 native 구현을 만들지 않고 Blueprint 외부 경계로 표시한다.
- `BlueprintNativeEvent`의 `Foo -> Foo_Implementation`은 일반 확정 `calls`가 아니라 native fallback 가능성을 나타내는 Unreal 관계다.
- RPC base 호출과 `_Implementation` 실행도 일반 `calls`가 아니라 RPC 전용 관계다.
- `callers(Foo)`에는 사용자 `Foo()` 호출과 interface의 `Execute_Foo()` 호출을 포함하지만 generated thunk와 등록 함수는 포함하지 않는다.
- generated 여부는 파일명이나 함수명으로 판단하지 않고 선택 profile의 UBT/UHT provenance로 판단한다.
- 사용자가 source에 직접 작성한 `CustomThunk` 등은 이름이 `exec*`여도 제거하지 않는다.
- 공개 synthetic fixture에서 generated header, `.gen.cpp`, `DEFINE_FUNCTION`과
  `Z_Construct_*`를 포함해 기본 결과의 generated-code 잡음 제거를 검증한다.

### 16.2 callable 세부 모델

- 사용자 선언 constructor, destructor, conversion function과 operator는 독립 callable이다.
- `= default`, `= delete`도 사용자 선언 심볼로 보존하고 상태를 표시한다.
- compiler가 암시적으로 합성한 special member는 기본 검색에서 제외하되 실제 참조된 경우 숨은 심볼로 보존한다.
- compiler가 확인한 생성, 복사, 이동, 변환, base/member initializer와 사용자 정의 operator 호출은 확정 호출 관계로 저장한다.
- 자동 객체 파괴는 `implicit lifetime` 성격의 관계로 저장하되 도구별 기본 표시 여부는 반환 규약에서 조절한다.
- 내장 operator는 callable로 만들지 않는다.
- `&Foo`, `&Type::Method`는 정확한 `references` 관계다.
- `(&Foo)(args)`와 `(obj.*&Type::Method)(args)`처럼 callsite 자체로 증명되는 즉시 간접 호출만 확정 호출로 연결한다.
- pointer 변수, `std::invoke`, `TFunction`이나 callback container를 경유한 대상은 데이터 흐름 없이 추측하지 않고 indirect/unresolved call로 남긴다.
- 가상 member pointer 호출은 base slot까지만 확정하고 override 후보는 질의 시 계산한다.
- 람다는 독립 callable이고 compiler closure의 `operator()`는 람다 심볼로 접는다.
- capture 대상, 값/참조, 명시적/암시적 여부, `this`/`*this`, init-capture를 보존하되 일반 def-use로 확장하지 않는다.
- init-capture initializer 안의 호출은 람다 생성 시 실행되므로 바깥 callable에 귀속하고, 람다 본문 호출은 람다에 귀속한다.
- 즉시 실행 람다는 `바깥 callable -> lambda -> 내부 호출` 구조로 보존하고 `[immediately invoked]`로 표시한다.
- generic lambda의 implicit instantiation마다 공개 심볼을 복제하지 않는다.

### 16.3 stable symbol ID와 증분 invalidation

- Clang USR은 compiler evidence와 진단용 provider key로 저장하지만 LiveCodeMap의 영구 ID로 그대로 사용하지 않는다.
- 공개 stable ID는 schema version이 포함된 `cm1:<hash>` 형태다.
- canonical key에는 repository member, language, symbol kind, semantic owner chain, canonical name, normalized signature와 linkage discriminator를 넣는다.
- 절대 경로, 행 번호, body hash, active profile과 UHT 생성 경로는 외부 linkage named symbol의 ID에 넣지 않는다.
- internal linkage와 anonymous namespace는 repository-relative file scope를 ID에 포함한다.
- lambda, 익명 타입과 local symbol은 enclosing symbol과 lexical anchor/ordinal을 사용한다. 이 계층은 앞쪽 익명 심볼 삽입 시 ID가 바뀔 수 있음을 허용한다.
- 본문·주석 변경과 외부 linkage named symbol의 파일 이동은 같은 ID를 유지한다.
- 이름, owner 또는 signature 변경은 새 ID이며 rename을 Git history나 휴리스틱으로 같은 identity로 승격하지 않는다.
- profile에 따라 같은 선언의 본문만 달라지면 같은 ID의 variant이고 signature가 달라지면 다른 ID다.
- 분석 실행 단위는 compile action/TU이며 저장·교체 단위는 원본 파일 및 심볼 contribution이다.
- TU별 evidence를 따로 보존하고 한 TU가 무효화돼도 다른 TU가 확인한 심볼과 관계는 유지한다.
- `.cpp` 변경은 관련 compile action, `.h`/`.inl` 변경은 선택 profile의 reverse include closure를 무효화한다.
- unity action은 전체 재분석하되 결과는 원본별로 교체한다.
- reflected header 변경 시 구조 결과는 갱신하고 UHT context는 stale로 표시한다.
- body-only 변경은 해당 심볼의 outgoing relation, retrieval과 deep input을 갱신한다.
- signature 변경은 기존 심볼 제거, 새 심볼 생성과 관련 dependent TU/resolver bucket 갱신을 유발한다.
- file content, declaration/API, symbol body, retrieval document, deep input과 compile action fingerprint를 분리한다.

### 16.4 구조 저장소와 랭킹 projection

- 로컬 SQLite를 사용하고 graph database나 monolithic JSON은 사용하지 않는다.
- 외부 stable ID와 별도로 SQLite join/traversal용 integer surrogate key를 둔다.
- 사실 저장소에는 workspace/profile, analysis unit, file, entity/symbol, location, relation, evidence, unresolved site, contribution과 diagnostic을 둔다.
- 자주 질의하는 필드는 정규 column으로 저장하고 드문 언어별 metadata만 JSON payload를 허용한다.
- 확정 relation, candidate relation, unresolved site와 query-time runtime expansion을 분리한다.
- relation에는 여러 evidence가 붙을 수 있으며 contribution 하나가 사라져도 다른 evidence가 남으면 relation을 유지한다.
- 새 분석은 staging contribution에 기록하고 성공할 때만 SQLite transaction으로 이전 contribution과 atomic 교체한다.
- 실패하면 마지막 committed 결과를 유지하고 stale/failed 상태를 별도로 기록한다.
- SQLite는 adjacency 조회를 담당하고 BFS, cycle 처리, relation filter와 pagination은 애플리케이션 코드에서 수행한다.
- ranking projection은 사실 그래프에서 재생성 가능하며 source of truth가 아니다.
- 검색 document 단위는 사용자 source symbol, 물리 file, 검색용 class aggregate, module/target과 선택적 deep summary다.
- UHT/unity/PCH, ignored source, compiler implicit symbol, 외부 placeholder, 흔한 wrapper type reference와 미확정 candidate edge는 기본 ranking projection에서 제외한다.
- identifier, qualified name, path, signature/type, 자연어, body/comment, CJK와 deep summary field를 분리한다.
- 전체 source 본문은 DB에 복제하지 않고 path/span/hash, 검색 token과 작은 파생 정보만 저장한다.
- fact generation과 ranking generation을 분리하며 ranking 실패와 무관하게 구조 도구는 동작한다.
- deep 결과는 content-addressed cache로 보존해 index 재생성 때 재사용한다.
- WAL을 선호하되 네트워크 filesystem 등 지원되지 않는 환경에서는 rollback journal로 폴백한다.

### 16.5 도구별 반환 규약

- CLI와 MCP는 같은 semantic response model을 사용하고 CLI는 사람이 읽기 좋게 렌더링만 한다.
- 모든 도구는 profile, graph generation, 계층별 freshness, `success|partial|stale|error`, stable ID, `confirmed|possible|unresolved`, diagnostics와 `total/shown/omitted/next_cursor`를 반환한다.
- cursor는 query와 graph generation에 종속된다. graph가 바뀌면 cursor를 만료시키고 다른 세대의 결과를 섞지 않는다.
- 모호한 symbol selector를 임의로 하나 선택하지 않고 후보와 signature를 반환한다.
- `ask`는 LLM 답변 생성기가 아니라 외부 에이전트용 context pack이다. query-time 외부 LLM 호출은 없다.
- CLI `ask` 기본 8개, MCP `find_code` 기본 5개다.
- `ask`는 identifier, 자연어, path와 graph 신호를 결합하고 클래스 질의에는 class card를 반환할 수 있다.
- `callers`는 기본 1 hop, 20 caller이며 caller별 여러 callsite를 묶고 고유 caller total과 callsite total을 분리한다.
- `trace_calls`는 기본 1 hop, 20 record이며 확정 경로와 가능한 dispatch 분기를 분리한다. `--all`은 reachable graph closure를 계산하지만 순환 그래프의 모든 단순 경로 조합을 열거하지 않는다.
- `blast`는 기본 2 hop, 50 impact이며 `directly_changed`, `confirmed_dependency_path`, `possible_runtime_path`, `unresolved_boundary`, `external_boundary`를 분리한다.
- `blast`의 confirmed는 동작 변화가 확정됐다는 뜻이 아니라 구조적 dependency path가 확정됐다는 뜻이다.
- `file_api`는 실제 물리 파일 기준이며 기본 100 entry다. module/profile/freshness, 직접 include, 물리적으로 선언·정의된 symbol, owner와 다른 위치 링크, `.inl` inclusion context를 반환한다.
- 분할 구현 파일의 내용을 `file_api`가 임의로 합치지 않고 logical symbol/class card 링크를 제공한다.
- 기본 snippet은 관련 부분만 제공하고 `--full`도 결과당 최대 약 80줄이며 clipping과 원래 범위를 표시한다.
- 요청한 traversal은 완전히 계산하고 표현 단계에서 페이지네이션한다.

### 16.6 한국어/CJK 검색과 identifier 분해

- 형태소 분석기나 외부 사전 없이 Unicode word token, identifier token과 CJK character bigram을 결합한다.
- 원문 identifier는 그대로 보존하고 compiler identity에는 검색용 정규형을 사용하지 않는다.
- 검색용 NFC/case-fold와 NFKC compatibility form을 추가하되 exact보다 낮게 랭킹한다.
- 라틴·키릴 문자처럼 시각적으로 비슷한 confusable을 동일 문자로 접지 않는다.
- tokenizer의 Unicode version을 고정하고 retrieval fingerprint에 포함한다.
- ASCII/Hangul/Han/Kana 등 script 전환에서 분리해 `Foo를`을 `Foo`와 `를`로 찾을 수 있게 한다.
- 한국어·중국어·일본어 span에는 character bigram을 추가하고 한 글자 CJK token도 버리지 않는다.
- 한국어 조사 제거 휴리스틱은 두지 않고 원형 token과 bigram을 사용한다.
- identifier는 원본 전체, qualified component, snake/kebab/camel/Pascal/acronym/digit 경계로 분해한다.
- Unreal의 `U`, `A`, `F`, `S`, `E`, `T`, `I` 단일 type prefix 제거 variant를 추가하되
  project-specific prefix는 hard-code하지 않는다.
- exact identifier, qualified/suffix, identifier component, path, natural-language BM25, graph, CJK bigram, body/comment/string 순으로 초기 상대 우선순위를 둔다.
- CJK bigram 기여도를 제한하고 흔한 bigram은 IDF로 감점한다.
- `ask`의 작은 한국어/영어 intent lexicon은 lexical 검색에 graph 결과를 추가할 뿐 lexical 결과를 완전히 대체하지 않는다.
- 공개 fixture에는 비공개 로컬 자료를 사용하지 않고 동등한 합성·오픈소스 사례를 사용한다.
- 한국어, 영어, 혼합, identifier 없는 기능 설명, acronym/Unreal prefix, Hangul normalization과 confusable 사례를 별도로 평가한다.

## 17. 확정 사항과 다음 논의 시작점

### 17.1 확정: UBT context, freshness, cache, workspace/worktree

다음 설계는 2026-09-17에 사용자가 명시적으로 승인했다.

- 명시적 `livecodemap build`만 UBT를 사용하고 query path는 UBT, 전체 project build, UHT 재생성, `--deep`과 외부 LLM을 자동 실행하지 않는다.
- `livecodemap build`는 binary compile/link가 아니라 LiveCodeMap index 준비 명령이다.
- 기본 흐름은 project/Engine/profile 발견, UBT compile action 획득, 필요한 UHT 준비, semantic 분석, fact/retrieval 갱신이며 `--deep`은 명시됐을 때만 실행한다.
- `Build.cs`, `Target.cs`, unity/PCH 설정을 변경하지 않는다.
- UE 버전에서 metadata/UHT 준비를 실제 project build 없이 할 수 없다면 몰래 전체 빌드하지 않고 `project build required`를 보고한다.
- compile context 우선순위는 선택 profile의 UBT action export, 동일 profile response/dependency artifact, UBT compilation database, structural-only fallback이다.
- 특정 undocumented UBT flag를 공개 계약으로 삼지 않고 UE version adapter가 지원되는 방식을 선택한다.
- raw compile argv와 analyzer-normalized argv, dropped option과 이유, source/working directory, profile/module/toolchain, defines/include/forced include, language standard, unity/PCH/UHT/dependency 정보를 보존한다.
- MSVC PCH binary를 Clang 분석기에 억지로 넣지 않고 forced include source를 해석하며 `pch_emulated`/degraded 상태를 표시한다.
- 사용자 상태는 `ready`, `partial`, `stale`, `missing`, `failed`로 요약하되 내부적으로 availability, freshness와 last attempt를 분리한다.
- structure, compile context, UHT context, semantic graph, retrieval과 deep 상태를 독립적으로 표시하고 total/success/failed/stale/ignored unit 수를 제공한다.
- 검색 도구는 stat probe, 변경 파일 hash, writer lock 후 재검사, structural/lexical 증분 갱신을 수행한다.
- 유효한 compile context가 있고 변경 범위가 query refresh budget 안이면 semantic 증분 갱신도 할 수 있다. 큰 header fan-out은 오래 막지 않고 stale semantic 상태와 `livecodemap build` 안내를 반환한다.
- 삭제되거나 새로 ignore된 파일은 stale 결과로 계속 노출하지 않는다. 존재하는 변경 파일의 이전 semantic evidence만 stale 표시로 유지할 수 있다.
- `check_freshness`는 상태를 검사하지만 UBT나 대규모 rebuild를 실행하지 않는다.
- worktree마다 별도 mutable SQLite DB와 OS lock을 사용한다. live DB는 공유하지 않고 immutable content-addressed cache만 공유한다.
- 한 writer와 마지막 committed generation을 읽는 reader 구조를 사용하고 PID lock file 삭제 방식은 사용하지 않는다.
- project-local index는 worktree별 `.livecodemap/`, immutable shared cache는 Windows user-local cache를 추천했다. 실제 경로와 `.gitignore` 처리는 패키징 논의에서 확정한다.
- 하나의 Git monorepo는 하나의 index로 보고 nested repository와 submodule은 감지 후 명시적으로 workspace member로 선택한다.
- 독립 repository workspace는 member별 index와 workspace catalog를 사용하고 실제 compiler가 확인한 cross-member 참조만 overlay relation으로 둔다.
- worktree는 compatible main snapshot으로 seed할 수 있지만 현재 worktree hash를 다시 확인하고 달라진 파일을 무효화한다.
- 선택 profile과 일치하지 않는 최신 response file을 재사용하면 안 된다.
- 지원되는 response/dependency artifact에서 source, shared flags, forced include, PCH,
  dependency output과 실제 include closure를 읽는 fallback을 검증한다.

### 17.2 확정: 시각화와 독립 HTML의 최소 표현 범위

다음 설계는 2026-09-17에 사용자가 명시적으로 승인했다.

- 시각화는 전체 그래프 전시가 아니라 질의 결과를 탐색하는 도구로 둔다.
- `livecodemap viz`의 기본 화면은 `map` 기반 target/module/directory 개요이며, 심볼이나 파일을 선택하면 기본 1-hop 관계를 보여주고 사용자가 방향별로 한 단계씩 확장한다.
- `blast` 시각화는 변경 심볼, 확정 dependency path, 가능한 runtime path, unresolved boundary와 external boundary를 구분한다.
- 전체 저장소의 모든 심볼과 간선을 브라우저에 한꺼번에 보내지 않는다. 노드·간선 제한값은 성능 benchmark에서 확정하되 항상 `shown/omitted`와 제한 이유를 표시한다.
- 최소 UI는 현재 범위 내 심볼 검색, node kind와 relation filter, 방향 구분, 선택 노드의 signature·repository-relative 위치·작은 snippet·포함 이유, profile·graph generation·계층별 freshness를 제공한다.
- 확정 관계는 실선, 가능한 관계는 점선으로 표시한다. 미해결 호출은 가짜 대상 노드로 연결하지 않고 callsite의 경고와 근거로 표시하며 stale 관계도 숨기지 않는다.
- 그래프와 같은 범위를 보여주는 목록/outline 보기를 제공한다.
- 로컬 viewer는 `127.0.0.1`에만 bind하고 read-only로 동작하며 임의 session token, 외부 network 요청 금지, telemetry 없음과 `--no-open`을 적용한다.
- viewer 조회와 확장은 기존 freshness 계약을 따르고 UBT, UHT, 전체 build와 `--deep`을 자동 실행하지 않는다. graph generation이 바뀌면 이전 응답을 섞지 않는다.
- 독립 HTML은 CSS, JavaScript와 선택된 graph data를 포함한 단일 offline 파일이며 `file://`에서 동작하고 CDN이나 server를 요구하지 않는다.
- HTML에는 현재의 제한된 view/query snapshot만 포함하고 SQLite DB나 전체 저장소 graph를 넣지 않는다. profile, generation, freshness, 포함·생략 수를 함께 기록한다.
- export에는 절대 경로 대신 repository-relative path만 넣고 기존 snippet 제한을 적용한다. 코드가 포함될 수 있음을 명확히 경고한다.
- 첫 릴리스에는 별도 LLM context graph tab을 만들지 않는다. cached deep summary가 있으면 detail panel에서만 보여준다.

### 17.3 확정: 에이전트별 init/remove, hook와 status 표시

다음 설계는 2026-09-17에 사용자가 명시적으로 승인했다.

- Codex 앱·CLI는 `AGENTS.md`, project-local `.codex/config.toml`과 `.codex/hooks.json`을 사용한다. Claude Code CLI는 `CLAUDE.md`, `.mcp.json`과 `.claude/settings.json`을 사용한다.
- Claude Desktop은 `.mcpb` Desktop Extension을 통한 MCP와 앱 자체 연결 상태만 사용하며 project hook를 설치하지 않는다.
- `livecodemap init`은 project-local 설정만 기본으로 설치하며 user home이나 machine-wide 설정은 별도 명시적 선택 없이는 수정하지 않는다.
- 감지한 host를 보여주고 사용자가 선택한 host만 연결한다. non-interactive 실행은 `--host`로 대상을 명시해야 한다.
- `--dry-run`은 수정할 path, repo/user scope, 생성·병합·충돌 여부를 모두 보여준다.
- `init`은 index를 만들거나 UBT를 실행하지 않으며 필요한 경우 명시적 `livecodemap build`를 안내한다.
- 기존 instruction file에는 짧은 LiveCodeMap managed block만 삽입하고 기존 내용을 덮어쓰거나 중복 block을 만들지 않는다.
- JSON/TOML은 LiveCodeMap 소유 entry만 병합한다. parse 실패나 같은 이름의 다른 MCP entry가 있으면 수정하지 않고 conflict를 보고한다.
- 여러 file 변경은 가능한 범위에서 staging 후 일괄 반영하고, install manifest와 managed block에 host, component, LiveCodeMap version과 ownership 정보를 기록한다.
- 동일 option의 재실행은 idempotent해야 한다.
- 상시 instruction은 LiveCodeMap 우선 탐색, freshness와 confidence 구분, 명시적 `--deep`, 일반 file tool 검증 원칙만 짧게 담는다. 긴 설명은 MCP tool description과 선택적 host-native skill에 둔다.
- `SessionStart`는 selected profile, index 존재 여부와 계층별 freshness만 짧게 제공하며 build, UBT, UHT와 deep을 실행하지 않는다.
- `UserPromptSubmit`은 identifier, path 또는 code exploration 의도가 분명할 때만 결정론적으로 local `ask`를 실행한다. 최대 5개 결과와 약 1,500-token 이하 context pack, 초기 foreground budget 2초를 적용하고 외부 LLM은 호출하지 않는다.
- `PostToolUse`는 실제 write/edit tool에만 반응해 변경을 debounce하고 structural/lexical incremental refresh를 async single-flight로 요청한다. 매번 blast 결과를 대화에 주입하지 않는다.
- `Stop`은 dirty/freshness 상태만 검사하고 필요하면 background refresh를 요청한다. turn 종료를 막거나 continuation, test 또는 `livecodemap build`를 강제하지 않는다.
- 모든 hook는 fail-open이며 실패나 timeout이 사용자 작업, tool call 또는 turn 종료를 막지 않는다. 한 줄 diagnostic, recursion guard와 기존 writer lock을 사용한다.
- 공통 상태는 profile, graph generation, structure/compile context/UHT/semantic/retrieval/deep 상태, unit 수, last success와 last attempt를 제공한다.
- Claude Code statusline은 사용자가 명시적으로 선택하고 기존 custom statusline이 없을 때만 설치한다. 기존 statusline은 덮어쓰거나 임의 합성하지 않는다.
- Codex는 공식 hook `statusMessage`와 MCP 결과만 사용하고 Claude Desktop은 Extensions/Connectors 상태와 `check_freshness`를 사용한다.
- 추정 token 절감량은 표시하지 않고 실제 provider usage 기반 통계가 준비됐을 때만 표시한다.
- `livecodemap remove`는 기본적으로 제거 plan만 보여주고 `--yes`가 있어야 반영한다. 특정 host 제거와 `--keep-index`를 지원한다.
- remove는 LiveCodeMap 소유 block, MCP entry, hook와 helper만 제거하며 사용자 내용이 남은 file 자체는 삭제하지 않는다.
- 설치 후 managed 영역이 수정됐다면 자동 삭제하지 않고 conflict로 남긴다. Claude Desktop extension 제거는 앱 UI에서 사용자가 승인한다.
- remove 결과는 `removed`, `unchanged`, `modified-preserved`, `missing`, `manual-action-required`로 구분한다.

### 17.4 확정: 공개 패키징, 명시적 업그레이드와 라이선스

다음 설계는 2026-09-17에 사용자가 명시적으로 승인했다.

- npm organization scope `@livecodemap`를 확보해 launcher package를 `@livecodemap/cli`로 배포한다. 실제 scope 생성과 소유권 확보는 release 선행조건이다.
- `@livecodemap/cli`는 작은 JavaScript launcher이며 `bin` mapping으로 설치 후 실행 명령을 `livecodemap`로 유지한다.
- native binary는 launcher와 version을 lockstep으로 맞춘 exact dependency platform package에 넣는다. 첫 릴리스는 `@livecodemap/win32-x64`만 제공하고 향후 architecture/platform package를 추가한다.
- install 중 GitHub나 별도 server에서 binary를 내려받는 `postinstall` script를 사용하지 않는다. platform package 자체에 검증된 binary와 필요한 runtime을 포함한다.
- 지원하지 않는 OS/architecture는 install 또는 최초 실행에서 명확히 보고한다.
- 하나의 `vX.Y.Z` source tag에서 npm package, Windows standalone ZIP, Claude Desktop `.mcpb`, SHA-256 checksum, SBOM과 `THIRD_PARTY_NOTICES`를 만든다.
- Git clone 설치는 source tag를 checkout해 문서화된 CMake preset으로 build하는 방식을 지원하며 npm과 동일한 CLI, MCP 동작과 version을 제공한다.
- Claude Desktop `.mcpb`는 첫 릴리스에서 앱 UI를 통한 명시적 install/update/remove를 사용하며 official extension directory 등록은 후속 단계로 둔다.
- tag 기반 CI만 release를 생성하고 source build, test, clean-machine npm install, `livecodemap --version`, MCP start와 init/remove smoke test를 통과해야 publish한다.
- npm publish는 OIDC trusted publishing과 provenance를 사용하며 package allowlist로 예상하지 않은 source, fixture, credential과 absolute path 포함을 차단한다.
- 첫 릴리스에서 Authenticode를 필수로 하지 않되 unsigned binary임을 숨기지 않는다. signing identity를 확보하면 Windows binary signing을 추가한다.
- SemVer를 사용한다. 1.0 전에는 breaking change가 minor version을 올리고 patch는 compatible fix만 포함한다.
- launcher, platform package, `.mcpb`와 standalone ZIP은 같은 version을 사용하며 CLI와 별도로 SQLite schema, MCP response schema와 stable ID schema version을 유지한다.
- `livecodemap check`를 사용자가 실행할 때만 npm registry에서 latest version을 확인한다. hook, MCP query와 일반 search는 version check를 위해 network에 접속하지 않는다.
- version check는 repository name, path, query, symbol이나 usage statistics를 보내지 않는다.
- LiveCodeMap는 자체 실행 파일을 교체하지 않는다. npm 사용자는 안내된 `npm install -g` 명령, source 사용자는 tag checkout과 rebuild로 명시적으로 업그레이드한다.
- package upgrade 후 agent wiring을 자동으로 다시 쓰지 않는다. `livecodemap check`가 version 차이를 감지해 `livecodemap init --dry-run`과 명시적 refresh를 안내한다.
- incompatible DB schema를 query 중 자동 migration하거나 삭제하지 않는다. `livecodemap build`가 새 DB generation을 만들고 성공했을 때만 교체한다.
- compatible content-addressed deep cache는 재사용하고 incompatible entry는 무시한다. downgrade도 기존 DB를 파괴하지 않고 `incompatible index`와 필요한 version/build를 안내한다.
- LiveCodeMap 자체 source, documentation과 공개 synthetic fixture의 최종 license는 `Apache-2.0`이다.
- bundled LLVM/Clang 등 third-party component는 원래 license를 유지하고 `THIRD_PARTY_NOTICES`와 배포 artifact에 필요한 attribution을 포함한다.
- Graft source와 비공개 로컬 검증 자료를 모든 공개 artifact에서 제외한다.
- 초기에는 별도 CLA를 요구하지 않으며 contribution이 project license로 제공됨을 `CONTRIBUTING.md`에 명시한다.

### 17.5 확정: 성능·비용 수치와 benchmark task set

다음 설계는 2026-09-17에 사용자가 명시적으로 승인했다.

- benchmark corpus는 공개 synthetic Unreal fixture, `{fmt}` 12.2.0 고정 commit과
  Cesium for Unreal v2.29.1 고정 commit으로 구성한다.
- synthetic fixture는 UHT, RPC, delegate, virtual dispatch, `.inl`, unity/PCH, ignore와 workspace를 포함한 graph 정답과 mutation 정답을 공개한다.
- `{fmt}`는 template, overload, alias, header-only와 Unicode 검색을 평가한다.
- Cesium은 engine-independent public structural lane과 UE가 설치된 전용 Windows runner의 full semantic lane을 분리한다.
- 선택적 비공개 로컬 검증은 repository 밖 또는 명시적으로 ignore된 경로에서 수행하고
  identity, source, 설정, 결과와 파생 정보를 공개 artifact에 포함하지 않는다.
- synthetic fixture의 기대 symbol, relation과 상태는 100% 일치해야 한다.
- 실제 corpus 수동 label 표본의 목표는 confirmed call/reference precision 99% 이상, direct call/reference recall 95% 이상, possible virtual/runtime target recall 90% 이상이다.
- 불확실한 target의 `confirmed` 오승격, 기본 결과의 UHT/unity/PCH 내부 노출과 삭제·ignore file 잔존은 0건이어야 한다.
- 동일 입력 3회 clean build의 fact hash는 100% 일치해야 한다.
- retrieval 목표는 exact identifier/path Top-1 98% 이상, 전체 relevant symbol Top-5 recall 90% 이상, identifier 없는 자연어 Top-5 recall 80% 이상이다.
- paired 한국어/영어 질의의 Top-5 recall 차이는 5%p 이하이고 의도와 무관한 test/helper의 Top-5 진입률은 5% 이하여야 한다.
- Graft 0.18.0과 공통 실행 가능한 C++ 질의에서 LiveCodeMap 품질은 Graft보다 낮지 않아야 한다.
- 공개 query set은 identifier, 영어 기능 설명, acronym과 path를 포함하고 크기와
  manifest를 튜닝 전에 고정한다.
- 기준 환경은 Windows 11, 16 logical core 이상, 32GiB RAM, NVMe SSD와 release에서
  명시한 지원 UE profile이며 실제 hardware와 toolchain을 결과에 기록한다.
- 초기 성능 목표는 no-change freshness p95 100ms, warm search p95 500ms, 기본 1~2 hop traversal p95 1초 이하다.
- 1개 file structural/retrieval 갱신 후 응답 p95는 2초, 단일 `.cpp` semantic 갱신 p95는 10초 이하를 목표로 한다.
- 큰 header fan-out은 2초 안에 stale을 반환하며 query를 오래 막지 않는다.
- worktree seed와 검증은 공개 benchmark corpus에서 1분 이하를 목표로 한다.
- CLI startup과 freshness 확인을 latency에 포함하고 project build required 시간은 분리하며 filesystem cache 상태를 기록한다.
- 10K/50K/100K synthetic symbol 규모에서 build throughput 저하는 30% 이내여야 한다.
- 성능 목표를 맞추기 위해 graph fact를 삭제하지 않는다. 초과하면 원인을 공개하고 저장 구조를 개선한다.
- incremental mutation set은 body-only 수정, `.cpp` 추가·삭제, signature 변경, 작은/큰 header fan-out, reflected header/UHT stale, ignore/profile 변경과 worktree divergence를 포함한다.
- 각 mutation에서 invalidated TU, 교체 contribution, 유지 evidence와 freshness를 gold data와 비교한다.
- deep 비용은 고정 달러 release gate를 두지 않고 provider가 반환한 실제 input/output/cache token, 비용과 p50/p95 시간을 기록한다.
- symbol deep workload의 기본 input 상한은 8K token, output 상한은 512 token이다.
- 변경 없는 deep 재실행의 provider token은 0이어야 하며 symbol body 변경은 해당 symbol과 명시적 dependent aggregate만 무효화한다.
- deep cache hit/miss, 미처리·실패 unit과 중단 이유를 모두 보고하며 local/in-house provider도 같은 workload와 output 검증을 사용한다.
- agent task는 공개 일반 C++와 Unreal corpus에서 navigation 4개, impact 4개,
  patch 4개의 총 12개로 구성한다.
- navigation은 class/file 찾기, call path, virtual/Unreal event boundary와 delegate/BindWidget boundary를 포함한다.
- impact는 signature 변경, module/include dependency, split implementation sibling과
  공개 Unreal UI code path 수집을 포함한다.
- patch는 pinned historical defect, base/override signature, UFUNCTION/RPC pair와 여러 sibling file을 요구하는 변경을 포함한다.
- 각 task에는 expected symbol/file set, 금지 file과 verification test 또는 manual rubric을 둔다.
- agent A/B는 baseline file tool, retrieval only, retrieval+managed instruction, retrieval+instruction+hook의 네 조건이다.
- 동일 model/version, reasoning setting, prompt와 snapshot을 사용하고 독립 clean worktree와 새 session에서 조건 순서를 무작위화해 최소 3회 실행한다.
- 결과가 갈리거나 변동계수가 20%를 넘으면 5회로 확대한다. Cold indexing 비용은 agent task 시간에서 분리하고 deep 효과는 일부 task의 secondary experiment로 측정한다.
- 성공 판정은 test, expected symbol set과 blinded human review를 우선하고 LLM judge만으로 판정하지 않는다.
- 최종 제품 gate는 baseline 대비 correctness 비열화, 대표 task 중앙값 실제 input token 30% 이상 감소, wrong-file edit와 필수 sibling 누락의 비증가다.
- tool call, wall-clock과 불필요한 hook context는 함께 공개하되 첫 릴리스의 독립 gate로 과장하지 않는다.
- public benchmark manifest와 trace는 재현 가능하게 보존하고 비공개 로컬 검증
  artifact는 repository 밖 또는 ignore된 경로에만 둔다.

### 17.6 다음 단계

- 현재 계획 논의 항목은 모두 합의됐다.
- 사용자의 명시적 요청에 따라 이 확정 사항을 `Workspace/Docs/LiveCodeMap-PRD.md`로 작성했다.

## 18. 다음 세션에서 유지할 판단 기준

- LiveCodeMap는 Graft의 약점까지 그대로 복제하지 않는다.
- C++/Unreal에 필요한 보강은 하되 그래프 팽창과 검색 잡음을 통제한다.
- 구조 사실은 최대한 손실 없이 보존한다.
- 불확실한 런타임 관계는 사실인 것처럼 표시하지 않는다.
- 검색 품질과 토큰 절감은 실제 동일 조건 A/B로 증명한다.
- 기능 추가가 코드 탐색이라는 핵심 목적에서 벗어나면 첫 릴리스에서 제외한다.
