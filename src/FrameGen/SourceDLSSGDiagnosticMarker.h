#pragma once

#include <d3d12.h>
#include <cwchar>
#include <pix.h>

namespace TheosRenderPipeline::SourceDLSSG
{
	// The Windows SDK's PIX wrapper uses its legacy Unicode annotation format.
	// Stage strings and bounded recording labels need no extra runtime/package.
	// Context capture still depends on DRED 1.2; a marker is not GPU completion.
	inline void DiagnosticMarker(ID3D12GraphicsCommandList* list, bool enabled, const wchar_t* text) noexcept
	{
		if (enabled && list && text) {
			PIXSetMarker(list, 0, text);
		}
	}
}
