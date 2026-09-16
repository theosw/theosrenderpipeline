#include "NeuralRenderingRuntimeIdentity.h"
#include "NeuralRenderingRuntimeContract.h"

#include <PCH.h>

#include <bcrypt.h>

#include <fstream>
#include <vector>

namespace TheosRenderPipeline::NeuralRenderingRuntimeIdentity
{

	namespace
	{
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

		std::string Sha256(const std::filesystem::path& a_path)
		{
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

			std::ifstream stream(a_path, std::ios::binary);
			std::vector<char> buffer(1024 * 1024);
			while (stream) {
				stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const auto count = stream.gcount();
				if (count > 0 && !BCRYPT_SUCCESS(::BCryptHashData(
						hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0))) {
					cleanup();
					return {};
				}
			}
			if (!stream.eof() || !BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest.data(), digestSize, 0))) {
				cleanup();
				return {};
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
		std::string_view a_expectedSha256)
	{
		Snapshot result{};
		result.path = Normalize(a_path);
		std::error_code error;
		result.present = std::filesystem::is_regular_file(result.path, error) && !error;
		if (!result.present) {
			return result;
		}
		result.size = std::filesystem::file_size(result.path, error);
		result.version = FileVersion(result.path);
		result.sha256 = Sha256(result.path);
		result.matched = !error && result.size == a_expectedSize &&
			result.sha256 == a_expectedSha256;
		return result;
	}

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
			a_allowReconstruction) != NeuralRendering::RuntimeBuild::Unknown;
		return result;
	}
}
