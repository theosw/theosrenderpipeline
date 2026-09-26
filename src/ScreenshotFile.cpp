#include "ScreenshotFile.h"
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <string>

namespace TheosRenderPipeline::ScreenshotFile
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        const GUID* Container(const std::filesystem::path& path)
        {
            auto extension = path.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (extension == L".bmp") { return &GUID_ContainerFormatBmp; }
            if (extension == L".png") { return &GUID_ContainerFormatPng; }
            if (extension == L".jpg" || extension == L".jpeg") { return &GUID_ContainerFormatJpeg; }
            return nullptr;
        }

        HRESULT Encode(const std::filesystem::path& path, const GUID& container, UINT width, UINT height,
            const std::vector<std::uint8_t>& bgr, int jpegQuality)
        {
            ComPtr<IWICImagingFactory> factory;
            auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            ComPtr<IWICStream> stream;
            if (SUCCEEDED(hr)) { hr = factory->CreateStream(&stream); }
            if (SUCCEEDED(hr)) { hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE); }
            ComPtr<IWICBitmapEncoder> encoder;
            if (SUCCEEDED(hr)) { hr = factory->CreateEncoder(container, nullptr, &encoder); }
            if (SUCCEEDED(hr)) { hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache); }
            ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2> options;
            if (SUCCEEDED(hr)) { hr = encoder->CreateNewFrame(&frame, &options); }
            if (SUCCEEDED(hr) && container == GUID_ContainerFormatJpeg) {
                // Like ReShade's encoder, keep full chroma above quality 90.
                const auto quality = std::clamp(jpegQuality, 1, 100);
                PROPBAG2 option[2]{};
                option[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                option[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
                VARIANT value[2]{};
                value[0].vt = VT_R4; value[0].fltVal = static_cast<float>(quality) / 100.0f;
                value[1].vt = VT_UI1;
                value[1].bVal = static_cast<BYTE>(quality > 90 ? WICJpegYCrCbSubsampling444 : WICJpegYCrCbSubsampling420);
                hr = options->Write(2, option, value);
            }
            if (SUCCEEDED(hr)) { hr = frame->Initialize(options.Get()); }
            if (SUCCEEDED(hr)) { hr = frame->SetSize(width, height); }
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(hr)) { hr = frame->SetPixelFormat(&format); }
            // All three containers accept 24-bit BGR; refuse a silent substitution.
            if (SUCCEEDED(hr) && format != GUID_WICPixelFormat24bppBGR) { hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; }
            if (SUCCEEDED(hr)) {
                hr = frame->WritePixels(height, width * 3, static_cast<UINT>(bgr.size()), const_cast<BYTE*>(bgr.data()));
            }
            if (SUCCEEDED(hr)) { hr = frame->Commit(); }
            if (SUCCEEDED(hr)) { hr = encoder->Commit(); }
            return hr; // Releasing the stream closes the file before it is moved.
        }
    }

    HRESULT Replace(const std::filesystem::path& path, UINT width, UINT height,
        const std::vector<std::uint8_t>& bgr, int jpegQuality)
    {
        const auto* container = Container(path);
        if (!container) { return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); }
        if (!width || !height || bgr.size() != std::size_t{width} * height * 3) { return E_INVALIDARG; }
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) { return initialized; }
        auto temporary = path;
        temporary += L".trp-tmp";
        auto hr = Encode(temporary, *container, width, height, bgr, jpegQuality);
        if (SUCCEEDED(hr) && !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
        if (FAILED(hr)) { DeleteFileW(temporary.c_str()); }
        if (SUCCEEDED(initialized)) { CoUninitialize(); }
        return hr;
    }
}
