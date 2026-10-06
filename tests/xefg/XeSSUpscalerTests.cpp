#include "XeFGTestSupport.h"
#include "FrameGen/XeSSUpscaler.h"

int wmain(int argc, wchar_t** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); Require(argc == 2, "Intel runtime directory");
    ComPtr<ID3D12Debug> debug; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug"); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory"); Check(factory->EnumAdapters1(0, &adapter), "adapter");
    ComPtr<ID3D11Device> d11; ComPtr<ID3D11DeviceContext> c11; ComPtr<ID3D12Device> d12;
    Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_DEBUG,
        nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr, &c11), "D3D11");
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12)), "D3D12");
    ComPtr<ID3D12InfoQueue> messages; Check(d12.As(&messages), "debug messages");
    D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue;
    Check(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Interop interop; Check(interop.Initialize(d11.Get(), d12.Get(), queue.Get()), "shared owner");
    TheosRenderPipeline::XeSSUpscaler sr;
    auto runtime = std::filesystem::absolute(argv[1]); Check(sr.Open(d12.Get(), runtime), "XeSS context");
    TheosRenderPipeline::FrameExtent last{};
    for (int quality : {3, 0, 1, 2, 4, 5}) {
        TheosRenderPipeline::FrameExtent size{}; Check(sr.QuerySize(640, 360, quality, size), "quality resolution");
        Require(size.width >= last.width && size.height >= last.height, "increasing quality resolution"); last = size;
        std::printf("quality=%d render=%ux%u\n", quality, size.width, size.height);
    }
    Require(last.width == 640 && last.height == 360, "AA is native resolution");
    for (int round = 0; round < 2; ++round) {
        const auto initialization = sr.Initialize(interop, d11.Get(), {640, 360}, round ? 0 : 2, DXGI_FORMAT_R8G8B8A8_UNORM, true);
        std::printf("initialize status=%s hr=0x%08X\n", sr.Status().c_str(), static_cast<unsigned>(initialization));
        if (FAILED(initialization)) {
            ComPtr<ID3D11InfoQueue> d11Messages; d11.As(&d11Messages);
            if (d11Messages) { for (UINT64 n = 0; n < d11Messages->GetNumStoredMessages(); ++n) {
                SIZE_T length{}; d11Messages->GetMessage(n, nullptr, &length); std::vector<unsigned char> storage(length);
                auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data()); d11Messages->GetMessage(n, message, &length);
                std::fprintf(stderr, "%s\n", message->pDescription);
            } }
            for (UINT64 n = 0; n < messages->GetNumStoredMessages(); ++n) {
                SIZE_T length{}; messages->GetMessage(n, nullptr, &length); std::vector<unsigned char> storage(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); messages->GetMessage(n, message, &length);
                std::fprintf(stderr, "%s\n", message->pDescription);
            }
        }
        Check(initialization, "XeSS initialize");
        const auto size = sr.InputSize();
        auto make = [&](UINT w, UINT h, DXGI_FORMAT format) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
            desc.Format = format; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> texture; Check(d11->CreateTexture2D(&desc, nullptr, &texture), "input texture"); return texture;
        };
        auto color = make(size.width, size.height, DXGI_FORMAT_R8G8B8A8_UNORM);
        auto motion = make(size.width, size.height, DXGI_FORMAT_R16G16_FLOAT);
        auto depth = make(size.width, size.height, DXGI_FORMAT_R32_FLOAT);
        auto output = make(640, 360, DXGI_FORMAT_R8G8B8A8_UNORM);
        std::vector<std::uint32_t> colors(size.width * size.height, 0xFF4080C0);
        std::vector<std::uint32_t> motions(colors.size(), 0);
        std::vector<float> depths(colors.size(), 0.5f);
        c11->UpdateSubresource(color.Get(), 0, nullptr, colors.data(), size.width * 4, 0);
        c11->UpdateSubresource(motion.Get(), 0, nullptr, motions.data(), size.width * 4, 0);
        c11->UpdateSubresource(depth.Get(), 0, nullptr, depths.data(), size.width * 4, 0);
        for (unsigned frame = 0; frame < 16; ++frame) {
            const auto hr = sr.Evaluate(c11.Get(), color.Get(), motion.Get(), depth.Get(), output.Get(),
                0, 0, frame == 8, frame < 10, frame >= 12 ? 0.5f : 0.0f);
            std::printf("frame=%u status=%s\n", frame, sr.Status().c_str()); Check(hr, "XeSS evaluate, reset, depth reinit, RCAS");
        }
        Check(interop.SignalD3D11(Work::Upscaling), "retire output consumers"); Check(interop.Drain(), "retirement");
        D3D11_TEXTURE2D_DESC readDesc{}; output->GetDesc(&readDesc); readDesc.Usage = D3D11_USAGE_STAGING;
        readDesc.BindFlags = 0; readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read; Check(d11->CreateTexture2D(&readDesc, nullptr, &read), "readback");
        c11->CopyResource(read.Get(), output.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(c11->Map(read.Get(), 0, D3D11_MAP_READ, 0, &mapped), "read output");
        const auto* pixel = static_cast<const unsigned char*>(mapped.pData) + 180 * mapped.RowPitch + 320 * 4;
        std::printf("pixel=%u,%u,%u,%u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
        Require(std::abs(static_cast<int>(pixel[0]) - 192) < 12 && std::abs(static_cast<int>(pixel[1]) - 128) < 12 &&
            std::abs(static_cast<int>(pixel[2]) - 64) < 12, "XeSS reconstructs constant color"); c11->Unmap(read.Get(), 0);
        Require(sr.Frames() == 16, "completed evaluation count"); Check(sr.ReleaseAfterRetirement(), "destroy retired XeSS");
        if (!round) { Check(sr.Open(d12.Get(), runtime), "recreate XeSS context"); }
    }
    for (UINT64 n = 0; n < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++n) {
        SIZE_T length{}; messages->GetMessage(n, nullptr, &length); std::vector<unsigned char> storage(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); messages->GetMessage(n, message, &length);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::fprintf(stderr, "%s\n", message->pDescription); Require(false, "D3D12 debug errors"); }
    }
    Require(!GetModuleHandleW(L"sl.interposer.dll") && !GetModuleHandleW(L"nvngx_dlss.dll") &&
        !GetModuleHandleW(L"nvngx_dlssg.dll"), "no NVIDIA SDK loaded");
    std::puts("PASS real XeSS D3D12 through production D3D11 bridge; AMD game acceptance pending");
}
