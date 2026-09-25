#include "FrameGen/SourceDLSSGNeuralComposeShader.h"
#include "FrameGen/SourceDLSSGNeuralResolveShader.h"
#include "TRPNeuralShaders.generated.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace TheosRenderPipeline::SourceDLSSG;

static void Compare(const CompiledNeuralShaders::Bytecode& embedded, const char* source,
    std::size_t length, const char* sourceName, const char* entry, const char* profile)
{
    Microsoft::WRL::ComPtr<ID3DBlob> reference, errors;
    const auto hr = D3DCompile(source, length, sourceName, nullptr, nullptr, entry, profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &reference, &errors);
    if (FAILED(hr)) {
        if (errors) { std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr); }
        std::fprintf(stderr, "FAIL: reference compilation for %s\n", entry);
        std::exit(1);
    }
    if (embedded.size != reference->GetBufferSize() ||
        std::memcmp(embedded.data, reference->GetBufferPointer(), embedded.size) != 0) {
        std::fprintf(stderr, "FAIL: embedded bytecode differs from original runtime compilation for %s\n", entry);
        std::exit(1);
    }
}

int main()
{
    // Independent manifest of the pre-change runtime entry order and profiles.
    const char* entries[]{ "Downsample", "Residual", "Ratio", "PackDepth", "PackMotion",
        "PrepareColor", "PackGuides", "ResizeColor", "RestoreSecond" };
    static_assert(std::size(entries) == CompiledNeuralShaders::Resolve.size());
    for (std::size_t i = 0; i < std::size(entries); ++i) {
        Compare(CompiledNeuralShaders::Resolve[i], kNeuralResolveShader, sizeof(kNeuralResolveShader) - 1,
            "TheosRenderPipeline-NR-resolve", entries[i], "cs_5_1");
    }
    Compare(CompiledNeuralShaders::Compose, kNeuralComposeShader, sizeof(kNeuralComposeShader) - 1,
        "SourceDLSSG-NR-compose", "main", "cs_5_0");
    std::puts("All ten embedded NR shaders exactly match the original runtime compilation.");
}
