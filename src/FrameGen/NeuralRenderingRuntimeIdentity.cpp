#include "NeuralRenderingRuntimeIdentity.h"
#include "NeuralRenderingRuntimeContract.h"

#include <PCH.h>

#include <bcrypt.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace TheosRenderPipeline::NeuralRenderingRuntimeIdentity
{

	namespace
	{
        std::atomic_uint64_t hashCalls{};
		std::string FileVersion(const std::filesystem::path& a_path)
		{
			DWORD ignored = 0;
			const auto size = ::GetFileVersionInfoSizeW(a_path.c_str(), &ignored);
			if (size == 0) {
				return "unavailable";
			}
			std::vector<std::byte> data(size);
			if (!::GetFileVersionInfoW(a_path.c_str(), 0, size, data.data())) {
				return "unavailable";
			}
			VS_FIXEDFILEINFO* info = nullptr;
			UINT infoSize = 0;
			if (!::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
				!info || infoSize < sizeof(VS_FIXEDFILEINFO)) {
				return "unavailable";
			}
			return std::format(
				"{}.{}.{}.{}",
				HIWORD(info->dwFileVersionMS),
				LOWORD(info->dwFileVersionMS),
				HIWORD(info->dwFileVersionLS),
				LOWORD(info->dwFileVersionLS));
		}

		std::string Sha256(HANDLE file)
		{
            ++hashCalls;
			BCRYPT_ALG_HANDLE algorithm = nullptr;
			BCRYPT_HASH_HANDLE hash = nullptr;
			std::vector<std::byte> hashObject;
			std::vector<UCHAR> digest;
			auto cleanup = [&]() {
				if (hash) {
					::BCryptDestroyHash(hash);
				}
				if (algorithm) {
					::BCryptCloseAlgorithmProvider(algorithm, 0);
				}
			};

			if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
					&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
				return {};
			}
			DWORD objectSize = 0;
			DWORD digestSize = 0;
			DWORD returned = 0;
			if (!BCRYPT_SUCCESS(::BCryptGetProperty(
					algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
					sizeof(objectSize), &returned, 0)) ||
				!BCRYPT_SUCCESS(::BCryptGetProperty(
					algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestSize),
					sizeof(digestSize), &returned, 0))) {
				cleanup();
				return {};
			}
			hashObject.resize(objectSize);
			digest.resize(digestSize);
			if (!BCRYPT_SUCCESS(::BCryptCreateHash(
					algorithm, &hash, reinterpret_cast<PUCHAR>(hashObject.data()), objectSize,
					nullptr, 0, 0))) {
				cleanup();
				return {};
			}

            std::vector<UCHAR> buffer(1024 * 1024);
            DWORD count{};
            bool read{};
            do {
                read = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) != 0;
                if (!read || (count && !BCRYPT_SUCCESS(::BCryptHashData(hash, buffer.data(), count, 0)))) {
                    cleanup(); return {};
                }
            } while (count);
            if (!BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest.data(), digestSize, 0))) {
                cleanup(); return {};
            }
			cleanup();

			std::string result;
			result.reserve(digest.size() * 2);
			static constexpr char hex[] = "0123456789ABCDEF";
			for (const auto byte : digest) {
				result.push_back(hex[byte >> 4]);
				result.push_back(hex[byte & 0x0F]);
			}
			return result;
		}
	}

	Snapshot VerifyExpected(
		const std::filesystem::path& a_path,
		std::uint64_t a_expectedSize,
		std::string_view a_expectedSha256, bool fresh)
	{
		Snapshot result{};
		result.path = Normalize(a_path);
        // Open once and deny writes/replacement through identity, hash and
        // publication of the cache entry. Path metadata alone misses a replaced
        // file (and TOCTOU between stat and hashing).
        struct OpenFile {
            HANDLE value{INVALID_HANDLE_VALUE};
            ~OpenFile() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
        } file{CreateFileW(result.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) { return result; }
        BY_HANDLE_FILE_INFORMATION info{};
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandle(file.value, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic))) { return result; }
        result.present = true;
        result.size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        struct Identity {
            DWORD volume{}, high{}, low{};
            std::uint64_t size{};
            LONGLONG written{}, changed{};
            bool operator==(const Identity&) const = default;
        };
        const Identity identity{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
            result.size, basic.LastWriteTime.QuadPart, basic.ChangeTime.QuadPart};
        struct Cached { Identity identity; std::string version, sha256; };
        static std::mutex cacheMutex;
        static std::unordered_map<std::wstring, Cached> cache;
        const auto key = result.path.wstring();
        std::scoped_lock lock(cacheMutex);
        const auto found = cache.find(key);
        if (!fresh && found != cache.end() && found->second.identity == identity) {
            result.version = found->second.version; result.sha256 = found->second.sha256;
        } else {
            result.version = FileVersion(result.path);
            result.sha256 = Sha256(file.value);
            if (!result.sha256.empty()) { cache[key] = {identity, result.version, result.sha256}; }
        }
        result.matched = result.size == a_expectedSize && result.sha256 == a_expectedSha256;
        return result;
	}

    std::uint64_t HashInvocations() { return hashCalls.load(); }

	Snapshot Verify(const std::filesystem::path& a_path)
	{
		return VerifyExpected(a_path, kExpectedRuntimeSize, kExpectedRuntimeSha256);
	}
	Snapshot VerifySource(const std::filesystem::path& a_path, bool a_allowReconstruction)
	{
		static_assert(kExpectedRuntimeSize == NeuralRendering::kRuntimeSize);
		static_assert(kExpectedRuntimeSha256 == NeuralRendering::kLegacyRuntimeSha256);
		auto result = Verify(a_path); // Hash once, before loading any module.
		result.matched = result.present && NeuralRendering::MatchRuntime(result.size, result.sha256,
			result.version, a_allowReconstruction) != NeuralRendering::RuntimeBuild::Unknown;
		return result;
	}
}
