# NVOF for PotPlayer

팟플레이어 등 x64 DirectShow 플레이어용 NVIDIA 광학흐름 기반 프레임 보간 필터입니다. **최대 60fps / 최대 120fps** 안에서 원본의 정수배로 보간합니다. 예를 들어 23.976fps는 각각 47.952fps(×2) / 119.880fps(×5)로 출력합니다.

**[v0.4.0 다운로드](https://github.com/mkyoon80-alt/nvof-potplayer/releases/tag/v0.4.0)** · **[온라인 사용 설명서](https://mkyoon80-alt.github.io/nvof-potplayer/)**

이번 버전은 압축을 풀어 사용하는 스탠드얼론 ZIP입니다. 프로그램 안에 오프라인 매뉴얼이 포함되어 있고, 매뉴얼만 HTML·ZIP으로 따로 받을 수도 있습니다.

## 자체 포함 버전

- 필터, 등록 도구, 설정 프로그램과 .NET/WPF 런타임을 함께 제공합니다.
- NvOFFRUC.dll, FRUC 연결 DLL과 CUDA 런타임은 포함하거나 호출하지 않습니다.
- GPU 텍스처와 CPU 메모리 입력 모두 Newton 기반 자체 보간을 사용합니다. Cost Map 생성·적용을 끄고 자체 신뢰도 검사를 사용합니다.
- 셰이더는 빌드할 때 미리 컴파일합니다.
- 별도 CUDA Toolkit, Visual Studio, .NET 설치가 필요하지 않습니다.
- Windows 10/11 x64, 지원 NVIDIA GPU·드라이버, 팟플레이어 등 호스트 플레이어는 별도입니다.

## 스탠드얼론 ZIP 연결

1. ZIP을 계속 사용할 폴더에 풉니다. 실행 파일·필터·런타임 파일은 함께 둡니다.
2. `NvofControl.exe` → **연결 설정 → 필터 등록**을 누릅니다. 별도 .NET 설치는 필요하지 않습니다.
3. 팟플레이어 **F5 → 코덱/필터 → 전역 필터 우선 순위 → 시스템 코덱 추가 → NVIDIA Optical Flow for PotPlayer → 최우선 사용**으로 연결합니다.

폴더를 옮기거나 삭제하려면 팟플레이어를 종료하고 **연결 설정 → 필터 등록 해제**를 누른 뒤 설정창을 닫습니다. 이동한 경우 새 폴더에서 다시 등록합니다. **현재 폴더 열기**로 위치를 확인할 수 있습니다. 관리자용 등록이 있으면 해제할 때 Windows 권한 승인을 요청합니다. 다른 폴더의 등록은 해제하지 않습니다.

설치 프로그램으로 설치한 기존 구성에는 **프로그램 제거**도 표시됩니다. 스탠드얼론 ZIP에는 이 버튼을 표시하지 않습니다. FRUC/CUDA 재배포 전용 동의 화면은 없으며, 남은 SDK·Microsoft·기타 구성요소 고지는 유지합니다.

기존 v0.3.0 설치본에서 옮길 때는 팟플레이어와 설정창을 닫고 `NvofPotPlayer.ini`를 백업한 뒤, Windows **설치된 앱**에서 이전 버전을 제거하세요. 새 ZIP을 별도 폴더에 풀고 필요한 설정을 복원한 후 필터를 등록합니다. 관리자용 등록이 있으면 제거 과정의 Windows 권한 승인을 허용하세요. 팟플레이어의 필터 선택과 최우선 사용도 다시 확인합니다.

## 지원 범위

프로그레시브 제한 범위 BT.709 SDR(NV12/P010)과 BT.2020 HDR10(PQ)·HLG(P010)를 대상으로 합니다. 네이티브 D3D11 P010 경로는 원본과 중간 프레임을 10bit로 출력합니다. 움직임 분석만 NV12를 사용하며 출력 정밀도를 8bit로 낮추지 않습니다. HDR 톤매핑은 플레이어·렌더러가 담당합니다. CPU 전달 경로는 NV12 SDR만 지원합니다. Dolby Vision·HDR10+ 동적 메타데이터는 지원하지 않습니다. HDR10 호환 기본 영상은 HDR10으로 처리합니다. 정밀도·색 정보·전달 검사와 제공된 HDR10 호환 파일의 실제 팟플레이어 정상 색상 재생을 확인했습니다. HLG는 신호 전달 검증을 통과했으며 실제 HLG 영상·화면 검증은 남아 있습니다. 상한 안에서 두 배 이상으로 늘릴 수 없는 영상은 원본으로 재생합니다. 상한보다 빠른 원본의 프레임을 줄이지 않습니다. 설정을 바꾸면 영상을 다시 열어 적용합니다.

머리카락, 겹친 물체, 글자 경계, 연기에는 잔여 아티팩트가 있습니다. 고정 흰 글자·코스 보호를 유지하면서 검은 배경의 스크롤 크레딧이 다른 줄과 뒤섞이는 오류를 줄입니다. 검은 배경에서 함께 움직이는 글자 묶음을 검증해 공통 이동으로 합성합니다. 검사에 통과하지 않는 영역과 화면 경계에는 잔여 깨짐이 있을 수 있습니다. 세션·GPU 버퍼 재사용 최적화는 유지합니다. 기본 Grid 4·Medium·최대 1920 분석과 Newton 합성은 유지합니다. RTX 5070의 4K60 → 120 실시간 성능이나 플루이드 모션 이상의 품질을 보장하지 않습니다. 실행 파일은 코드 서명되지 않았습니다.

## 문서

- [온라인 사용 설명서](https://mkyoon80-alt.github.io/nvof-potplayer/) · [오프라인 설명서](docs/manual/index.html)
- [팟플레이어 연결](docs/POTPLAYER_SETUP.ko.md)
- [빌드 및 패키징](docs/BUILD.md)
- [릴리스 변경 내역](RELEASE.md)
- [외부 구성요소 고지](THIRD_PARTY_NOTICES.md)

Original project source is under [MIT](LICENSE). Microsoft DirectShow baseclasses, self-contained .NET/WPF and other components retain their own terms. The developer uses official NVIDIA Optical Flow SDK interfaces; SDK terms remain applicable. This project is independent of NVIDIA, Microsoft, PotPlayer, AMD and Smootter.

Grid·Preset·분석 해상도 결과는 [실험 기록](docs/FLOW-PROFILE-TRIAL.md)에 있습니다. 이전 [Cost Map 결합 시험](docs/COST-MAP-FUSION.md)은 개발 이력으로 보존합니다.

세션 재사용의 검증·성능 측정과 남은 동기화 비용은 [최적화 기록](docs/SESSION-REUSE.md)에 있습니다.

경계 보완의 검증과 남은 문제는 [품질 시험 기록](docs/QUALITY-TRIAL.md)에 있습니다.

고정 흰 그래픽 보완의 효과·추가 비용·제한은 [시험 기록](docs/STATIC-OVERLAY-TRIAL.md)에 있습니다.

스크롤 크레딧의 보완과 한계는 [시험 기록](docs/CREDIT-SCROLL-TRIAL.md)에 있습니다.

10bit SDR·HDR10·HLG의 구현 범위와 검증은 [HDR 네이티브 시험 기록](docs/HDR-NATIVE-TRIAL.md)에 있습니다.
