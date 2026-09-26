#include "FrameGen/FinalFrameCapture.h"
#include "ScreenshotFile.h"
#include <dxgi1_4.h>
#include <wincodec.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::FinalFrameCapture;
namespace ScreenshotFile = TheosRenderPipeline::ScreenshotFile;

static void Require(bool ok, const char* why)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why)
{ if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s (0x%08X)\n", why, static_cast<unsigned>(hr)); std::exit(1); } }

// Distinct per-pixel colour so row-pitch and channel-order mistakes are visible.
static std::array<std::uint8_t, 3> Expected(UINT x, UINT y) // R, G, B
{ return {static_cast<std::uint8_t>(x * 3 + 1), static_cast<std::uint8_t>(y * 40 + 2), static_cast<std::uint8_t>((x + y) * 5 + 3)}; }

static void CheckConversion()
{
    std::vector<std::uint8_t> out;
    // Two pixels per row with a padded 12-byte pitch.
    const std::array<std::uint8_t, 24> rgba{10, 20, 30, 255, 40, 50, 60, 0, 0xEE, 0xEE, 0xEE, 0xEE,
                                            70, 80, 90, 1, 100, 110, 120, 2, 0xEE, 0xEE, 0xEE, 0xEE};
    Require(FinalFrameCapture::ToBGR(DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, rgba.data(), 12, out), "RGBA conversion");
    Require(out == std::vector<std::uint8_t>{30, 20, 10, 60, 50, 40, 90, 80, 70, 120, 110, 100}, "RGBA swaps to BGR and skips padding");
    Require(FinalFrameCapture::ToBGR(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 2, 2, rgba.data(), 12, out), "BGRA conversion");
    Require(out == std::vector<std::uint8_t>{10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120}, "BGRA keeps order and drops alpha");
    // R=0x3FF, G=0x200, B=0x004, A=3 packed as R10G10B10A2.
    const std::uint32_t packed = 0x3FFu | 0x200u << 10 | 0x004u << 20 | 3u << 30;
    std::array<std::uint8_t, 4> bytes{};
    std::memcpy(bytes.data(), &packed, 4);
    Require(FinalFrameCapture::ToBGR(DXGI_FORMAT_R10G10B10A2_UNORM, 1, 1, bytes.data(), 4, out), "10-bit conversion");
    Require(out == std::vector<std::uint8_t>{0x01, 0x80, 0xFF}, "10-bit channels keep their top eight bits");
    Require(!FinalFrameCapture::ToBGR(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, bytes.data(), 8, out), "HDR scRGB is rejected");
    Require(!FinalFrameCapture::ToBGR(DXGI_FORMAT_R8G8B8A8_UNORM, 2, 1, rgba.data(), 4, out), "short pitch is rejected");
    Require(!FinalFrameCapture::BytesPerPixel(DXGI_FORMAT_R16G16B16A16_FLOAT), "HDR output has no screenshot format");
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t value{};
    void Open() { Check(allocator->Reset(), "allocator reset"); Check(list->Reset(allocator.Get(), nullptr), "list reset"); }
    void Execute()
    {
        Check(list->Close(), "list close");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
    }
    void Wait()
    {
        Check(queue->Signal(fence.Get(), ++value), "fence signal");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(fence->SetEventOnCompletion(value, event), "fence event");
        Require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "GPU work completes"); CloseHandle(event);
    }
};

// Uploads the pattern to `texture` (COMMON before and after).
static void Fill(Gpu& gpu, ID3D12Resource* texture)
{
    const auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 total{};
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = total;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    Check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "upload buffer");
    std::uint8_t* mapped{}; Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "upload map");
    for (UINT y = 0; y < desc.Height; ++y) {
        for (UINT x = 0; x < desc.Width; ++x) {
            const auto c = Expected(x, y);
            auto* p = mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4;
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = 0x7F;
        }
    }
    upload->Unmap(0, nullptr);
    gpu.Open();
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
    gpu.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = texture; destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint = footprint;
    gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    gpu.list->ResourceBarrier(1, &barrier);
    gpu.Execute(); gpu.Wait();
}

