add_executable(TRPAmpereRuntimeTests RuntimeBridgeTests.cpp
    ../../extern/RTX40MFG/midpoint_fix.cpp ../../extern/RTX40MFG/dlssg_provider_policy.cpp)
target_compile_features(TRPAmpereRuntimeTests PRIVATE cxx_std_23)
target_compile_definitions(TRPAmpereRuntimeTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPAmpereRuntimeTests PRIVATE "${TRP_NGX_SDK_DIR}/include")
target_link_libraries(TRPAmpereRuntimeTests PRIVATE d3d12 dxgi bcrypt version)
foreach(case IN ITEMS policies requirements capabilities capabilities-reject-availability
    capabilities-reject-capacity create-ready create-unprepared create-bridge-missing
    create-failed-owner create-descriptor-changed create-arch-changed create-import-changed
    resolvers startup-reentry repeated-create)
    add_test(NAME AmpereRuntime-${case} COMMAND TRPAmpereRuntimeTests ${case})
    set_tests_properties(AmpereRuntime-${case} PROPERTIES TIMEOUT 15)
endforeach()
