add_executable(TRPTuringPlannerTests PlannerTests.cpp)
target_compile_features(TRPTuringPlannerTests PRIVATE cxx_std_23)
add_test(NAME TuringPlanner COMMAND TRPTuringPlannerTests)
set_tests_properties(TuringPlanner PROPERTIES TIMEOUT 30)
# Manual provider-as-data and publication audit. No driver or GPU dispatch.
add_executable(TRPTuringProviderAudit ProviderAudit.cpp
    ../../extern/RTX40MFG/midpoint_fix.cpp ../../extern/RTX40MFG/dlssg_provider_policy.cpp)
target_compile_features(TRPTuringProviderAudit PRIVATE cxx_std_23)
target_compile_definitions(TRPTuringProviderAudit PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPTuringProviderAudit PRIVATE "${TRP_NGX_SDK_DIR}/include")
target_link_libraries(TRPTuringProviderAudit PRIVATE d3d12 dxgi bcrypt version)
# Explicit manual invocation only; this initializes a graphics device.
add_executable(TRPCuModuleProbe CuModuleProbe.cpp)
target_compile_features(TRPCuModuleProbe PRIVATE cxx_std_23)
target_compile_definitions(TRPCuModuleProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(TRPCuModuleProbe PRIVATE d3d12 dxgi)
add_executable(TRPProviderGuardProbe ProviderGuardProbe.cpp
    ../../extern/RTX40MFG/midpoint_fix.cpp ../../extern/RTX40MFG/dlssg_provider_policy.cpp)
target_compile_features(TRPProviderGuardProbe PRIVATE cxx_std_23)
target_compile_definitions(TRPProviderGuardProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPProviderGuardProbe PRIVATE "${TRP_NGX_SDK_DIR}/include")
target_link_libraries(TRPProviderGuardProbe PRIVATE d3d12 dxgi bcrypt version)