static void RequirePattern(const FinalFrameCapture::Image& image, UINT width, UINT height, const char* why)
{
    Require(image.width == width && image.height == height && image.bgr.size() == std::size_t{width} * height * 3, why);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const auto c = Expected(x, y);
            const auto* p = image.bgr.data() + (std::size_t{y} * width + x) * 3;
            Require(p[0] == c[2] && p[1] == c[1] && p[2] == c[0], why);
        }
    }
}

static FinalFrameCapture::Image Capture(Gpu& gpu, FinalFrameCapture& capture, ID3D12Resource* output)
{
    FinalFrameCapture::Readback readback;
    Require(capture.Poll(readback) == S_FALSE && !capture.Busy(), "idle capture has nothing to hand over");
    gpu.Open();
    Check(capture.Record(gpu.device.Get(), gpu.list.Get(), output), "record final frame copy");
    Require(capture.Busy(), "recorded capture is busy");
    Require(capture.Record(gpu.device.Get(), gpu.list.Get(), output) == E_ILLEGAL_METHOD_CALL, "one capture at a time");
    Require(capture.Poll(readback) == S_FALSE, "unsubmitted capture is not complete");
    gpu.Execute();
    Check(capture.Submitted(gpu.queue.Get()), "signal after submission");
    Require(capture.Submitted(gpu.queue.Get()) == E_ILLEGAL_METHOD_CALL, "single submission signal");
    HRESULT hr = S_FALSE;
    for (int i = 0; i < 3000 && hr == S_FALSE; ++i) { hr = capture.Poll(readback); if (hr == S_FALSE) { Sleep(1); } }
    Check(hr, "capture completes");
    Require(!capture.Busy() && readback.buffer, "completed capture hands over its buffer");
    FinalFrameCapture::Image image;
    // Mapping is legal on any thread once the fence completed.
    std::thread([&] { hr = FinalFrameCapture::Read(readback, image); }).join();
    Check(hr, "read on a worker thread");
    return image;
}

static std::vector<std::uint8_t> Decode(const std::filesystem::path& path, UINT& width, UINT& height)
{
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "decode written file");
    ComPtr<IWICBitmapFrameDecode> frame; Check(decoder->GetFrame(0, &frame), "decoded frame");
    ComPtr<IWICFormatConverter> converter; Check(factory->CreateFormatConverter(&converter), "converter");
    Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "convert");
    Check(converter->GetSize(&width, &height), "decoded size");
    std::vector<std::uint8_t> pixels(std::size_t{width} * height * 3);
    Check(converter->CopyPixels(nullptr, width * 3, static_cast<UINT>(pixels.size()), pixels.data()), "decoded pixels");
    return pixels;
}

static std::string Contents(const std::filesystem::path& path)
{ std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}}; }

