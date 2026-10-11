# NVOF for PotPlayer

A standalone DirectShow frame interpolation filter for PotPlayer, powered by NVIDIA Optical Flow and custom Newton-based synthesis. Choose FPS caps or ×2–×5 output, with GPU interpolation for native D3D11 and supported software-decoded video, including 10-bit HDR processing. **No VapourSynth, NvOFFRUC.dll, or separate CUDA runtime required.**

**[Download v0.4.6](https://github.com/mkyoon80-alt/nvof-potplayer/releases/tag/v0.4.6)** · **[English manual](https://mkyoon80-alt.github.io/nvof-potplayer/en.html)** · **[한국어 매뉴얼](https://mkyoon80-alt.github.io/nvof-potplayer/)**

팟플레이어 등 x64 DirectShow 플레이어용 NVIDIA 광학흐름 기반 프레임 보간 필터입니다. **FPS 상한 또는 원본 배수**를 골라 원본의 정수배로 보간합니다.

압축을 풀어 사용하는 Windows x64 스탠드얼론 ZIP입니다. 필터·등록 도구·설정창과 .NET/WPF 런타임, 한국어·영어 오프라인 매뉴얼을 포함합니다. NVIDIA 드라이버와 팟플레이어는 별도로 필요합니다.

## v0.4.6의 변경 사항

- **SW 최초 재생 보간 수정:** HW로 시작한 뒤 SW로 바꾸면 동작하지만, 처음부터 HW 가속을 끄고 열면 보간이 빠지던 문제를 수정했습니다. 이제 HW를 먼저 켤 필요 없이 SW 상태에서 바로 열 수 있습니다.
- **재생·전환 검증:** 팟플레이어를 완전히 종료한 뒤 SW로 다시 시작하는 실제 재생을 확인했습니다. HW/SW 전환과 기존 10bit·HDR 경로도 검사했습니다.
- **한·영 매뉴얼 업데이트:** SW 시작 방법과 문제 해결 안내를 반영했습니다. [전체 변경 내역](RELEASE.md) · [SW 경로 검증](docs/SW-DECODING.md)

FPS 상한 **60 / 72 / 75 / 100 / 120 / 144 / 240fps**, 원본 배수 **×2 / ×3 / ×4 / ×5**, 원본 FPS·세로 해상도 조건은 그대로 사용할 수 있습니다.

24fps를 최대 60fps로 설정하면 **48fps**입니다. 72fps로 보려면 최대 72fps 또는 원본 ×3을 선택하세요. 정확한 목표 FPS로 변환하는 방식이 아니며, 높은 출력 FPS의 실시간 유지 여부는 GPU·영상·렌더러 설정에 따라 달라집니다.

## 처음 연결하기

1. ZIP을 계속 사용할 폴더에 풉니다. 실행 파일·필터·런타임 파일을 함께 둡니다.
2. `NvofControl.exe` → **연결 설정 → 필터 등록**을 누릅니다.
3. 팟플레이어 **F5 → 코덱/필터 → 전역 필터 우선 순위 → 시스템 코덱 추가**에서 **NVIDIA Optical Flow for PotPlayer**를 추가하고 **최우선 사용**을 선택합니다.
4. **F5 → 코덱/필터 → 비디오 코덱 → 내장 코덱/DXVA 설정**에서 **하드웨어 가속(DXVA)**과 **D3D11 DXVA 우선 사용**을 켭니다. **DXVA2 Copy-Back**은 **사용하지 않음**으로 둡니다. [설정 화면과 항목별 안내](https://mkyoon80-alt.github.io/nvof-potplayer/#hardware)를 참고하세요.
5. **F5 → 영상 → 영상 출력 장치**에서 **내장 Direct3D 11 영상 출력 장치**를 선택합니다.
6. CPU로 디코딩되는 인터레이스 영상은 **재생 화면 우클릭 → 영상 → 디인터레이스 → 사용하지 않음**으로 둡니다. [설정 이유와 화면](https://mkyoon80-alt.github.io/nvof-potplayer/#deinterlace)을 참고하세요.
7. 보간 설정을 선택한 뒤 영상을 다시 엽니다. 네이티브 디코딩은 진단의 **GPU 메모리로 직접 전달**, CPU 입력은 **실제 입력·출력 형식과 보간 FPS**를 확인하세요. 다른 보간 필터는 동시에 사용하지 않습니다.

하드웨어 디코딩이 가능한 영상에는 네이티브 GPU 전달 경로를 권장합니다. 네이티브 DXVA는 필수가 아니며, 지원되는 CPU NV12 SDR과 10bit 입력도 GPU에서 보간합니다. 내장 D3D11 렌더러가 필요합니다. 자세한 연결 설정과 동작 확인은 [사용 설명서](https://mkyoon80-alt.github.io/nvof-potplayer/)를 확인하세요.

## 업데이트와 제거

기존 스탠드얼론은 팟플레이어와 설정창을 닫고 폴더를 백업한 뒤 같은 위치에 교체합니다. **기존 `NvofPotPlayer.ini`를 보존하세요.** 이전 최대 60/120fps 선택은 유지됩니다.

폴더를 옮기거나 삭제하려면 팟플레이어를 종료하고 **연결 설정 → 필터 등록 해제**를 누릅니다. 해제 성공을 확인한 뒤 설정창을 닫고 폴더를 이동·삭제하세요. 이동했다면 새 위치에서 다시 등록합니다. 관리자용 등록은 해제할 때 Windows 권한 승인이 필요합니다. 다른 폴더의 등록은 해제하지 않습니다.

v0.3.0 등 설치 프로그램 버전에서 전환할 때는 설정 파일을 백업하고 Windows **설치된 앱**에서 이전 버전을 제거한 뒤 새 ZIP을 별도 폴더에 풀어 등록하세요.

## 지원 범위

- Windows 10 2004 이상 / Windows 11 x64, Optical Flow 지원 NVIDIA GPU와 드라이버. 실제 재생 검증 장비는 RTX 5090입니다.
- 네이티브 D3D11 경로: 제한 범위 BT.709 SDR(NV12/P010), BT.2020 HDR10(PQ)·HLG(P010). P010 원본과 중간 프레임을 10bit로 유지합니다.
- HDR10 파일과 YouTube AV1·VP9 HDR의 실제 재생 색상을 확인했습니다. HEVC HLG는 실제 재생·신호 전달을 확인했으며 별도 화면 색상 비교는 남아 있습니다.
- Dolby Vision은 디코더가 제공한 메타데이터의 전달을 지원합니다. Profile 8.1에서 확인했으며 Profile 5·7 등의 전체 처리나 화면의 Dolby Vision 모드는 보장하지 않습니다. HDR10+ 동적 메타데이터는 지원하지 않습니다. 톤매핑과 화면 HDR 전환은 플레이어·렌더러가 담당합니다.
- CPU 입력은 NV12 SDR과 P010/P016/P210/Y210을 지원합니다. 디코더가 NV12로 전달하면 원본 파일이 10bit여도 8bit 경로로 처리하며, 손실된 정밀도를 복원하지 않습니다. 10bit 경로는 P010 4:2:0으로 출력하며 P016의 상위 10bit를 사용합니다. CPU 10bit 프로그레시브와 1080i 인터레이스를 확인했습니다. 일반 텔레시네·단일 필드·반복 필드 및 다중 GPU 구성은 검증 범위 밖입니다.

Newton 기반 자체 보간, NVOFA 세션·GPU 버퍼 재사용, 빌드 시 셰이더 사전 컴파일을 사용합니다. 기본 분석은 Grid 4·Medium·긴 변 최대 1920이며 Cost Map·Temporal Hints는 꺼져 있습니다. NvOFFRUC.dll과 CUDA 런타임을 포함하거나 호출하지 않습니다. 별도 .NET·CUDA Toolkit 설치가 필요하지 않습니다. 실행 파일은 코드 서명되지 않았습니다.

전체화면 HDR 색상이 과하게 진해지는 일부 환경의 제보는 아직 재현되지 않았습니다. v0.4.6는 이 문제나 모든 4K 프레임 저하가 해결됐다고 주장하지 않습니다. [비교 방법](https://mkyoon80-alt.github.io/nvof-potplayer/#troubleshoot)을 참고하세요.

## 문서

- [English user guide](https://mkyoon80-alt.github.io/nvof-potplayer/en.html) · [English offline guide](docs/manual/en.html). The controller UI is currently Korean; the English guide includes matching Korean button labels.
- [온라인 사용 설명서](https://mkyoon80-alt.github.io/nvof-potplayer/) · [오프라인 설명서](docs/manual/index.html)
- [팟플레이어 연결](docs/POTPLAYER_SETUP.ko.md)
- [빌드 및 패키징](docs/BUILD.md)
- [릴리스 변경 내역](RELEASE.md)
- [정수배 출력 검증](docs/INTEGER-RATE-TRIAL.md) · [HDR 구현 범위](docs/HDR-NATIVE-TRIAL.md)
- [CPU 10bit·인터레이스](docs/CPU-10BIT.md) · [VP9 HDR](docs/VP9-HDR.md) · [Dolby Vision·HLG](docs/DOLBY-VISION-HLG.md)
- [외부 구성요소 고지](THIRD_PARTY_NOTICES.md)

Original project source is under [MIT](LICENSE). Microsoft DirectShow baseclasses, self-contained .NET/WPF and other components retain their own terms. The developer uses official NVIDIA Optical Flow SDK interfaces; SDK terms remain applicable. This project is independent of NVIDIA, Microsoft, PotPlayer, AMD and Smootter.
