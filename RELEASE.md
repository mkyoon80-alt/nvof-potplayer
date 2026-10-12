# v0.4.8 — GPU selection when switching HW/SW decoding

## 한국어

내장 GPU와 NVIDIA GPU가 함께 있는 PC에서 HW 디코딩을 SW로 바꾸면 재생이 멈추던 GPU 선택 문제를 수정했습니다. 렌더러가 전달한 GPU 번호를 검증하고, 전환 전 정상 사용하던 NVIDIA GPU를 유지합니다. 처음부터 SW로 시작할 때도 잘못된 GPU 번호를 그대로 사용하지 않습니다.

RTX 5080 + AMD 내장 GPU 환경에서 수정본의 정상 동작을 확인했습니다. 내장 GPU를 끌 필요가 없으며, 기존 보간 품질·HDR·10bit 경로와 사용자 설정은 유지합니다. 프레임마다 수행하는 추가 검사나 로그는 넣지 않았습니다. 한국어·영어 온라인 및 오프라인 매뉴얼도 업데이트했습니다.

업데이트 전에 팟플레이어와 NVOF 설정창을 닫고 기존 폴더를 백업하세요. 같은 폴더에 교체할 때 **기존 NvofPotPlayer.ini를 보존**하세요.

## English

Fixed GPU selection that could stop playback when switching from hardware to software decoding on PCs with both an integrated GPU and an NVIDIA GPU. Renderer adapter indices are validated, and the NVIDIA GPU already accepted by the renderer is retained across input-only reconnection. Software startup also rejects invalid/non-NVIDIA hints before selecting an available NVIDIA adapter.

The trial was confirmed working on an RTX 5080 + AMD integrated-GPU PC. Disabling the integrated GPU is unnecessary for this fix. Interpolation, HDR/10-bit processing, and user settings are preserved. No per-frame diagnostics or logging were added. Korean and English online, offline ZIP, and standalone HTML guides are updated.

Close PotPlayer and the controller, back up the existing folder, and **preserve NvofPotPlayer.ini** when replacing files in the same location.

## Validation and scope

Released 0.4.7 reproduced the reported non-NVIDIA adapter error under an injected renderer hint; the corrected build passed. Tests cover invalid hints, retained GPU identity, NV12/P010 HW/SW switching, software startup, HDR/10-bit precision and metadata, and renderer/resource lifetime. The main test PC has no integrated GPU; its injected-hint tests are separate from the user's successful 5080/AMD playback check.

This fixes the reported adapter-selection failure, not every possible multi-GPU configuration. Prior HDR display-color and moving-object occlusion limitations remain unchanged. See [GPU selection validation](docs/ADAPTER-SWITCH-TRIAL.md).

---

# v0.4.7 — Playback scheduling, software HDR, and thin fixed lines

## 한국어

- **HW 재생 대기 개선:** 디코더·보간·렌더러가 공유하는 GPU 장치를 기다리는 동안 잠금을 오래 잡지 않도록 했습니다. P010의 불필요한 완료 대기와 높은 배수에서의 출력 버퍼 대기도 줄였습니다. 저지연 모드 켜기 상태의 실제 재생으로 확인했습니다.
- **SW HDR·10bit 연결 개선:** HDR 영상을 처음부터 SW로 열거나 HW에서 SW로 전환할 때, 지원되는 디코더에서 실제 P010을 받아 10bit를 유지합니다. 반복 전환 중 멈추던 연결 문제도 수정했습니다.
- **고정 조준선 개선:** 가는 가로·세로 선이 움직이는 배경을 따라 갈라지는 현상을 줄였습니다. 움직이는 총 주변 등 가려짐 경계의 깨짐은 아직 남아 있습니다.
- **한·영 매뉴얼 업데이트:** 재생 성능, SW HDR 확인법과 SDR 감상 시 과밝기를 해결한 HDR 출력 설정을 반영했습니다. [전체 변경 내역](RELEASE.md)

설정과 출력 배수 정책은 그대로입니다. 업데이트 전에 팟플레이어와 설정창을 닫고 기존 폴더를 백업한 뒤, **기존 NvofPotPlayer.ini를 보존**해 교체하세요.

## English

