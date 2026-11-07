# Host tools are pinned by TRP's vcpkg manifest/baseline. The public Universal
# preset installs them into its own prefix, leaving supplied snapshots untouched.
set(TRP_VKNR_TOOLCHAIN "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}")
if(NOT EXISTS "${TRP_VKNR_TOOLCHAIN}/tools/glslang/glslang.exe")
    message(FATAL_ERROR "Vulkan NR needs the vulkan-nr vcpkg feature and x64-windows host tools; use the trp-universal preset")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(vknr_env "TRP_VKNR_BUILD=${CMAKE_CURRENT_BINARY_DIR}" "TRP_VKNR_TOOLS=${TRP_VKNR_TOOLCHAIN}/tools")
file(GLOB_RECURSE vknr_inputs CONFIGURE_DEPENDS "vendor/*" "shaders/*" "tools/*.py")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${vknr_env} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare-engine.py"
    RESULT_VARIABLE prepare_result)
if(NOT prepare_result EQUAL 0)
    message(FATAL_ERROR "Vulkan NR source preparation failed")
endif()
set(core "${CMAKE_CURRENT_BINARY_DIR}/engine-src/src/core")
# Use the installed system loader; never stage a vcpkg loader DLL.
set(VCPKG_APPLOCAL_DEPS OFF)
add_library(TRPVulkanNR SHARED src/trp_api.cpp src/device_context.cpp src/vk_context.cpp src/probes.cpp src/weights.cpp
    "${core}/nr_runtime.cpp" "${core}/nr_native_plan.cpp")
set_target_properties(TRPVulkanNR PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/VulkanNR"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/VulkanNR" OUTPUT_NAME TRPVulkanNR)
target_include_directories(TRPVulkanNR PRIVATE "${TRP_VKNR_TOOLCHAIN}/include" src "${core}")
target_compile_definitions(TRPVulkanNR PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    $<$<OR:$<CONFIG:Debug>,$<BOOL:${TRP_BUILD_COMPATIBILITY_TESTS}>>:TRP_VKNR_VALIDATION> NR_INT4=0 NR_ARCH_RDNA3=1 NR_STANDALONE_PORTABLE=1
    NR_GEMM_WIDE_MT=64 NR_GEMM_WIDE_NT=256 NR_GEMM_PROJ_MT=32 NR_GEMM_PROJ_NT=128
    NR_PROJW_MIN_TOKENS=768 NR_GEMM_REMAP_HOST=1 NR_FFWD_WGW=4 NR_FFWD_GMAJOR=1
    NR_UPSVIEW_VEC=1 NR_REPACK_VEC=1 NR_WIDE_UPS_MASK=7 NR_PERSIST_DF=1 NR_DS_FUSE=15
    NR_DECUPS_VEC=16 NR_DECQ_SMALL=1 NR_PERSIST_DS_MASK=7 NR_PERSIST_UPS_MASK=7
    NR_REPACK_FOLD=1 NR_STRAG_PCT=25 NR_PERSIST_ONE_MASK=6 NR_NOISE_FIELD=1
    NR_FFWD_FM2_MIN_TOKENS=2560 NR_POST_ALPHA=1 NR_TCHAIN_TILES=1)
target_compile_options(TRPVulkanNR PRIVATE /W3 /bigobj /utf-8 /fp:strict /Zi /experimental:deterministic
    "/pathmap:${PROJECT_SOURCE_DIR}=." "/pathmap:${CMAKE_CURRENT_BINARY_DIR}=./out/vknr")
target_link_libraries(TRPVulkanNR PRIVATE "${TRP_VKNR_TOOLCHAIN}/lib/vulkan-1.lib" d3d12 dxgi delayimp)
# make_device loads System32 explicitly before the first delayed import.
target_link_options(TRPVulkanNR PRIVATE /DELAYLOAD:vulkan-1.dll /DEBUG:FULL /OPT:REF /OPT:ICF /PDBALTPATH:TRPVulkanNR.pdb)
set(shader_stamp "${CMAKE_CURRENT_BINARY_DIR}/shaders.stamp")
add_custom_command(OUTPUT "${shader_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E env ${vknr_env} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile-engine-shaders.py" --backend portable
    COMMAND "${CMAKE_COMMAND}" -E env ${vknr_env} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile-engine-shaders.py" --backend rdna3
    COMMAND "${CMAKE_COMMAND}" -E touch "${shader_stamp}"
    DEPENDS ${vknr_inputs} VERBATIM)
add_custom_target(TRPVulkanNRShaders ALL DEPENDS "${shader_stamp}")
foreach(shader portable_gemm fragment_probe rounding_probe engine_fused upstream_quant)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/probe-shaders/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/probe-shaders"
        COMMAND "${TRP_VKNR_TOOLCHAIN}/tools/glslang/glslang.exe" -V --target-env vulkan1.3
            "-I${CMAKE_CURRENT_SOURCE_DIR}/shaders" "-I${CMAKE_CURRENT_BINARY_DIR}/engine-src/shaders/portable"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.comp" -o "${output}"
        COMMAND "${TRP_VKNR_TOOLCHAIN}/tools/spirv-tools/spirv-val.exe" --target-env vulkan1.3 "${output}"
        DEPENDS ${vknr_inputs} VERBATIM)
    list(APPEND probe_outputs "${output}")
endforeach()
add_custom_target(TRPVulkanNRProbes ALL DEPENDS ${probe_outputs})
add_dependencies(TRPVulkanNR TRPVulkanNRShaders TRPVulkanNRProbes)
add_custom_command(TARGET TRPVulkanNR POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/engine-package" "-DOUTPUT=$<TARGET_FILE_DIR:TRPVulkanNR>" -P "${CMAKE_CURRENT_SOURCE_DIR}/StagePayload.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_BINARY_DIR}/probe-shaders" "$<TARGET_FILE_DIR:TRPVulkanNR>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.txt" "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mauri/THIRD_PARTY.md" "$<TARGET_FILE_DIR:TRPVulkanNR>")
