#pragma once
#include <mutex>
#include <set>

namespace TheosRenderPipeline::NeuralRendering
{
    // A rejected CU chain invalidates every command on its D3D12 list: earlier
    // kernels may wait for producers in the dropped chain. Reset/discard only.
    class RejectedNeuralLists
    {
        std::mutex mutex;
        std::set<const void*> lists;
    public:
        static RejectedNeuralLists& Get() { static auto* state = new RejectedNeuralLists; return *state; }
        void Reject(const void* list) { std::scoped_lock lock(mutex); lists.insert(list); }
        void Discard(const void* list) { std::scoped_lock lock(mutex); lists.erase(list); }
        bool Contains(const void* list) { std::scoped_lock lock(mutex); return lists.contains(list); }
    };

    template<class Execute> bool ExecuteHealthyNeuralList(const void* list, Execute execute)
    {
        if (RejectedNeuralLists::Get().Contains(list)) { return false; }
        execute();
        return true;
    }
}