- Reduced shared-device lock retention during GPU waits. P010 uses ordered GPU delivery, and output buffering scales with the interpolation multiple to reduce renderer back-pressure.
- Software HDR startup and HW-to-SW transitions request real P010 from supporting decoders. Repeated HW/SW switching preserves the pending native contract and confirmed software 10-bit precision.
- Protects thin stationary horizontal/vertical strokes, improving fixed reticles over moving backgrounds. Original frames, timing, and the Newton synthesis method are unchanged.
- Updated Korean and English online, offline ZIP, and standalone HTML manuals, including the player HDR output setting that resolved local SDR-viewing overbrightness.

Close PotPlayer and the controller, back up the existing folder, and **keep your NvofPotPlayer.ini** when replacing files.

## Validation and limits / 검증과 한계

The accepted candidates were verified with shared-context/resource-lifetime stress, NV12/P010 precision, repeated HW/SW allocator negotiation, HDR signaling/metadata tests, and thin-line controls. Actual-player checks confirmed foreground HW 240fps playback with NVIDIA Low Latency Mode On on an RTX 5090, repeated P010 software returns, and improved reticles. Background drops and real-time 4K performance depend on the display, driver, source, and GPU.

Software Dune playback supplied no Dolby sample side data: confirmed HDR10/10-bit delivery does not mean full Dolby Vision processing or display output. The filter forwards Dolby metadata only when supplied. Local SDR-viewing overbrightness was resolved by disabling H/W HDR output in PotPlayer while keeping ST 2084 correction, Auto tone mapping, and 125-nit SDR luminance. This is a player-setting resolution, not a filter color-management fix; the separate red/oversaturated fullscreen report remains unconfirmed. Lupin gun/occlusion artifacts remain unresolved; rejected experiments are not included.

Thin-line protection adds two mask channels (about 33.2 MB at 3840×2160). A short local 4K synthetic midpoint comparison measured about +0.15 ms per pair; this is not a general playback overhead estimate. See docs/PLAYBACK-PERFORMANCE.md, docs/SOFTWARE-HDR-TRIAL.md, and docs/THIN-OVERLAY-TRIAL.md for scope and evidence.

---

# 0.4.6 — SW 최초 재생 보간 수정

A standalone DirectShow frame interpolation filter for PotPlayer, powered by NVIDIA Optical Flow and custom Newton-based synthesis. Choose FPS caps or ×2–×5 output, with GPU interpolation for native D3D11 and supported software-decoded video, including 10-bit HDR processing. **No VapourSynth, NvOFFRUC.dll, or separate CUDA runtime required.**

