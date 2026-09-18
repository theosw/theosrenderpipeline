// Render hooks derived from PureDark's MIT-licensed Skyrim-Upscaler
// (https://github.com/PureDark/Skyrim-Upscaler).

#include "UpscalerHooks.h"
#include "UpscalerDeviceHooks.h"
#include "UpscalerSamplerHooks.h"

#include <PCH.h>
#include "CommunityShaderIntegration.h"

#include "DLSSBackend.h"
#include "DRS.h"
#include "RenderPipeline.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/LoadingArtwork.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceHostBoundary.h"
#include "FrameGen/StartupUIClipping.h"
#include "NativeInput.h"
#include "NativeInputThunks.h"
#include "OverlayUI.h"
#include "PerformanceTuning.h"
#include "SkyrimRuntime.h"

#include <d3d11.h>
#include <dxgi1_6.h>

#include <map>

decltype(&GetClientRect)                      ptrGetClientRect;
decltype(&ID3D11Device::CreateTexture2D)      ptrCreateTexture2D;
decltype(&ID3D11DeviceContext::PSSetShaderResources) ptrPSSetShaderResources;
decltype(&ID3D11DeviceContext::OMSetRenderTargets) ptrOMSetRenderTargets;
decltype(&ID3D11DeviceContext::OMSetBlendState) ptrOMSetBlendState;
decltype(&ID3D11DeviceContext::RSSetViewports) ptrRSSetViewports;
decltype(&ID3D11DeviceContext::RSSetScissorRects) ptrRSSetScissorRects;
decltype(&ID3D11DeviceContext::DrawIndexed) ptrDrawIndexed;
decltype(&ID3D11DeviceContext::Draw) ptrDraw;

namespace
{
	struct ConsoleDiagnosticCounters
	{
		std::uint64_t generation{ 0 };
		std::uint32_t renderTargets{ 0 };
		std::uint32_t viewports{ 0 };
		std::uint32_t scissors{ 0 };
	};

	ConsoleDiagnosticCounters consoleDiagnostics;

	struct Inventory3DDiagnosticState
	{
		bool active{ false };
		bool logStateEvents{ false };
		std::uint32_t call{ 0 };
		std::uint32_t event{ 0 };
	};

	struct Inventory3DDrawIsolationState
	{
		bool active{ false };
		int mode{ 0 };
		std::uint32_t observed{ 0 };
		std::uint32_t skipped{ 0 };
	};

	thread_local Inventory3DDiagnosticState inventory3DDiagnostics;
	thread_local Inventory3DDrawIsolationState inventory3DDrawIsolation;
	thread_local bool inventory3DNativeDraw{};
	std::atomic<std::uint32_t> inventory3DDiagnosticCalls{ 0 };

	struct RenderDimensionsScopeState
	{
		std::uint32_t depth{ 0 };
		std::uint32_t savedWindowWidth{ 0 };
		std::uint32_t savedWindowHeight{ 0 };
	};

	thread_local RenderDimensionsScopeState renderDimensionsScope;
	thread_local bool nativeUIRenderSpaceViewport{ false };
	thread_local TheosRenderPipeline::StartupUIClipping sourceStartupUIClipping;

	// Skyrim's render calculations need to
	// see the reduced extent, but the real borderless HWND must remain native.
	// Keep the renderer cache reduced only while known render entry points are
	// executing. Focus/window-management code runs outside these scopes and
	// therefore cannot accidentally restore the HWND at the render size.
	class ScopedRenderDimensions
	{
	public:
		ScopedRenderDimensions()
		{
			auto* host = NvidiaHost::GetSingleton();
			auto* rendererData = RE::BSGraphics::Renderer::GetRendererDataSingleton();
			if (!host->ProxyActive() || !host->UpscalerReady() ||
				!host->RenderWidth() || !host->RenderHeight() || !rendererData) {
				return;
			}

			if (renderDimensionsScope.depth == 0) {
				renderDimensionsScope.savedWindowWidth = rendererData->renderWindows[0].windowWidth;
				renderDimensionsScope.savedWindowHeight = rendererData->renderWindows[0].windowHeight;
				rendererData->renderWindows[0].windowWidth = host->RenderWidth();
				rendererData->renderWindows[0].windowHeight = host->RenderHeight();
			}
			++renderDimensionsScope.depth;
			engaged_ = true;
		}

		~ScopedRenderDimensions()
		{
			if (!engaged_ || !renderDimensionsScope.depth) {
				return;
			}
			if (--renderDimensionsScope.depth != 0) {
				return;
			}
			if (auto* rendererData = RE::BSGraphics::Renderer::GetRendererDataSingleton()) {
				rendererData->renderWindows[0].windowWidth = renderDimensionsScope.savedWindowWidth;
				rendererData->renderWindows[0].windowHeight = renderDimensionsScope.savedWindowHeight;
			}
		}

		static bool Active()
		{
			return renderDimensionsScope.depth != 0;
		}

	private:
		bool engaged_{ false };
	};

	bool SubmitInventory3DDraw()
	{
		if (!inventory3DDrawIsolation.active) {
			return true;
		}
		const auto ordinal = ++inventory3DDrawIsolation.observed;
		const auto mode = inventory3DDrawIsolation.mode;
		const bool submit = mode >= 1 && mode <= 4 ? ordinal == static_cast<std::uint32_t>(mode) :
			mode >= 5 && mode <= 8 ? ordinal != static_cast<std::uint32_t>(mode - 4) : true;
		if (!submit) {
			++inventory3DDrawIsolation.skipped;
		}
		return submit;
	}

	bool TakeConsoleDiagnosticSlot(std::uint32_t& a_counter)
	{
		auto upscaler = RenderPipeline::GetSingleton();
		if (!upscaler->mConsoleDiagnosticsActive.load(std::memory_order_relaxed)) {
			return false;
		}
		const auto generation = upscaler->mConsoleDiagnosticGeneration.load(std::memory_order_relaxed);
		if (consoleDiagnostics.generation != generation) {
			consoleDiagnostics = {};
			consoleDiagnostics.generation = generation;
		}
		if (a_counter >= 12) {
			return false;
		}
		++a_counter;
		return true;
	}

