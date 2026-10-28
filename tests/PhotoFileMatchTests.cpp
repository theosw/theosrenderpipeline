#include "PhotoFileMatch.h"
#include "ScreenshotFile.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

using namespace TheosRenderPipeline;
using namespace std::chrono_literals;
static void Require(bool ok, const char* why)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s (%lu)\n", why, GetLastError()); std::exit(1); } }
static void Created(const std::filesystem::path& file, std::uint64_t time)
{
    const auto handle = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "set creation open");
    const FILETIME stamp{DWORD(time), DWORD(time >> 32)};
    Require(SetFileTime(handle, &stamp, nullptr, nullptr), "set deterministic creation time");
    CloseHandle(handle);
}
static void PNG(const std::filesystem::path& file, unsigned width = 8)
{
    Require(SUCCEEDED(ScreenshotFile::Replace(file, width, 4, std::vector<std::uint8_t>(width * 4 * 3, 80), 90)), "write PNG");
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("TRPPhotoMatch-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Require(std::filesystem::create_directory(root), "independent fixture directory");
    auto directory = [&](const char* name) { auto path = root / name; std::filesystem::create_directory(path); return path; };
    {
        PhotoFiles::Matcher matcher;
        const auto dir = directory("old"); auto request = matcher.Begin();
        PNG(dir / "recent.png"); Created(dir / "recent.png", request->since - 1);
        Require(!matcher.Find(request, dir, 8, 4, 0ms), "recent preexisting PNG is never selected, even with matching native dimensions");
        // Changing an old file's content/mtime must not admit it as a new photo.
        Require(!matcher.Find(request, dir, 8, 4, 100ms), "modified old file remains excluded by creation identity");
    }
    {
        PhotoFiles::Matcher matcher;
        const auto dir = directory("unique"); auto request = matcher.Begin();
        const auto path = dir / "photo.PNG"; PNG(path); Created(path, request->since + 1);
        auto match = matcher.Find(request, dir, 8, 4, 300ms);
        Require(match && match.Path() == path && match.StillMatches(), "one complete new PNG matches case-insensitively");
        Require(!matcher.Find(request, dir, 8, 4, 0ms), "one request can claim a file only once");
        const auto replaced = ScreenshotFile::Replace(path, 8, 4, std::vector<std::uint8_t>(8 * 4 * 3, 120), 90,
            [&] { return match.StillMatches(); });
        Require(replaced == S_OK, "native-size atomic replacement works with protected original open");
        Require(!match.StillMatches(), "replaced file is a different identity");
    }
    {
        PhotoFiles::Matcher matcher;
        const auto dir = directory("burst"); auto request = matcher.Begin();
        for (const auto* name : {"a.png", "b.png"}) { PNG(dir / name); Created(dir / name, request->since + 1); }
        Require(!matcher.Find(request, dir, 8, 4, 300ms), "multiple new photos decline instead of choosing newest");
        auto second = matcher.Begin();
        Require(!request->valid && !second->valid, "overlapping unresolved requests invalidate both");
        Require(!matcher.Find(second, dir, 8, 4, 0ms), "overlap cannot be assigned by directory order");
    }
    {
        PhotoFiles::Matcher matcher;
        const auto dir = directory("writer"); auto request = matcher.Begin(); const auto path = dir / "photo.png";
        PNG(path); Created(path, request->since + 1);
        const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        Require(writer != INVALID_HANDLE_VALUE, "writer fixture");
        Require(!matcher.Find(request, dir, 8, 4, 100ms), "writer still open blocks replacement even with complete PNG bytes");
        CloseHandle(writer);
        Require(bool(matcher.Find(request, dir, 8, 4, 300ms)), "completed closed writer becomes eligible");
    }
    {
        PhotoFiles::Matcher matcher;
        const auto dir = directory("partial"); auto request = matcher.Begin(); const auto path = dir / "photo.png";
        PNG(path); Created(path, request->since + 1);
        std::filesystem::resize_file(path, 24);
        Require(!matcher.Find(request, dir, 8, 4, 150ms), "IHDR-only file without terminal IEND is not complete");
        Require(!matcher.Find(request, root / "missing", 8, 4, 0ms), "missing directory preserves originals");
    }
    {
        PhotoFiles::Matcher matcher;
        const auto dir = directory("delayed"); auto request = matcher.Begin(); const auto path = dir / "photo.png";
        std::thread writer([&] { std::this_thread::sleep_for(100ms); PNG(path); Created(path, request->since + 1); });
        auto match = matcher.Find(request, dir, 8, 4, 1s);
        writer.join(); Require(bool(match), "delayed photo is waited for on worker");
        const auto moved = dir / "original.bin";
        Require(ScreenshotFile::Replace(path, 8, 4, std::vector<std::uint8_t>(8 * 4 * 3, 120), 90, [&] {
            std::filesystem::rename(path, moved); PNG(path); return match.StillMatches();
        }) == S_FALSE, "a destination changed during encoding is not overwritten");
        Require(!std::filesystem::exists(path.wstring() + L".trp-tmp"), "declined replacement cleans only its temporary");
    }
    {
        PhotoFiles::Matcher matcher;
        const auto dir = directory("dimensions"); auto request = matcher.Begin();
        PNG(dir / "wrong.png", 4); Created(dir / "wrong.png", request->since + 1);
        Require(!matcher.Find(request, dir, 8, 4, 100ms), "wrong dimensions are retained unchanged");
    }
    std::filesystem::remove_all(root);
    std::puts("PASS: photo identity, native dimensions, ambiguity, overlap, writer completion, delay, truncation and guarded replacement");
}
