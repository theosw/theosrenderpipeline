cmake_minimum_required(VERSION 3.21)
project(TRPNeuralPeripheralTests LANGUAGES CXX)
enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NeuralShaders.cmake")
add_executable(TRPNeuralPeripheralTests PeripheralTests.cpp FeatureDouble.cpp
    ../../src/FrameGen/SourceDLSSGNeuralRendering.cpp
    ../../src/FrameGen/SourceDLSSGNeuralResolve.cpp
    ../../src/FrameGen/NeuralObjectBlend.cpp
    ../../src/FrameGen/NeuralObjectControl.cpp
    ../../src/FrameGen/SourceDLSSGInterop.cpp ../../src/FrameGen/D3D12StageTimer.cpp)
target_compile_features(TRPNeuralPeripheralTests PRIVATE cxx_std_23)
target_compile_definitions(TRPNeuralPeripheralTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPNeuralPeripheralTests PRIVATE . ../../src)
target_link_libraries(TRPNeuralPeripheralTests PRIVATE d3d12 d3d11 d3dcompiler dxgi)
trp_target_neural_shaders(TRPNeuralPeripheralTests)
add_test(NAME NeuralPeripheralPixels COMMAND TRPNeuralPeripheralTests)

add_executable(TRPNeuralThirdFrameTests ThirdFrameTests.cpp FeatureDouble.cpp
    ../../src/FrameGen/SourceDLSSGNeuralRendering.cpp ../../src/FrameGen/SourceDLSSGNeuralResolve.cpp
    ../../src/FrameGen/NeuralObjectBlend.cpp ../../src/FrameGen/NeuralObjectControl.cpp
    ../../src/FrameGen/SourceDLSSGInterop.cpp ../../src/FrameGen/D3D12StageTimer.cpp)
target_compile_features(TRPNeuralThirdFrameTests PRIVATE cxx_std_23)
target_compile_definitions(TRPNeuralThirdFrameTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPNeuralThirdFrameTests PRIVATE . ../../src)
target_link_libraries(TRPNeuralThirdFrameTests PRIVATE d3d12 d3d11 d3dcompiler dxgi)
trp_target_neural_shaders(TRPNeuralThirdFrameTests)
add_test(NAME NeuralThirdFrame COMMAND TRPNeuralThirdFrameTests)

add_executable(TRPNeuralCombatPolicyTests ../NeuralCombatPolicyTests.cpp)
target_compile_features(TRPNeuralCombatPolicyTests PRIVATE cxx_std_20)
target_include_directories(TRPNeuralCombatPolicyTests PRIVATE ../../src)
add_test(NAME NeuralCombatPolicy COMMAND TRPNeuralCombatPolicyTests)

add_executable(TRPNeuralShaderBytecodeTests ../NeuralShaderBytecodeTests.cpp)
target_compile_features(TRPNeuralShaderBytecodeTests PRIVATE cxx_std_23)
target_compile_definitions(TRPNeuralShaderBytecodeTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPNeuralShaderBytecodeTests PRIVATE ../../src)
target_link_libraries(TRPNeuralShaderBytecodeTests PRIVATE d3dcompiler)
trp_target_neural_shaders(TRPNeuralShaderBytecodeTests)
add_test(NAME NeuralShaderBytecode COMMAND TRPNeuralShaderBytecodeTests)


add_executable(TRPNeuralLaunchRequirementTests LaunchRequirementTests.cpp)
target_compile_features(TRPNeuralLaunchRequirementTests PRIVATE cxx_std_23)
target_compile_definitions(TRPNeuralLaunchRequirementTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPNeuralLaunchRequirementTests PRIVATE . ../../src)
add_test(NAME NeuralLaunchRequirements COMMAND TRPNeuralLaunchRequirementTests)

