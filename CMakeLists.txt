cmake_minimum_required(VERSION 3.21)

project(
	TheosRenderPipeline
	VERSION 0.3.0
	LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

add_compile_definitions(SKYRIM)

add_library("${PROJECT_NAME}" SHARED)
include(cmake/ReleaseMetadata.cmake)

target_compile_features("${PROJECT_NAME}" PRIVATE cxx_std_23)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
set(BUILD_TESTS OFF)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)

function(arp_configure_plugin_target target)
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        target_compile_options(${target} PRIVATE /MP /W4 /permissive- /Zc:__cplusplus)
        target_link_options(${target} PRIVATE
            "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
            "$<$<CONFIG:RELEASE>:/LTCG;/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>")
    endif()
endfunction()

include(cmake/Detours.cmake)
include(cmake/BaselineSources.cmake)
target_sources(${PROJECT_NAME} PRIVATE ${ARP_BASELINE_SOURCES})

option(TRP_ENABLE_NEURAL_RENDERING "Include Neural Rendering integration and controls" ON)
option(TRP_ENABLE_OPTIONAL_FEATURES "Include optional Ada/Ampere/Turing MFG compatibility" ON)
if(TRP_ENABLE_NEURAL_RENDERING)
    target_sources(TheosRenderPipeline PRIVATE ${ARP_NEURAL_SOURCES})
else()
    target_compile_definitions(TheosRenderPipeline PRIVATE TRP_NO_NEURAL_RENDERING)
    target_sources(TheosRenderPipeline PRIVATE src/FrameGen/SourceDLSSGNeuralDisabled.cpp)
endif()
if(TRP_ENABLE_OPTIONAL_FEATURES)
    set(TRP_EDITION "Universal")
    target_sources(TheosRenderPipeline PRIVATE src/FrameGen/SourceDLSSGMFG.cpp)
    add_library(TheosRenderPipelineRTX40MFG STATIC
        extern/RTX40MFG/midpoint_fix.cpp extern/RTX40MFG/dlssg_provider_policy.cpp
        extern/MFGAmpere/runtime.cpp)
    target_compile_features(TheosRenderPipelineRTX40MFG PRIVATE cxx_std_23)
    target_compile_definitions(TheosRenderPipelineRTX40MFG PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(TheosRenderPipelineRTX40MFG PUBLIC bcrypt version d3d12)
    target_link_libraries(TheosRenderPipeline PRIVATE TheosRenderPipelineRTX40MFG)
else()
    target_sources(TheosRenderPipeline PRIVATE src/FrameGen/SourceDLSSGMFGNative.cpp)
    set(TRP_EDITION "Standard")
endif()

# A source archive nested inside another checkout must not report its parent's revision.
execute_process(COMMAND git rev-parse --show-toplevel WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE TRP_GIT_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(TRP_SOURCE_REVISION "source-archive")
if(TRP_GIT_ROOT STREQUAL PROJECT_SOURCE_DIR)
    execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE TRP_SOURCE_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND git diff --quiet HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        RESULT_VARIABLE TRP_SOURCE_DIRTY ERROR_QUIET)
    if(TRP_SOURCE_DIRTY)
        string(APPEND TRP_SOURCE_REVISION "-modified")
    endif()
endif()

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/Plugin.h.in
	${CMAKE_CURRENT_BINARY_DIR}/cmake/Plugin.h
	@ONLY
)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.rc.in
	${CMAKE_CURRENT_BINARY_DIR}/cmake/Version.rc
	@ONLY
)

target_sources(
	"${PROJECT_NAME}"
	PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}/cmake/Plugin.h
	${CMAKE_CURRENT_BINARY_DIR}/cmake/Version.rc
)

target_precompile_headers("${PROJECT_NAME}" PRIVATE include/PCH.h)

# Keep the adapter's Windows macro environment independent of the renderer PCH.
set_source_files_properties(compatibility/ImGuiCompat/ImGuiIntegration.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_DEFINITIONS "UNICODE;_UNICODE;WIN32_LEAN_AND_MEAN;NOMINMAX")

