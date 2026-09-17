#include "FrameGen/CommunityShaderFrame.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::CommunityShaderFrame;
using TheosRenderPipeline::D3D11ContextIsolation;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { if (FAILED(hr)) { std::fprintf(stderr, "%08lx: ", hr); } Require(SUCCEEDED(hr), why); }

static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, UINT width, UINT height, UINT bindings,
    const std::vector<float>& values = {})
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT; desc.BindFlags = bindings;
    D3D11_SUBRESOURCE_DATA initial{values.data(), width * 4, 0};
    ComPtr<ID3D11Texture2D> result;
    Check(device->CreateTexture2D(&desc, values.empty() ? nullptr : &initial, &result), "texture");
    return result;
}

static std::vector<float> Pixels(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    Require(texture != nullptr, "readback texture exists");
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map");
    std::vector<float> result(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(result.data() + y * desc.Width, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
    }
    context->Unmap(staging.Get(), 0);
    return result;
}

static unsigned dispatches{};
static void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    ++dispatches; context->Dispatch(x, y, z);
}

int main()
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
    std::vector<float> values(32);
    for (UINT i = 0; i < values.size(); ++i) { values[i] = float(i + 1) / 64; }
    auto motion = Texture(device.Get(), 8, 4, D3D11_BIND_SHADER_RESOURCE, values);
    auto depth = Texture(device.Get(), 8, 4, D3D11_BIND_SHADER_RESOURCE, values);
    auto world = Texture(device.Get(), 8, 4, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, values);
    auto ui = Texture(device.Get(), 8, 4, D3D11_BIND_SHADER_RESOURCE, std::vector<float>(32, 2));
    auto output = Texture(device.Get(), 8, 4, D3D11_BIND_UNORDERED_ACCESS, std::vector<float>(32, -7));
    ComPtr<ID3D11RenderTargetView> worldRTV; Check(device->CreateRenderTargetView(world.Get(), nullptr, &worldRTV), "world RTV");
    ComPtr<ID3D11ShaderResourceView> sceneSRV, uiSRV;
    Check(device->CreateShaderResourceView(world.Get(), nullptr, &sceneSRV), "scene SRV");
    Check(device->CreateShaderResourceView(ui.Get(), nullptr, &uiSRV), "UI SRV");
    ComPtr<ID3D11UnorderedAccessView> outputUAV; Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &outputUAV), "output UAV");
    constexpr char program[] = "Texture2D<float> scene:register(t0); Texture2D<float> ui:register(t1); RWTexture2D<float> result:register(u0); cbuffer C:register(b0){float gain;}; [numthreads(4,2,1)] void main(uint3 p:SV_DispatchThreadID){result[p.xy]=scene.Load(int3(p.xy,0))*gain+ui.Load(int3(p.xy,0));}";
    ComPtr<ID3DBlob> code;
    Check(D3DCompile(program, sizeof(program) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, nullptr), "display shader code");
    ComPtr<ID3D11ComputeShader> shader;
    Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "display shader");
    const float constants[]{3, 0, 0, 0};
    D3D11_BUFFER_DESC bufferDesc{}; bufferDesc.ByteWidth = sizeof(constants); bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constantData{constants, 0, 0};
    ComPtr<ID3D11Buffer> cb; Check(device->CreateBuffer(&bufferDesc, &constantData, &cb), "display constants");
    const D3D11_VIEWPORT viewport{1, 2, 3, 4, 0.2f, 0.8f};
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, worldRTV.GetAddressOf(), nullptr);
    context->PSSetShaderResources(4, 1, uiSRV.GetAddressOf());
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, outputUAV.GetAddressOf(), nullptr);

    CommunityShaderFrame frame;
    Check(frame.CaptureGuides(context.Get(), 1, motion.Get(), depth.Get(), {4, 2}, {8, 4}), "guides");
    Check(frame.CaptureScene(context.Get(), world.Get()), "scene bound as producer RTV");
    ComPtr<ID3D11RenderTargetView> restoredRTV; ComPtr<ID3D11ShaderResourceView> restoredPS;
    ComPtr<ID3D11ComputeShader> restoredShader; ComPtr<ID3D11Buffer> restoredCB;
    ComPtr<ID3D11UnorderedAccessView> restoredUAV;
    context->OMGetRenderTargets(1, &restoredRTV, nullptr); context->PSGetShaderResources(4, 1, &restoredPS);
    context->CSGetShader(&restoredShader, nullptr, nullptr); context->CSGetConstantBuffers(0, 1, &restoredCB);
    context->CSGetUnorderedAccessViews(0, 1, &restoredUAV);
    D3D11_VIEWPORT restoredViewport{}; UINT count = 1; context->RSGetViewports(&count, &restoredViewport);
    Require(restoredRTV == worldRTV && restoredPS == uiSRV && restoredShader == shader && restoredCB == cb &&
        restoredUAV == outputUAV && count == 1 && std::memcmp(&viewport, &restoredViewport, sizeof(viewport)) == 0,
        "graphics and compute state restored after both captures");
    {
        D3D11ContextIsolation::Scope first{frame.Isolation(), context.Get()}; Require(bool(first), "outer isolation");
        D3D11ContextIsolation::Scope nested{frame.Isolation(), context.Get()}; Require(!nested, "nested isolation rejected without restoring outer state");
    }
    ComPtr<ID3D11DeviceContext> deferred; Check(device->CreateDeferredContext(0, &deferred), "deferred context");
    Require(!frame.Isolation().Begin(deferred.Get()), "deferred context rejected");
    const std::vector<float> overwritten(32, 666);
    context->UpdateSubresource(motion.Get(), 0, nullptr, overwritten.data(), 32, 0);
    context->UpdateSubresource(depth.Get(), 0, nullptr, overwritten.data(), 32, 0);
    context->UpdateSubresource(world.Get(), 0, nullptr, overwritten.data(), 32, 0);
    const auto motionPixels = Pixels(context.Get(), frame.Motion());
    const auto depthPixels = Pixels(context.Get(), frame.Depth());
    Require(motionPixels.size() == 8 && depthPixels == motionPixels, "tight guide snapshots");
    for (UINT y = 0; y < 2; ++y) { for (UINT x = 0; x < 4; ++x) {
        Require(motionPixels[y * 4 + x] == values[y * 8 + x], "guides survive producer overwrite");
    } }
    D3D11_TEXTURE2D_DESC presentation{}; output->GetDesc(&presentation);
    Require(Pixels(context.Get(), frame.Hudless(presentation)) == values, "world snapshot survives later writes");
    Require(frame.CaptureGuides(context.Get(), 1, motion.Get(), depth.Get(), {4, 2}, {8, 4}) == S_FALSE, "duplicate boundary preserves original frame");

    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* sourceViews[]{sceneSRV.Get(), uiSRV.Get()};
    context->CSSetShaderResources(0, 2, sourceViews);
    Require(frame.CaptureDisplayTransform(context.Get(), 2, 2, 1, Dispatch) == S_FALSE,
        "texture slots alone do not identify the producer's UI");
    frame.SetUIBoundary(ui.Get());
    Require(!frame.Hudless(presentation), "separate UI requires a confirmed display conversion even with identical formats");
    Require(frame.CaptureDisplayTransform(deferred.Get(), 2, 2, 1, Dispatch) == S_FALSE, "deferred dispatch ignored");
    Require(frame.CaptureDisplayTransform(context.Get(), 2, 2, 2, Dispatch) == S_FALSE, "unrelated dispatch shape ignored");
    auto extra = Texture(device.Get(), 8, 4, D3D11_BIND_UNORDERED_ACCESS, std::vector<float>(32, 41));
    ComPtr<ID3D11UnorderedAccessView> extraUAV; Check(device->CreateUnorderedAccessView(extra.Get(), nullptr, &extraUAV), "extra UAV");
    context->CSSetUnorderedAccessViews(1, 1, extraUAV.GetAddressOf(), nullptr);
    Require(frame.CaptureDisplayTransform(context.Get(), 2, 2, 1, Dispatch) == S_FALSE && dispatches == 0, "multi-output pass never replayed");
    ID3D11UnorderedAccessView* nullUAV{}; context->CSSetUnorderedAccessViews(1, 1, &nullUAV, nullptr);
    Check(frame.CaptureDisplayTransform(context.Get(), 2, 2, 1, Dispatch), "UI-free display conversion");
    Require(dispatches == 1, "one private replay");
    ComPtr<ID3D11ShaderResourceView> restoredScene, restoredUI;
    context->CSGetShaderResources(0, 1, &restoredScene); context->CSGetShaderResources(1, 1, &restoredUI);
    context->CSGetUnorderedAccessViews(0, 1, restoredUAV.ReleaseAndGetAddressOf());
    Require(restoredScene == sceneSRV && restoredUI == uiSRV && restoredUAV == outputUAV, "producer dispatch bindings restored");
    Require(Pixels(context.Get(), output.Get()) == std::vector<float>(32, -7), "private replay never writes original destination");
    Require(!frame.ConfirmPresentationCopy(extra.Get()), "unrelated presentation copy not accepted");
    Require(!frame.Hudless(presentation), "unconfirmed conversion not published");
    Dispatch(context.Get(), 2, 2, 1);
    Require(frame.ConfirmPresentationCopy(output.Get()), "conversion selected by actual presentation source");
    auto encoded = Pixels(context.Get(), frame.Hudless(presentation));
    for (UINT i = 0; i < values.size(); ++i) { Require(encoded[i] == values[i] * 3, "same color conversion without UI"); }
    Require(dispatches == 2 && Pixels(context.Get(), output.Get()) == std::vector<float>(32, 2000), "original composite evaluated once with original scene and UI");
    Require(Pixels(context.Get(), ui.Get()) == std::vector<float>(32, 2) &&
        Pixels(context.Get(), extra.Get()) == std::vector<float>(32, 41), "UI and other producer resources unchanged");
    presentation.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Require(!frame.Hudless(presentation), "different display encoding rejected");
    frame.Consume(); Require(!frame.Ready() && !frame.Hudless(presentation), "completed frame consumed once");
    Require(frame.CaptureDisplayTransform(context.Get(), 2, 2, 1, Dispatch) == S_FALSE, "consumed frame not replayed");
    Require(FAILED(frame.CaptureGuides(context.Get(), 2, motion.Get(), depth.Get(), {9, 2}, {8, 4})) && !frame.Motion(), "invalid new frame cannot expose old guides");
    context->ClearState();
    Check(frame.CaptureGuides(context.Get(), 3, motion.Get(), depth.Get(), {3, 2}, {6, 4}), "resize guides");
    Check(frame.CaptureScene(context.Get(), world.Get()), "resize scene");
    presentation.Width = 6; presentation.Height = 4; presentation.Format = DXGI_FORMAT_R32_FLOAT;
    Require(Pixels(context.Get(), frame.Hudless(presentation)) == std::vector<float>(24, 666), "resized frame uses fresh scene without old conversion");
    frame.ResetAfterRetirement(); Require(!frame.Ready() && !frame.Motion() && !frame.Depth(), "retired resources no longer exposed");
    std::puts("CS frame snapshots: guide/scene overwrite, graphics/compute restoration, isolated display conversion, presentation confirmation, single consumption and resize passed.");
}
