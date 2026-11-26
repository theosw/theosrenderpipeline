#include "BottledLensPatch.h"
#include "BottledLensTarget.h"
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace TheosRenderPipeline::BottledLens
{
    namespace
    {
        // ID3D11RenderTargetView* CameraFX::GetLensTarget(const SceneTarget&, bool&) const
        using LensTarget = ID3D11RenderTargetView* (*)(void* self, void* scene, bool* uiLayer);
        std::mutex installMutex;
        std::atomic<LensTarget> original{};
        std::atomic<Provider> provider{};
        std::atomic<State> state{State::NotInstalled};
        Build build{}; // Written under installMutex before state becomes Installed.

        ID3D11RenderTargetView* Hook(void* self, void* scene, bool* uiLayer)
        {
            auto* target = original.load(std::memory_order_acquire)(self, scene, uiLayer);
            // The producer's own frame generation already chose its UI buffer.
            if (!target || !uiLayer || *uiLayer) { return target; }
            const auto ask = provider.load(std::memory_order_acquire);
            if (auto* layer = ask ? ask(target) : nullptr) { *uiLayer = true; return layer; }
            return target;
        }

        struct Section { std::size_t offset; std::span<const std::uint8_t> code; };

        bool ReadMemory(const void* address, void* destination, std::size_t size)
        {
            SIZE_T read{};
            return ReadProcessMemory(GetCurrentProcess(), address, destination, size, &read) && read == size;
        }

        bool ReadableRange(const std::uint8_t* base, std::size_t offset, std::size_t size)
        {
            std::size_t cursor = 0;
            while (cursor < size) {
                MEMORY_BASIC_INFORMATION mbi{};
                const auto* address = base + offset + cursor;
                if (!VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.AllocationBase != base ||
                    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) { return false; }
                const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                const auto current = reinterpret_cast<std::uintptr_t>(address);
                if (regionEnd <= current) { return false; }
                const auto step = regionEnd - current;
                cursor += step < (size - cursor) ? step : (size - cursor);
            }
            return true;
        }

        // Each executable section of a mapped image, bounded by its headers.
        bool ExecutableSections(HMODULE module, std::vector<Section>& sections, Build& identity)
        {
            const auto* base = reinterpret_cast<const std::uint8_t*>(module);
            IMAGE_DOS_HEADER dos{};
            if (!ReadMemory(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
                dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x100000) { return false; }
            IMAGE_NT_HEADERS64 nt{};
            if (!ReadMemory(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
                nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
                nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
                nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > 96) { return false; }
            const std::size_t tableOffset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(nt);
            const std::size_t tableBytes = static_cast<std::size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
            const std::size_t headerLimit = nt.OptionalHeader.SizeOfHeaders ? nt.OptionalHeader.SizeOfHeaders : nt.OptionalHeader.SizeOfImage;
            if (tableOffset >= headerLimit || tableBytes > headerLimit - tableOffset) { return false; }
            std::vector<IMAGE_SECTION_HEADER> headers(nt.FileHeader.NumberOfSections);
            if (!ReadMemory(base + tableOffset, headers.data(), tableBytes)) { return false; }
            identity.timestamp = nt.FileHeader.TimeDateStamp;
            identity.imageSize = nt.OptionalHeader.SizeOfImage;
            identity.checked = identity.timestamp == 0x6AFF0971 && identity.imageSize == 0x2AF6000;
            for (const auto& section : headers) {
                if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                    !(section.Characteristics & IMAGE_SCN_MEM_READ)) { continue; }
                const std::size_t size = section.Misc.VirtualSize;
                if (!size || section.VirtualAddress > nt.OptionalHeader.SizeOfImage ||
                    size > nt.OptionalHeader.SizeOfImage - section.VirtualAddress ||
                    !ReadableRange(base, section.VirtualAddress, size)) { return false; }
                sections.push_back({section.VirtualAddress, std::span<const std::uint8_t>(base + section.VirtualAddress, size)});
            }
            return true;
        }
    }

    State Install(HMODULE module, Provider a_provider)
    {
        std::lock_guard lock(installMutex);
        if (state.load() == State::Installed) {
            if (a_provider) { provider.store(a_provider, std::memory_order_release); }
            return State::Installed;
        }
        if (!module || !a_provider) { return state = State::NoModule; }
        std::vector<Section> sections;
        Build identity;
        if (!ExecutableSections(module, sections, identity)) { return state = State::NotFound; }
        std::size_t matches{}, function{};
        for (const auto& section : sections) {
            const auto search = Find(section.code);
            if (search.matches && !matches) { function = section.offset + search.offset; }
            matches += search.matches;
        }
        if (!matches) { return state = State::NotFound; }
        if (matches != 1) { return state = State::Ambiguous; }
        References references;
        for (const auto& section : sections) {
            const auto found = FindReferences(section.code, section.offset, function);
            references.calls += found.calls; references.jumps += found.jumps;
        }
        if (references.calls != 1 || references.jumps) { return state = State::Uncalled; }
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) { return state = State::HookFailed; }
        void* trampoline{};
        auto* target = reinterpret_cast<std::uint8_t*>(module) + function;
        if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook), &trampoline) != MH_OK) { return state = State::HookFailed; }
        // Publish both before enabling: the render thread can enter at once.
        provider.store(a_provider, std::memory_order_release);
        original.store(reinterpret_cast<LensTarget>(trampoline), std::memory_order_release);
        if (MH_EnableHook(target) != MH_OK) {
            // Never entered: take the entry back out so a later attempt can create it again.
            MH_RemoveHook(target);
            original.store(nullptr, std::memory_order_release);
            return state = State::HookFailed;
        }
        build = identity;
        return state = State::Installed;
    }

    State Installed() { return state.load(std::memory_order_acquire); }

    Build InstalledBuild()
    {
        std::lock_guard lock(installMutex);
        return state.load() == State::Installed ? build : Build{};
    }

    const char* Describe(State a_state)
    {
        switch (a_state) {
        case State::Installed: return "installed";
        case State::NoModule: return "Community Shaders module unavailable";
        case State::NotFound: return "the lens target function was not found in this Community Shaders build";
        case State::Ambiguous: return "the lens target function matched more than once in this Community Shaders build";
        case State::Uncalled: return "the lens target function does not have exactly one direct call, or something jumps to it, in this Community Shaders build";
        case State::HookFailed: return "the redirect could not be installed";
        default: return "not installed";
        }
    }
}
