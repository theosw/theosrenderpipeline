// Checks the hand-written context vtable slots against the SDK's C layout.
#define CINTERFACE
#define D3D11_NO_HELPERS
#include <d3d11_1.h>

#include "D3D11ContextSlots.h"

#include <cstddef>
#include <cstdio>

namespace Slots = TheosRenderPipeline::D3D11ContextSlots;

#define SLOT(member) (offsetof(ID3D11DeviceContext1Vtbl, member) / sizeof(void*))

static_assert(SLOT(PSSetShaderResources) == Slots::kPSSetShaderResources);
static_assert(SLOT(DrawIndexed) == Slots::kDrawIndexed);
static_assert(SLOT(Draw) == Slots::kDraw);
static_assert(SLOT(OMSetRenderTargets) == Slots::kOMSetRenderTargets);
static_assert(SLOT(OMSetBlendState) == Slots::kOMSetBlendState);
static_assert(SLOT(RSSetViewports) == Slots::kRSSetViewports);
static_assert(SLOT(RSSetScissorRects) == Slots::kRSSetScissorRects);
static_assert(SLOT(CopySubresourceRegion) == Slots::kCopySubresourceRegion);
static_assert(SLOT(ClearView) == Slots::kClearView);
static_assert(offsetof(ID3D11DeviceContext1Vtbl, CopySubresourceRegion) ==
    offsetof(ID3D11DeviceContextVtbl, CopySubresourceRegion));

int main()
{
    std::puts("D3D11 context vtable slots match the SDK layout");
    return 0;
}
