cmake_minimum_required(VERSION 3.21)
project(TRPNeuralAsyncPrototype LANGUAGES CXX)
enable_testing()

add_library(TRPNeuralAsync STATIC AsyncPipeline.cpp)
target_compile_features(TRPNeuralAsync PUBLIC cxx_std_23)
target_compile_definitions(TRPNeuralAsync PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPNeuralAsync PUBLIC . ../nr-peripheral ../../src)
target_link_libraries(TRPNeuralAsync PUBLIC d3d12 d3d11 d3dcompiler dxgi)

add_executable(TRPNeuralAsyncTests AsyncTests.cpp)
target_link_libraries(TRPNeuralAsyncTests PRIVATE TRPNeuralAsync)
add_test(NAME NeuralAsyncPixelsAndLifetime COMMAND TRPNeuralAsyncTests)
set_tests_properties(NeuralAsyncPixelsAndLifetime PROPERTIES TIMEOUT 60)

# Built and run explicitly; never run inference or launch anything from CTest.
option(TRP_NR_ASYNC_VENDOR "Build manual native NVIDIA NR experiment" OFF)
if(TRP_NR_ASYNC_VENDOR)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NeuralShaders.cmake")
    set(NGX_SDK "" CACHE PATH "Locally supplied NGX SDK")
    add_executable(TRPNeuralAsyncBenchmark VendorBenchmark.cpp
        ../../src/FrameGen/SourceDLSSGNeuralRendering.cpp
        ../../src/FrameGen/SourceDLSSGNeuralResolve.cpp
        ../../src/FrameGen/SourceDLSSGInterop.cpp
        ../../src/FrameGen/NeuralRenderingFeatureSession.cpp
        ../../src/FrameGen/NeuralRenderingModulePathHook.cpp
        ../../src/FrameGen/NeuralRenderingRuntimeIdentity.cpp)
    target_link_libraries(TRPNeuralAsyncBenchmark PRIVATE TRPNeuralAsync bcrypt version
        "${NGX_SDK}/lib/Windows_x86_64/nvsdk_ngx_d.lib")
    target_compile_definitions(TRPNeuralAsyncBenchmark PRIVATE TRP_NR_VENDOR_BENCHMARK)
    target_include_directories(TRPNeuralAsyncBenchmark PRIVATE "${NGX_SDK}/include")
    trp_target_neural_shaders(TRPNeuralAsyncBenchmark)
endif()