if(CMAKE_GENERATOR MATCHES "Visual Studio")
	add_compile_definitions(_UNICODE)
	target_compile_definitions(${PROJECT_NAME} PRIVATE "$<$<CONFIG:DEBUG>:DEBUG>")

	arp_configure_plugin_target("${PROJECT_NAME}")
	target_compile_options("${PROJECT_NAME}" PRIVATE /arch:AVX)

endif()

# Dependency paths can be supplied directly or placed under .dependencies/.
set(
	TRP_COMMONLIBSSE_NG_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/.dependencies/CommonLibSSE-NG"
	CACHE PATH
	"Path to a CommonLibSSE-NG checkout"
)

if(NOT EXISTS "${TRP_COMMONLIBSSE_NG_DIR}/CMakeLists.txt")
	message(
		FATAL_ERROR
		"CommonLibSSE-NG was not found at '${TRP_COMMONLIBSSE_NG_DIR}'. "
		"See README.md or set TRP_COMMONLIBSSE_NG_DIR."
	)
endif()

add_subdirectory("${TRP_COMMONLIBSSE_NG_DIR}" CommonLibSSE EXCLUDE_FROM_ALL)
get_directory_property(TRP_COMMONLIB_VERSION DIRECTORY "${TRP_COMMONLIBSSE_NG_DIR}" DEFINITION PROJECT_VERSION)
if(NOT TRP_COMMONLIB_VERSION VERSION_EQUAL "8.1.0")
    message(FATAL_ERROR "This runtime port requires CommonLibSSE-NG 8.1.0; see README.md for the pinned revision.")
endif()

if(ENABLE_SKYRIM_VR OR NOT ENABLE_SKYRIM_SE OR NOT ENABLE_SKYRIM_AE)
    message(FATAL_ERROR "Enable both Skyrim SE and AE, with VR disabled, for the universal 1.5.97/1.6.640/1.6.1170/1.7.104 build.")
endif()

find_package(spdlog CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

set(
	TRP_NGX_SDK_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/.dependencies/Streamline/external/ngx-sdk"
	CACHE PATH
	"Path to the NVIDIA NGX SDK root containing include/nvsdk_ngx.h"
)

if(NOT EXISTS "${TRP_NGX_SDK_DIR}/include/nvsdk_ngx.h")
	message(
		FATAL_ERROR
		"NGX SDK headers were not found under '${TRP_NGX_SDK_DIR}/include'. "
		"Install the NGX SDK or configure with -D TRP_NGX_SDK_DIR=<path>."
	)
endif()

if(TRP_ENABLE_OPTIONAL_FEATURES)
    target_include_directories(TheosRenderPipelineRTX40MFG PRIVATE "${TRP_NGX_SDK_DIR}/include")
endif()

set(TRP_DEFAULT_NGX_LIB_DIR "${TRP_NGX_SDK_DIR}/lib/Windows_x86_64")
if(NOT EXISTS "${TRP_DEFAULT_NGX_LIB_DIR}/nvsdk_ngx_d.lib")
	set(TRP_DEFAULT_NGX_LIB_DIR "${TRP_NGX_SDK_DIR}/lib/x64")
endif()

set(
	TRP_NGX_LIB
	"${TRP_DEFAULT_NGX_LIB_DIR}/nvsdk_ngx_d.lib"
	CACHE FILEPATH
	"Path to the NGX dynamic import library, usually nvsdk_ngx_d.lib for /MD builds"
)

if(NOT EXISTS "${TRP_NGX_LIB}")
	message(
		FATAL_ERROR
		"NGX import library was not found at '${TRP_NGX_LIB}'. "
		"Configure with -D TRP_NGX_LIB=<path-to-nvsdk_ngx_d.lib>."
	)
endif()

set(TRP_STREAMLINE_INCLUDE_DIR "${TRP_NGX_SDK_DIR}/../../include" CACHE PATH
	"Path to Streamline 2.11.1 public headers")
if(NOT EXISTS "${TRP_STREAMLINE_INCLUDE_DIR}/sl_core_api.h")
	message(FATAL_ERROR "Set TRP_STREAMLINE_INCLUDE_DIR to the Streamline 2.11.1 include directory")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE "${TRP_STREAMLINE_INCLUDE_DIR}")
