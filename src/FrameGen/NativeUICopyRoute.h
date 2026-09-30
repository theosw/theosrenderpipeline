#pragma once

#include <d3d11.h>

#include <algorithm>
#include <span>

namespace TheosRenderPipeline::NativeUICopyRoute
{
    // UI producers can copy directly between their own textures and the
    // game-facing surface, bypassing the render-target redirection. TRP reports
    // native screen dimensions while that surface keeps the render extent, so a
    // producer's native-space region can fall outside it. D3D11 validates copy
    // regions only in its debug layer; an out-of-range copy is undefined and can
    // fault the GPU. These helpers select a native UI destination for a producer
    // opted into redirection and reject regions that cannot fit.

    struct Extent
    {
        UINT width{};
        UINT height{};
        DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
        bool Valid() const { return width && height; }
    };

    struct Copy
    {
        ID3D11Resource* dst{};
        UINT dstSubresource{};
        UINT dstX{};
        UINT dstY{};
        UINT dstZ{};
        ID3D11Resource* src{};
        UINT srcSubresource{};
        const D3D11_BOX* box{};
    };

    enum class Decision { Original, Redirected, Dropped };

    // Only single-sample, single-level 2D textures are described; other
    // resources remain the runtime's responsibility.
    inline Extent TextureExtent(ID3D11Resource* resource, UINT subresource)
    {
        if (!resource || subresource != 0) { return {}; }
        D3D11_RESOURCE_DIMENSION dimension{};
        resource->GetType(&dimension);
        if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { return {}; }
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1) { return {}; }
        return { desc.Width, desc.Height, desc.Format };
    }

    inline bool RegionFits(const Extent& dst, UINT dstX, UINT dstY, UINT dstZ, const Extent& src, const D3D11_BOX* box)
    {
        if (!dst.Valid() || !src.Valid()) { return false; }
        const D3D11_BOX region = box ? *box : D3D11_BOX{ 0, 0, 0, src.width, src.height, 1 };
        // D3D11 treats an empty box as a no-op.
        if (region.left >= region.right || region.top >= region.bottom || region.front >= region.back) { return true; }
        if (region.right > src.width || region.bottom > src.height || region.front != 0 || region.back != 1 || dstZ != 0) {
            return false;
        }
        const UINT width = region.right - region.left;
        const UINT height = region.bottom - region.top;
        return dstX <= dst.width && dstY <= dst.height && width <= dst.width - dstX && height <= dst.height - dstY;
    }

    // `surfaces` are the render-extent targets a UI producer can hold. With
    // `redirect`, copies touching them use `native` instead when the result
    // is a valid same-format copy. Out-of-range copies touching a surface or the
    // native target are dropped whether or not redirection applies.
    inline Decision Route(Copy& copy, bool redirect, std::span<ID3D11Resource* const> surfaces, ID3D11Resource* native)
    {
        const auto isSurface = [&](ID3D11Resource* resource) {
            return resource && std::ranges::find(surfaces, resource) != surfaces.end();
        };
        const bool dstSurface = isSurface(copy.dst);
        const bool srcSurface = isSurface(copy.src);
        const bool touchesNative = native && (copy.dst == native || copy.src == native);
        if (!dstSurface && !srcSurface && !touchesNative) { return Decision::Original; }

        if (redirect && native && (dstSurface || srcSurface)) {
            Copy routed = copy;
            if (dstSurface) { routed.dst = native; }
            if (srcSurface) { routed.src = native; }
            const auto dst = TextureExtent(routed.dst, routed.dstSubresource);
            const auto src = TextureExtent(routed.src, routed.srcSubresource);
            if (dst.format == src.format && RegionFits(dst, routed.dstX, routed.dstY, routed.dstZ, src, routed.box)) {
                copy = routed;
                return Decision::Redirected;
            }
        }

        const auto dst = TextureExtent(copy.dst, copy.dstSubresource);
        const auto src = TextureExtent(copy.src, copy.srcSubresource);
        if (!dst.Valid() || !src.Valid()) { return Decision::Original; }
        return RegionFits(dst, copy.dstX, copy.dstY, copy.dstZ, src, copy.box) ? Decision::Original : Decision::Dropped;
    }

    // ClearView takes a view; redirect a view of a surface to the native RTV.
    inline ID3D11View* RouteClearView(ID3D11View* view, bool redirect, std::span<ID3D11Resource* const> surfaces,
        ID3D11RenderTargetView* native)
    {
        if (!redirect || !view || !native) { return view; }
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        const bool surface = resource && std::ranges::find(surfaces, resource) != surfaces.end();
        if (resource) { resource->Release(); }
        return surface ? native : view;
    }
}
