#pragma once

#include <ranges>

namespace TheosRenderPipeline::SourceDLSSG
{
    template<class Entry, class Pointer>
    struct OwnedCameraView
    {
        const Entry* entry{};
        Pointer camera;  // Keep the matched scene object alive through capture.
        bool ambiguous{};
    };

    // Cache camera pointers are borrowed identities, not ownership. Never walk
    // their parent chains: a retired view can remain in the graphics cache.
    // Start at the retained player root and follow owning child references on
    // the render thread. NiCamera is a leaf; nodes only supply descendants.
    template<class Pointer, class Entries, class IsCamera>
    auto SelectOwnedCameraView(Pointer root, const Entries& entries, bool jittered, IsCamera isCamera)
    {
        OwnedCameraView<std::ranges::range_value_t<Entries>, Pointer> result;
        auto visit = [&](auto&& self, Pointer object, unsigned depth) -> void {
            if (!object || result.ambiguous) { return; }
            if (auto* node = object->AsNode()) {
                // Preserve the previous eight-level player-root boundary.
                if (depth == 8) { return; }
                for (const auto& child : node->GetChildren()) {
                    self(self, Pointer(child), depth + 1);
                    if (result.ambiguous) { return; }
                }
                return;
            }
            for (const auto& entry : entries) {
                if (entry.pReferenceCamera != object.get() || entry.UseJitter != jittered) { continue; }
                // A retired camera's address may have been reused by another
                // scene object. Type-check only the retained live object.
                if (!isCamera(object.get())) { continue; }
                if (result.entry) {
                    result = {};
                    result.ambiguous = true;
                    return;
                }
                result.entry = &entry;
                result.camera = object;
            }
        };
        visit(visit, root, 0);
        return result;
    }
}