# Manual, opt-in vendor measurement. Never registered with CTest and never
# launched by a build. No runtimes or game launch are part of this fixture.
option(TRP_NR_VENDOR_BENCHMARK "Build the manual hardware NR benchmark" OFF)
if(TRP_NR_VENDOR_BENCHMARK)
    add_executable(TRPNeuralLaunchOverrideTests KernelLaunchOverrideTests.cpp)
    target_compile_features(TRPNeuralLaunchOverrideTests PRIVATE cxx_std_23)
    target_compile_definitions(TRPNeuralLaunchOverrideTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME NeuralLaunchOverride COMMAND TRPNeuralLaunchOverrideTests)
    set(NGX_SDK "" CACHE PATH "Locally supplied NGX SDK")
    option(TRP_NR_ADA_KERNELS "Include the Universal RTX 20/30/40 kernel installer in manual probes" OFF)
    set(nr_ada_sources ../../src/FrameGen/NeuralRuntimeKernelsNative.cpp)
    if(TRP_NR_ADA_KERNELS)
        set(nr_ada_sources ../../src/FrameGen/NeuralRuntimeKernels.cpp ../../src/FrameGen/NeuralRuntimeKernelPlan.cpp)
    endif()
    add_executable(TRPNeuralPeripheralBenchmark VendorBenchmark.cpp KernelProfiler.cpp
        ../../src/FrameGen/SourceDLSSGNeuralRendering.cpp
        ../../src/FrameGen/SourceDLSSGNeuralResolve.cpp
    ../../src/FrameGen/NeuralObjectBlend.cpp
    ../../src/FrameGen/NeuralObjectControl.cpp
        ../../src/FrameGen/SourceDLSSGInterop.cpp ../../src/FrameGen/D3D12StageTimer.cpp
        ../../src/FrameGen/NeuralLaunchHook.cpp
        ../../src/FrameGen/NeuralRenderingFeatureSession.cpp
        ../../src/FrameGen/NeuralRenderingModulePathHook.cpp
        ../../src/FrameGen/NeuralRenderingRuntimeIdentity.cpp ${nr_ada_sources})
    target_compile_features(TRPNeuralPeripheralBenchmark PRIVATE cxx_std_23)
    trp_target_neural_shaders(TRPNeuralPeripheralBenchmark)
    target_compile_definitions(TRPNeuralPeripheralBenchmark PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TRP_NR_VENDOR_BENCHMARK)
    target_include_directories(TRPNeuralPeripheralBenchmark PRIVATE . ../../src "${NGX_SDK}/include")
    target_link_libraries(TRPNeuralPeripheralBenchmark PRIVATE d3d12 d3d11 d3dcompiler dxgi bcrypt version
        "${NGX_SDK}/lib/Windows_x86_64/nvsdk_ngx_d.lib")
    # Manual colour probe: NR behaviour for SDR, linear-HDR and PQ encodings.
    add_executable(TRPNeuralHDRRangeProbe HDRRangeProbe.cpp
        ../../src/FrameGen/SourceDLSSGNeuralRendering.cpp
        ../../src/FrameGen/SourceDLSSGNeuralResolve.cpp
    ../../src/FrameGen/NeuralObjectBlend.cpp
    ../../src/FrameGen/NeuralObjectControl.cpp
        ../../src/FrameGen/SourceDLSSGInterop.cpp ../../src/FrameGen/D3D12StageTimer.cpp
        ../../src/FrameGen/NeuralLaunchHook.cpp
        ../../src/FrameGen/NeuralRenderingFeatureSession.cpp
        ../../src/FrameGen/NeuralRenderingModulePathHook.cpp
        ../../src/FrameGen/NeuralRenderingRuntimeIdentity.cpp ${nr_ada_sources})
    target_compile_features(TRPNeuralHDRRangeProbe PRIVATE cxx_std_23)
    trp_target_neural_shaders(TRPNeuralHDRRangeProbe)
    target_compile_definitions(TRPNeuralHDRRangeProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TRP_NR_VENDOR_BENCHMARK)
    target_include_directories(TRPNeuralHDRRangeProbe PRIVATE . ../../src "${NGX_SDK}/include")
    target_link_libraries(TRPNeuralHDRRangeProbe PRIVATE d3d12 d3d11 d3dcompiler dxgi bcrypt version
        "${NGX_SDK}/lib/Windows_x86_64/nvsdk_ngx_d.lib")
    add_executable(TRPNeuralObjectVendorProbe ObjectControlVendorProbe.cpp
        ../../src/FrameGen/SourceDLSSGNeuralRendering.cpp ../../src/FrameGen/SourceDLSSGNeuralResolve.cpp
        ../../src/FrameGen/NeuralObjectBlend.cpp ../../src/FrameGen/NeuralObjectControl.cpp
        ../../src/FrameGen/SourceDLSSGInterop.cpp ../../src/FrameGen/D3D12StageTimer.cpp
        ../../src/FrameGen/NeuralLaunchHook.cpp
        ../../src/FrameGen/NeuralRenderingFeatureSession.cpp ../../src/FrameGen/NeuralRenderingModulePathHook.cpp
        ../../src/FrameGen/NeuralRenderingRuntimeIdentity.cpp ${nr_ada_sources})
    target_compile_features(TRPNeuralObjectVendorProbe PRIVATE cxx_std_23)
    trp_target_neural_shaders(TRPNeuralObjectVendorProbe)
    target_compile_definitions(TRPNeuralObjectVendorProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TRP_NR_VENDOR_BENCHMARK)
    target_include_directories(TRPNeuralObjectVendorProbe PRIVATE . ../../src "${NGX_SDK}/include")
    target_link_libraries(TRPNeuralObjectVendorProbe PRIVATE d3d12 d3d11 d3dcompiler dxgi bcrypt version
        "${NGX_SDK}/lib/Windows_x86_64/nvsdk_ngx_d.lib")
