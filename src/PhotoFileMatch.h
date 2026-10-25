#pragma once
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

namespace TheosRenderPipeline::PhotoFiles
{
    struct Ticket
    {
        std::uint64_t since{}; // precise Windows creation-time domain, sampled before Main::Swap
        std::atomic_bool valid{true}, claimed{};
    };
    using Request = std::shared_ptr<Ticket>;

    // Holds a completed source file open against writes through replacement.
    // Renaming is shared so ScreenshotFile's atomic replacement can proceed.
    class File
    {
    public:
        File() = default;
        File(const File&) = delete;
        File& operator=(const File&) = delete;
        File(File&& other) noexcept;
        File& operator=(File&& other) noexcept;
        ~File();
        explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }
        const std::filesystem::path& Path() const { return path_; }
        bool StillMatches() const;
    private:
        friend class Matcher;
        HANDLE handle_{INVALID_HANDLE_VALUE};
        std::filesystem::path path_;
        BY_HANDLE_FILE_INFORMATION identity_{};
        Request request_;
    };

    class Matcher
    {
    public:
        // No filesystem work here: called on the render thread at the flag.
        // Concurrent unresolved requests invalidate both; never guess ordering.
        Request Begin();
        // Worker only. A unique newly created PNG, complete and closed by its
        // writer, stable across polls. Older/reused files and ambiguity decline.
        File Find(const Request& request, const std::filesystem::path& directory,
            unsigned width, unsigned height, std::chrono::milliseconds timeout);
    private:
        std::mutex mutex_;
        std::weak_ptr<Ticket> current_;
    };
}
