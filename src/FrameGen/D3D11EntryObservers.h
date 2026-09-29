#pragma once

#include <d3d11.h>
#include <cstddef>

namespace TheosRenderPipeline::D3D11EntryObservers
{
    using Dispatch = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
    using Copy = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
    // Observers run before forwarding. Dispatch receives the relocated original
    // for a private replay. Nested calls (including replay) are not re-observed.
    using DispatchObserver = void (*)(ID3D11DeviceContext*, UINT, UINT, UINT, Dispatch);
    using CopyObserver = void (*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);

    // Install at an engine boundary, never inside an observer. Entries and their
    // callbacks remain resident for the process lifetime. Repeated installation
    // of the same entry/callback is a no-op; an incompatible owner is rejected.
    // D3D11 can replace a context's table during Flush, queries or state swaps;
    // patch callable entries instead of writing into that mutable table.
    bool Ensure(ID3D11DeviceContext* context, DispatchObserver dispatch, CopyObserver copy);
    std::size_t DispatchEntries();
    std::size_t CopyEntries();
}
