#include "NativeUIComposition.h"
#include <d3dcompiler.h>
#include <spdlog/spdlog.h>
#include <iterator>

namespace TheosRenderPipeline
{
bool NativeUIComposition::Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context,
	ID3D11Texture2D* a_scene, const D3D11_TEXTURE2D_DESC& a_outputDesc, bool dedicated)
{
	ResetAfterRetirement();
	if (!a_device || !a_context || !a_scene ||
		a_outputDesc.Width == 0 || a_outputDesc.Height == 0 ||
		a_outputDesc.SampleDesc.Count != 1) {
		return false;
	}

	outputWidth_ = a_outputDesc.Width;
	outputHeight_ = a_outputDesc.Height;
	format_ = a_outputDesc.Format;
	if (!Matches(a_scene)) { ResetAfterRetirement(); return false; }

	D3D11_TEXTURE2D_DESC snapshotDesc = a_outputDesc;
	snapshotDesc.MipLevels = 1;
	snapshotDesc.ArraySize = 1;
	snapshotDesc.SampleDesc.Count = 1;
	snapshotDesc.SampleDesc.Quality = 0;
	snapshotDesc.Usage = D3D11_USAGE_DEFAULT;
	snapshotDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	snapshotDesc.CPUAccessFlags = 0;
	snapshotDesc.MiscFlags = 0;
	if (FAILED(a_device->CreateTexture2D(
			&snapshotDesc, nullptr, &nativeComposedSnapshot_)) ||
		FAILED(a_device->CreateShaderResourceView(
			nativeComposedSnapshot_.Get(), nullptr, &nativeComposedSnapshotSRV_)) ||
		FAILED(a_device->CreateShaderResourceView(
			a_scene, nullptr, &upscaleOutputSRV_))) {
		ResetAfterRetirement();
		return false;
	}

	D3D11_TEXTURE2D_DESC uiDesc = snapshotDesc;
	// The tagged UI texture supports SRV, RTV and UAV bindings.
	// TheosRenderPipeline writes through the UAV; the
	// remaining bindings keep the resource valid for Streamline and later
	// composition modes.
	uiDesc.BindFlags =
		D3D11_BIND_SHADER_RESOURCE |
		D3D11_BIND_RENDER_TARGET |
		D3D11_BIND_UNORDERED_ACCESS;
	if (FAILED(a_device->CreateTexture2D(&uiDesc, nullptr, &nativeUIColorAndAlpha_)) ||
		FAILED(a_device->CreateShaderResourceView(
			nativeUIColorAndAlpha_.Get(), nullptr, &nativeUIColorAndAlphaSRV_)) ||
		FAILED(a_device->CreateRenderTargetView(
			nativeUIColorAndAlpha_.Get(), nullptr, &nativeUIColorAndAlphaRTV_)) ||
		FAILED(a_device->CreateUnorderedAccessView(
			nativeUIColorAndAlpha_.Get(), nullptr, &nativeUIColorAndAlphaUAV_))) {
		ResetAfterRetirement();
		return false;
	}

	nativeUITextureMode_ = dedicated;
	if (nativeUITextureMode_) {
		D3D11_TEXTURE2D_DESC renderDesc = snapshotDesc;
		// The draw attachment needs only an RTV; the separate tagged texture
		// above retains stable SRV/RTV/UAV identity across frames.
		renderDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
		D3D11_TEXTURE2D_DESC compositionDesc = snapshotDesc;
		compositionDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(a_device->CreateTexture2D(&renderDesc, nullptr, &nativeUIRenderTexture_)) ||
			FAILED(a_device->CreateRenderTargetView(
				nativeUIRenderTexture_.Get(), nullptr, &nativeUIRenderRTV_)) ||
			FAILED(a_device->CreateTexture2D(
				&compositionDesc, nullptr, &nativeUICompositionTexture_)) ||
			FAILED(a_device->CreateUnorderedAccessView(
				nativeUICompositionTexture_.Get(), nullptr, &nativeUICompositionUAV_))) {
			nativeUIRenderTexture_.Reset();
			nativeUIRenderRTV_.Reset();
			nativeUICompositionTexture_.Reset();
			nativeUICompositionUAV_.Reset();
			nativeUITextureMode_ = false;
			spdlog::warn(
				"[NativeUI] dedicated UI texture allocation failed; using Hudless Detection");
		}
	}

	static constexpr char kShader[] = R"(
