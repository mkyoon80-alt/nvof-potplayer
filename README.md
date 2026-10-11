# NVOF for PotPlayer

A standalone DirectShow frame interpolation filter for PotPlayer, powered by NVIDIA Optical Flow and custom Newton-based synthesis. Supports selectable FPS caps or ×2–×5 output, GPU-resident D3D11 processing, and native 10-bit HDR10/HLG signal passthrough. **No VapourSynth, NvOFFRUC.dll, or separate CUDA runtime required.**

**[Download v0.4.2](https://github.com/mkyoon80-alt/nvof-potplayer/releases/tag/v0.4.2)** · **[English manual](https://mkyoon80-alt.github.io/nvof-potplayer/en.html)** · **[한국어 매뉴얼](https://mkyoon80-alt.github.io/nvof-potplayer/)**

팟플레이어 등 x64 DirectShow 플레이어용 NVIDIA 광학흐름 기반 프레임 보간 필터입니다. **FPS 상한 또는 원본 배수**를 골라 원본의 정수배로 보간합니다.

압축을 풀어 사용하는 Windows x64 스탠드얼론 ZIP입니다. 필터·등록 도구·설정창과 .NET/WPF 런타임, 한국어·영어 오프라인 매뉴얼을 포함합니다. NVIDIA 드라이버와 팟플레이어는 별도로 필요합니다.

## v0.4.2의 변경 사항

- **FPS 상한:** 60 / 72 / 75 / 100 / 120 / 144 / 240fps. 상한 안에서 가능한 가장 큰 정수배로 출력합니다.
- **원본 배수:** ×2 / ×3 / ×4 / ×5를 직접 선택합니다. 이 모드에는 FPS 상한을 적용하지 않습니다.
- **세로 해상도 조건:** 전체 / 1080p 이하 / 1440p 이하. 원본 FPS 조건과 함께 보간할 영상을 선택합니다.
- 30.003fps처럼 표준값과 조금 다른 영상도 상한 계산에 0.1% 여유를 두어 최대 60fps에서 ×2, 최대 120fps에서 ×4로 처리합니다. 원본의 재생 시간은 유지합니다.
- 일부 **YouTube AV1 HDR** 영상에서 색 정보가 SDR로 전달되어 색이 물빠지던 문제를 수정했습니다. 실제 팟플레이어 재생에서 정상 색상을 확인했습니다.
- 검은 배경에서 빠르게 움직이는 원·사각형의 경계와 모서리 처리를 개선했습니다. 모든 장면의 아티팩트를 제거하는 것은 아닙니다.

24fps를 최대 60fps로 설정하면 **48fps**입니다. 72fps로 보려면 최대 72fps 또는 원본 ×3을 선택하세요. 정확한 목표 FPS로 변환하는 방식이 아니며, 높은 출력 FPS의 실시간 유지 여부는 GPU·영상·렌더러 설정에 따라 달라집니다.

## 처음 연결하기

1. ZIP을 계속 사용할 폴더에 풉니다. 실행 파일·필터·런타임 파일을 함께 둡니다.
2. `NvofControl.exe` → **연결 설정 → 필터 등록**을 누릅니다.
3. 팟플레이어 **F5 → 코덱/필터 → 전역 필터 우선 순위 → 시스템 코덱 추가**에서 **NVIDIA Optical Flow for PotPlayer**를 추가하고 **최우선 사용**을 선택합니다.
4. **F5 → 코덱/필터 → 비디오 코덱 → 내장 코덱/DXVA 설정**에서 **하드웨어 가속(DXVA)**과 **D3D11 DXVA 우선 사용**을 켭니다. **DXVA2 Copy-Back**은 **사용하지 않음**으로 둡니다. [설정 화면과 항목별 안내](https://mkyoon80-alt.github.io/nvof-potplayer/#hardware)를 참고하세요.
5. **F5 → 영상 → 영상 출력 장치**에서 **내장 Direct3D 11 영상 출력 장치**를 선택합니다.
6. 보간 설정을 선택한 뒤 영상을 다시 엽니다. 진단의 **GPU 메모리로 직접 전달** 표시를 확인하세요. 다른 보간 필터는 동시에 사용하지 않습니다.

10bit/HDR에는 내장 D3D11 디코더·렌더러의 네이티브 GPU 전달 경로를 사용합니다. 자세한 연결 설정과 동작 확인은 [사용 설명서](https://mkyoon80-alt.github.io/nvof-potplayer/)를 확인하세요.

## 업데이트와 제거

기존 스탠드얼론은 팟플레이어와 설정창을 닫고 폴더를 백업한 뒤 같은 위치에 교체합니다. **기존 `NvofPotPlayer.ini`를 보존하세요.** 이전 최대 60/120fps 선택은 유지됩니다.

폴더를 옮기거나 삭제하려면 팟플레이어를 종료하고 **연결 설정 → 필터 등록 해제**를 누릅니다. 해제 성공을 확인한 뒤 설정창을 닫고 폴더를 이동·삭제하세요. 이동했다면 새 위치에서 다시 등록합니다. 관리자용 등록은 해제할 때 Windows 권한 승인이 필요합니다. 다른 폴더의 등록은 해제하지 않습니다.

v0.3.0 등 설치 프로그램 버전에서 전환할 때는 설정 파일을 백업하고 Windows **설치된 앱**에서 이전 버전을 제거한 뒤 새 ZIP을 별도 폴더에 풀어 등록하세요.

## 지원 범위

- Windows 10 2004 이상 / Windows 11 x64, Optical Flow 지원 NVIDIA GPU와 드라이버. 실제 재생 검증 장비는 RTX 5090입니다.
- 네이티브 D3D11 경로: 제한 범위 BT.709 SDR(NV12/P010), BT.2020 HDR10(PQ)·HLG(P010). P010 원본과 중간 프레임을 10bit로 유지합니다.
- HDR10 파일과 YouTube AV1 HDR의 실제 재생 색상을 확인했습니다. HLG는 색 정보 전달 검사를 완료했으며 실제 HLG 영상·화면 검증은 남아 있습니다.
- Dolby Vision·HDR10+ 동적 메타데이터는 지원하지 않습니다. HDR10 호환 기본 영상은 HDR10으로 처리합니다. 톤매핑과 화면 HDR 전환은 플레이어·렌더러가 담당합니다.
- CPU 메모리 입력은 NV12 SDR을 지원합니다. 인터레이스·다중 GPU 구성은 검증하지 않았습니다.

Newton 기반 자체 보간, NVOFA 세션·GPU 버퍼 재사용, 빌드 시 셰이더 사전 컴파일을 사용합니다. 기본 분석은 Grid 4·Medium·긴 변 최대 1920이며 Cost Map·Temporal Hints는 꺼져 있습니다. NvOFFRUC.dll과 CUDA 런타임을 포함하거나 호출하지 않습니다. 별도 .NET·CUDA Toolkit 설치가 필요하지 않습니다. 실행 파일은 코드 서명되지 않았습니다.

## 문서

- [English user guide](https://mkyoon80-alt.github.io/nvof-potplayer/en.html) · [English offline guide](docs/manual/en.html). The controller UI is currently Korean; the English guide includes matching Korean button labels.
- [온라인 사용 설명서](https://mkyoon80-alt.github.io/nvof-potplayer/) · [오프라인 설명서](docs/manual/index.html)
- [팟플레이어 연결](docs/POTPLAYER_SETUP.ko.md)
- [빌드 및 패키징](docs/BUILD.md)
- [릴리스 변경 내역](RELEASE.md)
- [정수배 출력 검증](docs/INTEGER-RATE-TRIAL.md) · [HDR 구현 범위](docs/HDR-NATIVE-TRIAL.md)
- [외부 구성요소 고지](THIRD_PARTY_NOTICES.md)

Original project source is under [MIT](LICENSE). Microsoft DirectShow baseclasses, self-contained .NET/WPF and other components retain their own terms. The developer uses official NVIDIA Optical Flow SDK interfaces; SDK terms remain applicable. This project is independent of NVIDIA, Microsoft, PotPlayer, AMD and Smootter.
