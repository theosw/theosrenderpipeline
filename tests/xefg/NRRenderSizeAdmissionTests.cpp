#include "XeFGTestSupport.h"
#include "FrameGen/XeSSUpscaler.h"
#include "FrameGen/NRRenderSize.h"

// Explicit manual hardware probe. Uses the same query, fixed-input init,
// texture allocation, velocity scale and execute path as NvidiaHost startup.
int wmain(int argc, wchar_t** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); Require(argc == 2, "Intel runtime directory");
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory"); Check(factory->EnumAdapters1(0, &adapter), "adapter");
    DXGI_ADAPTER_DESC1 adapterDesc{}; adapter->GetDesc1(&adapterDesc);
    std::wprintf(L"ADAPTER %ls\n", adapterDesc.Description);
    ComPtr<ID3D11Device> d11; ComPtr<ID3D11DeviceContext> c11; ComPtr<ID3D12Device> d12;
    Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr, &c11), "D3D11");
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12)), "D3D12");
    D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue;
    Check(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Interop interop; Check(interop.Initialize(d11.Get(), d12.Get(), queue.Get()), "shared owner");
    for (const TheosRenderPipeline::FrameExtent output : {TheosRenderPipeline::FrameExtent{1280,720}, {1920,1080}}) {
        for (int quality : {2, 1, 0}) {
            TheosRenderPipeline::XeSSUpscaler sr;
            Check(sr.Open(d12.Get(), std::filesystem::absolute(argv[1])), "XeSS context");
            TheosRenderPipeline::FrameExtent nominal{}; TheosRenderPipeline::UpscalerInputResolution range;
            Check(sr.QuerySize(output.width, output.height, quality, nominal), "nominal SDK query");
            Check(sr.QueryInputRange(output.width, output.height, quality, range), "SDK range query");
            const auto chosen = TheosRenderPipeline::NeuralRendering::SelectNRFriendlyRenderSize(nominal, output, range, {true,true,true,quality});
            Require(range.Accepts(chosen, output), "TRP SDK range/aspect admission");
            const auto hr = sr.Initialize(interop, d11.Get(), output, quality, DXGI_FORMAT_R8G8B8A8_UNORM, true, nullptr, chosen);
            std::printf("INIT output=%ux%u quality=%d nominal=%ux%u chosen=%ux%u hr=0x%08X status=%s\n",
                output.width, output.height, quality, nominal.width, nominal.height, chosen.width, chosen.height, unsigned(hr), sr.Status().c_str());
            Check(hr, "production fixed-size initialization");
            Require(sr.InputSize().width == chosen.width && sr.InputSize().height == chosen.height, "one input extent");
            auto make = [&](TheosRenderPipeline::FrameExtent size, DXGI_FORMAT format) {
                D3D11_TEXTURE2D_DESC desc{}; desc.Width = size.width; desc.Height = size.height;
                desc.MipLevels = desc.ArraySize = 1; desc.Format = format; desc.SampleDesc.Count = 1;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> texture; Check(d11->CreateTexture2D(&desc, nullptr, &texture), "texture"); return texture;
            };
            auto color = make(chosen, DXGI_FORMAT_R8G8B8A8_UNORM), motion = make(chosen, DXGI_FORMAT_R16G16_FLOAT);
            auto depth = make(chosen, DXGI_FORMAT_R32_FLOAT), destination = make(output, DXGI_FORMAT_R8G8B8A8_UNORM);
            std::vector<std::uint32_t> colors(chosen.width * chosen.height, 0xFF4080C0), motions(colors.size(), 0);
            std::vector<float> depths(colors.size(), 0.5f);
            c11->UpdateSubresource(color.Get(), 0, nullptr, colors.data(), chosen.width * 4, 0);
            c11->UpdateSubresource(motion.Get(), 0, nullptr, motions.data(), chosen.width * 4, 0);
            c11->UpdateSubresource(depth.Get(), 0, nullptr, depths.data(), chosen.width * 4, 0);
            for (unsigned frame = 0; frame < 2; ++frame) {
                Check(sr.Evaluate(c11.Get(), color.Get(), motion.Get(), depth.Get(), destination.Get(), 0, 0, frame == 0, true, 0), "production XeSS execute");
            }
            Check(interop.SignalD3D11(Work::Upscaling), "retire output consumers"); Check(interop.Drain(), "retirement");
            Check(sr.ReleaseAfterRetirement(), "retired context release");
            std::printf("PASS output=%ux%u quality=%d snapped=%ux%u range/init/execute=accepted frames=2\n", output.width, output.height, quality, chosen.width, chosen.height);
        }
    }
    std::puts("PASS six snapped XeSS sizes through production query/init/execute; Skyrim acceptance untested");
}
