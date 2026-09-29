# CommonLib's HDE64 dependency is the pinned MinHook v1.3.4 checkout. Populate
# it here as well when CommonLib's optional patch-safety checks are disabled.
include(FetchContent)
FetchContent_Declare(hde64
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR src/hde)
FetchContent_MakeAvailable(hde64)
FetchContent_GetProperties(hde64)
add_library(TRPD3D11EntryObservers STATIC
    "${PROJECT_SOURCE_DIR}/src/FrameGen/D3D11EntryObservers.cpp"
    "${hde64_SOURCE_DIR}/src/buffer.c"
    "${hde64_SOURCE_DIR}/src/hook.c"
    "${hde64_SOURCE_DIR}/src/trampoline.c"
    "${hde64_SOURCE_DIR}/src/hde/hde64.c")
target_compile_features(TRPD3D11EntryObservers PRIVATE cxx_std_23)
target_compile_definitions(TRPD3D11EntryObservers PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPD3D11EntryObservers PRIVATE "${hde64_SOURCE_DIR}/include")
target_link_libraries(TheosRenderPipeline PRIVATE TRPD3D11EntryObservers)