Texture2D<float4> FinalColor : register(t0);
Texture2D<float4> SceneColor : register(t1);
RWTexture2D<float4> UIColorAndAlpha : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width;
    uint height;
    FinalColor.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const int3 pixel = int3(id.xy, 0);
    const float4 finalColor = FinalColor.Load(pixel);
    const float4 sceneColor = SceneColor.Load(pixel);
    const float3 difference = abs(finalColor.rgb - sceneColor.rgb);
    const bool unchanged = all(difference <= 0.001);
    UIColorAndAlpha[id.xy] = unchanged ?
        float4(0.0, 0.0, 0.0, 0.0) : float4(finalColor.rgb, 1.0);
}
)";
	Microsoft::WRL::ComPtr<ID3DBlob> shader;
	Microsoft::WRL::ComPtr<ID3DBlob> errors;
	const auto compileResult = D3DCompile(
		kShader,
		sizeof(kShader) - 1,
		"TheosRenderPipelineNativeUIColorAndAlphaCS",
		nullptr,
		nullptr,
		"main",
		"cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		0,
		&shader,
		&errors);
	if (FAILED(compileResult) || !shader ||
		FAILED(a_device->CreateComputeShader(
			shader->GetBufferPointer(),
			shader->GetBufferSize(),
			nullptr,
			&nativeUIExtractionShader_))) {
		spdlog::warn(
			"[NativeUI] native UI extraction shader creation failed result=0x{:08X} details={}",
			static_cast<std::uint32_t>(compileResult),
			errors ? static_cast<const char*>(errors->GetBufferPointer()) : "none");
		ResetAfterRetirement();
		return false;
	}

	if (nativeUITextureMode_) {
		static constexpr char kCompositionShader[] = R"(
Texture2D<float4> SceneColor : register(t0);
Texture2D<float4> UIColorAndAlpha : register(t1);
RWTexture2D<float4> ComposedColor : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width;
    uint height;
    SceneColor.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const int3 pixel = int3(id.xy, 0);
    const float4 scene = SceneColor.Load(pixel);
    const float4 ui = UIColorAndAlpha.Load(pixel);
    // Premultiplied source-over color with maximum coverage alpha.
    ComposedColor[id.xy] = float4(
        ui.rgb + scene.rgb * (1.0 - ui.a),
        max(ui.a, scene.a));
}
)";
		shader.Reset();
		errors.Reset();
		const auto compositionCompileResult = D3DCompile(
			kCompositionShader,
			sizeof(kCompositionShader) - 1,
			"TheosRenderPipelineDedicatedNativeUICompositionCS",
			nullptr,
			nullptr,
			"main",
			"cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3,
			0,
			&shader,
			&errors);
		if (FAILED(compositionCompileResult) || !shader ||
			FAILED(a_device->CreateComputeShader(
				shader->GetBufferPointer(),
				shader->GetBufferSize(),
				nullptr,
				&nativeUICompositionShader_))) {
			spdlog::warn(
				"[NativeUI] dedicated UI composition shader unavailable result=0x{:08X} details={}; using Hudless Detection",
				static_cast<std::uint32_t>(compositionCompileResult),
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "none");
			nativeUIRenderTexture_.Reset();
			nativeUIRenderRTV_.Reset();
			nativeUICompositionTexture_.Reset();
			nativeUICompositionUAV_.Reset();
			nativeUICompositionShader_.Reset();
			nativeUITextureMode_ = false;
		}
	}

	const float transparent[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
	a_context->ClearUnorderedAccessViewFloat(nativeUIColorAndAlphaUAV_.Get(), transparent);
	nativeUIExtractionAvailable_ = true;
	spdlog::info(
		"[NativeUI] native UI resources ready method={} extent={}x{} format={}",
		nativeUITextureMode_ ? "UI Texture" : "Hudless Detection",
		a_outputDesc.Width,
		a_outputDesc.Height,
		static_cast<std::uint32_t>(a_outputDesc.Format));
	return true;
}

