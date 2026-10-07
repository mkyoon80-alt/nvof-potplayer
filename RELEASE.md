# v0.2.0-beta.1 — 10비트 SDR 재생 지원

**10비트 SDR 영상도 팟플레이어에서 ×2 보간으로 재생할 수 있습니다.** 검은 화면이 나오거나 재생 직후 멈추던 문제를 수정했습니다.

## 다운로드

- [Windows x64 설치 프로그램](https://github.com/mkyoon80-alt/nvof-potplayer/releases/download/v0.2.0-beta.1/NvofPotPlayer-0.2.0-beta.1-Setup-x64.exe)
- [웹 사용 설명서](https://mkyoon80-alt.github.io/nvof-potplayer/)
- 오프라인 매뉴얼: [단일 HTML](https://github.com/mkyoon80-alt/nvof-potplayer/releases/download/v0.2.0-beta.1/NvofPotPlayer-0.2.0-beta.1-Manual-ko.html) · [ZIP](https://github.com/mkyoon80-alt/nvof-potplayer/releases/download/v0.2.0-beta.1/NvofPotPlayer-0.2.0-beta.1-Manual-ko.zip)

## 달라진 점

- **10비트 SDR(P010) 입력 지원.** 별도 옵션 없이 GPU에서 NV12로 변환한 뒤 보간합니다.
- **재생 시작 안정성 개선.** 10비트 경로에서 GPU 작업이 끝난 뒤 프레임을 전달하도록 보완했습니다.
- 입력 형식과 변환 여부를 진단 기록에 추가했습니다.
- 매뉴얼에 10비트 영상의 처리·출력 방식과 문제 해결 안내를 추가했습니다.

팟플레이어 내장 디코더·D3D11 렌더러를 그대로 사용하며 VapourSynth는 필요하지 않습니다. 출력은 ×2입니다. GPU 중간 프레임 보정과 형태 변화 보호는 각각 켜고 끌 수 있습니다.

**10비트 입력의 출력은 8비트입니다.** 원본 시점의 프레임과 보간 프레임 모두 NV12 8비트로 출력합니다. 10비트 정밀도 유지와 HDR 톤매핑은 제공하지 않습니다.

## 설치·업데이트

팟플레이어와 NVOF 설정창을 닫고 설치 프로그램을 실행하세요. 기존 설치 폴더에 설치하면 설정을 유지합니다. NVIDIA 구성요소 약관에 직접 동의해야 설치됩니다.

.NET Desktop Runtime과 필요한 CUDA·Visual C++ 런타임을 포함하므로 별도 런타임 다운로드가 필요하지 않습니다. **Windows 10/11 x64, 지원 NVIDIA GPU·드라이버, 팟플레이어 x64**는 별도로 필요합니다. 처음 설치한다면 팟플레이어 F5 → 코덱/필터 → 전역 필터 우선 순위에서 NVOF를 추가하고 최우선 사용으로 설정하세요.

매뉴얼은 설치 폴더와 시작 메뉴에서도 인터넷 없이 열 수 있습니다. Assets의 단일 HTML은 바로 열고, ZIP은 압축을 모두 푼 뒤 `index.html`을 여세요.

## 확인한 범위

- RTX 5090 / Windows 10 / 팟플레이어 내장 디코더·D3D11 렌더러에서 1080p HEVC 10비트 SDR 실제 재생을 사용자가 확인했습니다. 로그에서도 P010 → NV12 변환, 23.976 → 47.952 fps 출력과 정상 종료를 확인했습니다.
- P010의 1,024단계 변환값, Y/UV 채널, 디코더 패딩·배열, 원본 프레임 보관과 탐색 초기화를 검사했습니다.
- 실제 영상 64프레임을 네 차례 반복해 256개 입력·512개 출력을 검사했습니다. 동시 GPU 작업을 포함한 검사입니다.
- P010·기존 NV12의 DirectShow 연결·재생·탐색·종료 검사와 기본 테스트 6개를 통과했습니다.

10비트 수정은 RTX 5090에서 확인했습니다. 다른 GPU의 10비트 재생과 모든 코덱·해상도 조합의 장시간 안정성은 추가 확인이 필요합니다. 지원 색 형식은 프로그레시브 BT.709 / 제한 범위 SDR이며 HDR·인터레이스·다중 GPU는 검증 범위 밖입니다. GPU 내부 복사와 동기화는 남아 있습니다. 실행 파일은 코드 서명되지 않았습니다.

## NVIDIA 구성요소

배포자는 NVIDIA 공식 다운로드 페이지가 연결한 2017년 SDK 약관 1.1(ii)의 애플리케이션 포함 바이너리 배포 조항을 근거로 이 설치형 배포를 구성합니다. SDK에 포함된 2022년 약관은 배포 범위를 다르게 표현합니다. NVIDIA로부터 별도 서면 승인이나 두 문서 관계의 확정을 받았다고 주장하지 않습니다. Click-through는 동의 절차이며 재배포 권한을 새로 부여하지 않습니다.

[NVIDIA 약관과 출처](licenses/README.md), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)를 확인하세요. NVIDIA 구성요소 소유권과 원문 약관은 설치 폴더에 보존됩니다.
