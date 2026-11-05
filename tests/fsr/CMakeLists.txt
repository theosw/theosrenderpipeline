cmake_minimum_required(VERSION 3.24)
project(TRPFSRTests LANGUAGES CXX)
set(TRP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(TRP_DEPENDENCIES "$ENV{TRP_DEPENDENCY_ROOT}")
set(XESS_SDK "${TRP_DEPENDENCIES}/xess" CACHE PATH "Intel XeSS SDK root")
add_library(TRPTemporalUpscalers STATIC
    "${TRP_ROOT}/src/FrameGen/FSRUpscaler.cpp"
    "${TRP_ROOT}/src/FrameGen/XeSSUpscaler.cpp"
    "${TRP_ROOT}/src/FrameGen/SourceDLSSGInterop.cpp"
    "${TRP_ROOT}/src/FrameGen/D3D12StageTimer.cpp")
target_compile_features(TRPTemporalUpscalers PUBLIC cxx_std_23)
target_compile_definitions(TRPTemporalUpscalers PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_include_directories(TRPTemporalUpscalers PUBLIC "${TRP_ROOT}/src" "${TRP_ROOT}/include" "${XESS_SDK}/inc")
target_link_libraries(TRPTemporalUpscalers PUBLIC d3d11 d3d12 dxgi d3dcompiler)
add_executable(TRPFSRSettingsTests FSRSettingsTests.cpp)
target_link_libraries(TRPFSRSettingsTests PRIVATE TRPTemporalUpscalers)
add_executable(TRPUpscalerBenchmark UpscalerBenchmark.cpp)
target_link_libraries(TRPUpscalerBenchmark PRIVATE TRPTemporalUpscalers windowscodecs ole32)
if(MSVC)
    target_compile_options(TRPTemporalUpscalers PRIVATE /W4 /permissive- /O2)
endif()
enable_testing()
add_test(NAME FSRSettings COMMAND TRPFSRSettingsTests)
