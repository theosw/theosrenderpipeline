#include "PhotoFileMatch.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <thread>
#include <utility>

namespace TheosRenderPipeline::PhotoFiles
{
    namespace
    {
        std::uint64_t Ticks(FILETIME time) { return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; }
        bool Same(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b)
        {
            return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
                a.nFileIndexLow == b.nFileIndexLow && Ticks(a.ftCreationTime) == Ticks(b.ftCreationTime) &&
                Ticks(a.ftLastWriteTime) == Ticks(b.ftLastWriteTime) &&
                a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow;
        }
        bool CompletePNG(HANDLE file, unsigned width, unsigned height)
        {
            constexpr std::array<unsigned char, 16> signature{0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0,0,0,13,'I','H','D','R'};
            constexpr std::array<unsigned char, 12> end{0,0,0,0,'I','E','N','D',0xAE,0x42,0x60,0x82};
            std::array<unsigned char, 24> head{};
            DWORD read{};
            if (!ReadFile(file, head.data(), DWORD(head.size()), &read, nullptr) || read != head.size() ||
                !std::equal(signature.begin(), signature.end(), head.begin())) { return false; }
            auto big = [&](unsigned at) { return (unsigned(head[at]) << 24) | (unsigned(head[at+1]) << 16) |
                (unsigned(head[at+2]) << 8) | head[at+3]; };
            if (big(16) != width || big(20) != height) { return false; }
            LARGE_INTEGER offset{}; offset.QuadPart = -static_cast<LONGLONG>(end.size());
            std::array<unsigned char, 12> tail{};
            return SetFilePointerEx(file, offset, nullptr, FILE_END) && ReadFile(file, tail.data(), DWORD(tail.size()), &read, nullptr) &&
                read == tail.size() && tail == end;
        }
    }

    File::File(File&& other) noexcept { *this = std::move(other); }
    File& File::operator=(File&& other) noexcept
    {
        if (this != &other) {
            if (*this) { CloseHandle(handle_); }
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
            path_ = std::move(other.path_); identity_ = other.identity_; request_ = std::move(other.request_);
        }
        return *this;
    }
    File::~File() { if (*this) { CloseHandle(handle_); } }
    bool File::StillMatches() const
    {
        if (!*this || !request_ || !request_->valid || !request_->claimed) { return false; }
        const auto file = CreateFileW(path_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) { return false; }
        BY_HANDLE_FILE_INFORMATION identity{};
        const bool same = GetFileInformationByHandle(file, &identity) && Same(identity, identity_);
        CloseHandle(file);
        return same;
    }

    Request Matcher::Begin()
    {
        auto request = std::make_shared<Ticket>();
        FILETIME now{}; GetSystemTimePreciseAsFileTime(&now); request->since = Ticks(now);
        std::lock_guard lock(mutex_);
        if (const auto previous = current_.lock(); previous && !previous->claimed) {
            previous->valid = false; request->valid = false;
        }
        current_ = request;
        return request;
    }

    File Matcher::Find(const Request& request, const std::filesystem::path& directory,
        unsigned width, unsigned height, std::chrono::milliseconds timeout)
    {
        if (!request || !request->valid || request->claimed || !request->since || !width || !height) { return {}; }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        BY_HANDLE_FILE_INFORMATION previous{};
        bool seen{};
        for (;;) {
            if (!request->valid || request->claimed) { return {}; }
            File candidate;
            unsigned fresh = 0;
            std::error_code error;
            std::filesystem::directory_iterator it(directory, error), end;
            // Missing/unreadable directory is a refusal, never a partial scan.
            if (error) { return {}; }
            for (; it != end && !error; it.increment(error)) {
                auto extension = it->path().extension().wstring();
                for (auto& c : extension) { c = static_cast<wchar_t>(std::towlower(c)); }
                if (extension != L".png") { continue; }
                const auto file = CreateFileW(it->path().c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (file == INVALID_HANDLE_VALUE) { return {}; }
                BY_HANDLE_FILE_INFORMATION info{};
                const bool known = GetFileInformationByHandle(file, &info);
                CloseHandle(file);
                if (!known) { return {}; }
                if (Ticks(info.ftCreationTime) <= request->since ||
                    (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) { continue; }
                // Count all new PNGs before dimensions/readiness filtering. A
                // still-open second photo must not make the first look unique.
                if (++fresh > 1) { return {}; }
                candidate.path_ = it->path(); candidate.identity_ = info;
            }
            if (error) { return {}; }
            if (fresh == 1) {
                // Refuse while any writer holds the file. Keep this protection
                // through encoding and atomic replacement once it is claimed.
                candidate.handle_ = CreateFileW(candidate.path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                BY_HANDLE_FILE_INFORMATION locked{};
                if (candidate && GetFileInformationByHandle(candidate.handle_, &locked) && Same(locked, candidate.identity_) &&
                    seen && Same(previous, locked) && CompletePNG(candidate.handle_, width, height)) {
                    bool unused = false;
                    if (!request->valid || !request->claimed.compare_exchange_strong(unused, true)) { return {}; }
                    candidate.request_ = request;
                    if (!candidate.StillMatches()) { return {}; }
                    return candidate;
                }
                previous = candidate.identity_; seen = true;
            } else { seen = false; }
            if (std::chrono::steady_clock::now() >= deadline) { return {}; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