	std::pair<UINT, UINT> GetViewSize(ID3D11View* a_view)
	{
		if (!a_view) {
			return {};
		}
		ID3D11Resource* resource = nullptr;
		a_view->GetResource(&resource);
		if (!resource) {
			return {};
		}
		ID3D11Texture2D* texture = nullptr;
		resource->QueryInterface(IID_PPV_ARGS(&texture));
		resource->Release();
		if (!texture) {
			return {};
		}
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		texture->Release();
		return { desc.Width, desc.Height };
	}

	std::pair<UINT, UINT> GetRenderTargetSize(ID3D11RenderTargetView* a_rtv)
	{
		return GetViewSize(a_rtv);
	}

	bool ViewReferencesTexture(ID3D11View* a_view, ID3D11Texture2D* a_texture)
	{
		if (!a_view || !a_texture) {
			return false;
		}
		Microsoft::WRL::ComPtr<ID3D11Resource> resource;
		a_view->GetResource(&resource);
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		return resource && SUCCEEDED(resource.As(&texture)) && texture.Get() == a_texture;
	}

	bool TakeInventory3DDiagnosticSlot()
	{
		if (!inventory3DDiagnostics.active || !inventory3DDiagnostics.logStateEvents ||
			inventory3DDiagnostics.event >= 32) {
			return false;
		}
		++inventory3DDiagnostics.event;
		return true;
	}

	void LogInventory3DState(const char* a_stage)
	{
		if (!TakeInventory3DDiagnosticSlot()) {
			return;
		}
		auto upscaler = RenderPipeline::GetSingleton();
		auto context = upscaler ? upscaler->mContext : nullptr;
		if (!context) {
			return;
		}

		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
		context->OMGetRenderTargets(1, rtv.GetAddressOf(), dsv.GetAddressOf());
		UINT viewportCount = 1;
		D3D11_VIEWPORT viewport{};
		context->RSGetViewports(&viewportCount, &viewport);
		UINT scissorCount = 1;
		D3D11_RECT scissor{};
		context->RSGetScissorRects(&scissorCount, &scissor);

		const auto [rtWidth, rtHeight] = GetViewSize(rtv.Get());
		const auto [depthWidth, depthHeight] = GetViewSize(dsv.Get());
		logger::info(
			"[Inventory3DProbe] call {} event {} {} target={}x{} depth={}x{} viewport=({:.1f},{:.1f} {:.1f}x{:.1f}) scissor=({},{} {}x{}) uiActive={} targetBound={}",
			inventory3DDiagnostics.call,
			inventory3DDiagnostics.event,
			a_stage,
			rtWidth,
			rtHeight,
			depthWidth,
			depthHeight,
			viewport.TopLeftX,
			viewport.TopLeftY,
			viewport.Width,
			viewport.Height,
			scissor.left,
			scissor.top,
			scissor.right - scissor.left,
			scissor.bottom - scissor.top,
			NvidiaHost::GetSingleton()->NativeUIPassActive(),
			NvidiaHost::GetSingleton()->NativeUITargetBound());
	}
}

// Render-size proxy: rendering/layout queries must tell the render-size
// story, but focus/window-management queries must retain the native HWND
// extent. Scope the override to known render entry points; the stable
// outer swapchain otherwise exposes the native presentation contract.
BOOL WINAPI hk_GetClientRect(HWND hWnd, LPRECT lpRect)
{
	auto* nvidiaHost = NvidiaHost::GetSingleton();
	if (ScopedRenderDimensions::Active() &&
		nvidiaHost->ProxyActive() && nvidiaHost->UpscalerReady() && lpRect &&
		hWnd == nvidiaHost->GameWindow()) {
		lpRect->left = 0;
		lpRect->top = 0;
		lpRect->right = static_cast<LONG>(nvidiaHost->RenderWidth());
		lpRect->bottom = static_cast<LONG>(nvidiaHost->RenderHeight());
		return TRUE;
	}

	return ptrGetClientRect(hWnd, lpRect);
}

HRESULT WINAPI hk_ID3D11Device_CreateTexture2D(ID3D11Device* This, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D)
{
	auto        hr = (This->*ptrCreateTexture2D)(pDesc, pInitialData, ppTexture2D);
	if (pDesc && inventory3DDiagnostics.active && TakeInventory3DDiagnosticSlot()) {
		logger::info(
			"[Inventory3DProbe] call {} event {} CreateTexture2D {}x{} format={} bind=0x{:X} usage={} samples={}",
			inventory3DDiagnostics.call,
			inventory3DDiagnostics.event,
			pDesc->Width,
			pDesc->Height,
			static_cast<std::uint32_t>(pDesc->Format),
			pDesc->BindFlags,
			static_cast<std::uint32_t>(pDesc->Usage),
			pDesc->SampleDesc.Count);
	}
	static bool locking = false;
	if (locking || FAILED(hr) || !pDesc) {
		return hr;
	}
	auto upscaler = RenderPipeline::GetSingleton();
	auto* nvidiaHost = NvidiaHost::GetSingleton();
	const auto captureWidth = nvidiaHost->ProxyActive() && nvidiaHost->UpscalerReady() ?
		nvidiaHost->RenderWidth() : static_cast<UINT>(upscaler->mDisplaySizeX);
	const auto captureHeight = nvidiaHost->ProxyActive() && nvidiaHost->UpscalerReady() ?
		nvidiaHost->RenderHeight() : static_cast<UINT>(upscaler->mDisplaySizeY);
	// Native companions have the same extent as game guides in DLAA. Do not
	// recapture our own resources while creating them through the hooked device.
	if (nvidiaHost->NativeUIInternalBind()) { return hr; }
	if (pDesc->Format == DXGI_FORMAT_R16G16_FLOAT && pDesc->BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET)) {
		// The game's motion-vector target is the first source-sized R16G16
		// texture created with these bind flags.
		if (captureWidth > 0) {
			if (pDesc->Width != captureWidth || pDesc->Height != captureHeight) {
				return hr;
			}
		}
		// Re-capture on recreation: the owner releasing and recreating its
		// target would otherwise leave us holding a stale texture.
		if (upscaler->mMotionVectors.mImage != nullptr) {
			logger::info("Motion vector candidate re-created by the game, swapping capture: {} x {}", pDesc->Width, pDesc->Height);
		} else {
			logger::info("Motion vector candidate found: {} x {}", pDesc->Width, pDesc->Height);
		}
		locking = true;
		upscaler->SetupMotionVector(*ppTexture2D);
		locking = false;
	} else if (pDesc->Format >= DXGI_FORMAT_R24G8_TYPELESS && pDesc->Format <= DXGI_FORMAT_X24_TYPELESS_G8_UINT) {
		if (pDesc->Width == captureWidth &&
			pDesc->Height == captureHeight &&
			pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) {
			if (upscaler->mDepthBuffer.mImage != nullptr) {
				logger::info("Depth buffer candidate re-created by the game, swapping capture: {} x {}", pDesc->Width, pDesc->Height);
				locking = true;
				upscaler->SetupDepth(*ppTexture2D);
				locking = false;
				return hr;
			}
			logger::info("Depth buffer candidate found: {} x {}", pDesc->Width, pDesc->Height);
			locking = true;
			upscaler->SetupDepth(*ppTexture2D);
			locking = false;
		}
	}
	return hr;
}

