#include "FSR4Compatibility.h"
#if !defined(TRP_FSR4_FORCE_UNIVERSAL)
#error FSR4 compatibility must only be compiled in the Universal experiment
#endif
#include "../PluginPaths.h"
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>

namespace TheosRenderPipeline::FSR4Compatibility
{
    namespace
    {
        constexpr DWORD functionRva = 0x9960, functionEnd = 0x9a65, patchRva = 0x99b5;
        constexpr DWORD providerVtableRva = 0x3635260, modelVtableRva = 0x36352a8;
        constexpr std::uintptr_t modelOffset = 0x28, builtFlagOffset = 0x214;
        constexpr unsigned char original[]{0x48, 0x8d, 0x54, 0x24, 0x20};
        constexpr unsigned char replacement[]{0xe9, 0x96, 0, 0, 0};
        constexpr char functionHash[] = "b1dcfd59cfa43265fb7e6984f7499755a2f9cbb5147e0480200ec27090ff5c44";
        constexpr const wchar_t* names[]{L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll"};
        constexpr const char* hashes[]{
            "e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb",
            "aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace"};

        struct ScopedHandle
        {
            HANDLE value{INVALID_HANDLE_VALUE};
            explicit ScopedHandle(HANDLE h) : value(h) {}
            ~ScopedHandle() { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
            ScopedHandle(const ScopedHandle&) = delete;
            ScopedHandle& operator=(const ScopedHandle&) = delete;
            bool Valid() const { return value && value != INVALID_HANDLE_VALUE; }
        };

        void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
        std::wstring FinalPath(HANDLE file)
        {
            std::wstring path(32768, L'\0');
            const DWORD n = GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
            Require(n && n < path.size(), "cannot resolve runtime file path"); path.resize(n); return path;
        }
        bool EqualPath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
        void Signature(HANDLE file, const std::filesystem::path& path)
        {
            WINTRUST_FILE_INFO info{sizeof(info)}; info.pcwszFilePath = path.c_str(); info.hFile = file;
            WINTRUST_DATA data{sizeof(data)}; data.dwUIChoice = WTD_UI_NONE;
            data.fdwRevocationChecks = WTD_REVOKE_NONE; data.dwUnionChoice = WTD_CHOICE_FILE;
            data.pFile = &info; data.dwStateAction = WTD_STATEACTION_VERIFY;
            data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
            GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
            const LONG result = WinVerifyTrust(nullptr, &policy, &data);
            bool amd = false;
            if (result == ERROR_SUCCESS) {
                const auto provider = WTHelperProvDataFromStateData(data.hWVTStateData);
                const auto signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
                if (signer && signer->csCertChain) {
                    wchar_t subject[512]{};
                    CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subject, 512);
                    amd = std::wstring(subject) == L"Advanced Micro Devices";
                }
            }
            data.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &policy, &data);
            Require(result == ERROR_SUCCESS, "AMD Authenticode verification failed (cached trust)");
            Require(amd, "Authenticode signer is not Advanced Micro Devices");
        }
        void Modules(HMODULE loader = nullptr, HMODULE upscaler = nullptr)
        {
            ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId()));
            Require(snapshot.Valid(), "cannot enumerate process modules");
            MODULEENTRY32W entry{sizeof(entry)}; bool competing = false;
            const BOOL first = Module32FirstW(snapshot.value, &entry);
            if (first) do {
                std::wstring name(entry.szModule);
                std::transform(name.begin(), name.end(), name.begin(), towlower);
                if ((name.starts_with(L"amd_fidelityfx") || name == L"amdxcffx64.dll") &&
                    entry.hModule != loader && entry.hModule != upscaler) competing = true;
            } while (Module32NextW(snapshot.value, &entry));
            const DWORD error = GetLastError();
            Require(first && error == ERROR_NO_MORE_FILES, "incomplete module enumeration");
            Require(!competing, "competing preloaded AMD module; require a fresh process");
        }
        struct Image
        {
            std::byte* base; DWORD size{}; std::vector<IMAGE_SECTION_HEADER> sections;
            explicit Image(HMODULE module) : base(reinterpret_cast<std::byte*>(module))
            {
                IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
                Require(ReadMemory(base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
                    dos.e_lfanew >= sizeof(dos) && dos.e_lfanew <= 0x100000, "invalid DOS/PE header bounds");
                Require(ReadMemory(base + dos.e_lfanew, &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE &&
                    nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                    nt.FileHeader.SizeOfOptionalHeader == sizeof(IMAGE_OPTIONAL_HEADER64), "invalid x64 PE identity");
                size = nt.OptionalHeader.SizeOfImage;
                Require(size == 0x36ff000 && nt.FileHeader.NumberOfSections > 0 && nt.FileHeader.NumberOfSections <= 96,
                    "unexpected PE image size/sections");
                const size_t offset = dos.e_lfanew + sizeof(nt);
                Require(offset < nt.OptionalHeader.SizeOfHeaders &&
                    nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) <= nt.OptionalHeader.SizeOfHeaders - offset,
                    "section table outside PE headers");
                sections.resize(nt.FileHeader.NumberOfSections);
                Require(ReadMemory(base + offset, sections.data(), sections.size() * sizeof(sections[0])), "unreadable PE sections");
                for (const auto& s : sections) Require(s.VirtualAddress < size && s.Misc.VirtualSize <= size - s.VirtualAddress,
                    "section outside PE image");
            }
            void Check(DWORD rva, size_t count, bool executable = false) const
            {
                Require(rva < size && count <= size - rva, "RVA outside PE image");
                bool valid = false;
                for (const auto& s : sections) if (rva >= s.VirtualAddress && rva - s.VirtualAddress < s.Misc.VirtualSize &&
                    count <= s.Misc.VirtualSize - (rva - s.VirtualAddress) && (s.Characteristics & IMAGE_SCN_MEM_READ) &&
                    (!executable || (s.Characteristics & IMAGE_SCN_MEM_EXECUTE))) valid = true;
                Require(valid, "RVA not contained in required PE section");
                size_t cursor = 0;
                while (cursor < count) {
                    MEMORY_BASIC_INFORMATION memory{};
                    const auto* address = base + rva + cursor;
                    Require(VirtualQuery(address, &memory, sizeof(memory)) && memory.AllocationBase == base &&
                        memory.Type == MEM_IMAGE && memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)),
                        "RVA not owned by loaded PE image");
                    const auto regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
                    const auto current = reinterpret_cast<uintptr_t>(address);
                    Require(regionEnd > current, "RVA virtual memory region invalid");
                    cursor += (std::min)(count - cursor, static_cast<size_t>(regionEnd - current));
                }
            }
            template<class T> T At(DWORD rva) const
            {
                Check(rva, sizeof(T)); T value{};
                Require(ReadMemory(base + rva, &value, sizeof(value)), "unreadable image data"); return value;
            }
            DWORD Rva(uintptr_t pointer) const
            {
                const auto start = reinterpret_cast<uintptr_t>(base);
                Require(pointer >= start && pointer - start < size, "pointer outside verified module"); return DWORD(pointer - start);
            }
            void RTTI(DWORD vtable, const char* name, DWORD method) const
            {
                Check(vtable - 8, 32);
                Require(Rva(At<uintptr_t>(vtable + 16)) == method, "unexpected vtable method"); Check(method, 1, true);
                const DWORD locator = Rva(At<uintptr_t>(vtable - 8)); Check(locator, 24);
                Require(At<DWORD>(locator) == 1 && At<DWORD>(locator + 4) == 0 && At<DWORD>(locator + 20) == locator,
                    "unexpected RTTI complete object locator");
                const DWORD type = At<DWORD>(locator + 12);
                const size_t nameBytes = strlen(name) + 1;
                Check(type, 16 + nameBytes);
                std::vector<char> actual(nameBytes);
                Require(ReadMemory(base + type + 16, actual.data(), nameBytes) &&
                    memcmp(actual.data(), name, nameBytes) == 0, "unexpected RTTI type identity");
            }
        };
    }
    bool ReadMemory(const void* address, void* destination, size_t size)
    {
        SIZE_T read{}; return ReadProcessMemory(GetCurrentProcess(), address, destination, size, &read) && read == size;
    }
    std::string VerifyForcedModel(const void* ffxContext, HMODULE upscaler)
    {
        // Exact-build identity (pinned hash only): AMD's internal context holds
        // ffxProvider_FSR4_Int8 first, the provider holds Fsr4Int8UpscalerModel
        // at +0x28, and the context byte at +0x214 is set once a dispatch builds
        // the model. Read-only; never touches weights.
        const auto base = reinterpret_cast<std::uintptr_t>(upscaler);
        if (!base || !ffxContext) { return "no runtime or context"; }
        auto read = [](std::uintptr_t address, auto& value) {
            return address && ReadMemory(reinterpret_cast<const void*>(address), &value, sizeof(value));
        };
        const auto handle = reinterpret_cast<std::uintptr_t>(ffxContext);
        for (std::uintptr_t offset = 0; offset < 0x40; offset += sizeof(std::uintptr_t)) {
            std::uintptr_t candidate{}, provider{}, providerVtable{}, model{}, modelVtable{};
            unsigned char built{};
            if (!read(handle + offset, candidate) || !read(candidate, provider) || !read(provider, providerVtable) ||
                providerVtable != base + providerVtableRva) { continue; }
            if (!read(provider + modelOffset, model) || !read(model, modelVtable) || modelVtable != base + modelVtableRva) {
                return std::format("wrapper+{:#x}: INT8 provider without the INT8 model", offset);
            }
            if (!read(candidate + builtFlagOffset, built) || !built) {
                return std::format("wrapper+{:#x}: INT8 model not built yet", offset);
            }
            return {};
        }
        return "no ffxProvider_FSR4_Int8 context behind the handle";
    }
    std::string Sha256(const void* data, size_t size)
    {
        BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
        Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 provider failed");
        unsigned char digest[32]{};
        NTSTATUS status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
        const auto* p = static_cast<const unsigned char*>(data);
        while (status >= 0 && size) {
            const ULONG n = static_cast<ULONG>(std::min<size_t>(size, 1u << 20));
            status = BCryptHashData(hash, const_cast<PUCHAR>(p), n, 0); p += n; size -= n;
        }
        if (status >= 0) status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
        if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
        Require(status >= 0, "SHA256 computation failed");
        std::string result; constexpr char hex[] = "0123456789abcdef";
        for (auto b : digest) { result += hex[b >> 4]; result += hex[b & 15]; } return result;
    }
    void Session::CloseFiles()
    {
        for (auto& f : files_) {
            if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); f = INVALID_HANDLE_VALUE; }
        }
    }
    Session::~Session() { CloseFiles(); }
    bool Session::Refuse(const std::string& reason) { CloseFiles(); reason_ = reason; state_ = State::Refused; return false; }
    bool Session::LoadVerified(const std::filesystem::path& directory)
    {
        if (state_ != State::Fresh) return Refuse("session already used; require a fresh process");
        try {
            Require(directory.is_absolute(), "AMD directory must be absolute"); Modules();
            std::array<std::wstring, 2> paths;
            for (size_t i = 0; i < 2; ++i) {
                const auto path = directory / names[i];
                files_[i] = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                Require(files_[i] != INVALID_HANDLE_VALUE, "cannot lock runtime file against write/replacement");
                paths[i] = FinalPath(files_[i]); LARGE_INTEGER length{};
                Require(GetFileSizeEx(files_[i], &length) && length.QuadPart > 0 && length.QuadPart <= 100 * 1024 * 1024,
                    "runtime file size outside bounds");
                std::vector<unsigned char> data(static_cast<size_t>(length.QuadPart)); DWORD read{};
                Require(ReadFile(files_[i], data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size(), "runtime read failed");
                const auto hash = Sha256(data.data(), data.size());
                audit_ += PluginPaths::ToUtf8(path) + " sha256=" + hash + "\n";
                Require(hash == hashes[i], "runtime SHA-256 mismatch; force supports exact SDK 2.4 files only");
                Signature(files_[i], path); audit_ += "AMD Authenticode valid (cached trust; online revocation not requested)\n";
            }
            Modules();
            loader_ = LoadLibraryExW((directory / names[0]).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            Require(loader_ != nullptr, "verified loader failed to load");
            upscaler_ = LoadLibraryExW((directory / names[1]).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            Require(upscaler_ != nullptr, "verified sibling upscaler failed to load");
            for (size_t i = 0; i < 2; ++i) {
                const HMODULE module = i ? upscaler_ : loader_; wchar_t loaded[32768]{};
                const auto n = GetModuleFileNameW(module, loaded, 32768);
                Require(n && n < 32768, "cannot resolve loaded module path");
                ScopedHandle file(CreateFileW(loaded, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                Require(file.Valid(), "cannot verify loaded module file");
                const auto actual = FinalPath(file.value);
                Require(EqualPath(actual, paths[i]), "loaded module path differs from verified input");
                HMODULE resident{};
                Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(module), &resident) && resident == module, "cannot retain verified module");
                audit_ += PluginPaths::ToUtf8(std::filesystem::path(loaded)) + " loaded and pinned\n";
            }
            Modules(loader_, upscaler_); state_ = State::Loaded; reason_ = "verified runtime loaded; no query made"; return true;
        } catch (const std::exception& e) { return Refuse(e.what()); }
    }
    bool Session::ApplyBeforeFirstQuery(ID3D12Device* device, LUID intendedAdapter, bool explicitlyRequested)
    {
        if (state_ != State::Loaded) return Refuse("patch requires one freshly verified runtime load");
        try {
            Require(explicitlyRequested, "force not explicitly requested"); Require(device, "real D3D12 device required");
            const auto luid = device->GetAdapterLuid();
            Require(luid.HighPart == intendedAdapter.HighPart && luid.LowPart == intendedAdapter.LowPart, "device differs from intended real adapter");
            D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
            Require(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) && sm.HighestShaderModel >= D3D_SHADER_MODEL_6_6,
                "real Shader Model 6.6 unavailable");
            D3D12_FEATURE_DATA_D3D12_OPTIONS4 options{};
            Require(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options, sizeof(options))) && options.Native16BitShaderOpsSupported,
                "real native 16-bit shader operations unavailable; selected shaders not qualified");
            Modules(loader_, upscaler_); Image image(upscaler_); image.Check(functionRva, functionEnd - functionRva, true);
            std::array<unsigned char, functionEnd - functionRva> bytes{};
            Require(ReadMemory(image.base + functionRva, bytes.data(), bytes.size()), "cannot read full admission function");
            const auto hash = Sha256(bytes.data(), bytes.size()); audit_ += "function preimage sha256=" + hash + "\n";
            Require(hash == functionHash, "full admission function preimage mismatch; no patch written");
            image.RTTI(providerVtableRva, ".?AVffxProvider_FSR4_Int8@@", functionRva);
            image.RTTI(modelVtableRva, ".?AVFsr4Int8UpscalerModel@@", 0x98b0);
            auto* target = image.base + patchRva;
            unsigned char before[sizeof(original)]{};
            Require(ReadMemory(target, before, sizeof(before)) && memcmp(before, original, sizeof(original)) == 0,
                "patch instruction preimage mismatch");
            image.Check(0x9a50, 21, true);
            DWORD old{}; Require(VirtualProtect(target, sizeof(original), PAGE_EXECUTE_READWRITE, &old), "patch VirtualProtect failed; no write");
            // No SDK calls/threads exist in this session. No trampoline, query spoof,
            // model/preset change or file mutation. Never expose a partial success.
            memcpy(target, replacement, sizeof(replacement));
            DWORD ignored{}; const bool restored = VirtualProtect(target, sizeof(original), old, &ignored) != FALSE;
            const bool flushed = FlushInstructionCache(GetCurrentProcess(), target, sizeof(original)) != FALSE;
            unsigned char after[5]{};
            const bool verified = ReadMemory(target, after, sizeof(after)) && memcmp(after, replacement, sizeof(after)) == 0;
            if (!restored || !flushed || !verified) {
                DWORD rollback{};
                if (VirtualProtect(target, sizeof(original), PAGE_EXECUTE_READWRITE, &rollback)) {
                    memcpy(target, original, sizeof(original)); VirtualProtect(target, sizeof(original), old, &ignored);
                    FlushInstructionCache(GetCurrentProcess(), target, sizeof(original));
                }
                CloseFiles();
                state_ = State::Poisoned; reason_ = "patch transaction uncertain; rollback attempted; SDK use prohibited until process exit"; return false;
            }
            audit_ += "RTTI/vtables/PE validated; 0x99b5: 488d542420 -> e996000000; protection restored, cache flushed, readback exact\n";
            state_ = State::Patched; reason_ = "full INT8 admission patched; execution UNVERIFIED"; return true;
        } catch (const std::exception& e) { return Refuse(e.what()); }
    }
}
