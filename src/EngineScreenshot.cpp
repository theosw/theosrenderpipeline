#include <PCH.h>
#include "EngineScreenshot.h"
#include "HookDetour.h"

#include <d3d11.h>

#include <array>
#include <atomic>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

namespace TheosRenderPipeline::EngineScreenshot
{
    namespace
    {
        using Format = RE::BSGraphics::TextureFileFormat;
        using Save = void (*)(RE::BSGraphics::Renderer*, RE::RENDER_TARGETS::RENDER_TARGET, const char*, Format);
        Save g_original{};
        std::mutex g_mutex;
        // A flag-only request waits one Present: the flag rises before the frame
        // Skyrim screenshots is finished.
        struct Pending
        {
            Request request;
            int wait{};
            bool flagged{};  // queued by the flag, still without a path
        };
        std::deque<Pending> g_pending;

        ID3D11Texture2D* ScreenshotTarget()
        {
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            return renderer ? reinterpret_cast<ID3D11Texture2D*>(
                                  renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSCREENSHOT].texture)
                            : nullptr;
        }

        bool Queued()
        {
            auto* controls = RE::MenuControls::GetSingleton();
            return controls && controls->screenshotHandler && controls->screenshotHandler->screenshotQueued;
        }

        // PNG width and height from its IHDR chunk.
        bool PngSize(const std::filesystem::path& a_file, unsigned& a_width, unsigned& a_height)
        {
            std::ifstream file(a_file, std::ios::binary);
            std::array<unsigned char, 24> head{};
            if (!file.read(reinterpret_cast<char*>(head.data()), head.size())) { return false; }
            constexpr std::array<unsigned char, 8> signature{ 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
            if (!std::equal(signature.begin(), signature.end(), head.begin())) { return false; }
            const auto big = [&](std::size_t at) {
                return (unsigned(head[at]) << 24) | (unsigned(head[at + 1]) << 16) | (unsigned(head[at + 2]) << 8) | head[at + 3];
            };
            a_width = big(16);
            a_height = big(20);
            return true;
        }

        void Thunk(RE::BSGraphics::Renderer* a_renderer, RE::RENDER_TARGETS::RENDER_TARGET a_target, const char* a_path,
            Format a_format)
        {
            // The engine's file is written first and stays as the fallback.
            g_original(a_renderer, a_target, a_path, a_format);
            try {
                const bool replaceable = a_format == Format::kBMP || a_format == Format::kJPG || a_format == Format::kPNG;
                logger::info("[Screenshot] Skyrim saved target {} to {} (format {}){}", static_cast<int>(a_target),
                    a_path ? a_path : "(no path)", static_cast<int>(a_format),
                    a_target == RE::RENDER_TARGETS::kSCREENSHOT && a_path && replaceable
                        ? "; replacing it with the finished frame" : "");
                if (a_target != RE::RENDER_TARGETS::kSCREENSHOT || !a_path || !replaceable) { return; }
                std::lock_guard lock(g_mutex);
                // The flag queued this screenshot already: give it Skyrim's file.
                for (auto& pending : g_pending) {
                    if (pending.flagged) {
                        pending.request.path = a_path;
                        pending.flagged = false;
                        return;
                    }
                }
                if (g_pending.size() < 4) { g_pending.push_back({ { a_path } }); }
            } catch (...) {
                // A screenshot must never take the game down; the engine's file stays.
            }
        }
    }

    void Install()
    {
        const auto site = REL::RelocationID(75522, 77316).address();
        std::array<std::uint8_t, 16> entry{};
        if (!HookSafety::Read(site, entry.data(), entry.size())) {
            logger::warn("[Screenshot] Skyrim's screenshot save is unreadable; screenshots stay at the render size");
            return;
        }
        // Another plugin's jump is chained by InstallEntryDetour; original code must be safe to move.
        const bool chained = entry[0] == 0xE9 || (entry[0] == 0xFF && entry[1] == 0x25);
        if (!chained && !HookSafety::SafePrologueLength(entry.data(), entry.size())) {
            std::string bytes;
            for (const auto byte : entry) { bytes += std::format("{:02X} ", byte); }
            logger::warn("[Screenshot] Skyrim's screenshot save starts with code TRP cannot hook safely ({}); "
                         "screenshots stay at the render size", bytes);
            return;
        }
        g_original = reinterpret_cast<Save>(HookSafety::InstallEntryDetour(site, reinterpret_cast<std::uintptr_t>(&Thunk)));
        if (g_original) {
            logger::info("[Screenshot] screenshot save hooked{}; saved screenshots get the finished frame",
                chained ? " behind another plugin" : "");
        } else {
            logger::warn("[Screenshot] could not hook Skyrim's screenshot save; screenshots stay at the render size");
        }
    }

    namespace
    {
        std::atomic_bool g_queuedThisFrame{};
    }

    void OnFrameBegin()
    {
        if (Queued()) { g_queuedThisFrame = true; }
    }

    void OnPresent()
    {
        static bool wasQueued = false;
        const bool queued = g_queuedThisFrame.exchange(false) || Queued();
        const bool rose = queued && !wasQueued;
        wasQueued = queued;
        if (!rose || !g_original) { return; }
        try {
            Request request;
            request.since = std::chrono::system_clock::now();
            if (auto* target = ScreenshotTarget()) {
                D3D11_TEXTURE2D_DESC desc{};
                target->GetDesc(&desc);
                request.width = desc.Width;
                request.height = desc.Height;
            }
            std::lock_guard lock(g_mutex);
            if (g_pending.size() < 4) { g_pending.push_back({ std::move(request), 1, true }); }
        } catch (...) {
        }
    }

    bool Take(Request& a_request)
    {
        std::lock_guard lock(g_mutex);
        if (g_pending.empty()) { return false; }
        if (g_pending.front().wait > 0) {
            --g_pending.front().wait;
            return false;
        }
        a_request = std::move(g_pending.front().request);
        g_pending.pop_front();
        return true;
    }

    std::filesystem::path FindPhoto(const Request& a_request, std::chrono::milliseconds a_timeout)
    {
        const auto log = SKSE::log::log_directory();
        if (!log || !a_request.width || !a_request.height) { return {}; }
        // My Games\<game>\SKSE -> My Games\<game>\Photos, where Photo Mode saves.
        const auto photos = log->parent_path() / "Photos";
        const auto deadline = std::chrono::steady_clock::now() + a_timeout;
        // File times have a coarse resolution; allow a little before the flag.
        const auto earliest = a_request.since - std::chrono::seconds(1);
        for (;;) {
            std::filesystem::path newest;
            std::filesystem::file_time_type newestTime{};
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(photos, error)) {
                if (!entry.is_regular_file(error) || entry.path().extension() != ".png") { continue; }
                const auto written = entry.last_write_time(error);
                if (error || std::chrono::clock_cast<std::chrono::system_clock>(written) < earliest) { continue; }
                unsigned width{}, height{};
                if (!PngSize(entry.path(), width, height) || width != a_request.width || height != a_request.height) { continue; }
                if (newest.empty() || written > newestTime) {
                    newest = entry.path();
                    newestTime = written;
                }
            }
            if (!newest.empty() || std::chrono::steady_clock::now() >= deadline) { return newest; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
