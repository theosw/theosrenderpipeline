// Render hooks derived from PureDark's MIT-licensed Skyrim-Upscaler
// (https://github.com/PureDark/Skyrim-Upscaler).

#include <PCH.h>
#include "UpscalerSamplerHooks.h"
#include "RenderPipeline.h"
#include <unordered_map>
#include <unordered_set>

namespace
{
static float mipLodBias = 0;
static std::unordered_set<ID3D11SamplerState*> passThroughSamplers;
static std::unordered_set<ID3D11SamplerState*> retainedPassThroughSamplers;
static std::unordered_map<ID3D11SamplerState*, ID3D11SamplerState*> mappedSamplers;

decltype(&ID3D11DeviceContext::PSSetSamplers) ptrPSSetSamplers;
decltype(&ID3D11DeviceContext::VSSetSamplers) ptrVSSetSamplers;
decltype(&ID3D11DeviceContext::GSSetSamplers) ptrGSSetSamplers;
decltype(&ID3D11DeviceContext::HSSetSamplers) ptrHSSetSamplers;
decltype(&ID3D11DeviceContext::DSSetSamplers) ptrDSSetSamplers;
decltype(&ID3D11DeviceContext::CSSetSamplers) ptrCSSetSamplers;

// Mostly from vrperfkit, thanks to fholger for showing how to do mip lod bias
// https://github.com/fholger/vrperfkit/blob/037c09f3168ac045b5775e8d1a0c8ac982b5854f/src/d3d11/d3d11_post_processor.cpp#L76
static bool SetMipLodBias(ID3D11SamplerState** outSamplers, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers)
{
    if (!outSamplers || (NumSamplers > 0 && !ppSamplers) || NumSamplers > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
    {
        return false;
    }
    if (mipLodBias != RenderPipeline::GetSingleton()->mMipLodBias)
    {
        logger::info("MIP LOD Bias changed from {} to {}, recreating samplers", mipLodBias,
                     RenderPipeline::GetSingleton()->mMipLodBias);
        for (auto& [original, sampler] : mappedSamplers)
        {
            if (original)
            {
                original->Release();
            }
            if (sampler)
            {
                sampler->Release();
            }
        }
        for (auto* sampler : retainedPassThroughSamplers)
        {
            if (sampler)
            {
                sampler->Release();
            }
        }
        passThroughSamplers.clear();
        retainedPassThroughSamplers.clear();
        mappedSamplers.clear();
        mipLodBias = RenderPipeline::GetSingleton()->mMipLodBias;
    }
    if (NumSamplers == 0)
    {
        return true;
    }
    memcpy(outSamplers, ppSamplers, NumSamplers * sizeof(ID3D11SamplerState*));
    auto retainPassThrough = [](ID3D11SamplerState* a_sampler)
    {
        passThroughSamplers.insert(a_sampler);
        if (retainedPassThroughSamplers.insert(a_sampler).second)
        {
            a_sampler->AddRef();
        }
    };
    for (UINT i = 0; i < NumSamplers; ++i)
    {
        auto orig = outSamplers[i];
        if (orig == nullptr || passThroughSamplers.find(orig) != passThroughSamplers.end())
        {
            continue;
        }
        auto mapped = mappedSamplers.find(orig);
        if (mapped == mappedSamplers.end())
        {
            D3D11_SAMPLER_DESC sd;
            orig->GetDesc(&sd);
            if (sd.MipLODBias != 0 || sd.MaxAnisotropy <= 1)
            {
                // do not mess with samplers that already have a bias or are not doing anisotropic filtering.
                retainPassThrough(orig);
                continue;
            }
            sd.MipLODBias = mipLodBias;
            ID3D11SamplerState* replacement = nullptr;
            const auto device = RenderPipeline::GetSingleton()->mDevice;
            const auto hr = device ? device->CreateSamplerState(&sd, &replacement) : E_POINTER;
            if (FAILED(hr) || !replacement)
            {
                logger::warn("MIP LOD sampler replacement failed hr=0x{:08X}; preserving the original sampler",
                             static_cast<std::uint32_t>(hr));
                retainPassThrough(orig);
                continue;
            }
            orig->AddRef(); // map keys must remain valid and cannot be pointer-reused
            mapped = mappedSamplers.emplace(orig, replacement).first;
            passThroughSamplers.insert(replacement);
        }
        outSamplers[i] = mapped->second;
    }
    return true;
}

void WINAPI hk_ID3D11DeviceContext_PSSetSamplers(ID3D11DeviceContext* This, UINT StartSlot, UINT NumSamplers,
                                                 ID3D11SamplerState* const* ppSamplers)
{
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    (This->*ptrPSSetSamplers)(StartSlot, NumSamplers,
                              SetMipLodBias(samplers, NumSamplers, ppSamplers) ? samplers : ppSamplers);
}

void WINAPI hk_ID3D11DeviceContext_VSSetSamplers(ID3D11DeviceContext* This, UINT StartSlot, UINT NumSamplers,
                                                 ID3D11SamplerState* const* ppSamplers)
{
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    (This->*ptrVSSetSamplers)(StartSlot, NumSamplers,
                              SetMipLodBias(samplers, NumSamplers, ppSamplers) ? samplers : ppSamplers);
}

void WINAPI hk_ID3D11DeviceContext_GSSetSamplers(ID3D11DeviceContext* This, UINT StartSlot, UINT NumSamplers,
                                                 ID3D11SamplerState* const* ppSamplers)
{
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    (This->*ptrGSSetSamplers)(StartSlot, NumSamplers,
                              SetMipLodBias(samplers, NumSamplers, ppSamplers) ? samplers : ppSamplers);
}

void WINAPI hk_ID3D11DeviceContext_HSSetSamplers(ID3D11DeviceContext* This, UINT StartSlot, UINT NumSamplers,
                                                 ID3D11SamplerState* const* ppSamplers)
{
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    (This->*ptrHSSetSamplers)(StartSlot, NumSamplers,
                              SetMipLodBias(samplers, NumSamplers, ppSamplers) ? samplers : ppSamplers);
}

void WINAPI hk_ID3D11DeviceContext_DSSetSamplers(ID3D11DeviceContext* This, UINT StartSlot, UINT NumSamplers,
                                                 ID3D11SamplerState* const* ppSamplers)
{
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    (This->*ptrDSSetSamplers)(StartSlot, NumSamplers,
                              SetMipLodBias(samplers, NumSamplers, ppSamplers) ? samplers : ppSamplers);
}

void WINAPI hk_ID3D11DeviceContext_CSSetSamplers(ID3D11DeviceContext* This, UINT StartSlot, UINT NumSamplers,
                                                 ID3D11SamplerState* const* ppSamplers)
{
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    (This->*ptrCSSetSamplers)(StartSlot, NumSamplers,
                              SetMipLodBias(samplers, NumSamplers, ppSamplers) ? samplers : ppSamplers);
}

} // namespace

namespace TheosRenderPipeline
{
void InstallPixelSamplerHook(ID3D11DeviceContext* deviceContext)
{
    *(uintptr_t*)&ptrPSSetSamplers =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_PSSetSamplers, 10);
}

void InstallAdditionalSamplerHooks(ID3D11DeviceContext* deviceContext)
{
    *(uintptr_t*)&ptrVSSetSamplers =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_VSSetSamplers, 26);
    *(uintptr_t*)&ptrGSSetSamplers =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_GSSetSamplers, 32);
    *(uintptr_t*)&ptrHSSetSamplers =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_HSSetSamplers, 61);
    *(uintptr_t*)&ptrDSSetSamplers =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_DSSetSamplers, 65);
    *(uintptr_t*)&ptrCSSetSamplers =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_CSSetSamplers, 70);
}
} // namespace TheosRenderPipeline
