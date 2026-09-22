cmake_minimum_required(VERSION 3.21)
project(TRPNeuralPeripheralTests LANGUAGES CXX)
enable_testing()
add_executable(TRPNeuralPeripheralTests PeripheralTests.cpp FeatureDouble.cpp
    ../../src/FrameGen/SourceDLSSGNeuralRendering.cpp
    ../../src/FrameGen/SourceDLSSGNeuralResolve.cpp
    ../../src/FrameGen/SourceDLSSGInterop.cpp)
target_compile_features(TRPNeuralPeripheralTests PRIVATE cxx_std_23)
target_compile_definitions(TRPNeuralPeripheralTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPNeuralPeripheralTests PRIVATE . ../../src)
target_link_libraries(TRPNeuralPeripheralTests PRIVATE d3d12 d3d11 d3dcompiler dxgi)
add_test(NAME NeuralPeripheralPixels COMMAND TRPNeuralPeripheralTests)

# Manual, opt-in vendor measurement. Never registered with CTest and never
# launched by a build. No runtimes or game launch are part of this fixture.
option(TRP_NR_VENDOR_BENCHMARK "Build the manual hardware NR benchmark" OFF)
if(TRP_NR_VENDOR_BENCHMARK)
    set(NGX_SDK "" CACHE PATH "Locally supplied NGX SDK")
    add_executable(TRPNeuralPeripheralBenchmark VendorBenchmark.cpp
        ../../src/FrameGen/SourceDLSSGNeuralRendering.cpp
        ../../src/FrameGen/SourceDLSSGNeuralResolve.cpp
        ../../src/FrameGen/SourceDLSSGInterop.cpp
        ../../src/FrameGen/NeuralRenderingFeatureSession.cpp
        ../../src/FrameGen/NeuralRenderingModulePathHook.cpp
        ../../src/FrameGen/NeuralRenderingRuntimeIdentity.cpp)
    target_compile_features(TRPNeuralPeripheralBenchmark PRIVATE cxx_std_23)
    target_compile_definitions(TRPNeuralPeripheralBenchmark PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TRP_NR_VENDOR_BENCHMARK)
    target_include_directories(TRPNeuralPeripheralBenchmark PRIVATE . ../../src "${NGX_SDK}/include")
    target_link_libraries(TRPNeuralPeripheralBenchmark PRIVATE d3d12 d3d11 d3dcompiler dxgi bcrypt version
        "${NGX_SDK}/lib/Windows_x86_64/nvsdk_ngx_d.lib")
endif()
# Header-only configuration contract; the main build already supplies SimpleIni.
if(TRP_SIMPLEINI_INCLUDE_DIR)
    add_executable(TRPNeuralPassSettingsTests ../NeuralPassSettingsTests.cpp)
    target_compile_features(TRPNeuralPassSettingsTests PRIVATE cxx_std_20)
    target_include_directories(TRPNeuralPassSettingsTests PRIVATE ../../src "${TRP_SIMPLEINI_INCLUDE_DIR}")
    add_test(NAME NeuralPassSettings COMMAND TRPNeuralPassSettingsTests)
endif()
