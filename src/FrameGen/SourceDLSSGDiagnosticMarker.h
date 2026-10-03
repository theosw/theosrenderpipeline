#pragma once

#include <d3d12.h>
#include <cwchar>

namespace TheosRenderPipeline::SourceDLSSG
{
	// D3D12's legacy Unicode annotation format (metadata 0). Fixed strings,
	// including their terminator, avoid a PIX runtime/package dependency.
	// Context capture still depends on DRED 1.2; a marker is not GPU completion.
	inline void DiagnosticMarker(ID3D12GraphicsCommandList* list, bool enabled, const wchar_t* text) noexcept
	{
		if (enabled && list && text) {
			list->SetMarker(0, text, static_cast<UINT>((std::wcslen(text) + 1) * sizeof(wchar_t)));
		}
	}
}
