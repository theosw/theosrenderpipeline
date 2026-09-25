include_guard(GLOBAL)

# Shared by the renderer and the standalone production-pass fixtures. Generated
# bytecode stays in each build tree and is never compiled during a game frame.
function(trp_target_neural_shaders target)
    if(NOT TARGET TRPNeuralShaderBytecode)
        get_filename_component(source_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
        add_executable(TRPCompileNeuralShaders "${source_root}/tools/CompileNeuralShaders.cpp")
        target_compile_features(TRPCompileNeuralShaders PRIVATE cxx_std_23)
        target_compile_definitions(TRPCompileNeuralShaders PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_include_directories(TRPCompileNeuralShaders PRIVATE "${source_root}/src")
        target_link_libraries(TRPCompileNeuralShaders PRIVATE d3dcompiler)

        set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/neural-shaders/$<CONFIG>")
        set(generated_header "${generated_dir}/TRPNeuralShaders.generated.h")
        add_custom_command(OUTPUT "${generated_header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${generated_dir}"
            COMMAND TRPCompileNeuralShaders "${generated_header}"
            DEPENDS TRPCompileNeuralShaders
                "${source_root}/src/FrameGen/SourceDLSSGNeuralResolveShader.h"
                "${source_root}/src/FrameGen/SourceDLSSGNeuralComposeShader.h"
            COMMENT "Compiling embedded Neural Rendering shaders"
            VERBATIM)
        add_custom_target(TRPNeuralShaderBytecode DEPENDS "${generated_header}")
        set_property(TARGET TRPNeuralShaderBytecode PROPERTY TRP_GENERATED_HEADER "${generated_header}")
        set_property(TARGET TRPNeuralShaderBytecode PROPERTY TRP_GENERATED_DIRECTORY "${generated_dir}")
    endif()
    get_target_property(generated_header TRPNeuralShaderBytecode TRP_GENERATED_HEADER)
    get_target_property(generated_dir TRPNeuralShaderBytecode TRP_GENERATED_DIRECTORY)
    add_dependencies(${target} TRPNeuralShaderBytecode)
    target_sources(${target} PRIVATE "${generated_header}")
    target_include_directories(${target} PRIVATE "${generated_dir}")
endfunction()
