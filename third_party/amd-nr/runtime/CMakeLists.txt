# The in-game runtime: dlss5nr_runtime.dll with its gate (nvcuda.dll), its
# NGX host (nvngx.dll), the cache preparation tool and a test harness.
#
#   cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# With Visual Studio (build.bat), or with mingw-w64 from Linux (build.sh, which
# passes mingw.cmake and builds into build-mingw).
#
# Output, laid out as it ships (build\bin):
#   dlss5nr_runtime.dll, dlss5nr_prepare.exe, dlss5nr_harness.exe,
#   dlss5nr_interop_probe.exe
#   zluda\nvcuda.dll (the gate), zluda\nvngx.dll

cmake_minimum_required(VERSION 3.20)
project(dlss5nr_runtime LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The runtime is Windows only: the network is a Windows library.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    # Statically linked C runtime: a game brings its own redistributable, or none.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W3 /utf-8)
    set(D3DCOMPILER d3dcompiler)
else()
    # mingw: the same, with libgcc, libstdc++ and winpthreads linked in, and
    # `lib` not put in front of the DLLs' names.
    add_link_options(-static)
    set(CMAKE_SHARED_LIBRARY_PREFIX "")
    set(D3DCOMPILER d3dcompiler_47)
endif()

set(BIN ${CMAKE_BINARY_DIR}/bin)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ---------------------------------------------------------------- the gate
# MSVC takes the forwarders as linker pragmas in the header; GNU ld reads a .def,
# which MSVC would take for exports of symbols of its own and fail to link.
if(MSVC)
    set(GATE_DEF "")
else()
    set(GATE_DEF ${CMAKE_BINARY_DIR}/gate.def)
endif()
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/gate_forwards.h ${GATE_DEF}
    COMMAND Python3::Interpreter -B ${CMAKE_CURRENT_SOURCE_DIR}/gate/gen_forwards.py
            ${CMAKE_CURRENT_SOURCE_DIR}/gate/zluda_exports.txt ${CMAKE_BINARY_DIR}/gate_forwards.h
            ${GATE_DEF}
    DEPENDS gate/gen_forwards.py gate/memory_api.py gate/zluda_exports.txt
    COMMENT "Generating the gate's forwarders")
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/memory_wrappers.h
    COMMAND Python3::Interpreter -B ${CMAKE_CURRENT_SOURCE_DIR}/gate/memory_api.py ${CMAKE_BINARY_DIR}/memory_wrappers.h
    DEPENDS gate/memory_api.py)
add_library(gate SHARED gate/gate.cpp gate/native_hook.h gate/memory_trace.h
            ${CMAKE_BINARY_DIR}/memory_wrappers.h ${CMAKE_BINARY_DIR}/gate_forwards.h
            ${GATE_DEF})
target_include_directories(gate PRIVATE ${CMAKE_BINARY_DIR} gate)
target_compile_definitions(gate PRIVATE TRP_AMD_DIAGNOSTIC_LABEL="TRP-0.7.0")
set_target_properties(gate PROPERTIES OUTPUT_NAME nvcuda RUNTIME_OUTPUT_DIRECTORY ${BIN}/zluda)

# ----------------------------------------------------------- the NGX host
add_library(ngx SHARED ngx/ngx_runtime.cpp common/ngx_cuda.h)
set_target_properties(ngx PROPERTIES OUTPUT_NAME nvngx RUNTIME_OUTPUT_DIRECTORY ${BIN}/zluda)

# ------------------------------------------------------------- the runtime
add_library(dlss5nr_runtime SHARED
    include/dlss5nr.h
    core/runtime.cpp
    core/network.cpp core/network.h
    core/passes.cpp core/passes.h core/shaders.h
    common/cuda_min.h common/ngx_cuda.h)
