# NVOF for PotPlayer

팟플레이어 등 x64 DirectShow 플레이어용 NVIDIA 광학흐름 기반 프레임 보간 필터입니다. 원본 시점의 프레임 사이에 중간 프레임을 추가해 **×2**로 출력합니다.

현재 개발 브랜치는 **0.3.1-overlay.1**입니다. 공개된 [v0.3.0](https://github.com/mkyoon80-alt/nvof-potplayer/releases/tag/v0.3.0)과 구분되는 로컬 시험판이며 아직 업로드하지 않았습니다.

## 자체 포함 버전

- 필터, 등록 도구, 설정 프로그램과 .NET/WPF 런타임을 함께 제공합니다.
- NvOFFRUC.dll, FRUC 연결 DLL과 CUDA 런타임은 포함하거나 호출하지 않습니다.
- GPU 텍스처와 CPU 메모리 입력 모두 Newton 기반 자체 보간을 사용합니다. Cost Map 생성·적용을 끄고 자체 신뢰도 검사를 사용합니다.
- 셰이더는 빌드할 때 미리 컴파일합니다.
- 별도 CUDA Toolkit, Visual Studio, .NET 설치가 필요하지 않습니다.
- Windows 10/11 x64, 지원 NVIDIA GPU·드라이버, 팟플레이어 등 호스트 플레이어는 별도입니다.

## 설치와 연결

1. 팟플레이어와 NVOF 설정 창을 종료하고 설치 프로그램을 실행합니다.
2. 설치 폴더를 확인합니다. 현재 사용자용 필터가 등록되고 기존 설정은 유지됩니다.
3. 팟플레이어 **F5 → 코덱/필터 → 전역 필터 우선 순위 → 시스템 코덱 추가 → NVIDIA Optical Flow for PotPlayer → 최우선 사용**으로 연결합니다.

FRUC/CUDA 바이너리를 배포하지 않으므로 이전의 해당 구성요소 전용 동의 화면이 없습니다. 필요한 SDK·Microsoft·기타 고지는 유지합니다. 동의 화면 제거가 모든 제3자 사용 조건의 면제를 뜻하지 않습니다.

제거는 설정 창의 **연결 설정 → 프로그램 제거**, 시작 메뉴 또는 Windows 설치된 앱에서 진행합니다. 해당 폴더에 관리자용 필터가 등록돼 있으면 필요한 Windows 권한 승인을 요청해 자동 해제합니다. 다른 폴더의 등록과 사용자 설정은 보존합니다.

## 지원 범위

프로그레시브 SDR, NV12 입력과 GPU P010 입력을 대상으로 합니다. P010은 GPU에서 8비트 NV12로 변환하므로 10비트 출력이나 HDR 톤매핑을 제공하지 않습니다. 고정 목표 60/120fps 모드는 아직 없습니다.

머리카락, 겹친 물체, 글자 경계, 연기에는 잔여 아티팩트가 있습니다. 이번 변경은 배경 위에 고정된 불투명 흰 글자·코스 선에서 바깥으로 튀는 밝은 조각을 줄입니다. 배경 왜곡과 머리카락 문제는 남습니다. 세션·GPU 버퍼 재사용 최적화는 유지합니다. 기본 Grid 4·Medium·최대 1920 분석과 Newton 합성은 유지합니다. RTX 5070의 4K60 → 120 실시간 성능이나 플루이드 모션 이상의 품질을 보장하지 않습니다. 실행 파일은 코드 서명되지 않았습니다.

## 문서

- [사용 설명서](docs/manual/index.html)
- [팟플레이어 연결](docs/POTPLAYER_SETUP.ko.md)
- [빌드 및 패키징](docs/BUILD.md)
- [시험판 변경 내역](RELEASE.md)
- [외부 구성요소 고지](THIRD_PARTY_NOTICES.md)

Original project source is under [MIT](LICENSE). Microsoft DirectShow baseclasses, self-contained .NET/WPF and other components retain their own terms. The developer uses official NVIDIA Optical Flow SDK interfaces; SDK terms remain applicable. This project is independent of NVIDIA, Microsoft, PotPlayer, AMD and Smootter.

Grid·Preset·분석 해상도 결과는 [실험 기록](docs/FLOW-PROFILE-TRIAL.md)에 있습니다. 이전 [Cost Map 결합 시험](docs/COST-MAP-FUSION.md)은 개발 이력으로 보존합니다.

세션 재사용의 검증·성능 측정과 남은 동기화 비용은 [최적화 기록](docs/SESSION-REUSE.md)에 있습니다.

경계 보완의 검증과 남은 문제는 [품질 시험 기록](docs/QUALITY-TRIAL.md)에 있습니다.

고정 흰 그래픽 보완의 효과·추가 비용·제한은 [시험 기록](docs/STATIC-OVERLAY-TRIAL.md)에 있습니다.
