# NVOFA 세션 재사용 — 0.3.1-opt.1

## 적용 범위

연속 재생에서는 세션과 분석·합성 텍스처를 유지한다. 입력은 6개 NV12 버퍼를 (0,1), (2,3), (4,5) 순서로 사용한다. 매 쌍의 두 입력 모두 다시 업로드/축소하므로 이전 프레임 포인터가 같다는 이유로 내용을 재사용하지 않는다.

NVOFA에 등록하는 입력·흐름 출력·선택적 Cost 출력은 `D3D11_RESOURCE_MISC_SHARED`로 생성한다. 외부 프로세스에 공유 핸들을 전달하지 않는다. 같은 즉시 컨텍스트와 기존 엔진 잠금을 사용하며 매 execute 전에 이전 합성 읽기와 현재 입력 쓰기의 완료를 기다린다. 중간 합성용 텍스처는 일반 D3D11 자원이다.

Temporal Hints는 계속 비활성화한다. Cost Map 생성·적용도 기본 경로에서 비활성화한다. Newton 합성, 보호 임계값, 프레임 시간, 원본 반복 판단, 분석 Grid·Preset·해상도는 바꾸지 않았다.

탐색, 장면 전환, 동일 프레임으로 보간 생략, 입력 연속성 상실, 해상도 변경 및 오류는 기존 방식대로 세션을 파괴한다. 준비 중 예외도 전체 상태를 초기화한다. 모드 `MotionSessionMode::fresh`는 비교용 기존 경로이며 사용자 설정에 노출하지 않는다.

## 원인 분리 실험

RTX 5090 / 617.42에서 관찰했다. 드라이버 내부의 정확한 캐시 동작을 규명했다는 주장은 아니다.

| 변경 | 관찰 |
|---|---|
| 일반 텍스처 그대로 재사용, 힌트 OFF, 완료 대기 | 원시 벡터와 합성 오류 재현 |
| 일반 흐름 출력 초기화 또는 등록 해제·재등록 | 오류 지속 |
| 세션 유지, 입력·흐름 출력 새로 할당 | 96쌍 원시 벡터·출력 일치 |
| 세션·입력 유지, 흐름 출력만 새로 할당 | 96쌍 원시 벡터·출력 일치 |
| 공유 흐름 출력만 재사용 | 합성 시험 통과, Kokoore 시작 구간에서 주기적 차이 |
| 입력과 흐름 출력 모두 공유 자원으로 재사용 | 실제 영상 10구간과 원시 벡터 비교 통과 |

세션 내부 힌트가 유일한 원인이라는 가설은 지지되지 않았다. 이번 장비에서는 반복 사용되는 D3D11 자원의 NVOFA 상호 운용 경로를 수정하는 것이 필요했다. 다른 GPU·드라이버에서 동일 결과를 보장하지 않는다.

## 검증

- `native_session_reuse`: 원시 전·후방 벡터와 합성 픽셀을 fresh 경로와 비교한다. 기준 실행 전체를 먼저 끝낸 뒤 재사용 실행을 하며, 원시 벡터 읽기는 합성 이후에 수행해 테스트 자체가 추가 동기화를 제공하지 않도록 했다. 속도 변화, 방향 반전, 입력 풀 순환, 반복 초기화를 포함한다.
- `native_synthesis_compare`: 긴 세로 이동, 서로 반대 방향으로 움직이는 물체, 밝기 변화, 여러 중간 시점, 100회 리셋, 방향 반전, 보관한 출력 불변성을 검증한다. 이전 FRUC 백엔드는 제거했다.
- 기존 CPU/NV12 및 GPU/P010 필터 연결·탐색·동시 flush/stop, 프레임 수명, 장면 보호, 출력 버퍼 경쟁 검사 통과.
- `compare-session-reuse.py`: 보관된 원본 영상 입력을 flow.1 실행 파일과 새 실행 파일에 각각 통과시켜 전체 NV12 출력 SHA-256, 시간, 장면/반복 판단을 비교한다. 일치한 임시 출력은 삭제하고 로그·해시·시간만 남긴다. 실제 원본 영상은 저장소에 포함하지 않는다.

```powershell
build/opt1/native/Release/native_session_reuse.exe 1 128
build/opt1/native/Release/native_synthesis_compare.exe build/opt1/absent-runtime 3840 2160 24
python tools/compare-session-reuse.py --baseline build/flow1/native/Release/video_x2_repro.exe --candidate build/opt1/native/Release/video_x2_repro.exe --output build/session-comparison
```

## 측정 범위와 남은 비용

프레임 쌍 처리 호출부터 GPU 완료까지의 벽시계 시간이다. 처음 5쌍을 제외한 구간 평균이며 영상 디코딩·파일 쓰기·표시 시간은 포함하지 않는다. 실제 플레이어 전체 FPS 측정이 아니다. 대상 원본은 주로 약 24fps이며 4K 자료의 크기는 3840×1604이다. 분석은 최대 긴 변 1920이다.

매 쌍의 입력 완료 대기, 광학흐름 계산 후 신뢰도 버퍼 `Map()` 대기는 남아 있다. 이번 성능 개선은 주로 세션 생성/등록/할당 반복을 줄인 효과다. 첫 쌍은 세션을 생성하므로 재생 시작 지연 개선을 같은 비율로 주장하지 않는다. 5070의 4K60 → 120 보장은 하지 않는다.

NVIDIA의 [효율적인 NVOF API 사용 가이드](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html#guidelines-for-efficient-usage-of-nvof-api)는 세션 유지·입력 풀과 연속 프레임의 힌트 사용을 권한다. 이번에는 품질 비교를 위해 힌트 OFF를 유지했다. 힌트 ON 및 대기 축소는 별도의 변경으로 검증해야 한다.

## 측정 결과 (최종 빌드)

기존 `0.3.1-flow.1` 실행 파일과 비교했다. 10구간 모두 전체 출력·시간·판단이 일치했다. 774개 입력 쌍, 출력 1,568프레임이다.

| 구간 | 기존 평균 ms | opt.1 평균 ms | 감소 |
|---|---:|---:|---:|
| kokoore-opening | 4.235 | 2.336 | 44.9% |
| kokoore-80 | 4.313 | 2.486 | 42.3% |
| jishou-hair | 4.232 | 2.399 | 43.3% |
| jishou-51 | 4.459 | 2.449 | 45.1% |
| jishou-320 | 4.673 | 2.745 | 41.3% |
| jishou-1404 | 4.189 | 2.350 | 43.9% |
| f1-title | 4.331 | 2.334 | 46.1% |
| f1-84 | 4.342 | 2.362 | 45.6% |
| f1-90 | 4.928 | 2.882 | 41.5% |
| f1-130 | 4.299 | 2.374 | 44.8% |

측정 로그와 출력 해시는 `build/opt1/videos/summary.json`, 최종 실행 로그는 `build/opt1/video-comparison-final.log`에 보관했다. 합성 회귀 결과는 `build/opt1/final-session.log`, `build/opt1/final-engine.log`, 통합 검증은 `build/opt1/validation-final.log`에 있다.
