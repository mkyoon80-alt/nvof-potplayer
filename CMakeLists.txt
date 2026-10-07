cmake_minimum_required(VERSION 3.20)
project(NvofPotPlayer VERSION 0.2.0 LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(MSVC)
  add_compile_options(/utf-8 /EHsc /W3)
endif()
add_library(nvof_pipeline STATIC src/pipeline.cpp)
target_include_directories(nvof_pipeline PUBLIC include)
add_executable(pipeline_tests tests/pipeline_tests.cpp)
target_link_libraries(pipeline_tests PRIVATE nvof_pipeline)
enable_testing()
add_test(NAME pipeline COMMAND pipeline_tests)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/engine.cpp")
  find_package(CUDAToolkit REQUIRED)
  add_library(nvof_engine STATIC src/engine.cpp)
  target_include_directories(nvof_engine PUBLIC include)
  target_link_libraries(nvof_engine PUBLIC CUDA::cuda_driver)
  add_executable(engine_performance tests/engine_performance.cpp)
  target_link_libraries(engine_performance PRIVATE nvof_engine nvof_pipeline)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_smoke.cpp")
    add_executable(engine_smoke tests/engine_smoke.cpp)
    target_link_libraries(engine_smoke PRIVATE nvof_engine nvof_pipeline)
  endif()
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/directshow_filter.cpp" AND TARGET nvof_engine)
  set(BASE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/windows_samples/Samples/Win7Samples/multimedia/directshow/baseclasses")
  file(GLOB BASE_SOURCES "${BASE}/*.cpp")
  add_library(strmbase STATIC ${BASE_SOURCES})
  target_include_directories(strmbase PUBLIC "${BASE}")
  target_compile_definitions(strmbase PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NON_CONFORMING_SWPRINTFS _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS)
  target_compile_options(strmbase PRIVATE /wd4312 /wd4311 /wd4996 /wd4244)
  add_library(NvofPotPlayer SHARED src/directshow_filter.cpp src/filter.def src/filter.rc)
  target_link_libraries(NvofPotPlayer PRIVATE nvof_engine nvof_pipeline strmbase strmiids ole32 oleaut32 uuid winmm advapi32 shell32)
  set_target_properties(NvofPotPlayer PROPERTIES PREFIX "" SUFFIX ".ax")
  add_executable(directshow_smoke tests/directshow_smoke.cpp)
  target_link_libraries(directshow_smoke PRIVATE strmbase strmiids ole32 oleaut32 uuid winmm advapi32)
  target_compile_definitions(directshow_smoke PRIVATE NOMINMAX)
endif()


add_executable(NvofRegister src/register_filter.cpp)
target_link_libraries(NvofRegister PRIVATE advapi32 ole32 oleaut32 strmiids)




# Experimental GPU-resident path; integration requires actual host negotiation.
add_library(nvof_gpu_pipeline STATIC src/gpu_pipeline.cpp)
target_include_directories(nvof_gpu_pipeline PUBLIC include)
target_compile_definitions(nvof_gpu_pipeline PUBLIC NOMINMAX)
add_executable(gpu_pipeline_tests tests/gpu_pipeline_tests.cpp)
target_link_libraries(gpu_pipeline_tests PRIVATE nvof_gpu_pipeline d3d11)
add_test(NAME gpu_pipeline COMMAND gpu_pipeline_tests)
add_library(nvof_gpu_engine STATIC src/gpu_engine.cpp)
target_include_directories(nvof_gpu_engine PUBLIC include)
target_compile_definitions(nvof_gpu_engine PUBLIC NOMINMAX)
target_link_libraries(nvof_gpu_engine PUBLIC CUDA::cuda_driver d3d11 dxgi d3dcompiler)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu_engine_smoke.cpp")
  add_executable(gpu_engine_smoke tests/gpu_engine_smoke.cpp)
  target_link_libraries(gpu_engine_smoke PRIVATE nvof_gpu_engine nvof_gpu_pipeline)
endif()
if(TARGET NvofPotPlayer)
  target_link_libraries(NvofPotPlayer PRIVATE nvof_gpu_engine nvof_gpu_pipeline d3d11 dxgi)
endif()
add_executable(directshow_gpu_smoke tests/directshow_gpu_smoke.cpp)
target_include_directories(directshow_gpu_smoke PRIVATE include)
target_compile_definitions(directshow_gpu_smoke PRIVATE NOMINMAX)
target_link_libraries(directshow_gpu_smoke PRIVATE strmbase strmiids ole32 oleaut32 uuid winmm advapi32 d3d11 dxgi)

add_library(nvof_mf_bridge STATIC src/mf_bridge.cpp)
target_include_directories(nvof_mf_bridge PUBLIC include)
target_compile_definitions(nvof_mf_bridge PUBLIC NOMINMAX)
target_link_libraries(nvof_mf_bridge PUBLIC mfplat mfuuid d3d11 dxgi ole32 oleaut32 strmiids strmbase winmm advapi32)
target_link_libraries(NvofPotPlayer PRIVATE nvof_mf_bridge)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/mf_bridge_smoke.cpp")
  add_executable(mf_bridge_smoke tests/mf_bridge_smoke.cpp)
  target_link_libraries(mf_bridge_smoke PRIVATE nvof_mf_bridge nvof_gpu_engine)
endif()
target_link_libraries(directshow_gpu_smoke PRIVATE nvof_mf_bridge)

add_executable(input_rate_policy_tests tests/input_rate_policy_tests.cpp)
target_include_directories(input_rate_policy_tests PRIVATE include)
add_test(NAME input_rate_policy COMMAND input_rate_policy_tests)

add_executable(output_rate_policy_tests tests/output_rate_policy_tests.cpp)
target_link_libraries(output_rate_policy_tests PRIVATE nvof_pipeline)
add_test(NAME output_rate_policy COMMAND output_rate_policy_tests)

# Isolated /MT packaging oracle; intentionally links no GPU/filter libraries.
add_executable(runtime_dependencies tests/runtime_dependencies.cpp)
set_target_properties(runtime_dependencies PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(runtime_dependencies PRIVATE NOMINMAX)
# Independent motion phases: the legacy resampler stays available only to tests.
add_library(nvof_phase_pipeline STATIC src/phase_pipeline.cpp)
target_include_directories(nvof_phase_pipeline PUBLIC include)
add_executable(phase_pipeline_tests tests/phase_pipeline_tests.cpp)
target_link_libraries(phase_pipeline_tests PRIVATE nvof_phase_pipeline)
add_test(NAME phase_pipeline COMMAND phase_pipeline_tests)
add_library(nvof_gpu_phase_pipeline STATIC src/gpu_phase_pipeline.cpp)
target_include_directories(nvof_gpu_phase_pipeline PUBLIC include)
target_compile_definitions(nvof_gpu_phase_pipeline PUBLIC NOMINMAX)
add_executable(gpu_phase_pipeline_tests tests/gpu_phase_pipeline_tests.cpp)
target_link_libraries(gpu_phase_pipeline_tests PRIVATE nvof_gpu_phase_pipeline d3d11)
add_test(NAME gpu_phase_pipeline COMMAND gpu_phase_pipeline_tests)
target_link_libraries(NvofPotPlayer PRIVATE nvof_phase_pipeline nvof_gpu_phase_pipeline)
add_executable(gpu_phase_engine_smoke tests/gpu_phase_engine_smoke.cpp)
target_link_libraries(gpu_phase_engine_smoke PRIVATE nvof_gpu_engine nvof_gpu_pipeline nvof_gpu_phase_pipeline)
add_executable(phase_engine_smoke tests/phase_engine_smoke.cpp)
target_link_libraries(phase_engine_smoke PRIVATE nvof_engine)

option(NVOF_BUILD_FRUC_BRIDGE "Build the project FRUC adapter using an acquired official NVIDIA SDK" ON)
if(NVOF_BUILD_FRUC_BRIDGE)
  find_path(NVOF_FRUC_INCLUDE_DIR NvOFFRUC.h HINTS ENV OPTICAL_FLOW_SDK
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpticalFlowSDK"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpticalFlowSDK/Optical_Flow_SDK_5.0.7"
    PATH_SUFFIXES NvOFFRUC/Interface Interface)
  if(NOT NVOF_FRUC_INCLUDE_DIR)
    message(FATAL_ERROR "Official NvOFFRUC.h is required. Set NVOF_FRUC_INCLUDE_DIR to its directory. For scheduler-only development, use -DNVOF_BUILD_FRUC_BRIDGE=OFF.")
  endif()
  add_library(NvofFrucBridge SHARED src/fruc_bridge.cpp)
  target_include_directories(NvofFrucBridge PRIVATE include "${NVOF_FRUC_INCLUDE_DIR}")
  set_target_properties(NvofFrucBridge PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_executable(fruc_bridge_smoke tests/fruc_bridge_smoke.cpp)
  target_include_directories(fruc_bridge_smoke PRIVATE include "${NVOF_FRUC_INCLUDE_DIR}")
  target_link_libraries(fruc_bridge_smoke PRIVATE CUDA::cuda_driver)
endif()

# Opt-in hardware regression: exercises the shared immediate context concurrently.
add_executable(gpu_interop_stress tests/gpu_interop_stress.cpp)
target_link_libraries(gpu_interop_stress PRIVATE nvof_gpu_engine)
set(NVOF_INTEROP_BASELINE_LIBRARY "" CACHE FILEPATH "Optional pre-fix GPU engine library for interop regression comparison")
if(NVOF_INTEROP_BASELINE_LIBRARY)
  add_executable(gpu_interop_stress_baseline tests/gpu_interop_stress.cpp)
  target_include_directories(gpu_interop_stress_baseline PRIVATE include)
  target_compile_definitions(gpu_interop_stress_baseline PRIVATE NOMINMAX)
  target_link_libraries(gpu_interop_stress_baseline PRIVATE "${NVOF_INTEROP_BASELINE_LIBRARY}" CUDA::cuda_driver d3d11 dxgi d3dcompiler)
endif()

# Explicit hardware endpoint/continuous-motion diagnostic; never automatic CTest.
add_executable(phase_endpoint_smoke tests/phase_endpoint_smoke.cpp)
target_link_libraries(phase_endpoint_smoke PRIVATE nvof_engine nvof_phase_pipeline)

# Explicit continuous multi-rate hardware quality diagnostic.
add_executable(multi_phase_quality tests/multi_phase_quality.cpp)
target_link_libraries(multi_phase_quality PRIVATE nvof_engine nvof_phase_pipeline)

# Explicit surface/subpixel quality probe, never automatic CTest.
if(NVOF_BUILD_FRUC_BRIDGE)
  add_executable(fruc_surface_probe tests/fruc_surface_probe.cpp)
  target_include_directories(fruc_surface_probe PRIVATE include "${NVOF_FRUC_INCLUDE_DIR}")
  target_link_libraries(fruc_surface_probe PRIVATE CUDA::cuda_driver)
endif()

# Native fractional-position refinement; official headers stay outside tracked source.
find_path(NVOF_API_INCLUDE_DIR nvOpticalFlowD3D11.h HINTS ENV OPTICAL_FLOW_SDK
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpticalFlowSDK"
  "${NVOF_FRUC_INCLUDE_DIR}/../.." PATH_SUFFIXES NvOFInterface)
if(NOT NVOF_API_INCLUDE_DIR)
  message(FATAL_ERROR "Official nvOpticalFlowD3D11.h and nvOpticalFlowCommon.h are required. Set NVOF_API_INCLUDE_DIR to the SDK NvOFInterface directory.")
endif()
add_library(nvof_fractional_refiner STATIC src/fractional_refiner.cpp)
target_include_directories(nvof_fractional_refiner PUBLIC include PRIVATE "${NVOF_API_INCLUDE_DIR}")
target_compile_definitions(nvof_fractional_refiner PUBLIC NOMINMAX)
target_link_libraries(nvof_fractional_refiner PUBLIC d3d11 dxgi d3dcompiler)
add_executable(fractional_refiner_quality tests/fractional_refiner_quality.cpp)
target_link_libraries(fractional_refiner_quality PRIVATE nvof_fractional_refiner nvof_engine nvof_phase_pipeline)
option(NVOF_ENABLE_EXPERIMENTAL_SUBPIXEL "Enable unaccepted fractional refiner for isolated experiments only" OFF)
target_compile_definitions(nvof_gpu_engine PUBLIC NVOF_ENABLE_EXPERIMENTAL_SUBPIXEL=$<BOOL:${NVOF_ENABLE_EXPERIMENTAL_SUBPIXEL}>)
target_link_libraries(nvof_gpu_engine PRIVATE nvof_fractional_refiner)

# Adversarial layered/occlusion diagnostic; intentionally not an automatic CTest.
add_executable(fractional_occlusion_quality tests/fractional_occlusion_quality.cpp)
target_link_libraries(fractional_occlusion_quality PRIVATE nvof_fractional_refiner nvof_engine nvof_phase_pipeline)

# REJECTED research probe: native NV12 DirectX/fence path failed moving
# chroma and extended re-registration tests. Never used by the product.
if(NVOF_BUILD_FRUC_BRIDGE)
  add_executable(fruc_d3d11_probe tests/fruc_d3d11_probe.cpp)
  target_include_directories(fruc_d3d11_probe PRIVATE "${NVOF_FRUC_INCLUDE_DIR}")
  target_link_libraries(fruc_d3d11_probe PRIVATE nvof_gpu_engine nvof_gpu_pipeline)
endif()

add_executable(gpu_x2_benchmark tests/gpu_x2_benchmark.cpp)
target_link_libraries(gpu_x2_benchmark PRIVATE nvof_gpu_engine nvof_gpu_pipeline)

# Explicit local-video reproduction, not an automatic test or bundled media.
add_executable(video_x2_repro tests/video_x2_repro.cpp)
target_link_libraries(video_x2_repro PRIVATE nvof_gpu_engine nvof_gpu_phase_pipeline nvof_gpu_pipeline)

if(NVOF_BUILD_FRUC_BRIDGE)
  add_executable(video_fruc_reference tests/video_fruc_reference.cpp)
  target_include_directories(video_fruc_reference PRIVATE include "${NVOF_FRUC_INCLUDE_DIR}")
  target_link_libraries(video_fruc_reference PRIVATE CUDA::cuda_driver)
endif()

add_executable(video_midpoint_refine tests/video_midpoint_refine.cpp)
target_link_libraries(video_midpoint_refine PRIVATE nvof_fractional_refiner nvof_gpu_engine nvof_gpu_pipeline)

add_executable(midpoint_stability_quality tests/midpoint_stability_quality.cpp)
target_link_libraries(midpoint_stability_quality PRIVATE nvof_fractional_refiner nvof_engine nvof_phase_pipeline)

add_executable(appearance_midpoint_quality tests/appearance_midpoint_quality.cpp)
target_link_libraries(appearance_midpoint_quality PRIVATE nvof_fractional_refiner nvof_engine nvof_phase_pipeline)

# Explicit GPU regression for independent enhancement switches; not automatic CTest.
add_executable(enhancement_options_gpu tests/enhancement_options_gpu.cpp)
target_link_libraries(enhancement_options_gpu PRIVATE nvof_fractional_refiner nvof_gpu_engine nvof_engine nvof_phase_pipeline)

# GPU bit-depth normalization regression, requires an NVIDIA GPU/runtime.
add_executable(gpu_p010_conversion tests/gpu_p010_conversion.cpp)
target_link_libraries(gpu_p010_conversion PRIVATE nvof_gpu_engine nvof_mf_bridge)

add_executable(p010_video_stress tests/p010_video_stress.cpp)
target_link_libraries(p010_video_stress PRIVATE nvof_gpu_engine nvof_gpu_phase_pipeline)
