#pragma once

#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace TheosRenderPipeline::ScreenshotFile
{
    // Encodes tightly packed 24-bit BGR rows in the container named by the
    // extension (.bmp, .png, .jpg/.jpeg) and atomically replaces `path`.
    // Any existing file is left intact on failure. Runs on a worker thread;
    // COM is initialized for the duration of the call when required.
    HRESULT Replace(const std::filesystem::path& path, UINT width, UINT height,
        const std::vector<std::uint8_t>& bgr, int jpegQuality);
}
