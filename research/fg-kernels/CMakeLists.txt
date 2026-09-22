cmake_minimum_required(VERSION 3.24)
project(TRPFrameGenerationProbe LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "This isolated probe requires Windows and the MSVC NGX ABI")
endif()
set(NGX_SDK "" CACHE PATH "Local Streamline external/ngx-sdk")
if(NOT EXISTS "${NGX_SDK}/include/nvsdk_ngx.h")
  message(FATAL_ERROR "Set NGX_SDK")
endif()
set(REPO "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(TRPFrameGenerationProbe Probe.cpp Observer.cpp
  "${REPO}/extern/RTX40MFG/midpoint_fix.cpp"
  "${REPO}/extern/RTX40MFG/dlssg_provider_policy.cpp"
  "${REPO}/src/FrameGen/NeuralRenderingRuntimeIdentity.cpp")
# The provider's direct-call contract checks the caller module name. This is
# an isolated oracle process, not a replacement NGX DLL or an installed proxy.
set_target_properties(TRPFrameGenerationProbe PROPERTIES OUTPUT_NAME "nvngx.dll.FGProbe" MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
target_compile_features(TRPFrameGenerationProbe PRIVATE cxx_std_20)
target_compile_definitions(TRPFrameGenerationProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(TRPFrameGenerationProbe PRIVATE /W4 /EHsc)
target_include_directories(TRPFrameGenerationProbe PRIVATE . "${REPO}/src" "${REPO}/extern/RTX40MFG" "${NGX_SDK}/include")
target_link_libraries(TRPFrameGenerationProbe PRIVATE d3d12 dxgi bcrypt version "${NGX_SDK}/lib/Windows_x86_64/nvsdk_ngx_d.lib")
include(CTest)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME FGProbeAnalysis COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_analysis.py")