static void CheckFiles(const FinalFrameCapture::Image& image, const std::filesystem::path& directory)
{
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    for (const wchar_t* name : {L"shot.png", L"shot.BMP", L"shot.jpg"}) {
        const auto path = directory / name;
        { std::ofstream(path, std::ios::binary) << "ReShade UI-layer capture"; }
        Check(ScreenshotFile::Replace(path, image.width, image.height, image.bgr, 100), "replace screenshot");
        Require(!std::filesystem::exists(std::filesystem::path(path) += L".trp-tmp"), "temporary file is moved into place");
        UINT width{}, height{};
        const auto decoded = Decode(path, width, height);
        Require(width == image.width && height == image.height, "written dimensions");
        const bool lossy = std::wstring(name).ends_with(L".jpg");
        std::size_t total{};
        for (std::size_t i = 0; i < decoded.size(); ++i) {
            const int difference = std::abs(static_cast<int>(decoded[i]) - static_cast<int>(image.bgr[i]));
            Require(difference <= (lossy ? 32 : 0), lossy ? "JPEG approximates every pixel" : "lossless containers keep exact pixels");
            total += difference;
        }
        Require(total <= decoded.size() * 4, "written image matches the captured frame");
    }
    // Failures must leave ReShade's own file untouched.
    const auto kept = directory / L"kept.png";
    { std::ofstream(kept, std::ios::binary) << "ReShade UI-layer capture"; }
    auto truncated = image.bgr; truncated.pop_back();
    Require(ScreenshotFile::Replace(kept, image.width, image.height, truncated, 90) == E_INVALIDARG, "size mismatch rejected");
    const auto unsupported = directory / L"kept.tga";
    { std::ofstream(unsupported, std::ios::binary) << "ReShade UI-layer capture"; }
    Require(FAILED(ScreenshotFile::Replace(unsupported, image.width, image.height, image.bgr, 90)), "unknown container rejected");
    Require(Contents(kept) == "ReShade UI-layer capture" && Contents(unsupported) == "ReShade UI-layer capture", "failed replacement keeps the original file");
    Require(!std::filesystem::exists(std::filesystem::path(kept) += L".trp-tmp"), "failed replacement leaves no temporary file");
}

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    CheckConversion();
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");

    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    Gpu gpu;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)))) {
        ComPtr<IDXGIAdapter> warp; Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
        Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)), "WARP device");
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    Check(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.queue)), "queue");
    Check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)), "allocator");
    Check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list)), "list");
    Check(gpu.list->Close(), "initial close");
    Check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)), "fence");

    FinalFrameCapture capture;
    // 67 pixels is not a multiple of the 256-byte row alignment.
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = 67; desc.Height = 5;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> texture;
    Check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)), "texture");
    Fill(gpu, texture.Get());
    const auto image = Capture(gpu, capture, texture.Get());
    RequirePattern(image, 67, 5, "texture capture matches every pixel");
    RequirePattern(Capture(gpu, capture, texture.Get()), 67, 5, "capture object is reusable");

    // A rejected format records nothing, leaves the capture idle and the list usable.
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<ID3D12Resource> hdr;
    Check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&hdr)), "HDR texture");
    gpu.Open();
    Require(capture.Record(gpu.device.Get(), gpu.list.Get(), hdr.Get()) == E_INVALIDARG && !capture.Busy(), "HDR output rejected without recording");
    Require(capture.Submitted(gpu.queue.Get()) == E_ILLEGAL_METHOD_CALL, "nothing to signal after rejection");
    gpu.Execute(); gpu.Wait();

    // The production source is a flip-model swapchain buffer in COMMON/PRESENT.
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW; wc.lpszClassName = L"TRPFinalFrameCapture";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"TRP final frame capture", WS_POPUP, 0, 0, 67, 5, nullptr, nullptr, wc.hInstance, nullptr);
    Require(window != nullptr, "hidden window");
    DXGI_SWAP_CHAIN_DESC1 chain{}; chain.Width = 67; chain.Height = 5; chain.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    chain.SampleDesc.Count = 1; chain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; chain.BufferCount = 2; chain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapChain;
    Check(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &chain, nullptr, nullptr, &swapChain), "swapchain");
    ComPtr<ID3D12Resource> backBuffer; Check(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "back buffer");
    Fill(gpu, backBuffer.Get());
    // B8G8R8A8 memory order: Fill wrote R,G,B into bytes 0..2, which BGRA reads as B,G,R.
    auto swapped = Capture(gpu, capture, backBuffer.Get());
    for (std::size_t i = 0; i < swapped.bgr.size(); i += 3) { std::swap(swapped.bgr[i], swapped.bgr[i + 2]); }
    RequirePattern(swapped, 67, 5, "swapchain buffer capture matches every pixel");
    Check(swapChain->Present(0, 0), "present after capture");
    gpu.Wait();
    backBuffer.Reset(); swapChain.Reset(); DestroyWindow(window);

    wchar_t temp[MAX_PATH]{}; Require(GetTempPathW(MAX_PATH, temp) > 0, "temporary directory");
    CheckFiles(image, std::filesystem::path(temp) / L"TRPFinalFrameCaptureTests");
    std::filesystem::remove_all(std::filesystem::path(temp) / L"TRPFinalFrameCaptureTests");
    CoUninitialize();
    std::puts("PASS: final frame capture, conversion and screenshot replacement");
    return 0;
}
