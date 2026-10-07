# NVOF for PotPlayer

**현재 소스 릴리스: [v0.2.0-preview.8](https://github.com/mkyoon80-alt/nvof-potplayer/releases/tag/v0.2.0-preview.8)** · ×2 전용 · Windows x64

팟플레이어 내장 디코더·렌더러와 함께 사용하는 **NVIDIA Optical Flow 프레임 보간 필터**입니다. Windows x64용 DirectShow 필터와 작은 설정창으로 구성됩니다.

NVIDIA Optical Flow frame interpolation for PotPlayer, with a native D3D11 GPU path, seek-aware re-priming, and a compact controller. **Preview release — validated on an RTX 5090.**

![설정창 미리보기 — 표시된 재생 상태는 예시입니다.](docs/assets/controller.png)

## 기능

- NVIDIA NvOFFRUC를 사용하는 단일 보간 엔진
- **×2 전용** 출력: 23.976 → 47.952, 24 → 48, 30 → 60 fps
- 원본 FPS별 적용: **24 / 25 / 30 / 50 / 60 / 기타**
- 팟플레이어 **내장 디코더 + 내장 Direct3D 11 렌더러** 유지
- 네이티브 경로에서 프레임을 CPU RAM으로 되돌리지 않고 GPU에서 처리
- 앞뒤 탐색 후 이전 프레임 이력을 버리고 새 위치에서 보간 재시작
- VSR과 함께 사용 가능: **디코더 → 보간 → 렌더러의 VSR → 화면**
- 필터 속성에서 설정창 열기, 자체 영상 OSD 없음

원본 A와 B 사이의 중간 시점에 NvOFFRUC 프레임 하나를 생성합니다. 원본은 그대로 유지하고, 각 세션에 같은 입력을 반복 제출하지 않습니다. 장면 전환 또는 NVIDIA의 품질 보호 신호가 감지되면 해당 구간은 이전 원본을 유지합니다. 고정 60p·120p 모드는 제거했으며, 기존 설정 파일에 남아 있어도 ×2로 동작합니다.

## 실행과 빌드

**완성된 로컬 패키지는 .NET, CUDA Toolkit, Visual C++ 재배포 패키지를 따로 설치하지 않아도 실행하도록 구성됩니다.** 팟플레이어 x64와 지원되는 NVIDIA GPU/드라이버는 필요합니다. 전체 폴더를 함께 유지해야 합니다.

이 저장소에는 소스와 빌드·패키징 도구를 공개합니다. NVIDIA 보간 DLL의 적용 가능한 재배포 권한을 확정하기 전까지, 해당 DLL을 담은 완성 ZIP은 GitHub에 업로드하지 않습니다. 런타임을 적법하게 확보한 환경에서 로컬 패키지를 만들 수 있습니다. 자세한 구성과 조건은 [외부 구성요소 안내](THIRD_PARTY_NOTICES.md)를 확인하세요.

- [팟플레이어 설정 방법](docs/POTPLAYER_SETUP.ko.md)
- [소스 빌드 및 패키징](docs/BUILD.md)
- [설치형 배포까지 남은 작업](docs/BUILD.md#standalone-installer-readiness)
- [처리 순서와 검증 범위](docs/ARCHITECTURE.md)
- [GPU 최적화와 측정 결과](docs/GPU-OPTIMIZATION.md)

설정 변경은 영상을 다시 열 때 적용됩니다. 프로그램은 설정창의 선택값과 실제 재생 상태를 구분해 표시합니다.

## 지원 범위

확인된 구성은 **Windows 10 x64, RTX 5090, 팟플레이어 x64 내장 디코더·내장 D3D11 렌더러**, 프로그레시브 **8비트 NV12 / BT.709 / 제한 범위 SDR**입니다. 이전 설치본에서 1080p ×2, VSR, 탐색, 종료를 확인했습니다. preview.8 적용 후 시험 사용자가 이전보다 크게 개선됐다고 확인했습니다. 전체 소스 빌드, 6개 기본 테스트, 52개 합성 품질 사례와 GPU·탐색·종료 검사를 통과했습니다. 다른 PC에서의 설치·실재생 검증은 아직 필요합니다.

HDR/P010, 인터레이스, 다중 GPU, 모든 코덱·해상도 조합에서의 장시간 안정성은 아직 보장하지 않습니다. 다른 GPU에서의 동작은 별도 검증이 필요합니다. Optical Flow 보간 특유의 경계 왜곡이나 장면 전환 시 반복 프레임이 발생할 수 있습니다. 60p·120p의 소수 시점에서 세부 무늬 떨림과 실험적 보정의 겹침 문제가 확인되어, 해당 출력 모드를 제거했습니다. ×2의 복잡한 움직임에서도 보간 왜곡이 완전히 사라지는 것은 아닙니다.

진단은 로컬에만 저장되며 자동 전송하지 않습니다. 문제를 제보할 때는 조작창 진단 내용, GPU/드라이버, 영상 형식, 재현 순서를 알려주세요. 개인 영상 파일은 필요하지 않습니다.

## License and acknowledgements

Original project source is under the [MIT license](LICENSE). Third-party binaries and source retain their separate licenses; they are **not** relicensed under MIT. See [third-party notices](THIRD_PARTY_NOTICES.md).

Built on Microsoft DirectShow baseclasses. The original adapter integration used rigaya's NVEncNVOFFRUC wrapper; the phase upgrade uses the project-built NvofFrucBridge with repetition metadata. The project uses public COM interface contracts for interoperability. It is an independent project, not affiliated with or endorsed by NVIDIA, PotPlayer, AMD, or Smootter. No Smootter binaries or proprietary source are included.

Slow-motion midpoint quality work and local validation: [preview 7 notes](docs/SLOW-MIDPOINT.md).

입 모양 변화와 카메라 이동 개선: [preview 8 검증 및 제한](docs/APPEARANCE-MOTION.md).