**[Download v0.4.6](https://github.com/mkyoon80-alt/nvof-potplayer/releases/tag/v0.4.6)** · **[English manual](https://mkyoon80-alt.github.io/nvof-potplayer/en.html)** · **[한국어 매뉴얼](https://mkyoon80-alt.github.io/nvof-potplayer/)**

## What's new

- Fixed interpolation failing when PotPlayer starts with hardware decoding disabled. Software-decoded NV12 now prepares GPU output during the initial renderer connection. Starting with HW decoding and then switching to SW is no longer necessary.
- Verified SW-only startup in actual PotPlayer playback, with regression checks for repeated HW/SW switching, seeking, pause/resume, and 10-bit/HDR paths.
- Updated both Korean and English online/offline manuals with SW startup instructions and troubleshooting.

## 변경 사항

- **처음부터 SW로 재생해도 보간:** HW로 시작한 뒤 SW로 바꾸면 동작하지만, 처음부터 HW 가속을 끄고 열면 보간이 빠지던 문제를 수정했습니다. 소프트웨어 NV12 입력의 GPU 출력 준비를 최초 렌더러 연결 단계로 앞당겼습니다.
- **실제 재생 확인:** 팟플레이어를 완전히 종료한 뒤 SW 상태로 새로 실행해도 보간되는 것을 확인했습니다. HW/SW 반복 전환, 탐색·일시정지와 기존 10bit·HDR 경로도 검증했습니다.
- **한·영 매뉴얼:** 네이티브 DXVA가 필수는 아니라는 점과 SW로 처음 여는 방법, 업데이트 후 확인 순서를 반영했습니다.

## 업데이트 / Update

팟플레이어와 설정창을 완전히 닫고 기존 폴더를 백업한 뒤 프로그램 파일을 교체하세요. **기존 `NvofPotPlayer.ini`는 보존합니다.** 같은 폴더에서 업데이트하면 기존 등록을 사용할 수 있습니다. 내장 D3D11 렌더러를 사용하며, SW 디코딩을 쓸 때 HW를 먼저 켤 필요는 없습니다.

Close PotPlayer and the controller, back up the folder, and replace the program files while **keeping your existing `NvofPotPlayer.ini`**. Registration remains valid when updating in the same folder. Use PotPlayer's built-in D3D11 renderer. You can open the video directly with hardware decoding disabled.

스탠드얼론 ZIP에는 자체 .NET/WPF 런타임과 한국어·영어 오프라인 매뉴얼이 포함됩니다. 매뉴얼은 언어별 HTML·ZIP으로도 제공합니다. NVIDIA GPU와 드라이버, 팟플레이어는 별도로 필요합니다.

The standalone ZIP includes the self-contained .NET/WPF runtime and both offline manuals. Separate HTML and ZIP manuals are also available in each language. An NVIDIA GPU, its driver, and PotPlayer are still required.

원본이 10bit라도 디코더가 NV12로 넘기면 8bit 경로입니다. 10bit를 유지하려면 지원되는 10bit 입력 형식을 사용하세요. 인터레이스 영상 설정과 HDR 지원 범위는 매뉴얼을 참고하세요. 이번 변경은 SW 최초 연결 수정이며, 모든 4K 성능 저하나 별도 제보된 전체화면 HDR 색상 문제의 해결을 의미하지 않습니다.

A 10-bit source decoded to NV12 reaches the filter as 8-bit. Use a supported 10-bit decoder output to retain precision. This update addresses SW startup; it does not claim to resolve every 4K frame-rate drop or the separate fullscreen HDR color report.

[SW decoding and verification](docs/SW-DECODING.md) · [CPU 10-bit/interlaced input](docs/CPU-10BIT.md)

---

# 0.4.5 — SW 디코딩 전환 멈춤 수정

A standalone DirectShow frame interpolation filter for PotPlayer, powered by NVIDIA Optical Flow and custom Newton-based synthesis. Choose FPS caps or ×2–×5 output, with GPU interpolation for native D3D11 and supported software-decoded video, including 10-bit HDR processing. **No VapourSynth, NvOFFRUC.dll, or separate CUDA runtime required.**

**[Download v0.4.5](https://github.com/mkyoon80-alt/nvof-potplayer/releases/tag/v0.4.5)** · **[English manual](https://mkyoon80-alt.github.io/nvof-potplayer/en.html)** · **[한국어 매뉴얼](https://mkyoon80-alt.github.io/nvof-potplayer/)**

## What's new

- Fixed playback freezing when switching from hardware decoding to software-decoded NV12 while retaining the D3D11 renderer. Output format is refreshed when switching between NV12 and P010.
- Software-decoded NV12 now uploads to the GPU and delivers GPU surfaces to a compatible D3D11 renderer, without full-frame CPU readback on that path. Existing CPU 10-bit support remains available.
- Added optional playback timing logs, disabled by default, to distinguish filter processing from allocator and renderer waits. These do not measure frames actually displayed.
- Updated Korean and English manuals with software decoding, bit-depth checks, and fullscreen HDR troubleshooting.

## 변경 사항

- **HW → SW 전환 시 멈춤 수정:** 하드웨어 가속을 끈 뒤 NV12 입력과 기존 P010 출력 연결이 맞지 않아 멈추던 문제를 수정했습니다. HW/SW 전환 시 실제 GPU 출력 형식에 맞게 연결 정보를 갱신합니다.
- **CPU NV12 → GPU 전달:** 소프트웨어로 디코딩한 8bit SDR 프레임도 GPU에 올려 보간하고, 호환 D3D11 렌더러에 GPU 프레임으로 전달합니다. 이 경로에서 보간 결과를 CPU로 다시 읽어오지 않습니다. 실제 팟플레이어에서 멈춤 없이 재생되는 것을 확인했습니다.
- **선택형 성능 진단:** 처리·버퍼 대기·렌더러 전달 시간을 기록할 수 있습니다. 기본값은 꺼짐이며, 기록된 전달 FPS는 화면의 실제 표시 FPS와 다릅니다.
- **한·영 매뉴얼:** 네이티브 DXVA가 필수는 아니라는 점, CPU 디코딩 경로와 10bit 유지 조건, 인터레이스 설정, 전체화면 HDR 색상 문제의 비교 방법을 정리했습니다.

## 업데이트

팟플레이어와 설정창을 완전히 닫고 기존 폴더를 백업한 뒤 파일을 교체하세요. **기존 `NvofPotPlayer.ini`는 보존합니다.** 같은 폴더에서 업데이트하면 기존 등록을 사용할 수 있습니다. ZIP에는 자체 .NET/WPF 런타임과 한국어·영어 오프라인 매뉴얼이 포함됩니다.

Close PotPlayer and the controller before replacing files. Back up the folder and **keep your existing `NvofPotPlayer.ini`**. Both offline manuals and the self-contained .NET/WPF runtime are included.

## 지원 범위와 확인 사항

네이티브 DXVA는 권장 경로이며 필수는 아닙니다. 지원되는 CPU 입력은 NV12 SDR 및 P010/P016/P210/Y210입니다. **원본 파일이 10bit라도 디코더가 NV12로 넘기면 보간 경로는 8bit입니다.** 10bit 유지는 P010 등의 실제 입력으로 확인하세요. CPU 10bit 인터레이스 영상은 팟플레이어 **영상 → 디인터레이스 → 사용하지 않음**으로 두고 다시 엽니다.

The tested setup uses an RTX 5090 and PotPlayer's built-in decoder/D3D11 renderer. A 10-bit source decoded to NV12 reaches the filter as 8-bit; the filter cannot restore precision lost upstream. Use a supported 10-bit decoder output to preserve 10-bit samples.

이번 버전은 모든 4K 프레임 저하를 해결한 성능 수정본이 아닙니다. 일부 환경의 전체화면 HDR 과포화 제보는 아직 재현·원인이 확인되지 않았으며, 해당 문제의 해결을 주장하지 않습니다. Dolby Vision은 디코더가 제공한 메타데이터 전달을 지원하며 모든 프로파일·화면 출력을 보장하지 않습니다.

This release does not claim to fix every 4K frame-rate drop or the unconfirmed fullscreen HDR oversaturation report. Dolby Vision support forwards decoder-supplied metadata; it is not certification of every profile or display mode.

[SW decoding and verification](docs/SW-DECODING.md) · [CPU 10-bit/interlaced input](docs/CPU-10BIT.md)

---

# 0.4.4 — CPU 10bit GPU 보간 · VP9 HDR · 인터레이스 처리

A standalone DirectShow frame interpolation filter for PotPlayer, powered by NVIDIA Optical Flow and custom Newton-based synthesis. Choose FPS caps or ×2–×5 output, with native 10-bit HDR processing and GPU interpolation of supported software-decoded 10-bit video. **No VapourSynth, NvOFFRUC.dll, or separate CUDA runtime required.**

**[Download v0.4.4](https://github.com/mkyoon80-alt/nvof-potplayer/releases/tag/v0.4.4)** · **[English manual](https://mkyoon80-alt.github.io/nvof-potplayer/en.html)** · **[한국어 매뉴얼](https://mkyoon80-alt.github.io/nvof-potplayer/)**

## What's new

- GPU interpolation of supported software-decoded 10-bit video (P010/P016/P210/Y210), including H.264 10-bit sources. This does not add hardware decoding support.
- GPU deinterlacing for CPU 10-bit interlaced input; fixes for progressive cadence detection, pause-related green flashes, restart, and post-seek field handling.
- VP9 HDR color-signaling recovery and forwarding of decoder-supplied Dolby Vision metadata. Profile 8.1 metadata transport was verified; this is not universal Dolby Vision display certification.
- Updated Korean and English guides, including why PotPlayer deinterlacing must be disabled for the supported interlaced path.

## 변경 사항

- **CPU → GPU 10bit 경로:** CPU는 디코딩한 프레임을 전달하고 GPU가 변환·디인터레이스·보간합니다. 지원 입력은 P010/P016/P210/Y210, 출력은 P010 10bit 4:2:0입니다. P016의 상위 10bit를 사용하며 4:2:2 출력 보존을 뜻하지 않습니다.
- **인터레이스와 재생 안정성:** 실제 원본 필드는 시간 순서에 따라 GPU에서 처리합니다. 프로그레시브 H.264 10bit를 인터레이스로 오인하던 문제, 일시정지의 녹색 화면과 정지 후 재시작을 수정했습니다. 같은 영상 내 탐색에서는 이전 픽셀을 지우되 판별 힌트를 유지하고 새 화면의 빗살을 재확인합니다.
- **VP9 HDR:** 디코더가 출력 연결에서 생략한 HDR 색 정보를 연결된 VP9 원본 설정에서 복구합니다. 명시적인 SDR 정보는 존중합니다. 실제 YouTube VP9 HDR의 정상 색상을 확인했습니다.
- **Dolby Vision·HLG:** 디코더가 제공한 Dolby Vision RPU·해석된 메타데이터를 프레임 수명에 맞춰 전달합니다. 중간 프레임은 앞선 원본의 정보를 사용합니다. Profile 8.1 전달과 HEVC HLG 재생·신호 전달을 확인했습니다. 해석·톤매핑·화면 출력은 플레이어와 렌더러가 담당합니다.
- **한·영 매뉴얼:** CPU 디코딩과 GPU 보간의 차이, 디인터레이스 설정과 이유, 10bit·HDR·돌비 비전 지원 범위를 정리했습니다. 기존 FPS 상한·배수 선택과 설정 UI는 유지합니다.

## 인터레이스 영상 설정

CPU로 디코딩되는 인터레이스 영상은 **재생 화면 우클릭 → 영상 → 디인터레이스 → 사용하지 않음**으로 설정한 뒤 다시 여세요. 팟플레이어가 서로 다른 시점의 필드를 먼저 섞으면 움직임 정보가 줄어 보간이 불규칙해질 수 있어, NVOF가 원본 필드부터 디인터레이스·보간하도록 맡깁니다. NVOF를 사용하지 않을 때는 **자동 판단**으로 되돌립니다.

For supported CPU-decoded interlaced input: **Video → Deinterlacing → Do not use**, then reopen the video. NVOF deinterlaces the original fields before interpolation. Restore Automatic when playing interlaced video without NVOF.

## 업데이트와 검증 범위

팟플레이어와 설정창을 닫고 기존 폴더를 백업한 뒤 교체합니다. **기존 NvofPotPlayer.ini를 보존하세요.** 폴더를 옮기면 이전 위치에서 등록을 해제하고 새 위치에서 등록합니다. ZIP에 한·영 오프라인 매뉴얼과 .NET/WPF 런타임이 들어 있습니다.

검증 장비는 RTX 5090입니다. CPU 10bit 정밀도·필드 처리, DirectShow 연결, 탐색·일시정지·재시작, 기존 네이티브 HDR 전달을 검사했습니다. 최신 탐색 직후 빗살 수정은 재현 검사로 확인했으며 추가 실제 재생 확인은 남아 있습니다. HLG의 별도 화면 색상 비교, 모든 Dolby Vision 프로파일·출력 모드, HDR10+ 동적 메타데이터, 일반 텔레시네·반복/단일 필드 및 다중 GPU는 확인된 범위가 아닙니다. 모든 GPU에서 4K 고주사율 실시간 성능을 보장하지 않습니다.

---

# 0.4.2 — 출력 선택 확장 · AV1 HDR 색상 수정 · 움직임 경계 개선

A standalone DirectShow frame interpolation filter for PotPlayer, powered by NVIDIA Optical Flow and custom Newton-based synthesis. Supports selectable FPS caps or ×2–×5 output, GPU-resident D3D11 processing, and native 10-bit HDR10/HLG signal passthrough. **No VapourSynth, NvOFFRUC.dll, or separate CUDA runtime required.**

**[Download v0.4.2](https://github.com/mkyoon80-alt/nvof-potplayer/releases/tag/v0.4.2)** · **[English manual](https://mkyoon80-alt.github.io/nvof-potplayer/en.html)** · **[한국어 매뉴얼](https://mkyoon80-alt.github.io/nvof-potplayer/)**

v0.4.0 이후 검증한 변경을 통합했습니다. Windows x64 스탠드얼론 ZIP과 최신 설정 화면을 반영한 한국어·영문 매뉴얼을 제공합니다.

- **출력 방식 선택:** FPS 상한 60 / 72 / 75 / 100 / 120 / 144 / 240fps, 또는 원본 배수 ×2 / ×3 / ×4 / ×5. 두 방식의 선택을 각각 기억합니다. 기존 최대 60/120fps 설정도 유지합니다.
- **세로 해상도 조건:** 전체 / 1080p 이하 / 1440p 이하. 원본 FPS 조건과 함께 적용하며, 제외된 영상은 원본 FPS로 재생합니다.
- **작은 FPS 편차 허용:** 30.003fps 영상도 최대 60fps에서는 ×2, 최대 120fps에서는 ×4로 처리합니다. 0.1% 범위의 편차를 허용하며 원본 재생 시간은 바꾸지 않습니다.
- **YouTube AV1 HDR 색상 수정:** 디코더 연결 정보에서 HDR 표시가 빠진 경우 연결된 AV1 원본의 색 정보를 확인해 전달합니다. 디코더가 명시한 SDR 정보는 우선합니다. LG Jazz HDR 영상의 실제 팟플레이어 정상 색상 재생을 확인했습니다.
- **움직이는 도형의 경계 개선:** 검은 배경의 빠른 원·사각형에 대해 움직임 추정과 모서리 처리를 보완했습니다. 일반 영상의 기존 보간 결과는 별도 비교로 확인했습니다.
- **설정창과 매뉴얼 정리:** 현재의 세 탭과 새 출력 선택을 안내합니다. 매뉴얼의 오래된 화질 보정·입모양 기능 설명과 이미지를 제거하고 v0.4.2 화면으로 교체했습니다. 영문 매뉴얼도 추가했으며 프로그램 UI는 한국어로 유지합니다.

## 출력 예시

24fps → 최대 60fps: **48fps**, 최대 72fps 또는 원본 ×3: **72fps**, 최대 120fps 또는 원본 ×5: **120fps**.

25fps → 최대 120fps: **100fps**, 최대 240fps: **225fps**. FPS 상한은 정확한 목표 FPS가 아닙니다. 원본 배수 모드에서는 상한을 적용하지 않습니다. 변경 사항은 영상을 다시 열어 적용합니다.

## 업데이트

팟플레이어와 설정창을 닫고 기존 폴더를 백업한 뒤 교체합니다. **기존 NvofPotPlayer.ini는 보존하세요.** 폴더를 옮길 경우 기존 위치에서 필터 등록을 해제하고 새 위치에서 등록합니다. 설치 프로그램 버전에서 전환하는 방법은 [사용 설명서](https://mkyoon80-alt.github.io/nvof-potplayer/)에 있습니다.

## 검증과 범위

출력 정책·색 정보·타임스탬프 등 14개 자동 검사, 28개 DirectShow 연결 사례, 32개 GPU 합성 사례와 기존 영상 7개 구간 비교를 수행했습니다. ×3·×5는 24fps와 23.976fps의 한 시간 분량 타임스탬프, 원본 보존, 탐색과 종료를 검사했습니다. 이는 한 시간의 실제 팟플레이어 연속 재생 시험과는 구분됩니다.

HDR10 파일과 YouTube AV1 HDR 색상은 실제 팟플레이어 재생에서 확인했습니다. HLG는 신호 전달 검사까지 완료했습니다. Dolby Vision·HDR10+ 동적 메타데이터는 지원하지 않으며, HDR10 호환 기본 영상은 HDR10으로 처리합니다. 검증 GPU는 RTX 5090입니다. 출력 FPS 선택이 모든 GPU의 4K 고주사율 실시간 재생을 보장하지는 않습니다.

---

# 0.4.0 — 스탠드얼론 · 정수배 출력 · 네이티브 10bit/HDR

v0.3.0 이후의 보간 개선과 HDR 색상 수정본을 통합했습니다. 압축을 풀어 사용하는 Windows x64 ZIP과 한국어 매뉴얼을 배포합니다.

- **최대 60fps / 120fps** 안에서 원본의 정수배로 출력합니다. 23.976fps는 각각 47.952fps(×2) / 119.880fps(×5), 25fps는 50fps / 100fps입니다. ×5는 20·40·60·80% 시점의 서로 다른 네 중간 프레임을 생성합니다. 기본값은 최대 60fps이며 변경 후 영상을 다시 열어야 합니다.
- **10bit SDR·HDR10·HLG**의 네이티브 D3D11 P010 경로를 추가했습니다. 원본과 중간 프레임을 10bit로 유지하며 움직임 분석만 NV12로 수행합니다. 색 정보와 제공된 HDR 정적 메타데이터를 렌더러까지 전달합니다.
- 팟플레이어가 기본 연결 정보에 HDR 표식을 생략할 때, 연결된 HEVC 원본의 색 정보를 확인해 전달합니다. 프레임별 HDR 메타데이터 누락도 수정했습니다. 실제 HDR10 호환 파일에서 물빠진 색상이 정상으로 돌아온 것을 확인했습니다. 디코더가 명시한 SDR/HDR 정보가 우선하며 P010을 무조건 HDR로 취급하지 않습니다.
- **Newton 보간을 유지**하면서 고정 글자·코스 그림, 스크롤 크레딧, 반복 문살과 움직임 경계의 깨짐을 줄였습니다. NVOFA 세션·GPU 버퍼를 재사용합니다. Cost Map·Temporal Hints는 꺼져 있으며 기본 분석은 Grid 4·Medium·긴 변 최대 1920입니다. 셰이더는 빌드할 때 미리 컴파일합니다.
- **FRUC·CUDA 런타임 의존성을 제거**했습니다. 필터·등록 도구·설정창과 .NET/WPF를 ZIP에 포함합니다. GPU 드라이버와 팟플레이어는 별도입니다.
- 설정창을 **보간 설정 / 연결 설정 / 진단** 세 탭으로 정리했습니다. 연결 설정에서 현재 폴더를 열거나 필터 등록을 해제할 수 있습니다. 관리자용 등록은 Windows 권한 승인을 받아 해제하며, 등록 해제 자체는 파일을 삭제하지 않습니다.

## 업데이트

기존 설치 프로그램 버전은 설정 파일을 백업하고 Windows 설치된 앱에서 제거한 뒤 새 ZIP을 별도 폴더에 풀어 등록하세요. 기존 스탠드얼론은 플레이어와 설정창을 닫고 폴더를 백업한 뒤 같은 위치에 교체할 수 있습니다. `NvofPotPlayer.ini`는 보존합니다. 자세한 연결·이동·제거 방법은 [사용 설명서](https://mkyoon80-alt.github.io/nvof-potplayer/)에 있습니다.

## 검증과 제한

10bit 정밀도·서로 다른 ×5 중간 시점, HDR 색 정보·메타데이터 수명, 탐색·종료, 기존 8bit 회귀 검사를 통과했습니다. 실제 재생 검증 장비는 RTX 5090과 팟플레이어 내장 디코더·D3D11 렌더러입니다. **HDR10 실제 재생은 확인했으며 HLG는 신호 전달 검사까지만 완료**했습니다.

필터는 HDR 톤매핑이나 모니터 HDR 전환을 수행하지 않습니다. Dolby Vision·HDR10+ 동적 메타데이터, CPU P010 경로는 지원하지 않습니다. 머리카락·연기·가려짐·일부 글자에는 잔여 아티팩트가 있으며 모든 GPU의 4K120 실시간 성능을 보장하지 않습니다. 실행 파일은 코드 서명되지 않았습니다.

[HDR 구현·검증 기록](docs/HDR-NATIVE-TRIAL.md) · [외부 구성요소 고지](THIRD_PARTY_NOTICES.md)

---

# 0.3.2-rate.3 — 숫자와 반복 문살 잔존 오류 보정 시험판

- 촬영된 전광판의 작은 잘못된 이동도 재검사해 F1 22:12 숫자의 잔존 깨짐을 줄였습니다. 보정 채택 시의 영상 오차·최대 오차·수렴 조건은 유지합니다.
- 반복 문살의 가까운 이웃까지 잘못 대응되는 경우, 더 먼 양쪽 이웃을 함께 확인합니다. Jishou 4화 0:05 문 왼쪽의 패임을 줄였습니다.
- ×5가 같은 중간 프레임을 반복하는지 별도 검사했습니다. 20픽셀 이동 시험에서 ×2는 10, ×5는 4·8·12·16픽셀 위치에 생성됩니다. 가려짐 경계에는 여전히 오차가 있으며 원본 모션 블러를 복원하지 않습니다.

F1 23:23의 이동 물체 경계 처리, 기존 정수배 60/120fps 정책, Newton 역좌표 계산과 사용자 설정은 유지합니다. 이번 버전은 여전히 8bit NV12 출력이며 10bit/HDR 지원을 추가한 버전은 아닙니다. 로컬 시험판으로 GitHub에는 올리지 않았습니다.

검증 기록: [MOTION-BOUNDARY-TRIAL.md](docs/MOTION-BOUNDARY-TRIAL.md).

---

# 0.3.2-rate.2 — 문살·이동 물체·촬영된 화면의 경계 보정 시험판

- 반복되는 곧은 문살이 옆 칸으로 잘못 대응될 때, 양쪽 이웃의 움직임과 넓은 원본 영역을 함께 대조합니다.
- 차와 배경의 경계처럼 서로 다른 움직임이 섞인 영역에서, 영상 대응도 나쁜 과도한 늘어남의 신뢰도를 낮춥니다.
- 화면의 반복 픽셀 무늬가 거의 정지한 숫자를 큰 이동으로 오인하는 경우, 작은 이동을 직접 추정하고 넓은 영역의 영상 오차가 명확히 줄어들 때만 적용합니다.

최대 60/120fps 정수배 출력, 각 시점의 Newton 역좌표 계산, 원본 프레임 보존, 기존 설정을 유지합니다. 분석 프로필은 긴 변 최대 1920·Grid 4·Medium이며 Cost Map과 Temporal Hints는 끔입니다. 재생 중 셰이더 컴파일은 하지 않습니다.

원본에 촬영된 블러를 선명하게 복원하는 기능은 아니며, 모든 가림 경계의 잔상을 제거하는 것은 아닙니다. 로컬 시험판이며 GitHub에는 업로드하지 않았습니다. 검증 기록: [MOTION-BOUNDARY-TRIAL.md](docs/MOTION-BOUNDARY-TRIAL.md).

---

# 0.3.2-rate.1 — 최대 60/120fps 정수배 출력 시험판

기존 60fps·120fps 버튼을 복원하고, 정확히 그 FPS로 재표본화하는 대신 상한을 넘지 않는 가장 큰 정수배로 보간합니다. 기본값은 최대 60fps입니다.

| 원본 | 최대 60fps | 최대 120fps |
|---|---|---|
| 23.976 | 47.952 ×2 | 119.880 ×5 |
| 24 | 48 ×2 | 120 ×5 |
| 25 | 50 ×2 | 100 ×4 |
| 29.97 | 59.94 ×2 | 119.880 ×4 |
| 30 | 60 ×2 | 120 ×4 |
| 50 | 원본 50 | 100 ×2 |
| 59.94 | 원본 59.94 | 119.880 ×2 |
| 60 | 원본 60 | 120 ×2 |

원본 FPS 선택·보간 켜짐 설정을 유지하며, 상한 안에서 두 배로 늘릴 수 없으면 원본 재생합니다. 원본이 상한보다 빨라도 프레임을 버리지 않습니다. 변경은 영상을 다시 열 때 적용됩니다.

프레임 쌍당 광학흐름·글자 이동 분석을 한 번 수행하고 필요한 시점들을 직접 합성합니다. ×5는 20·40·60·80%의 네 중간 프레임을 만듭니다. 기존 credits.5의 Newton·고정 글자·스크롤 크레딧·경계 보정을 유지하며 사전 컴파일, 세션 재사용, Cost Map·Temporal Hints 끔도 유지합니다.

새 설정은 OutputFpsLimit=60 또는 120입니다. 오래된 TargetFps/DoubleRate 값은 동작을 바꾸지 않으며, 누락·잘못된 값은 60으로 처리합니다. 비정상적으로 낮은 원본 FPS는 엔진의 프레임 쌍당 중간 프레임 32장 한도 내에서 동작합니다. 가변 프레임률 영상은 모든 프레임 쌍의 정수배를 보장하지 않습니다.

여러 중간 프레임은 추가 합성 비용이 들며, 모든 영상의 품질과 RTX 5070의 4K60→120 실시간 재생을 보장하지 않습니다. 로컬 시험판으로 GitHub에는 올리지 않았습니다. 검증 기록: [INTEGER-RATE-TRIAL.md](docs/INTEGER-RATE-TRIAL.md).