endif()
# Header-only configuration contract; the main build already supplies SimpleIni.
if(TRP_SIMPLEINI_INCLUDE_DIR)
    add_executable(TRPNeuralPassSettingsTests ../NeuralPassSettingsTests.cpp)
    target_compile_features(TRPNeuralPassSettingsTests PRIVATE cxx_std_20)
    target_include_directories(TRPNeuralPassSettingsTests PRIVATE ../../src "${TRP_SIMPLEINI_INCLUDE_DIR}")
    add_test(NAME NeuralPassSettings COMMAND TRPNeuralPassSettingsTests)
endif()

add_executable(TRPNeuralSelectiveModelTests SelectiveModelTests.cpp FeatureDouble.cpp
    ../../src/FrameGen/SourceDLSSGNeuralRendering.cpp ../../src/FrameGen/SourceDLSSGNeuralResolve.cpp
    ../../src/FrameGen/NeuralObjectBlend.cpp ../../src/FrameGen/NeuralObjectControl.cpp
    ../../src/FrameGen/SourceDLSSGInterop.cpp ../../src/FrameGen/D3D12StageTimer.cpp)
target_compile_features(TRPNeuralSelectiveModelTests PRIVATE cxx_std_23)
target_compile_definitions(TRPNeuralSelectiveModelTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPNeuralSelectiveModelTests PRIVATE . ../../src)
target_link_libraries(TRPNeuralSelectiveModelTests PRIVATE d3d12 d3d11 d3dcompiler dxgi)
trp_target_neural_shaders(TRPNeuralSelectiveModelTests)
add_test(NAME NeuralSelectiveModels COMMAND TRPNeuralSelectiveModelTests)

add_executable(TRPStartupTransportTests StartupTransportTests.cpp ../../src/FrameGen/SourceDLSSGInterop.cpp ../../src/FrameGen/D3D12StageTimer.cpp)
target_compile_features(TRPStartupTransportTests PRIVATE cxx_std_23)
target_compile_definitions(TRPStartupTransportTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPStartupTransportTests PRIVATE . ../../src)
target_link_libraries(TRPStartupTransportTests PRIVATE d3d12 d3d11 d3dcompiler dxgi)
add_test(NAME StartupTransportPixels COMMAND TRPStartupTransportTests)

add_executable(TRPNeuralRecoveryTests RecoveryTests.cpp FeatureDouble.cpp
    ../../src/FrameGen/SourceDLSSGNeuralRendering.cpp ../../src/FrameGen/SourceDLSSGNeuralResolve.cpp
    ../../src/FrameGen/NeuralObjectBlend.cpp ../../src/FrameGen/NeuralObjectControl.cpp
    ../../src/FrameGen/SourceDLSSGInterop.cpp ../../src/FrameGen/D3D12StageTimer.cpp)
target_compile_features(TRPNeuralRecoveryTests PRIVATE cxx_std_23)
target_compile_definitions(TRPNeuralRecoveryTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TRP_NR_RECOVERY_TEST)
target_include_directories(TRPNeuralRecoveryTests PRIVATE . ../../src)
target_link_libraries(TRPNeuralRecoveryTests PRIVATE d3d12 d3d11 d3dcompiler dxgi user32)
trp_target_neural_shaders(TRPNeuralRecoveryTests)
add_test(NAME NeuralRecovery COMMAND TRPNeuralRecoveryTests)
