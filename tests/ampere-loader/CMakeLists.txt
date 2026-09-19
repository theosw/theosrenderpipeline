add_executable(TRPAmpereModuleLoaderTests ModuleLoaderTests.cpp)
target_compile_features(TRPAmpereModuleLoaderTests PRIVATE cxx_std_23)
target_compile_definitions(TRPAmpereModuleLoaderTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(TRPAmpereModuleLoaderTests PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(TRPAmpereModuleLoaderTests PRIVATE ARPDetours)
add_dependencies(TRPAmpereModuleLoaderTests TRPModuleIdentityFixture1 TRPModuleIdentityFixture2)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/not-a-pe.dll" "Not a Windows executable image.")
set(alias "${CMAKE_CURRENT_BINARY_DIR}/module-alias")
add_test(NAME AmpereLoader-AliasSetup COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/CreateModuleAlias.ps1"
    -SourceDirectory $<TARGET_FILE_DIR:TRPModuleIdentityFixture1> -AliasPath "${alias}")
set_tests_properties(AmpereLoader-AliasSetup PROPERTIES FIXTURES_SETUP AmpereLoaderAlias)
foreach(mode IN ITEMS direct alias foreign missing invalid-image query-failed query-truncated already-owned relative)
    add_test(NAME AmpereLoader-${mode} COMMAND TRPAmpereModuleLoaderTests
        $<TARGET_FILE:TRPModuleIdentityFixture1> $<TARGET_FILE:TRPModuleIdentityFixture2>
        "${alias}" "${CMAKE_CURRENT_BINARY_DIR}/not-a-pe.dll" ${mode})
endforeach()
set_tests_properties(AmpereLoader-alias PROPERTIES FIXTURES_REQUIRED AmpereLoaderAlias)