find_path(TRP_SIMPLEINI_INCLUDE_DIR SimpleIni.h REQUIRED)

target_include_directories(
	${PROJECT_NAME}
	PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}/cmake
	${CMAKE_CURRENT_SOURCE_DIR}/src
	"${TRP_NGX_SDK_DIR}/include"
	"${TRP_SIMPLEINI_INCLUDE_DIR}"
)

target_link_libraries(
	${PROJECT_NAME}
	PUBLIC
	CommonLibSSE::CommonLibSSE
	PRIVATE
	ARPDetours
	imgui::imgui
	d3d11.lib
	d3d12.lib
	d3dcompiler.lib
	dxgi.lib
	dxguid.lib
	version.lib
	bcrypt.lib
	"${TRP_NGX_LIB}"
)

find_package(xbyak CONFIG REQUIRED)

# Build the plugin without packaging, deployment or game launch.
add_custom_target(ARPBaseline DEPENDS TheosRenderPipeline)

option(TRP_BUILD_COMPATIBILITY_TESTS "Build offline runtime and frame contract checks" OFF)
if(TRP_BUILD_COMPATIBILITY_TESTS)
    enable_testing()
    add_executable(TRPOverlayHotkeysTests tests/OverlayHotkeysTests.cpp src/OverlayHotkeys.cpp)
    target_compile_features(TRPOverlayHotkeysTests PRIVATE cxx_std_20)
    target_include_directories(TRPOverlayHotkeysTests PRIVATE src)
    target_link_libraries(TRPOverlayHotkeysTests PRIVATE user32)
    add_test(NAME OverlayHotkeys COMMAND TRPOverlayHotkeysTests)
    set_tests_properties(OverlayHotkeys PROPERTIES TIMEOUT 15)
    add_executable(TRPOverlayLayoutTests tests/OverlayLayoutTests.cpp src/OverlayLayout.cpp)
    target_compile_features(TRPOverlayLayoutTests PRIVATE cxx_std_20)
    target_compile_definitions(TRPOverlayLayoutTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPOverlayLayoutTests PRIVATE src "${TRP_SIMPLEINI_INCLUDE_DIR}")
    target_link_libraries(TRPOverlayLayoutTests PRIVATE imgui::imgui)
    add_test(NAME OverlayLayout COMMAND TRPOverlayLayoutTests)
    set_tests_properties(OverlayLayout PROPERTIES TIMEOUT 15)
    add_executable(TRPStartupPreferencesTests tests/StartupPreferencesTests.cpp)
    target_compile_features(TRPStartupPreferencesTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPStartupPreferencesTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPStartupPreferencesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${TRP_SIMPLEINI_INCLUDE_DIR}")
    add_test(NAME StartupPreferences COMMAND TRPStartupPreferencesTests "${CMAKE_CURRENT_SOURCE_DIR}/package/SKSE/Plugins/TheosRenderPipeline.ini")
    add_executable(TRPRendererSettingsActionTests tests/RendererSettingsActionTests.cpp)
    target_compile_features(TRPRendererSettingsActionTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPRendererSettingsActionTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPRendererSettingsActionTests PRIVATE src include "${TRP_SIMPLEINI_INCLUDE_DIR}" "${TRP_STREAMLINE_INCLUDE_DIR}")
    add_test(NAME RendererSettingsActions COMMAND TRPRendererSettingsActionTests)
    add_executable(TRPRuntimeModuleDiagnosticTests tests/RuntimeModuleDiagnosticTests.cpp)
    target_compile_features(TRPRuntimeModuleDiagnosticTests PRIVATE cxx_std_23)
    target_include_directories(TRPRuntimeModuleDiagnosticTests PRIVATE src)
    add_test(NAME RuntimeModuleDiagnostics COMMAND TRPRuntimeModuleDiagnosticTests)
    add_subdirectory(tests/ampere-runtime)
    add_subdirectory(tests/turing)
    add_executable(TRPSourceDLSSGSessionTests tests/SourceDLSSGSessionTests.cpp src/FrameGen/SourceDLSSGSession.cpp)
    target_compile_features(TRPSourceDLSSGSessionTests PRIVATE cxx_std_23)
    target_include_directories(TRPSourceDLSSGSessionTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${TRP_STREAMLINE_INCLUDE_DIR}")
    add_test(NAME SourceDLSSGSession COMMAND TRPSourceDLSSGSessionTests)
    add_executable(TRPSourceDLSSGCameraTests tests/SourceDLSSGCameraTests.cpp
        src/FrameGen/SourceDLSSGCameraMath.cpp src/FrameGen/SourceDLSSGSession.cpp)
    target_compile_features(TRPSourceDLSSGCameraTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPSourceDLSSGCameraTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPSourceDLSSGCameraTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${TRP_STREAMLINE_INCLUDE_DIR}")
    add_test(NAME SourceDLSSGCamera COMMAND TRPSourceDLSSGCameraTests)
    add_executable(TRPDLSSQualityTests tests/DLSSQualityTests.cpp)
    target_compile_features(TRPDLSSQualityTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPDLSSQualityTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPDLSSQualityTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${TRP_NGX_SDK_DIR}/include" "${TRP_STREAMLINE_INCLUDE_DIR}")
    add_test(NAME DLSSQuality COMMAND TRPDLSSQualityTests)
    add_executable(TRPReShadeIntegrationTests tests/ReShadeIntegrationTests.cpp src/ReShadeIntegration.cpp)
    target_compile_features(TRPReShadeIntegrationTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPReShadeIntegrationTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPReShadeIntegrationTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(TRPReShadeIntegrationTests PRIVATE d3d11 d3d12 d3dcompiler dxgi)
    add_test(NAME ReShadeAbsent COMMAND TRPReShadeIntegrationTests)
    add_executable(TRPSourceDLSSGInteropFenceTests tests/SourceDLSSGInteropFenceTests.cpp
        src/FrameGen/SourceDLSSGInterop.cpp)
    target_compile_features(TRPSourceDLSSGInteropFenceTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPSourceDLSSGInteropFenceTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPSourceDLSSGInteropFenceTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(TRPSourceDLSSGInteropFenceTests PRIVATE d3d11 d3d12 dxgi d3dcompiler)
    add_test(NAME SourceDLSSGInteropFence COMMAND TRPSourceDLSSGInteropFenceTests)
    set_tests_properties(SourceDLSSGInteropFence PROPERTIES TIMEOUT 30)
    foreach(module IN ITEMS 1 2)
        add_library(TRPModuleIdentityFixture${module} SHARED tests/ModuleIdentityFixture.cpp)
        target_compile_definitions(TRPModuleIdentityFixture${module} PRIVATE TRP_TEST_MODULE_ID=${module})
        set_target_properties(TRPModuleIdentityFixture${module} PROPERTIES
            OUTPUT_NAME "TRPModuleIdentityFixture"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/module-fixtures/${module}"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/module-fixtures/${module}")
    endforeach()
    add_executable(TRPPluginPathsTests tests/PluginPathsTests.cpp)
    target_compile_features(TRPPluginPathsTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPPluginPathsTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPPluginPathsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(TRPPluginPathsTests PRIVATE ARPDetours)
    add_dependencies(TRPPluginPathsTests TRPModuleIdentityFixture1 TRPModuleIdentityFixture2)
    add_subdirectory(tests/ampere-loader)
    foreach(order IN ITEMS a-first b-first)
        add_test(NAME PluginPaths-${order} COMMAND TRPPluginPathsTests
            $<TARGET_FILE:TRPModuleIdentityFixture1> $<TARGET_FILE:TRPModuleIdentityFixture2> ${order} direct)
        add_test(NAME PluginPaths-virtual-${order} COMMAND TRPPluginPathsTests
            $<TARGET_FILE:TRPModuleIdentityFixture1> $<TARGET_FILE:TRPModuleIdentityFixture2> ${order} virtual)
    endforeach()
    add_executable(TRPD3D11FrameCopyTests tests/D3D11FrameCopyTests.cpp)
    target_compile_features(TRPD3D11FrameCopyTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPD3D11FrameCopyTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPD3D11FrameCopyTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(TRPD3D11FrameCopyTests PRIVATE d3d11 d3dcompiler dxgi)
    add_test(NAME D3D11FrameCopy COMMAND TRPD3D11FrameCopyTests)
    add_executable(TRPNeuralRatioColorTests tests/NeuralRatioColorTests.cpp)
    target_compile_features(TRPNeuralRatioColorTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPNeuralRatioColorTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPNeuralRatioColorTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(TRPNeuralRatioColorTests PRIVATE d3d11 d3dcompiler)
    add_test(NAME NeuralRatioColor COMMAND TRPNeuralRatioColorTests)
    add_subdirectory(tests/nr-peripheral)
    add_executable(TRPCommunityShaderFrameTests tests/CommunityShaderFrameTests.cpp)
    target_compile_features(TRPCommunityShaderFrameTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPCommunityShaderFrameTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPCommunityShaderFrameTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(TRPCommunityShaderFrameTests PRIVATE d3d11 d3dcompiler)
    add_test(NAME CommunityShaderFrame COMMAND TRPCommunityShaderFrameTests)
    add_executable(TRPNeuralWorldContractTests tests/NeuralWorldContractTests.cpp)
    target_compile_features(TRPNeuralWorldContractTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPNeuralWorldContractTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPNeuralWorldContractTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${TRP_STREAMLINE_INCLUDE_DIR}")
    add_test(NAME NeuralWorldContract COMMAND TRPNeuralWorldContractTests)
    add_executable(TRPSourceNvidiaFrameEvaluatorTests tests/SourceNvidiaFrameEvaluatorTests.cpp)
    target_compile_features(TRPSourceNvidiaFrameEvaluatorTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPSourceNvidiaFrameEvaluatorTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPSourceNvidiaFrameEvaluatorTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(TRPSourceNvidiaFrameEvaluatorTests PRIVATE d3d11)
    add_test(NAME SourceNvidiaFrameEvaluation COMMAND TRPSourceNvidiaFrameEvaluatorTests)
    add_executable(TRPRuntimeProfileTests tests/RuntimeProfileTests.cpp)
    add_executable(TRPHookSafetyTests tests/HookSafetyTests.cpp)
    target_compile_features(TRPHookSafetyTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPHookSafetyTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPHookSafetyTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(TRPHookSafetyTests PRIVATE user32)
    add_test(NAME HookSafety COMMAND TRPHookSafetyTests)
    add_executable(TRPHookChainTests tests/HookChainTests.cpp)
    target_compile_features(TRPHookChainTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPHookChainTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPHookChainTests PRIVATE src)
    target_link_libraries(TRPHookChainTests PRIVATE ARPDetours CommonLibSSE::CommonLibSSE)
    add_test(NAME HookChains COMMAND TRPHookChainTests)
    set_tests_properties(HookChains PROPERTIES TIMEOUT 15)
    target_compile_features(TRPRuntimeProfileTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPRuntimeProfileTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(TRPRuntimeProfileTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(TRPRuntimeProfileTests PRIVATE CommonLibSSE::CommonLibSSE)
    add_test(NAME RuntimeProfiles COMMAND TRPRuntimeProfileTests)
    add_executable(TRPRuntimeLayoutTests tests/RuntimeLayoutTests.cpp)
    target_compile_features(TRPRuntimeLayoutTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPRuntimeLayoutTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN ENABLE_COMMONLIBSSE_TESTING=1)
    target_include_directories(TRPRuntimeLayoutTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(TRPRuntimeLayoutTests PRIVATE CommonLibSSE::CommonLibSSE)
    foreach(runtime IN ITEMS 1.5.97 1.6.640 1.6.1170 1.7.104)
        add_test(NAME RuntimeLayout-${runtime} COMMAND TRPRuntimeLayoutTests ${runtime})
    endforeach()
endif()
