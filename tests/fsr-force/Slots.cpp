// Validate the small set of COM ABI slots used by the fixture observer against
// the installed SDK's C interface, including aggregate-return ABI conditionals.
#define CINTERFACE
#include <d3d12.h>
#include <cstddef>
static_assert(offsetof(ID3D12Device2Vtbl, CreateComputePipelineState) / sizeof(void*) == 11);
static_assert(offsetof(ID3D12Device2Vtbl, CreatePipelineLibrary) / sizeof(void*) == 44);
static_assert(offsetof(ID3D12Device2Vtbl, CreatePipelineState) / sizeof(void*) == 47);
static_assert(offsetof(ID3D12GraphicsCommandListVtbl, Dispatch) / sizeof(void*) == 14);
static_assert(offsetof(ID3D12GraphicsCommandListVtbl, SetPipelineState) / sizeof(void*) == 25);
static_assert(offsetof(ID3D12GraphicsCommandListVtbl, ExecuteBundle) / sizeof(void*) == 27);
static_assert(offsetof(ID3D12GraphicsCommandListVtbl, ExecuteIndirect) / sizeof(void*) == 59);
static_assert(offsetof(ID3D12CommandQueueVtbl, ExecuteCommandLists) / sizeof(void*) == 10);
