#include "ScreenshotFile.h"
#include "ScreenshotWorker.h"
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <string>

namespace TheosRenderPipeline::ScreenshotFile
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        HRESULT ReplaceOpenFile(const std::filesystem::path& temporary, const std::filesystem::path& destination)
        {
            // MoveFileEx cannot replace an open destination even with delete
            // sharing. Windows 10's POSIX rename keeps the old handle valid
            // while atomically publishing the new file at that name.
            const auto name = std::filesystem::absolute(destination).wstring();
            std::vector<std::uint8_t> storage(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
            auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
            rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
            rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
            std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
            const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file == INVALID_HANDLE_VALUE) { return HRESULT_FROM_WIN32(GetLastError()); }
            // SDK minwinbase.h defines FileRenameInfoEx as ABI value 22, but
            // hides the name with this project's older NTDDI declarations.
            constexpr auto renameInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
#if NTDDI_VERSION >= NTDDI_WIN10_RS1
            static_assert(renameInfoEx == FileRenameInfoEx);
#endif
            const bool replaced = FlushFileBuffers(file) && SetFileInformationByHandle(file, renameInfoEx,
                rename, static_cast<DWORD>(storage.size()));
            const auto error = replaced ? ERROR_SUCCESS : GetLastError();
            CloseHandle(file);
            return HRESULT_FROM_WIN32(error); // Unsupported filesystems retain the original.
        }

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
        const std::vector<std::uint8_t>& bgr, int jpegQuality, const std::function<bool()>& stillMatches) noexcept
    {
        return ScreenshotBoundary([&]() -> HRESULT {
            const auto* container = Container(path);
            if (!container) { return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); }
            if (!width || !height || bgr.size() != std::size_t{width} * height * 3) { return E_INVALIDARG; }
            const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) { return initialized; }
            struct Apartment
            {
                bool initialized;
                ~Apartment() { if (initialized) { CoUninitialize(); } }
            } apartment{SUCCEEDED(initialized)};
            auto temporary = path;
            temporary += L".trp-tmp";
            struct TemporaryFile
            {
                const std::filesystem::path& path;
                ~TemporaryFile() { DeleteFileW(path.c_str()); }
            } cleanup{temporary};
            auto hr = Encode(temporary, *container, width, height, bgr, jpegQuality);
            // An inferred Photo Mode destination must still name the claimed
            // original after encoding. Decline without touching it if replaced.
            if (SUCCEEDED(hr) && stillMatches && !stillMatches()) { return S_FALSE; }
            if (SUCCEEDED(hr) && stillMatches) { return ReplaceOpenFile(temporary, path); }
            if (SUCCEEDED(hr) && !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                hr = HRESULT_FROM_WIN32(GetLastError());
            }
            return hr;
        });
    }
}