bool NativeUIComposition::Extract(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_presentation)
{
	if (!nativeUIExtractionAvailable_ || !a_context || !Matches(a_presentation) ||
		!nativeComposedSnapshot_ || !nativeComposedSnapshotSRV_ ||
		!upscaleOutputSRV_ || !nativeUIColorAndAlphaUAV_ || !nativeUIExtractionShader_) {
		return false;
	}

	a_context->CopyResource(nativeComposedSnapshot_.Get(), a_presentation);

	Microsoft::WRL::ComPtr<ID3D11ComputeShader> savedShader;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> savedSRVs[2];
	Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> savedUAV;
	a_context->CSGetShader(savedShader.GetAddressOf(), nullptr, nullptr);
	ID3D11ShaderResourceView* savedSRVRaw[2]{};
	a_context->CSGetShaderResources(0, 2, savedSRVRaw);
	for (std::size_t index = 0; index < std::size(savedSRVs); ++index) {
		savedSRVs[index].Attach(savedSRVRaw[index]);
	}
	ID3D11UnorderedAccessView* savedUAVRaw = nullptr;
	a_context->CSGetUnorderedAccessViews(0, 1, &savedUAVRaw);
	savedUAV.Attach(savedUAVRaw);

	a_context->CSSetShader(nativeUIExtractionShader_.Get(), nullptr, 0);
	ID3D11ShaderResourceView* sourceViews[]{
		nativeComposedSnapshotSRV_.Get(),
		upscaleOutputSRV_.Get()
	};
	a_context->CSSetShaderResources(0, static_cast<UINT>(std::size(sourceViews)), sourceViews);
	ID3D11UnorderedAccessView* outputViews[]{ nativeUIColorAndAlphaUAV_.Get() };
	a_context->CSSetUnorderedAccessViews(0, 1, outputViews, nullptr);
	a_context->Dispatch((outputWidth_ + 7) / 8, (outputHeight_ + 7) / 8, 1);

	ID3D11ShaderResourceView* nullSRVs[2]{};
	ID3D11UnorderedAccessView* nullUAVs[1]{};
	a_context->CSSetShaderResources(0, 2, nullSRVs);
	a_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	ID3D11ShaderResourceView* restoreSRVs[]{ savedSRVs[0].Get(), savedSRVs[1].Get() };
	ID3D11UnorderedAccessView* restoreUAVs[]{ savedUAV.Get() };
	a_context->CSSetShader(savedShader.Get(), nullptr, 0);
	a_context->CSSetShaderResources(0, 2, restoreSRVs);
	a_context->CSSetUnorderedAccessViews(0, 1, restoreUAVs, nullptr);

	++nativeUIExtractionCount_;
	if (nativeUIExtractionCount_ <= 3 || nativeUIExtractionCount_ % 600 == 0) {
		spdlog::info(
			"[NativeUI] native UI color-and-alpha extracted frame={} extent={}x{}",
			nativeUIExtractionCount_,
			outputWidth_,
			outputHeight_);
	}
	return true;
}

bool NativeUIComposition::Compose(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_presentation)
{
	if (!nativeUITextureMode_ || !nativeUIExtractionAvailable_ || !a_context ||
        !Matches(a_presentation) || !nativeComposedSnapshot_ || !nativeComposedSnapshotSRV_ ||
		!nativeUIRenderTexture_ || !nativeUIColorAndAlpha_ || !nativeUIColorAndAlphaSRV_ ||
		!nativeUICompositionTexture_ || !nativeUICompositionUAV_ ||
		!nativeUICompositionShader_) {
		return false;
	}

	// The draw attachment and tagged UI texture are distinct.
	// Copying here retains stable Streamline resource identity while
	// allowing the UI render target to be cleared and reused next frame.
	a_context->CopyResource(nativeUIColorAndAlpha_.Get(), nativeUIRenderTexture_.Get());
	return ComposeLayer(a_context, a_presentation, nativeUIColorAndAlphaSRV_.Get());
}

bool NativeUIComposition::ComposeOverlay(ID3D11DeviceContext* context, ID3D11Texture2D* presentation,
    ID3D11ShaderResourceView* overlay)
{
    if (!nativeUITextureMode_ || !nativeUIExtractionAvailable_ || !context || !overlay || !Matches(presentation)) { return false; }
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    overlay->GetResource(&resource);
    if (!resource || FAILED(resource.As(&texture)) || !Matches(texture.Get()) || texture.Get() == presentation) { return false; }
    return ComposeLayer(context, presentation, overlay);
}

