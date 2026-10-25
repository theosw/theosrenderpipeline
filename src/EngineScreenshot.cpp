#include <PCH.h>
#include "EngineScreenshot.h"
#include "HookDetour.h"

#include <d3d11.h>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>

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
        PhotoFiles::Matcher g_photos;
        std::optional<Request> g_flagRequest;
        bool g_wasQueued{};

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

        void ObserveFlag()
        {
            if (!Queued() || g_wasQueued || g_flagRequest) { return; }
            g_flagRequest.emplace();
            // Sample before Main::Swap writes Photo Mode's file. No directory
            // scan, file open or worker wait is added to rendering.
            g_flagRequest->photo = g_photos.Begin();
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
                if (g_flagRequest) {
                    g_flagRequest->path = a_path;
                    g_flagRequest->photo.reset(); // exact-path requests need no inference
                    return;
                }
                // The flag queued this screenshot already: give it Skyrim's file.
                for (auto& pending : g_pending) {
                    if (pending.flagged) {
                        pending.request.path = a_path;
                        pending.request.photo.reset();
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

    void OnFrameBegin()
    {
        try { ObserveFlag(); } catch (...) { g_flagRequest.reset(); }
    }

    void OnPresent()
    {
        try {
            ObserveFlag();
            const bool queued = g_flagRequest.has_value() || Queued();
            const bool rose = queued && !g_wasQueued;
            g_wasQueued = queued;
            if (!rose || !g_original) { g_flagRequest.reset(); return; }
            Request request = g_flagRequest ? std::move(*g_flagRequest) : Request{};
            g_flagRequest.reset();
            if (auto* target = ScreenshotTarget()) {
                D3D11_TEXTURE2D_DESC desc{};
                target->GetDesc(&desc);
                request.width = desc.Width;
                request.height = desc.Height;
            }
            std::lock_guard lock(g_mutex);
            const bool flagged = request.path.empty();
            if (g_pending.size() < 4) { g_pending.push_back({ std::move(request), 1, flagged }); }
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

    PhotoFiles::File FindPhoto(const Request& a_request, std::chrono::milliseconds a_timeout)
    {
        const auto log = SKSE::log::log_directory();
        if (!log || !a_request.width || !a_request.height) { return {}; }
        // My Games\<game>\SKSE -> My Games\<game>\Photos, where Photo Mode saves.
        const auto photos = log->parent_path() / "Photos";
        return g_photos.Find(a_request.photo, photos, a_request.width, a_request.height, a_timeout);
    }
}
