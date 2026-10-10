cmake_minimum_required(VERSION 3.20)
project(NvofPotPlayer VERSION 0.4.0 LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NVOF_ENABLE_EXPERIMENTAL_SUBPIXEL=0)
if(MSVC)
  add_compile_options(/utf-8 /EHsc /W3)
endif()
enable_testing()
find_path(NVOF_API_INCLUDE_DIR nvOpticalFlowD3D11.h HINTS ENV OPTICAL_FLOW_SDK
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpticalFlowSDK"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpticalFlowSDK/Optical_Flow_SDK_5.0.7"
  PATH_SUFFIXES NvOFInterface)
if(NOT NVOF_API_INCLUDE_DIR)
  message(FATAL_ERROR "Official Optical Flow SDK NvOFInterface headers required; SDK is not bundled.")
endif()
set(BASE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/windows_samples/Samples/Win7Samples/multimedia/directshow/baseclasses")
file(GLOB BASE_SOURCES "${BASE}/*.cpp")
add_library(strmbase STATIC ${BASE_SOURCES})
target_include_directories(strmbase PUBLIC "${BASE}")
target_compile_definitions(strmbase PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NON_CONFORMING_SWPRINTFS _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS)
target_compile_options(strmbase PRIVATE /wd4312 /wd4311 /wd4996 /wd4244)
foreach(component pipeline gpu_pipeline phase_pipeline gpu_phase_pipeline motion_synthesizer gpu_engine engine mf_bridge)
  add_library(nvof_${component} STATIC src/${component}.cpp)
  target_include_directories(nvof_${component} PUBLIC include)
  target_compile_definitions(nvof_${component} PUBLIC NOMINMAX)
endforeach()
target_include_directories(nvof_motion_synthesizer PRIVATE "${NVOF_API_INCLUDE_DIR}")
target_link_libraries(nvof_motion_synthesizer PUBLIC d3d11 dxgi d3dcompiler)
target_link_libraries(nvof_gpu_engine PUBLIC nvof_motion_synthesizer d3d11 dxgi)
target_link_libraries(nvof_engine PUBLIC nvof_gpu_engine)
target_link_libraries(nvof_mf_bridge PUBLIC mfplat mfuuid d3d11 dxgi ole32 oleaut32 strmiids strmbase winmm advapi32)
add_library(NvofPotPlayer SHARED src/directshow_filter.cpp src/filter.def src/filter.rc)
target_link_libraries(NvofPotPlayer PRIVATE nvof_engine nvof_pipeline nvof_gpu_engine nvof_gpu_pipeline
  nvof_phase_pipeline nvof_gpu_phase_pipeline nvof_mf_bridge strmbase strmiids ole32 oleaut32 uuid winmm advapi32 shell32)
set_target_properties(NvofPotPlayer PROPERTIES PREFIX "" SUFFIX ".ax")
add_executable(NvofRegister src/register_filter.cpp)
target_link_libraries(NvofRegister PRIVATE advapi32 ole32 oleaut32 strmiids)
foreach(test pipeline gpu_pipeline phase_pipeline gpu_phase_pipeline)
  add_executable(${test}_tests tests/${test}_tests.cpp)
  target_link_libraries(${test}_tests PRIVATE nvof_${test} d3d11)
  add_test(NAME ${test} COMMAND ${test}_tests)
endforeach()
foreach(test input_rate_policy output_rate_policy)
  add_executable(${test}_tests tests/${test}_tests.cpp)
  target_link_libraries(${test}_tests PRIVATE nvof_pipeline)
  add_test(NAME ${test} COMMAND ${test}_tests)
endforeach()
add_executable(runtime_dependencies tests/runtime_dependencies.cpp)
foreach(test directshow_smoke directshow_gpu_smoke)
  add_executable(${test} tests/${test}.cpp)
  target_link_libraries(${test} PRIVATE nvof_mf_bridge strmbase strmiids ole32 oleaut32 uuid winmm advapi32 d3d11 dxgi)
endforeach()
foreach(test native_synthesis_compare native_motion_guard native_motion_layers native_startup native_cost_map
             gpu_interop_stress gpu_p010_conversion p010_video_stress video_x2_repro native_cpu_transport)
  add_executable(${test} tests/${test}.cpp)
  target_link_libraries(${test} PRIVATE nvof_engine nvof_gpu_engine nvof_gpu_pipeline nvof_gpu_phase_pipeline nvof_mf_bridge)
endforeach()

# Compile the playback shaders once, on the build machine. No HLSL compilation
# is performed when a player opens a video. Build failures abort packaging.
add_executable(nvof_shader_compiler tools/shader_compiler.cpp)
target_link_libraries(nvof_shader_compiler PRIVATE d3dcompiler)
function(nvof_shaders group source_label)
  set(header "${CMAKE_CURRENT_BINARY_DIR}/generated/${group}_shaders.hpp")
  add_custom_command(OUTPUT "${header}"
    COMMAND nvof_shader_compiler "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${group}.hlsl" "${header}" "${group}" "${source_label}" ${ARGN}
    DEPENDS nvof_shader_compiler "src/shaders/${group}.hlsl" VERBATIM)
  target_sources(nvof_gpu_engine PRIVATE "${header}")
endfunction()
nvof_shaders(blend nvof-gpu-blend vs vs_4_0 ps ps_4_0)
nvof_shaders(p010 nvof-gpu-blend ps ps_5_0)
nvof_shaders(scene nvof-scene main cs_5_0)
nvof_shaders(motion nvof-motion-synthesis vs vs_5_0 midpoint ps_5_0 downsample ps_5_0 inverseMap ps_5_0 inverseMapBlend ps_5_0 inverseMapOff ps_5_0 stationaryMask ps_5_0 expandStationaryMask ps_5_0 repairMotion ps_5_0 glyphGroup cs_5_0 assessMotion cs_5_0)
# A dedicated dependency prevents parallel builds from compiling the consumer
# before its generated header exists.
add_custom_target(nvof_shader_headers DEPENDS
 "${CMAKE_CURRENT_BINARY_DIR}/generated/motion_shaders.hpp"
 "${CMAKE_CURRENT_BINARY_DIR}/generated/blend_shaders.hpp"
 "${CMAKE_CURRENT_BINARY_DIR}/generated/p010_shaders.hpp"
 "${CMAKE_CURRENT_BINARY_DIR}/generated/scene_shaders.hpp")
foreach(consumer nvof_gpu_engine nvof_motion_synthesizer)
 add_dependencies(${consumer} nvof_shader_headers)
 target_include_directories(${consumer} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endforeach()


add_dependencies(native_cost_map nvof_shader_headers)
target_include_directories(native_cost_map PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_executable(native_session_reuse tests/native_session_reuse.cpp)
target_link_libraries(native_session_reuse PRIVATE nvof_engine nvof_gpu_engine nvof_gpu_pipeline nvof_gpu_phase_pipeline nvof_mf_bridge)
add_dependencies(native_session_reuse nvof_shader_headers)
target_include_directories(native_session_reuse PRIVATE "${NVOF_API_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_executable(native_motion_sampling tests/native_motion_sampling.cpp)
target_link_libraries(native_motion_sampling PRIVATE nvof_engine nvof_gpu_engine nvof_gpu_pipeline nvof_gpu_phase_pipeline nvof_mf_bridge)
add_dependencies(native_motion_sampling nvof_shader_headers)
target_include_directories(native_motion_sampling PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_test(NAME motion_sampling COMMAND native_motion_sampling)

add_executable(native_overlay_probe tests/native_overlay_probe.cpp)
target_link_libraries(native_overlay_probe PRIVATE nvof_engine nvof_gpu_engine nvof_gpu_pipeline nvof_gpu_phase_pipeline nvof_mf_bridge)
add_dependencies(native_overlay_probe nvof_shader_headers)
target_include_directories(native_overlay_probe PRIVATE "${NVOF_API_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_executable(native_stationary_plate tests/native_stationary_plate.cpp)
target_link_libraries(native_stationary_plate PRIVATE nvof_engine nvof_gpu_engine nvof_gpu_pipeline nvof_gpu_phase_pipeline nvof_mf_bridge)
add_dependencies(native_stationary_plate nvof_shader_headers)
target_include_directories(native_stationary_plate PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_test(NAME stationary_plate COMMAND native_stationary_plate)

add_executable(native_credit_scroll tests/native_credit_scroll.cpp)
target_link_libraries(native_credit_scroll PRIVATE nvof_engine nvof_gpu_engine nvof_gpu_pipeline nvof_gpu_phase_pipeline nvof_mf_bridge)
add_dependencies(native_credit_scroll nvof_shader_headers)
target_include_directories(native_credit_scroll PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_test(NAME credit_scroll COMMAND native_credit_scroll)

add_executable(native_stroke_alias tests/native_stroke_alias.cpp)
target_link_libraries(native_stroke_alias PRIVATE nvof_engine nvof_gpu_engine nvof_gpu_pipeline nvof_gpu_phase_pipeline nvof_mf_bridge)
add_dependencies(native_stroke_alias nvof_shader_headers)
target_include_directories(native_stroke_alias PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_test(NAME stroke_alias COMMAND native_stroke_alias)