bool NativeUIComposition::ComposeLayer(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_presentation,
    ID3D11ShaderResourceView* layer)
{
	// Preserve drawing performed after the upscaler copied its result. A separate
	// snapshot avoids reading and writing the presentation texture simultaneously.
	a_context->CopyResource(nativeComposedSnapshot_.Get(), a_presentation);

	Microsoft::WRL::ComPtr<ID3D11ComputeShader> savedShader;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> savedSRVs[2];
	Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> savedUAV;
	a_context->CSGetShader(savedShader.GetAddressOf(), nullptr, nullptr);
	ID3D11ShaderResourceView* savedSRVRaw[2]{};
	a_context->CSGetShaderResources(0, 2, savedSRVRaw);
	for (std::size_t index = 0; index < std::size(savedSRVs); ++index) {
		savedSRVs[index].Attach(savedSRVRaw[index]);
	}
	ID3D11UnorderedAccessView* savedUAVRaw = nullptr;
	a_context->CSGetUnorderedAccessViews(0, 1, &savedUAVRaw);
	savedUAV.Attach(savedUAVRaw);

	a_context->CSSetShader(nativeUICompositionShader_.Get(), nullptr, 0);
	ID3D11ShaderResourceView* sourceViews[]{
		nativeComposedSnapshotSRV_.Get(),
		layer
	};
	a_context->CSSetShaderResources(0, static_cast<UINT>(std::size(sourceViews)), sourceViews);
	ID3D11UnorderedAccessView* outputViews[]{ nativeUICompositionUAV_.Get() };
	a_context->CSSetUnorderedAccessViews(0, 1, outputViews, nullptr);
	a_context->Dispatch((outputWidth_ + 7) / 8, (outputHeight_ + 7) / 8, 1);

	ID3D11ShaderResourceView* nullSRVs[2]{};
	ID3D11UnorderedAccessView* nullUAVs[1]{};
	a_context->CSSetShaderResources(0, 2, nullSRVs);
	a_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	ID3D11ShaderResourceView* restoreSRVs[]{ savedSRVs[0].Get(), savedSRVs[1].Get() };
	ID3D11UnorderedAccessView* restoreUAVs[]{ savedUAV.Get() };
	a_context->CSSetShader(savedShader.Get(), nullptr, 0);
	a_context->CSSetShaderResources(0, 2, restoreSRVs);
	a_context->CSSetUnorderedAccessViews(0, 1, restoreUAVs, nullptr);

	a_context->CopyResource(a_presentation, nativeUICompositionTexture_.Get());
	++nativeUIExtractionCount_;
	if (nativeUIExtractionCount_ <= 3 || nativeUIExtractionCount_ % 600 == 0) {
		spdlog::info(
			"[NativeUI] dedicated native UI captured and composed frame={} extent={}x{}",
			nativeUIExtractionCount_,
			outputWidth_,
			outputHeight_);
	}
	return true;
}

void NativeUIComposition::ResetAfterRetirement()
{
	outputWidth_ = outputHeight_ = 0;
	format_ = DXGI_FORMAT_UNKNOWN;
	nativeUIExtractionAvailable_ = false;
	nativeUITextureMode_ = false;
	nativeUICompositionShader_.Reset();
	nativeUICompositionUAV_.Reset();
	nativeUICompositionTexture_.Reset();
	nativeUIRenderRTV_.Reset();
	nativeUIRenderTexture_.Reset();
	nativeUIExtractionShader_.Reset();
	nativeUIColorAndAlphaUAV_.Reset();
	nativeUIColorAndAlphaRTV_.Reset();
	nativeUIColorAndAlphaSRV_.Reset();
	nativeUIColorAndAlpha_.Reset();
	nativeComposedSnapshotSRV_.Reset();
	nativeComposedSnapshot_.Reset();
	upscaleOutputSRV_.Reset();
}

bool NativeUIComposition::Matches(ID3D11Texture2D* texture) const
{
    if (!texture) { return false; }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    return desc.Width == outputWidth_ && desc.Height == outputHeight_ &&
        desc.Format == format_ && desc.SampleDesc.Count == 1 && desc.MipLevels == 1 && desc.ArraySize == 1;
}
}
