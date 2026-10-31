#pragma once
#include "NeuralRuntimeKernels.h"
#include "NeuralRuntimeKernelsInternal.h"
#include <map>
#include <optional>

namespace TheosRenderPipeline::NeuralRendering::KernelPacks
{
    struct Installation
    {
        void* storage{};
        std::size_t storageBytes{};
        bool research{};
        std::string digest;
        LaunchRequirements launches;
        std::vector<Patch> plan;
        RuntimeKernelResult result;
    };

    // Serialized by the installer. Entries outlive every caller: pinning prevents
    // HMODULE reuse, including after a clean rollback has freed pack storage.
    class InstallationRegistry
    {
    public:
        Installation* Find(HMODULE module)
        {
            const auto it = entries_.find(module);
            return it == entries_.end() ? nullptr : &it->second;
        }
        template<class Validate, class Prepare>
        RuntimeKernelResult Apply(HMODULE module, bool newlyLoaded, Validate validate, Prepare prepare)
        {
            auto* retained = Find(module);
            if (retained) {
                if (retained->result.unsafe) { return retained->result; }
                if (auto refusal = validate(*retained)) { return *refusal; }
                if (retained->result.applied || retained->result.unsafe) { return retained->result; }
                // Clean refusal owns a pinned, uninitialized image. Revalidate
                // original bytes and reacquire storage through the full preparation.
            } else if (!module || !newlyLoaded) {
                return {false, false, "NR installation needs exclusive ownership of a fresh runtime; restart required"};
            }
            return prepare(retained);
        }
        template<class Pin>
        Installation* Retain(HMODULE module, Installation pending, Pin pin)
        {
            if (auto* retained = Find(module)) {
                *retained = std::move(pending);
                return retained; // already pinned; never erase a refused entry
            }
            auto [entry, inserted] = entries_.emplace(module, std::move(pending));
            if (!pin()) { entries_.erase(entry); return nullptr; }
            return &entry->second;
        }
    private:
        std::map<HMODULE, Installation> entries_;
    };
}