target_link_libraries(dlss5nr_runtime PRIVATE d3d12 ${D3DCOMPILER} version)
target_compile_definitions(dlss5nr_runtime PRIVATE TRP_AMD_DIAGNOSTIC_LABEL="TRP-0.7.0")
set_target_properties(dlss5nr_runtime PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${BIN})
if(MSVC)
    foreach(target gate ngx dlss5nr_runtime)
        target_compile_options(${target} PRIVATE /Zi)
        target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endforeach()
endif()

# ------------------------------------------------------------ test harness
add_executable(dlss5nr_harness tests/harness.cpp)
target_include_directories(dlss5nr_harness PRIVATE include)
target_link_libraries(dlss5nr_harness PRIVATE dlss5nr_runtime d3d12 dxgi)
if(MINGW)
    target_link_libraries(dlss5nr_harness PRIVATE windowscodecs ole32 uuid)
    target_link_options(dlss5nr_harness PRIVATE -municode)
endif()
set_target_properties(dlss5nr_harness PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${BIN})

enable_testing()
add_executable(dlss5nr_tracking_pixels tests/tracking_pixels.cpp core/passes.cpp)
target_link_libraries(dlss5nr_tracking_pixels PRIVATE d3d12 dxgi version ${D3DCOMPILER})
add_test(NAME AMDTrackingPixels COMMAND dlss5nr_tracking_pixels)
set_tests_properties(AMDTrackingPixels PROPERTIES TIMEOUT 120)
add_executable(dlss5nr_tracking_bytecode tests/tracking_pixels.cpp core/passes.cpp)
target_compile_definitions(dlss5nr_tracking_bytecode PRIVATE DLSS5NR_TEST_BYTECODE)
target_link_libraries(dlss5nr_tracking_bytecode PRIVATE d3d12 dxgi version ${D3DCOMPILER})
add_test(NAME AMDTrackingBytecode COMMAND dlss5nr_tracking_bytecode)
set_tests_properties(AMDTrackingBytecode PROPERTIES TIMEOUT 120)
add_executable(dlss5nr_gen_dxbc tools/gen_dxbc.cpp)
target_link_libraries(dlss5nr_gen_dxbc PRIVATE ${D3DCOMPILER})

# Asks a D3D12 device, under Wine or Proton, for a shared handle and the Unix
# descriptor behind it: the first thing an interop with ROCm needs.
add_executable(dlss5nr_interop_probe tests/interop_probe.cpp)
target_link_libraries(dlss5nr_interop_probe PRIVATE d3d12 dxgi)
set_target_properties(dlss5nr_interop_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${BIN})

# Private CPU-testable memory diagnostic. Does not initialize the model/game.
set(TRP_PROBE_SOURCE "unidentified" CACHE STRING "Immutable diagnostic source revision")
add_executable(dlss5nr_memory_probe tests/memory_probe.cpp)
target_compile_definitions(dlss5nr_memory_probe PRIVATE TRP_PROBE_SOURCE="${TRP_PROBE_SOURCE}" TRP_AMD_DIAGNOSTIC_LABEL="TRP-0.7.0")
target_link_libraries(dlss5nr_memory_probe PRIVATE dxgi dbghelp version)
set_target_properties(dlss5nr_memory_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${BIN})
if(MSVC)
    target_compile_options(dlss5nr_memory_probe PRIVATE /Zi)
    target_link_options(dlss5nr_memory_probe PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
option(TRP_MEMORY_MOCK_TESTS "Build CPU-only memory diagnostic oracle" OFF)
if(TRP_MEMORY_MOCK_TESTS)
    add_library(memory_mock SHARED tests/memory_mock.cpp)
    set_target_properties(memory_mock PROPERTIES OUTPUT_NAME zluda_real RUNTIME_OUTPUT_DIRECTORY ${BIN}/mock)
    add_executable(memory_dispatch_test tests/memory_dispatch_test.cpp)
    set_target_properties(memory_dispatch_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${BIN}/mock)
endif()

add_executable(dlss5nr_result_admission tests/result_admission.cpp)
add_test(NAME AMDResultAdmission COMMAND dlss5nr_result_admission)
