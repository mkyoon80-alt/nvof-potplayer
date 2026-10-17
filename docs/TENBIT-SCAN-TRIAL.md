# v0.4.12: HEVC·AV1 10비트 출력 연결 검증

2026-10-17 · Windows 10 x64 · RTX 5090 · PotPlayer 260819

## 달라진 동작

팟플레이어 **F5 → 영상 → 비디오 버퍼 포맷 옆 ‘10비트 출력’**을 켰을 때, 일부 HEVC·AV1 프로그레시브 영상이 인터레이스 입력으로 표시되었습니다. v0.4.11에서는 원본 FPS를 두 배로 계산하거나 네이티브 GPU 입력 연결을 거절할 수 있었습니다.

v0.4.12는 디코더의 원본 헤더를 연결 단계에서 확인합니다. HEVC는 SPS의 프로그레시브·프레임 전용 제약과 필드 신호를, AV1은 유효한 시퀀스 헤더와 프로그레시브 표시·크기·프레임 주기를 함께 확인합니다. 근거가 없거나 서로 충돌하면 기존 처리를 유지합니다. 실제 필드 영상의 FPS를 임의로 절반으로 줄이지 않습니다.

이 판별은 입력 연결·형식 변경 시 적용됩니다. 매 프레임 보간 계산이나 셰이더, 품질 수치, 재생·탐색 대기열, HDR 메타데이터 전달은 변경하지 않았습니다. 기본 INI와 진단 기본값도 그대로입니다.

일반 출력과 해상도별 출력에 **최대 160fps**를 추가했습니다. 원본의 정수배를 유지하므로 24fps는 144fps, 30fps는 150fps, 60fps는 120fps로 출력합니다. 기존에 선택한 상한과 설정은 유지됩니다.

## 확인 결과

160fps 추가 검증에서는 24/25/30/50/60/80fps·23.976fps, 해상도별 상한, AV1 10bit 입력의 GPU 그래프 9개가 통과했습니다. 80fps → 160fps와 각 원본의 정수배 출력·타임스탬프를 확인했고, 설정창의 선택·저장·재읽기 검사도 통과했습니다. 기존 설정값을 바꾸는 마이그레이션은 없습니다.

| 확인 항목 | 결과 |
| --- | --- |
| 실제 HEVC 1080p 23.976fps, 10비트 출력, 최대 120fps | 공개 v0.4.11에서 입력 47.952 → 출력 95.904 재현. 수정본에서 23.976 → 119.88, P010 출력·보간 동작 확인 |
| 실제 AV1 8K HDR 60fps, 10비트 출력, 최대 240fps | 내장 디코더의 D3D11 P010 GPU 표면 수신, HDR10, 60 → 240fps, 보간 프레임 생성 확인. GPU 디코더 사용률도 함께 확인 |
| 27가지 입력 조건을 이전·수정본으로 각각 실행 | 총 54회 통과. 수정 대상 5개는 정상 프로그레시브 대조군과 픽셀 해시·출력 시각이 일치 |
| 나머지 22가지 조건 | 정상 입력, TFF/BFF 필드, 혼합·불명·상충·잘린 헤더, AV1 크기·주기·GUID 불일치 등의 픽셀 해시·출력 시각이 이전 배포본과 일치 |
| 실제 1080i 원본에서 디코딩한 P210 24프레임 | AVC 필드 처리 후 전체 출력 픽셀 해시·시각이 이전 배포본과 일치 |
| AV1 잘못된 필드 표시가 있는 8K GPU 연결 | 이전 배포본의 연결 실패 재현. 수정본의 60 → 240fps, 출력 시각·EOS·탐색·재시작 검사 통과 |
| HDR·입력 전환·재생 상태 | PQ/HLG, CPU NV12·P010·P210·Y210, 필드 처리, HW/SW 전환, 잘못된 GPU 힌트, 일시정지·재개·반복 연결 검사 통과 |
| 기본 자동 검사 | 25개 통과. HEVC 판별 192개 조합, 잘린 헤더·상충 SPS·4,000개 변형 입력 포함 |
| 설정 프로그램 | 독립 실행 .NET/WPF 런타임과 설정 자체 검사 통과 |

실제 재생은 사용자 설치본과 분리한 팟플레이어로 확인했습니다. HEVC 실제 재생 비교는 동일한 10비트 출력·자동 디코더 조건이며, 소프트웨어 입력의 회귀 검사는 별도 DirectShow CPU 입력 그래프에서 수행했습니다. 그래프의 GPU 메모리 전달 표시는 하드웨어 디코딩 여부와 동일한 의미가 아니므로 실제 디코딩은 별도 확인했습니다.

짧은 재생·회귀 검사로 모든 영상의 장시간 무드랍이나 동일 소비전력을 보장하지 않습니다. 기존 초기 드랍, 어려운 장면의 보간 흔적, HDR 과밝기 문제는 이번 수정 범위에 포함되지 않습니다. 10비트 출력은 HDR 전환 기능이 아닙니다.

## 업데이트 후 확인

1. 팟플레이어와 설정창을 닫고 기존 폴더를 백업합니다.
2. 프로그램 파일을 교체하되 기존 `NvofPotPlayer.ini`는 보존합니다.
3. 영상을 다시 열어 NVOF 진단의 원본 FPS·출력 FPS·P010 10bit를 확인합니다.
4. 하드웨어 디코딩은 팟플레이어 Tab 정보의 H/W·Native D3D11 DXVA 표시도 확인합니다.

## English

Version 0.4.12 resolves false interlace flags before frame-rate selection and native GPU negotiation. Correction requires reliable progressive source evidence; true fields, unknown facts and malformed or conflicting headers keep their previous handling. Synthesis, shaders, HDR metadata, scheduling and default settings are unchanged.

On Windows 10 / RTX 5090 / PotPlayer 260819, actual 23.976fps HEVC playback changed from the reproduced 47.952 → 95.904 error to 23.976 → 119.88 at a 120fps cap. Actual 8K60 AV1 HDR used D3D11 P010 input and 240fps interpolation with 10-bit output enabled. Hardware decode activity was checked separately from the filter's GPU transport status.

All 54 paired graph runs passed. The five corrected cases matched the progressive controls exactly; the other 22 cases retained identical output hashes and timestamps. Real decoded 1080i P210 frames also matched the previous release. GPU negotiation, HDR, HW/SW transitions and lifecycle regressions passed, as did 25 core tests and the controller self-test. The HEVC player comparison used automatic decoder selection; software-input regression coverage came from separate CPU-input DirectShow graphs.

These are bounded correctness checks, not a guarantee of zero long-run drops or identical power on every source. Preserve `NvofPotPlayer.ini`, fully restart the player after updating, and verify the decoder, formats and FPS. HDR overbrightness and existing startup drops are outside this fix.

The new 160fps cap passed nine GPU graph checks covering 24/25/30/50/60/80fps, 23.976fps, per-resolution selection and AV1 10-bit input. Output retained integer cadence, including 80fps → 160fps. Controller selection and persistence checks passed without migrating existing settings.
