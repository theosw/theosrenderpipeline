#include "FrameGen/SourceDLSSGNeuralComposeShader.h"
#include "FrameGen/SourceDLSSGNeuralResolveShader.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace
{
    void Compile(std::ostream& output, const char* name, const char* source, std::size_t size,
        const char* sourceName, const char* entry, const char* profile)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> shader, errors;
        const auto hr = D3DCompile(source, size, sourceName, nullptr, nullptr, entry, profile,
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
        if (FAILED(hr)) {
            if (errors) {
                std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            }
            throw std::runtime_error(std::string("NR shader compilation failed: ") + name);
        }
        const auto* bytes = static_cast<const unsigned char*>(shader->GetBufferPointer());
        output << "inline constexpr unsigned char " << name << "[] = {\n";
        for (std::size_t i = 0; i < shader->GetBufferSize(); ++i) {
            output << static_cast<unsigned>(bytes[i]) << ',';
            if (i % 24 == 23) { output << '\n'; }
        }
        output << "\n};\n";
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) { std::cerr << "Usage: TRPCompileNeuralShaders <output-header>\n"; return 2; }
    try {
        using namespace TheosRenderPipeline::SourceDLSSG;
        std::ostringstream generated;
        generated << "// Generated at build time. Edit the NR HLSL sources, not this file.\n"
            "#pragma once\n#include <array>\n#include <cstddef>\n"
            "namespace TheosRenderPipeline::SourceDLSSG::CompiledNeuralShaders {\n"
            "struct Bytecode { const unsigned char* data; std::size_t size; };\n";
        const char* entries[]{ "Downsample", "Residual", "Ratio", "PackDepth", "PackMotion",
            "PrepareColor", "PackGuides", "ResizeColor", "RestoreSecond" };
        for (const auto* entry : entries) {
            Compile(generated, entry, kNeuralResolveShader, sizeof(kNeuralResolveShader) - 1,
                "TheosRenderPipeline-NR-resolve", entry, "cs_5_1");
        }
        Compile(generated, "ComposeBytes", kNeuralComposeShader, sizeof(kNeuralComposeShader) - 1,
            "SourceDLSSG-NR-compose", "main", "cs_5_0");
        generated << "inline constexpr std::array<Bytecode, 9> Resolve{{\n";
        for (const auto* entry : entries) { generated << '{' << entry << ", sizeof(" << entry << ")},\n"; }
        generated << "}};\ninline constexpr Bytecode Compose{ComposeBytes, sizeof(ComposeBytes)};\n}\n";

        // Publish only a complete set. A failed compilation leaves the previous
        // output intact and fails the build rather than generating a partial DLL.
        const std::filesystem::path destination(argv[1]);
        auto temporary = destination; temporary += L".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << generated.str();
        output.close();
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Could not publish generated NR shader header");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