// UI-at-native: while the source coordinator redirects the game UI pass, rebinds of the fake backbuffer are remapped to the
// native present source and render-space viewports are scaled up, so the
// whole pass rasterizes at display resolution.
void WINAPI hk_ID3D11DeviceContext_PSSetShaderResources(ID3D11DeviceContext* This, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* views)
{
	auto* host = NvidiaHost::GetSingleton();
	host->LogSourceContextHook(2, This);
	if (host->SourceContext(This) && host->NativeUIPassActive() && !host->NativeUIInternalBind()) {
		Microsoft::WRL::ComPtr<ID3D11Resource> backgroundDepth;
		if (auto* view = host->NativeUIBackgroundDepth()) { view->GetResource(&backgroundDepth); }
		ID3D11ShaderResourceView* replacements[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
		const auto replaced = host->UIAttachments().SubstituteDepths(NumViews, views, replacements,
			RenderPipeline::GetSingleton()->mDepthBuffer.mImage, backgroundDepth.Get());
		if (replaced) {
			static unsigned logged = 0;
			if (logged < 3) {
				++logged;
				logger::info("[InventoryPreview] PS depth binding start={} count={} replaced={} background={}",
					StartSlot, NumViews, replaced, static_cast<void*>(backgroundDepth.Get()));
			}
			(This->*ptrPSSetShaderResources)(StartSlot, NumViews, replacements);
			return;
		}
	}
	(This->*ptrPSSetShaderResources)(StartSlot, NumViews, views);
}

void WINAPI hk_ID3D11DeviceContext_OMSetRenderTargets(ID3D11DeviceContext* This, UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView)
{
	auto* nvidiaHost = NvidiaHost::GetSingleton();
	nvidiaHost->LogSourceContextHook(0, This);
	if (nvidiaHost->SourceContext(This) && nvidiaHost->StartupOverlay().Active() && !nvidiaHost->NativeUIInternalBind()) {
		if (NumViews == 1 && ppRenderTargetViews && ViewReferencesTexture(ppRenderTargetViews[0], nvidiaHost->GameFacingTexture())) {
			auto* target = nvidiaHost->StartupOverlay().RTV();
			(This->*ptrOMSetRenderTargets)(1, &target, nullptr);
		} else {
			(This->*ptrOMSetRenderTargets)(NumViews, ppRenderTargetViews, pDepthStencilView);
		}
		return;
	}
	if (inventory3DDiagnostics.active && TakeInventory3DDiagnosticSlot()) {
		const auto [width, height] = GetRenderTargetSize(
			NumViews > 0 && ppRenderTargetViews ? ppRenderTargetViews[0] : nullptr);
		const auto [depthWidth, depthHeight] = GetViewSize(pDepthStencilView);
		logger::info(
			"[Inventory3DProbe] call {} event {} OMSetRenderTargets input={}x{} depth={}x{} views={} uiActive={} targetBound={}",
			inventory3DDiagnostics.call,
			inventory3DDiagnostics.event,
			width,
			height,
			depthWidth,
			depthHeight,
			NumViews,
			NvidiaHost::GetSingleton()->NativeUIPassActive(),
			NvidiaHost::GetSingleton()->NativeUITargetBound());
	}
	if (TakeConsoleDiagnosticSlot(consoleDiagnostics.renderTargets)) {
		const auto [width, height] = GetRenderTargetSize(
			NumViews > 0 && ppRenderTargetViews ? ppRenderTargetViews[0] : nullptr);
		logger::info(
			"[ConsoleDiag] OM {} input target={}x{} views={} uiActive={} targetBound={} internal={} hasDSV={}",
			consoleDiagnostics.renderTargets,
			width,
			height,
			NumViews,
			NvidiaHost::GetSingleton()->NativeUIPassActive(),
			NvidiaHost::GetSingleton()->NativeUITargetBound(),
			NvidiaHost::GetSingleton()->NativeUIInternalBind(),
			pDepthStencilView != nullptr);
	}
	if (nvidiaHost->SourceContext(This) && nvidiaHost->NativeUIPassActive() && !nvidiaHost->NativeUIInternalBind()) {
		auto* first = NumViews && ppRenderTargetViews ? ppRenderTargetViews[0] : nullptr;
		const bool gameFacing = ViewReferencesTexture(first, nvidiaHost->GameFacingTexture());
		const bool enbUI = nvidiaHost->UIAttachments().IsENBTarget(first);
		const bool ownUI = ViewReferencesTexture(first, nvidiaHost->NativeUIRenderTexture());
		nvidiaHost->SetNativeUITargetBound(gameFacing || enbUI || ownUI);
		if (!nvidiaHost->NativeUITargetBound()) { nativeUIRenderSpaceViewport = false; }
		if (gameFacing || enbUI || ownUI) {
			ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			if (nvidiaHost->UIAttachments().RedirectTargets(NumViews, ppRenderTargetViews, nvidiaHost->NativeUIRenderRTV(), targets)) {
				(This->*ptrOMSetRenderTargets)(NumViews, targets, pDepthStencilView ? nvidiaHost->NativeUIDepthDSV() : nullptr);
				nvidiaHost->LogNativeUIState(enbUI ? "OM-enb-native" : "OM-native", false);
				return;
			}
			nvidiaHost->SetNativeUITargetBound(false);
			nativeUIRenderSpaceViewport = false;
			if (nvidiaHost->TakeNativeUITraceSlot()) { logger::warn("[NativeUIRoute] frame={} unsupported native attachment set views={}; original binding retained", nvidiaHost->PresentCount(), NumViews); }
		}
	}

	(This->*ptrOMSetRenderTargets)(NumViews, ppRenderTargetViews, pDepthStencilView);
}

void WINAPI hk_ID3D11DeviceContext_RSSetViewports(ID3D11DeviceContext* This, UINT NumViewports, const D3D11_VIEWPORT* pViewports)
{
	auto* nvidiaHost = NvidiaHost::GetSingleton();
	nvidiaHost->LogSourceContextHook(1, This);
	if (nvidiaHost->SourceContext(This) && nvidiaHost->StartupOverlay().Active() && !nvidiaHost->NativeUIInternalBind()) {
		sourceStartupUIClipping.Reset();
		auto& overlay = nvidiaHost->StartupOverlay();
		const auto previous = overlay.EndViewport(This);
		if (previous.count) { (This->*ptrRSSetScissorRects)(previous.count, previous.rects.data()); }
		D3D11_VIEWPORT native{};
		if (overlay.Viewport(This, NumViewports, pViewports, nvidiaHost->RenderWidth(), nvidiaHost->RenderHeight(), native)) {
			D3D11_RECT clips[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			if (overlay.Scissors(This, previous.count, previous.rects.data(), clips)) {
				(This->*ptrRSSetScissorRects)(previous.count, clips);
			}
			(This->*ptrRSSetViewports)(1, &native);
		} else {
			(This->*ptrRSSetViewports)(NumViewports, pViewports);
		}
		return;
	}
	// A producer's new viewport (including a restore/internal bind) supersedes
	// the previous startup clip space. Keep it tied to this context and frame.
	TheosRenderPipeline::StartupUIClipping::Clips startupClips;
	if (nvidiaHost->SourceContext(This)) {
		startupClips = sourceStartupUIClipping.EndViewport(This, nvidiaHost->PresentCount());
		if (startupClips.count) { (This->*ptrRSSetScissorRects)(startupClips.count, startupClips.rects.data()); }
	}
	if (nvidiaHost->SourceContext(This) && nvidiaHost->NativeUIInternalBind()) {
		(This->*ptrRSSetViewports)(NumViewports, pViewports);
		return;
	}
	if (inventory3DDiagnostics.active && TakeInventory3DDiagnosticSlot()) {
		logger::info(
			"[Inventory3DProbe] call {} event {} RSSetViewports input=({:.1f},{:.1f} {:.1f}x{:.1f}) count={} uiActive={} targetBound={}",
			inventory3DDiagnostics.call,
			inventory3DDiagnostics.event,
			pViewports && NumViewports ? pViewports[0].TopLeftX : 0.0f,
			pViewports && NumViewports ? pViewports[0].TopLeftY : 0.0f,
			pViewports && NumViewports ? pViewports[0].Width : 0.0f,
			pViewports && NumViewports ? pViewports[0].Height : 0.0f,
			NumViewports,
			NvidiaHost::GetSingleton()->NativeUIPassActive(),
			NvidiaHost::GetSingleton()->NativeUITargetBound());
	}

	if (TakeConsoleDiagnosticSlot(consoleDiagnostics.viewports)) {
		const bool renderSpace = NumViewports > 0 && pViewports && NvidiaHost::GetSingleton()->NativeUIPassActive() &&
			NvidiaHost::GetSingleton()->NativeUITargetBound() && NvidiaHost::GetSingleton()->RenderWidth() > 0 &&
			static_cast<UINT>(pViewports[0].Width) == NvidiaHost::GetSingleton()->RenderWidth() &&
			static_cast<UINT>(pViewports[0].Height) == NvidiaHost::GetSingleton()->RenderHeight();
		const float sx = NvidiaHost::GetSingleton()->RenderWidth() > 0 ?
			static_cast<float>(NvidiaHost::GetSingleton()->OutputWidth()) / static_cast<float>(NvidiaHost::GetSingleton()->RenderWidth()) : 1.0f;
		const float sy = NvidiaHost::GetSingleton()->RenderHeight() > 0 ?
			static_cast<float>(NvidiaHost::GetSingleton()->OutputHeight()) / static_cast<float>(NvidiaHost::GetSingleton()->RenderHeight()) : 1.0f;
		logger::info(
			"[ConsoleDiag] viewport {} input=({:.1f},{:.1f} {:.1f}x{:.1f}) uiActive={} targetBound={} renderSpace={} output={:.1f}x{:.1f}",
			consoleDiagnostics.viewports,
			pViewports && NumViewports ? pViewports[0].TopLeftX : 0.0f,
			pViewports && NumViewports ? pViewports[0].TopLeftY : 0.0f,
			pViewports && NumViewports ? pViewports[0].Width : 0.0f,
			pViewports && NumViewports ? pViewports[0].Height : 0.0f,
			NvidiaHost::GetSingleton()->NativeUIPassActive(),
			NvidiaHost::GetSingleton()->NativeUITargetBound(),
			renderSpace,
			pViewports && NumViewports ? pViewports[0].Width * (renderSpace ? sx : 1.0f) : 0.0f,
			pViewports && NumViewports ? pViewports[0].Height * (renderSpace ? sy : 1.0f) : 0.0f);
	}
	if (nvidiaHost->SourceContext(This) && nvidiaHost->UpscalerReady()) {
		auto* ui = RE::UI::GetSingleton();
		const bool mainOrLoading = ui && (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
		D3D11_VIEWPORT changed{};
		const auto change = TheosRenderPipeline::NativeUIViewport(nvidiaHost->NativeFrame(), mainOrLoading,
			nvidiaHost->NativeUIPassActive() && nvidiaHost->NativeUITargetBound(), nvidiaHost->NativeUIInternalBind(),
			NumViewports, pViewports, nvidiaHost->RenderWidth(), nvidiaHost->RenderHeight(), nvidiaHost->OutputWidth(), nvidiaHost->OutputHeight(), changed);
		if (change != TheosRenderPipeline::ViewportChange::None) {
			nativeUIRenderSpaceViewport = change == TheosRenderPipeline::ViewportChange::NativeUI;
			sourceStartupUIClipping.Observe(This, nvidiaHost->PresentCount(), change, *pViewports, changed,
				nvidiaHost->OutputWidth(), nvidiaHost->OutputHeight());
			if (startupClips.count) {
				D3D11_RECT clips[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
				if (sourceStartupUIClipping.Transform(This, nvidiaHost->PresentCount(), nvidiaHost->NativeFrame(),
					startupClips.count, startupClips.rects.data(), clips)) {
					(This->*ptrRSSetScissorRects)(startupClips.count, clips);
				}
			}
			(This->*ptrRSSetViewports)(1, &changed);
			if (nvidiaHost->TakeNativeUITraceSlot()) {
				logger::info("[NativeUIRoute] frame={} viewport route={} input=({:.1f},{:.1f} {:.1f}x{:.1f}) output=({:.1f},{:.1f} {:.1f}x{:.1f})",
					nvidiaHost->PresentCount(), nativeUIRenderSpaceViewport ? "native-ui" : "startup-render",
					pViewports[0].TopLeftX, pViewports[0].TopLeftY, pViewports[0].Width, pViewports[0].Height,
					changed.TopLeftX, changed.TopLeftY, changed.Width, changed.Height);
			}
			return;
		}
		if (!nvidiaHost->NativeUIInternalBind()) { nativeUIRenderSpaceViewport = false; }
	}

	(This->*ptrRSSetViewports)(NumViewports, pViewports);
}

// Scaleform clips masked HUD elements (compass strip, lists) with scissor
// rects in render-size space; they need the same scaling as viewports while
// the UI pass rasterizes at native.
void WINAPI hk_ID3D11DeviceContext_OMSetBlendState(ID3D11DeviceContext* This, ID3D11BlendState* state, const FLOAT factors[4], UINT mask)
{
    auto* host = NvidiaHost::GetSingleton();
    auto* mapped = state;
    if (host->SourceContext(This)) { mapped = host->StartupOverlay().ForegroundBlend(This, state); }
    if (mapped != state) {
        static unsigned logged = 0;
        if (logged++ < 3) { logger::info("[StartupOverlay] legacy ImGui alpha corrected for transparent foreground; RGB unchanged"); }
    }
    (This->*ptrOMSetBlendState)(mapped, factors, mask);
}

void WINAPI hk_ID3D11DeviceContext_RSSetScissorRects(ID3D11DeviceContext* This, UINT NumRects, const D3D11_RECT* pRects)
{
	auto* nvidiaHost = NvidiaHost::GetSingleton();
	if (nvidiaHost->SourceContext(This) && (nvidiaHost->StartupOverlay().Active() || nvidiaHost->NativeUIInternalBind())) {
		D3D11_RECT mapped[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		const bool changed = !nvidiaHost->NativeUIInternalBind() && nvidiaHost->StartupOverlay().Scissors(This, NumRects, pRects, mapped);
		(This->*ptrRSSetScissorRects)(NumRects, changed ? mapped : pRects);
		return;
	}
	if (nvidiaHost->SourceContext(This) && !nvidiaHost->NativeUIInternalBind()) {
		D3D11_RECT mapped[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		if (sourceStartupUIClipping.Transform(This, nvidiaHost->PresentCount(), nvidiaHost->NativeFrame(), NumRects, pRects, mapped)) {
			(This->*ptrRSSetScissorRects)(NumRects, mapped);
			if (nvidiaHost->TakeNativeUITraceSlot()) {
				logger::info("[NativeUIRoute] frame={} scissor route=startup-render count={} input=({},{},{},{}) output=({},{},{},{})",
					nvidiaHost->PresentCount(), NumRects, pRects[0].left, pRects[0].top, pRects[0].right, pRects[0].bottom,
					mapped[0].left, mapped[0].top, mapped[0].right, mapped[0].bottom);
			}
			return;
		}
	}
	if (inventory3DDiagnostics.active && TakeInventory3DDiagnosticSlot()) {
		logger::info(
			"[Inventory3DProbe] call {} event {} RSSetScissorRects input=({},{} {}x{}) count={} uiActive={} targetBound={}",
			inventory3DDiagnostics.call,
			inventory3DDiagnostics.event,
			pRects && NumRects ? pRects[0].left : 0,
			pRects && NumRects ? pRects[0].top : 0,
			pRects && NumRects ? pRects[0].right - pRects[0].left : 0,
			pRects && NumRects ? pRects[0].bottom - pRects[0].top : 0,
			NumRects,
			NvidiaHost::GetSingleton()->NativeUIPassActive(),
			NvidiaHost::GetSingleton()->NativeUITargetBound());
	}

	if (TakeConsoleDiagnosticSlot(consoleDiagnostics.scissors)) {
		const bool renderSpace = NumRects > 0 && pRects && NvidiaHost::GetSingleton()->NativeUIPassActive() && NvidiaHost::GetSingleton()->NativeUITargetBound() &&
			NvidiaHost::GetSingleton()->RenderWidth() > 0 && pRects[0].right <= static_cast<LONG>(NvidiaHost::GetSingleton()->RenderWidth()) + 2 &&
			pRects[0].bottom <= static_cast<LONG>(NvidiaHost::GetSingleton()->RenderHeight()) + 2;
		const float sx = NvidiaHost::GetSingleton()->RenderWidth() > 0 ?
			static_cast<float>(NvidiaHost::GetSingleton()->OutputWidth()) / static_cast<float>(NvidiaHost::GetSingleton()->RenderWidth()) : 1.0f;
		const float sy = NvidiaHost::GetSingleton()->RenderHeight() > 0 ?
			static_cast<float>(NvidiaHost::GetSingleton()->OutputHeight()) / static_cast<float>(NvidiaHost::GetSingleton()->RenderHeight()) : 1.0f;
		logger::info(
			"[ConsoleDiag] scissor {} input=({},{} {}x{}) uiActive={} targetBound={} renderSpace={} output={}x{}",
			consoleDiagnostics.scissors,
			pRects && NumRects ? pRects[0].left : 0,
			pRects && NumRects ? pRects[0].top : 0,
			pRects && NumRects ? pRects[0].right - pRects[0].left : 0,
			pRects && NumRects ? pRects[0].bottom - pRects[0].top : 0,
			NvidiaHost::GetSingleton()->NativeUIPassActive(),
			NvidiaHost::GetSingleton()->NativeUITargetBound(),
			renderSpace,
			pRects && NumRects ? static_cast<LONG>((pRects[0].right - pRects[0].left) * (renderSpace ? sx : 1.0f)) : 0,
			pRects && NumRects ? static_cast<LONG>((pRects[0].bottom - pRects[0].top) * (renderSpace ? sy : 1.0f)) : 0);
	}
	if (nvidiaHost->NativeUIPassActive() && nvidiaHost->NativeUITargetBound() &&
		nativeUIRenderSpaceViewport && !nvidiaHost->NativeUIInternalBind() &&
		NumRects > 0 && pRects &&
		nvidiaHost->RenderWidth() > 0 && nvidiaHost->RenderHeight() > 0) {
		const float sx = static_cast<float>(nvidiaHost->OutputWidth()) / static_cast<float>(nvidiaHost->RenderWidth());
		const float sy = static_cast<float>(nvidiaHost->OutputHeight()) / static_cast<float>(nvidiaHost->RenderHeight());
		D3D11_RECT scaled[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		const UINT n = (std::min)(NumRects, static_cast<UINT>(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
		for (UINT i = 0; i < n; ++i) {
			scaled[i].left = static_cast<LONG>(pRects[i].left * sx);
			scaled[i].top = static_cast<LONG>(pRects[i].top * sy);
			scaled[i].right = static_cast<LONG>(pRects[i].right * sx);
			scaled[i].bottom = static_cast<LONG>(pRects[i].bottom * sy);
		}
		(This->*ptrRSSetScissorRects)(n, scaled);
		return;
	}

	(This->*ptrRSSetScissorRects)(NumRects, pRects);
}

namespace
{
	template<class Draw> void DrawNativeInventoryPreview(ID3D11DeviceContext* context, const Draw& original)
	{
		auto* host = NvidiaHost::GetSingleton();
		if (!inventory3DNativeDraw || !host->SourceContext(context) ||
			!host->NativeUITargetBound() || host->NativeUIInternalBind()) { original(); return; }
		Microsoft::WRL::ComPtr<ID3D11Resource> backgroundDepth;
		if (auto* view = host->NativeUIBackgroundDepth()) { view->GetResource(&backgroundDepth); }
		auto* guide = RenderPipeline::GetSingleton()->mDepthBuffer.mImage;
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto* declaredMain = renderer ? renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN].texture : nullptr;
		const auto result = host->PreviewDraw().Draw(context, host->NativeUIRenderRTV(), original,
			{&host->UIAttachments(), guide, backgroundDepth.Get()});
		static unsigned depthLogged = 0;
		if (host->PreviewDraw().DepthSubstitutions() && depthLogged < 3) {
			++depthLogged;
			logger::info("[InventoryPreview] draw-time depth replaced={} background={} guide={} declaredMain={}",
				host->PreviewDraw().DepthSubstitutions(), static_cast<void*>(backgroundDepth.Get()),
				static_cast<void*>(guide), static_cast<void*>(declaredMain));
		}
		static bool correctedLogged = false, unsupportedLogged = false;
		if (result == S_OK && !correctedLogged) {
			correctedLogged = true; logger::info("[InventoryPreview] native geometry coverage correction active");
		} else if (result != S_OK && !unsupportedLogged) {
			unsupportedLogged = true;
			logger::warn("[InventoryPreview] draw coverage unavailable for a preview state (0x{:08X}); original drawing retained", static_cast<unsigned>(result));
		}
	}
}

void WINAPI hk_ID3D11DeviceContext_DrawIndexed(
	ID3D11DeviceContext* This,
	UINT IndexCount,
	UINT StartIndexLocation,
	INT BaseVertexLocation)
{
	if (SubmitInventory3DDraw()) {
		const auto original = [&] {
			(This->*ptrDrawIndexed)(IndexCount, StartIndexLocation, BaseVertexLocation);
		};
		DrawNativeInventoryPreview(This, original);
	}
}

void WINAPI hk_ID3D11DeviceContext_Draw(
	ID3D11DeviceContext* This,
	UINT VertexCount,
	UINT StartVertexLocation)
{
	// The coverage stamp is a host draw, outside the game's draw isolation and
	// preview correction. The original geometry callbacks use the saved vtable.
	if (NvidiaHost::GetSingleton()->PreviewDraw().Internal()) {
		(This->*ptrDraw)(VertexCount, StartVertexLocation); return;
	}
	if (SubmitInventory3DDraw()) {
		const auto original = [&] {
			(This->*ptrDraw)(VertexCount, StartVertexLocation);
		};
		DrawNativeInventoryPreview(This, original);
	}
}

// When frame generation proxies the swapchain, the game's swapchain request
// is satisfied with a D3D12-backed proxy (doodlum's ENBFrameGeneration
// architecture, ffx_api runtime).
void InstallUpscalerContextHooks(ID3D11Device* device, ID3D11DeviceContext* deviceContext)
{
    *(uintptr_t*)&ptrCreateTexture2D =
        Detours::X64::DetourClassVTable(*(uintptr_t*)device, &hk_ID3D11Device_CreateTexture2D, 5);
    TheosRenderPipeline::InstallPixelSamplerHook(deviceContext);
    *(uintptr_t*)&ptrPSSetShaderResources =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_PSSetShaderResources, 8);
    TheosRenderPipeline::InstallAdditionalSamplerHooks(deviceContext);
    *(uintptr_t*)&ptrOMSetRenderTargets =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_OMSetRenderTargets, 33);
    *(uintptr_t*)&ptrOMSetBlendState =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_OMSetBlendState, 35);
    *(uintptr_t*)&ptrRSSetViewports =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_RSSetViewports, 44);
    *(uintptr_t*)&ptrRSSetScissorRects =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_RSSetScissorRects, 45);
    *(uintptr_t*)&ptrDrawIndexed =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_DrawIndexed, 12);
    *(uintptr_t*)&ptrDraw =
        Detours::X64::DetourClassVTable(*(uintptr_t*)deviceContext, &hk_ID3D11DeviceContext_Draw, 13);
}

struct UpscalerHooks
{
	struct Inventory3DManagerRender
	{
		static std::uint32_t thunk(RE::Inventory3DManager* a_manager)
		{
			auto upscaler = RenderPipeline::GetSingleton();
			auto ui = RE::UI::GetSingleton();
			const bool magicMenuOpen = ui && ui->IsMenuOpen(RE::MagicMenu::MENU_NAME);
			const auto isolationMode = std::clamp(upscaler->mInventory3DDrawIsolationMode, 0, 8);
			const bool isolateDraws = magicMenuOpen && isolationMode != 0;
			if (isolateDraws) {
				inventory3DDrawIsolation = { true, isolationMode, 0, 0 };
			}
			const auto stateCall = magicMenuOpen && upscaler->mLogMenuMetrics ?
				inventory3DDiagnosticCalls.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
			const bool stateProbe = stateCall > 0 && stateCall <= 12;

			if (stateProbe) {
				inventory3DDiagnostics = {};
				inventory3DDiagnostics.active = true;
				inventory3DDiagnostics.logStateEvents = true;
				inventory3DDiagnostics.call = stateCall;
			}
			if (stateProbe) {
				LogInventory3DState("begin");
			}

			auto* host = NvidiaHost::GetSingleton();
			const bool savedNativeDraw = inventory3DNativeDraw;
			inventory3DNativeDraw = ui && host->DedicatedUITextureMode() && host->NativeUIPassActive() &&
				(magicMenuOpen || ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) ||
					ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) || ui->IsMenuOpen(RE::BarterMenu::MENU_NAME));
			const auto result = func(a_manager);
			inventory3DNativeDraw = savedNativeDraw;
			if (isolateDraws) {
				upscaler->mInventory3DLastObservedDraws.store(
					inventory3DDrawIsolation.observed,
					std::memory_order_relaxed);
				upscaler->mInventory3DLastSkippedDraws.store(
					inventory3DDrawIsolation.skipped,
					std::memory_order_relaxed);
				inventory3DDrawIsolation = {};
			}

			if (stateProbe) {
				LogInventory3DState("end");
			}
			if (stateProbe) {
				inventory3DDiagnostics = {};
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSGraphics_Renderer_Init_InitD3D
	{
		static void thunk()
		{
			func();
			MenuOpenCloseEventHandler::Register();
			if (TheosRenderPipeline::CommunityShaders::Active()) { return; }
			RenderPipeline::GetSingleton()->InitUpscaler();
			// Establish native engine dimensions immediately after renderer
			// initialization. These fields are distinct from the HWND cache
			// and from the cursor's movement bounds.
			if (TheosRenderPipeline::NativeInput::Active()) {
				const auto extent = std::atomic_ref(TheosRenderPipeline::NativeInput::extentWord)
					.load(std::memory_order_acquire);
				auto* state = RE::BSGraphics::State::GetSingleton();
				state->screenWidth = static_cast<std::uint32_t>(extent);
				state->screenHeight = static_cast<std::uint32_t>(extent >> 32);
				state->frameBufferViewport[0] = state->screenWidth;
				state->frameBufferViewport[1] = state->screenHeight;
				logger::info("[NativeInput] initial engine screen and viewport set to {}x{}",
					state->screenWidth, state->screenHeight);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// After the world finishes rendering: copy depth/motion vectors into the
	// frame-generation shared buffers while their contents are this frame's.
	struct Main_RenderWorld
	{
		static void thunk(bool a1)
		{
			ScopedRenderDimensions renderSizeScope;
			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static bool MainOrLoadingMenuOpen()
	{
		auto* ui = RE::UI::GetSingleton();
		return ui &&
			(ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
	}

	// Evaluate after the inner world draw completes. The enclosing function
	// has other work outside this boundary; preserve both thunk arguments.
	struct MainDrawWorldCompletion
	{
		static void thunk(std::uint64_t a1, std::uint32_t a2)
		{
			func(a1, a2);
			NvidiaHost::GetSingleton()->OnBackgroundReady(
				TheosRenderPipeline::BackgroundBoundary::World, MainOrLoadingMenuOpen());
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// The alternative completion boundary runs after the loading background.
	struct MistMenuPostDisplayBackground
	{
		static void thunk(std::uint64_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4)
		{
			func(a1, a2, a3, a4);
			NvidiaHost::GetSingleton()->OnBackgroundReady(
				TheosRenderPipeline::BackgroundBoundary::Mist, MainOrLoadingMenuOpen());
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceStart
    {
        static void thunk(int64_t a1)
        {
            ScopedRenderDimensions renderSizeScope;
            func(a1);
        }
        static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSGraphics_Renderer_Begin_UpdateJitter
	{
		static void thunk(BSGraphics::State* a_state)
		{
			ScopedRenderDimensions renderSizeScope;
			func(a_state);
			auto upscaler = RenderPipeline::GetSingleton();
			PerformanceTuning::GetSingleton()->BeginD3D11Frame(
				upscaler->mDevice,
				upscaler->mContext,
				upscaler->mRenderedFrameCount + 1);
			upscaler->mGraphicsState = a_state;
			++upscaler->mRenderedFrameCount;
			// The TAA singleton is unavailable during early initialization. Keep
			// its pass disabled once it exists; NVIDIA receives jittered input.
			if (GetGameTAA()) {
				SetGameTAA(false);
				static bool logged = false;
				if (!logged) {
					logged = true;
					logger::info("Game TAA disabled for the NVIDIA host");
				}
			}
			if (upscaler->IsEnabled() && upscaler->mEnableJitter) {
				float x = 0.0f;
				float y = 0.0f;
				upscaler->GetJitters(&x, &y);
				float w = float(upscaler->mRenderSizeX);
				float h = float(upscaler->mRenderSizeY);
				a_state->jitter[0] = -2 * x / w;
				a_state->jitter[1] = 2 * y / h;
				upscaler->SetJitterOffsets(-x, -y);
			} else {
				a_state->jitter[0] = 0;
				a_state->jitter[1] = 0;
				upscaler->SetJitterOffsets(0, 0);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Override only the renderer's GetClientRect call, changing its width and
	// height to the source render extent. Scaleform layout and renderer setup
	// therefore agree without lying to focus, resize, or window-management code.
	struct BSGraphics_Renderer_GetClientRect
	{
		static BOOL WINAPI thunk(HWND a_window, LPRECT a_rect)
		{
			const auto result = func(a_window, a_rect);
			auto* nvidiaHost = NvidiaHost::GetSingleton();
			if (nvidiaHost->ProxyActive() && nvidiaHost->UpscalerReady() && a_rect &&
				a_window == nvidiaHost->GameWindow()) {
				const auto nativeWidth = a_rect->right - a_rect->left;
				const auto nativeHeight = a_rect->bottom - a_rect->top;
				a_rect->right = a_rect->left + static_cast<LONG>(nvidiaHost->RenderWidth());
				a_rect->bottom = a_rect->top + static_cast<LONG>(nvidiaHost->RenderHeight());

				static std::atomic_bool logged{ false };
				if (!logged.exchange(true)) {
					logger::info(
						"[NvidiaHost] renderer client extent overridden {}x{} -> {}x{} at renderer callsite",
						nativeWidth,
						nativeHeight,
						nvidiaHost->RenderWidth(),
						nvidiaHost->RenderHeight());
				}
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		const auto* profile = TheosRenderPipeline::SkyrimRuntime::Find(REL::Module::get().version());
		if (!profile) {
			util::report_and_fail("No verified hook profile for this Skyrim runtime.");
		}
		const auto& offsets = profile->hooks;
		if (TheosRenderPipeline::CommunityShaders::Active()) {
			stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));
			TheosRenderPipeline::InstallUpscalerDeviceHooks(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
			TheosRenderPipeline::LoadingArtwork::Install();
			return;
		}
		{
			// Validate all sites before publishing
			// any patch; retain the original calling convention through a tail jump.
			const auto mouseSite = REL::RelocationID(50604, 51498).address() + 0xB;
			const auto screenSite = REL::RelocationID(75590, 77397).address() + 0x102;
			const auto dimensionsSite = REL::RelocationID(99938, 106583).address() + REL::Relocate(0x8E, 0x84);
			const auto mistSite = REL::RelocationID(51855, 52727).address() + offsets.mistBackground;
			const auto worldSite = REL::RelocationID(79947, 82084).address() + REL::Relocate(0x16F, 0x17A);
			if (*reinterpret_cast<const std::uint8_t*>(mouseSite) != 0xE8 ||
				*reinterpret_cast<const std::uint8_t*>(screenSite) != 0xE8 ||
				*reinterpret_cast<const std::uint8_t*>(dimensionsSite) != 0xE8 ||
				*reinterpret_cast<const std::uint8_t*>(mistSite) != 0xE8 ||
				*reinterpret_cast<const std::uint8_t*>(worldSite) != 0xE8) {
				util::report_and_fail("Native input/loading call sites do not match this Skyrim runtime; refusing an incomplete source host.");
			}
			const auto originalTarget = [](std::uintptr_t site) {
				std::int32_t relative{};
				std::memcpy(&relative, reinterpret_cast<const void*>(site + 1), sizeof(relative));
				return site + 5 + relative;
			};
			using namespace TheosRenderPipeline::NativeInput;
			static Thunk mouse{ Target::CursorBounds, &extentWord, originalTarget(mouseSite) };
			static Thunk screen{ Target::ScreenSize, &extentWord, originalTarget(screenSite) };
			static_assert(offsetof(RE::BSGraphics::State, screenWidth) == 0x24);
			static_assert(offsetof(RE::BSGraphics::State, screenHeight) == 0x28);
			static_assert(offsetof(RE::BSGraphics::State, frameBufferViewport) == 0x2C);
			static EngineDimensionsThunk dimensions{ &extentWord,
				&RE::BSGraphics::State::GetSingleton()->screenWidth, originalTarget(dimensionsSite) };
			SKSE::GetTrampoline().write_call<5>(mouseSite, mouse.getCode());
			SKSE::GetTrampoline().write_call<5>(screenSite, screen.getCode());
			SKSE::GetTrampoline().write_call<5>(dimensionsSite, dimensions.getCode());
			hooksInstalled = true;
			logger::info("[NativeInput] cursor bounds, screen-size and engine dimension hooks installed");
			stl::write_thunk_call<MistMenuPostDisplayBackground>(mistSite);
			stl::write_thunk_call<MainDrawWorldCompletion>(worldSite);
			logger::info("[NativeUIRoute] world completion boundary installed at runtime RVA 0x{:X}",
				worldSite - REL::Module::get().base());
			logger::info("[LoadingUI] MistMenu background boundary installed at runtime RVA 0x{:X}",
				mistSite - REL::Module::get().base());
		}
		// InitD3D runs after the swapchain exists; used to register the menu
		// handler and create the DLSS feature.
		stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));
		// Hook swapchain creation through the IAT so CreateTexture2D is
		// detoured before the depth and motion vector targets are created.
		auto moduleBase = (uintptr_t)GetModuleHandleW(nullptr);
		TheosRenderPipeline::InstallUpscalerDeviceHooks(moduleBase);
		*(FARPROC*)&ptrGetClientRect = GetProcAddress(GetModuleHandleA("user32.dll"), "GetClientRect");
		const auto clientRectOffset = offsets.rendererClientRect;
		stl::write_thunk_call<BSGraphics_Renderer_GetClientRect, 6>(
			REL::RelocationID(75460, 77245).address() + clientRectOffset);
		Detours::IATHook(moduleBase, "user32.dll", "GetClientRect", (uintptr_t)hk_GetClientRect);

		// Setup our own jitters
		stl::write_thunk_call<BSGraphics_Renderer_Begin_UpdateJitter>(REL::RelocationID(75460, 77245).address() + offsets.updateJitter);
		// Always enable TAA jitters, even without TAA
		static REL::Relocation<uintptr_t> updateJitterHook{ REL::RelocationID(75709, 77518) };          // D7CFB0, DB96E0
		static REL::Relocation<uintptr_t> buildCameraStateDataHook{ REL::RelocationID(75711, 77520) };  // D7D130, DB9850
		uint8_t                           patch1[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		uint8_t                           patch2[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		REL::safe_write<uint8_t>(updateJitterHook.address() + REL::Relocate(0xE, 0x11), patch1);
		REL::safe_write<uint8_t>(buildCameraStateDataHook.address() + offsets.cameraBranch, patch2);

		// Frame generation input capture points.
		stl::write_thunk_call<Main_RenderWorld>(REL::RelocationID(35560, 36559).address() + offsets.renderWorld);
		stl::detour_thunk<MenuManagerDrawInterfaceStart>(REL::RelocationID(79947, 82084));
		stl::detour_thunk<Inventory3DManagerRender>(REL::RelocationID(50882, 51755));
		TheosRenderPipeline::LoadingArtwork::Install();

			logger::info("[NvidiaHost] source NVIDIA runtime selected");

		logger::info("Installed upscaler hooks");
	}
};

void InstallUpscalerHooks()
{
	UpscalerHooks::Install();
}
