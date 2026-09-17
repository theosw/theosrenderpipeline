#include "FrameGen/NativeUIComposition.h"
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using Microsoft::WRL::ComPtr;
using Pixel = std::array<float, 4>;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "WARP debug device");
    ComPtr<ID3D11InfoQueue> info; Check(device.As(&info), "debug queue");
    auto retire = [&] {
        ComPtr<ID3D11Query> query;
        D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        Check(device->CreateQuery(&desc, &query), "retirement query");
        context->End(query.Get()); context->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        HRESULT result;
        while ((result = context->GetData(query.Get(), nullptr, 0, 0)) == S_FALSE) {
            Require(std::chrono::steady_clock::now() < deadline, "GPU retirement timeout"); Sleep(0);
        }
        Check(result, "GPU retired");
    };
    unsigned comparisons = 0;
    TheosRenderPipeline::NativeUIComposition ui;
    for (UINT width : {9u, 17u}) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = 3; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> scene, presentation, sentinel;
        Check(device->CreateTexture2D(&desc, nullptr, &scene), "scene");
        Check(device->CreateTexture2D(&desc, nullptr, &presentation), "presentation");
        Check(device->CreateTexture2D(&desc, nullptr, &sentinel), "sentinel");
        auto stagingDesc = desc; stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0; stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(device->CreateTexture2D(&stagingDesc, nullptr, &staging), "readback");
        auto compare = [&](ID3D11Texture2D* texture, const std::vector<Pixel>& expected) {
            context->CopyResource(staging.Get(), texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map readback");
            for (UINT y = 0; y < desc.Height; ++y) {
                auto row = reinterpret_cast<const Pixel*>(static_cast<const char*>(mapped.pData) + mapped.RowPitch * y);
                for (UINT x = 0; x < width; ++x) {
                    for (unsigned c = 0; c < 4; ++c) {
                        Require(std::abs(row[x][c] - expected[y * width + x][c]) < 0.00001f, "pixel agrees with reference");
                        ++comparisons;
                    }
                }
            }
            context->Unmap(staging.Get(), 0);
        };
        const Pixel sceneColor{0.2f, 0.4f, 0.6f, 0.8f};
        std::vector<Pixel> scenePixels(width * desc.Height, sceneColor), drawPixels(scenePixels.size()), composed(scenePixels.size());
        auto currentPresentation = scenePixels;
        for (size_t i = 0; i < drawPixels.size(); ++i) {
            const float alpha = static_cast<float>(i % 5) / 4;
            drawPixels[i] = {alpha * 0.8f, alpha * 0.3f, alpha * 1.5f, alpha};
            // A late green mark exists only in the presentation image. Check
            // transparent and partially covered pixels, not just opaque UI.
            if (i % 7 == 0) { currentPresentation[i] = {0.1f, 0.9f, 0.2f, 0.95f}; }
            for (unsigned c = 0; c < 3; ++c) { composed[i][c] = drawPixels[i][c] + currentPresentation[i][c] * (1 - alpha); }
            composed[i][3] = std::max(alpha, currentPresentation[i][3]);
        }
        context->UpdateSubresource(scene.Get(), 0, nullptr, scenePixels.data(), width * sizeof(Pixel), 0);
        context->UpdateSubresource(presentation.Get(), 0, nullptr, currentPresentation.data(), width * sizeof(Pixel), 0);
        Require(ui.Initialize(device.Get(), context.Get(), scene.Get(), desc, true) && ui.Dedicated() && ui.Available(), "dedicated initialization");
        Require(ui.RenderTexture() != ui.TaggedTexture(), "draw and tagged identities separate");
        D3D11_TEXTURE2D_DESC actual{}; ui.RenderTexture()->GetDesc(&actual);
        Require(actual.BindFlags == D3D11_BIND_RENDER_TARGET, "private draw target contract");
        ui.TaggedTexture()->GetDesc(&actual);
        Require(actual.BindFlags == desc.BindFlags, "tagged SRV/RTV/UAV contract");
        auto taggedIdentity = ui.TaggedTexture();
        context->UpdateSubresource(ui.RenderTexture(), 0, nullptr, drawPixels.data(), width * sizeof(Pixel), 0);

        ComPtr<ID3D11ShaderResourceView> sentinelSRV;
        ComPtr<ID3D11UnorderedAccessView> sentinelUAV;
        Check(device->CreateShaderResourceView(scene.Get(), nullptr, &sentinelSRV), "sentinel SRV");
        Check(device->CreateUnorderedAccessView(sentinel.Get(), nullptr, &sentinelUAV), "sentinel UAV");
        constexpr char shaderText[] = "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) {}";
        ComPtr<ID3DBlob> bytecode;
        Check(D3DCompile(shaderText, sizeof(shaderText)-1, "sentinel", nullptr, nullptr, "main", "cs_5_0", 0, 0, &bytecode, nullptr), "sentinel compile");
        ComPtr<ID3D11ComputeShader> sentinelShader;
        Check(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &sentinelShader), "sentinel shader");
        ID3D11ShaderResourceView* srvs[]{sentinelSRV.Get(), nullptr};
        ID3D11UnorderedAccessView* uavs[]{sentinelUAV.Get()};
        context->CSSetShader(sentinelShader.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 2, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        Require(ui.Compose(context.Get(), presentation.Get()), "compose");
        ComPtr<ID3D11ComputeShader> restoredShader;
        ComPtr<ID3D11ShaderResourceView> restoredSRV;
        ComPtr<ID3D11UnorderedAccessView> restoredUAV;
        context->CSGetShader(&restoredShader, nullptr, nullptr);
        context->CSGetShaderResources(0, 1, &restoredSRV);
        context->CSGetUnorderedAccessViews(0, 1, &restoredUAV);
        Require(restoredShader.Get() == sentinelShader.Get() && restoredSRV.Get() == sentinelSRV.Get() &&
            restoredUAV.Get() == sentinelUAV.Get(), "compute state restored");
        compare(presentation.Get(), composed); compare(ui.TaggedTexture(), drawPixels);
        const float transparent[4]{}; context->ClearRenderTargetView(ui.RenderRTV(), transparent);
        compare(ui.TaggedTexture(), drawPixels);
        Require(ui.TaggedTexture() == taggedIdentity, "stable tag across draw-target reuse");
        // Compare one shared UI layer with two separately composed layers.
        // This isolates layer splitting from background selection and ordering.
        ComPtr<ID3D11Texture2D> foreground;
        ComPtr<ID3D11ShaderResourceView> foregroundSRV;
        Check(device->CreateTexture2D(&desc, nullptr, &foreground), "comparison foreground");
        Check(device->CreateShaderResourceView(foreground.Get(), nullptr, &foregroundSRV), "foreground view");
        const Pixel base{0.1f,0.2f,0.3f,1}, laterBase{0.7f,0.8f,0.9f,1};
        std::vector<Pixel> lower(drawPixels.size()), upper(lower.size()), shared(lower.size()), expected(lower.size());
        auto over = [](const Pixel& front, const Pixel& back) {
            return Pixel{front[0]+back[0]*(1-front[3]),front[1]+back[1]*(1-front[3]),
                front[2]+back[2]*(1-front[3]),front[3]+back[3]*(1-front[3])};
        };
        for(size_t i=0;i<lower.size();++i){
            const float a=float(i%5)/4;
            lower[i]={0.08f,0.12f,0.16f,0.4f}; upper[i]={0.05f*a,0.1f*a,0.15f*a,a};
            shared[i]=over(upper[i],lower[i]); expected[i]=over(shared[i],base);
        }
        auto upload = [&](ID3D11Texture2D* target,const std::vector<Pixel>& values){
            context->UpdateSubresource(target,0,nullptr,values.data(),width*sizeof(Pixel),0);
        };
        const std::vector<Pixel> basePixels(lower.size(),base), laterPixels(lower.size(),laterBase);
        upload(presentation.Get(),basePixels); upload(ui.RenderTexture(),shared);
        Require(ui.Compose(context.Get(),presentation.Get()), "shared layer comparison");
        compare(presentation.Get(),expected);
        upload(presentation.Get(),basePixels); upload(ui.RenderTexture(),lower); upload(foreground.Get(),upper);
        Require(ui.Compose(context.Get(),presentation.Get())&&
            ui.ComposeOverlay(context.Get(),presentation.Get(),foregroundSRV.Get()), "split layer comparison");
        compare(presentation.Get(),expected);
        // Same opacity, different base: surviving scene changes remain visible.
        auto changedExpected=expected;
        for(size_t i=0;i<lower.size();++i){changedExpected[i]=over(shared[i],laterBase);}
        upload(presentation.Get(),laterPixels); upload(ui.RenderTexture(),shared);
        Require(ui.Compose(context.Get(),presentation.Get()), "later background comparison");
        compare(presentation.Get(),changedExpected);
        Require(std::abs(changedExpected[2][0]-expected[2][0]-0.18f)<0.00001f,
            "half-opacity popup plus lower UI retains 30 percent of changed base");
        // Reordering overlapping layers is a separate observable change.
        for(size_t i=0;i<shared.size();++i){shared[i]=over(lower[i],upper[i]);changedExpected[i]=over(shared[i],base);}
        upload(presentation.Get(),basePixels); upload(ui.RenderTexture(),shared);
        Require(ui.Compose(context.Get(),presentation.Get()), "reversed order comparison");
        compare(presentation.Get(),changedExpected);
        Require(std::abs(changedExpected[2][0]-expected[2][0])>0.01f,"overlap order changes RGB");
        std::printf("Composition comparison %ux%u: shared=split with coverage alpha; changed base and reversed order differ\n",width,desc.Height);
        auto wrongDesc = desc; wrongDesc.Width += 1;
        ComPtr<ID3D11Texture2D> wrongExtent;
        Check(device->CreateTexture2D(&wrongDesc, nullptr, &wrongExtent), "wrong extent");
        Require(!ui.Compose(context.Get(), wrongExtent.Get()) && !ui.Extract(context.Get(), nullptr), "invalid destination rejected");
        ui.Invalidate();
        Require(!ui.Available() && !ui.Dedicated() && ui.TaggedTexture() == taggedIdentity &&
            !ui.Compose(context.Get(), presentation.Get()), "failure retains resources and stops work");
        context->ClearState(); retire();
        ui.ResetAfterRetirement();
        Require(!ui.TaggedTexture() && !ui.RenderTexture() && !ui.Available(), "retired reset");

        Require(ui.Initialize(device.Get(), context.Get(), scene.Get(), desc, false) && !ui.Dedicated(), "detection initialization");
        auto finalPixels = scenePixels, extracted = scenePixels;
        for (size_t i = 0; i < finalPixels.size(); ++i) {
            // Exercise unchanged, below-threshold, and visible UI without relying
            // on the implementation shader to calculate expected pixels.
            finalPixels[i][0] += i % 3 == 0 ? 0 : (i % 3 == 1 ? 0.0005f : 0.02f);
            extracted[i] = i % 3 == 2 ? Pixel{finalPixels[i][0], sceneColor[1], sceneColor[2], 1} : Pixel{};
        }
        context->UpdateSubresource(presentation.Get(), 0, nullptr, finalPixels.data(), width * sizeof(Pixel), 0);
        Require(ui.Extract(context.Get(), presentation.Get()), "detection dispatch");
        compare(ui.TaggedTexture(), extracted);
        retire(); ui.ResetAfterRetirement();
        Require(!ui.Initialize(device.Get(), context.Get(), wrongExtent.Get(), desc, true) && !ui.Available(), "mismatched scene rejected");
    }
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T size{}; info->GetMessage(i, nullptr, &size); std::vector<char> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data()); info->GetMessage(i, message, &size);
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) { std::fprintf(stderr, "%s\n", message->pDescription); return 1; }
    }
    std::printf("PASS: %u pixel-channel checks; premultiplied UI, binary extraction, stable identity, state restoration, retirement, invalid inputs, clean D3D11 debug layer\n", comparisons);
}
