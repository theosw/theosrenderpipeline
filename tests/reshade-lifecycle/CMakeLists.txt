cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(TRPReShadeLifecycleTests LANGUAGES CXX)
    enable_testing()
endif()

add_executable(TRPReShadeLifecycleTests ReShadeLifecycleTests.cpp)
target_compile_features(TRPReShadeLifecycleTests PRIVATE cxx_std_20)
target_compile_definitions(TRPReShadeLifecycleTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPReShadeLifecycleTests PRIVATE ../../include ../../src)
target_link_libraries(TRPReShadeLifecycleTests PRIVATE d3d11 dxgi user32)

# Optional real-runtime checks: no injector download or installation at configure/build time.
set(TRP_RESHADE_TEST_RUNTIME "" CACHE FILEPATH "Supplied x64 ReShade DLL for isolated runtime tests")
set(TRP_RESHADE_TEST_INPUT_POLICY "observe" CACHE STRING "Shared input expectation: observe, every-runtime, first-runtime")
set_property(CACHE TRP_RESHADE_TEST_INPUT_POLICY PROPERTY STRINGS observe every-runtime first-runtime)
if(NOT TRP_RESHADE_TEST_INPUT_POLICY MATCHES "^(observe|every-runtime|first-runtime)$")
    message(FATAL_ERROR "Invalid TRP_RESHADE_TEST_INPUT_POLICY")
endif()
if(TRP_RESHADE_TEST_RUNTIME)
    if(NOT EXISTS "${TRP_RESHADE_TEST_RUNTIME}")
        message(FATAL_ERROR "TRP_RESHADE_TEST_RUNTIME must name an existing DLL")
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ReShadeLifecycleRuntime COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/run.py"
        --exe "$<TARGET_FILE:TRPReShadeLifecycleTests>"
        --runtime "${TRP_RESHADE_TEST_RUNTIME}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/runs/$<CONFIG>"
        --input-policy "${TRP_RESHADE_TEST_INPUT_POLICY}")
    set_tests_properties(ReShadeLifecycleRuntime PROPERTIES TIMEOUT 360 RUN_SERIAL TRUE LABELS "ReShade;runtime")
endif()
