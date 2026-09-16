cmake_minimum_required(VERSION 3.21)

project(
	TheosRenderPipeline
	VERSION 0.1.0
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

option(TRP_ENABLE_OPTIONAL_FEATURES "Include Neural Rendering and the optional Ada MFG unlock" ON)
if(TRP_ENABLE_OPTIONAL_FEATURES)
    target_sources(TheosRenderPipeline PRIVATE src/FrameGen/SourceDLSSGMFG.cpp ${ARP_OPTIONAL_SOURCES})
    add_library(TheosRenderPipelineRTX40MFG STATIC
        extern/RTX40MFG/midpoint_fix.cpp extern/RTX40MFG/dlssg_provider_policy.cpp)
    target_compile_features(TheosRenderPipelineRTX40MFG PRIVATE cxx_std_23)
    target_compile_definitions(TheosRenderPipelineRTX40MFG PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(TheosRenderPipelineRTX40MFG PUBLIC bcrypt version d3d12)
    target_link_libraries(TheosRenderPipeline PRIVATE TheosRenderPipelineRTX40MFG)
else()
    target_compile_definitions(TheosRenderPipeline PRIVATE TRP_BASE_RENDERER)
    target_sources(TheosRenderPipeline PRIVATE src/FrameGen/SourceDLSSGMFGNative.cpp
        src/FrameGen/SourceDLSSGNeuralDisabled.cpp)
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
		"See docs/BUILD.md or set TRP_COMMONLIBSSE_NG_DIR."
	)
endif()

add_subdirectory("${TRP_COMMONLIBSSE_NG_DIR}" CommonLibSSE EXCLUDE_FROM_ALL)

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
