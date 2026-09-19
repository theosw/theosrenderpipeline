#include "DLSSBackend.h"
#include "RenderPipeline.h"
#include "GameSwapChain.h"
#include "NativeInput.h"
#include "NvidiaHost.h"
#include "CommunityShaderIntegration.h"
#include "SourceDLSSGBackend.h"
#include "SourceDLSSGCamera.h"
#include "SourceFrameGeneration.h"
#include <PCH.h>

HRESULT NvidiaHost::CreateSwapChain(IDXGIFactory* a_factory, ID3D11Device* a_device, DXGI_SWAP_CHAIN_DESC* a_desc, IDXGISwapChain** a_swapChain)
{
    if (FAILED(FailureResult())) { return FailureResult(); }
    if (!a_factory || !a_device || !a_desc || !a_swapChain)
    {
        status_ = "NVIDIA DLSS-G swapchain inputs are incomplete";
        return E_INVALIDARG;
    }
    const auto* upscalerSettings = RenderPipeline::GetSingleton();
    sourceUpscalerSettings_.Initialize(
        {upscalerSettings->mUpscaleType, upscalerSettings->mQualityLevel, upscalerSettings->mDLSSPreset,
         upscalerSettings->mSharpening, upscalerSettings->mAutoExposure});
    logger::info("[SourceUpscaler] startup size authority=QualityLevel mode={} "
                 "quality={}",
                 upscalerSettings->mUpscaleType, upscalerSettings->mQualityLevel);

    splitSourceRuntimeFailureLogged_ = false;

    // Preserve the tested two-buffer native presentation contract. The outer
    // wrapper publishes one stable render-sized buffer independently of it.
    outputWindow_ = a_desc->OutputWindow;
    TheosRenderPipeline::ReShadeIntegration::Get().Discover(outputWindow_);
    logger::info("[ReShade] {}", TheosRenderPipeline::ReShadeIntegration::Get().Status());
    const auto requestedBufferCount = a_desc->BufferCount;
    a_desc->BufferCount = 2;
    logger::info("[NvidiaHost] normalized swapchain buffer count requested={} proxy=2", requestedBufferCount);

    // Start generation disabled
    // before proxy creation so an undocumented backend default cannot affect the
    // first proxied Present while frame inputs are still unavailable.
    SetRuntimeEnabled(false);

    const auto& settings = SourceFrameGeneration::GetSingleton()->settings;
    auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    backend.ConfigureReflex(static_cast<sl::ReflexMode>(settings.sourceDLSSG.reflexMode));
    backend.ConfigureOutputFPSLimit(settings.sourceDLSSG.outputFPSLimit);
    backend.ConfigureGeneration(settings.sourceDLSSG.generation);
    backend.ConfigureMFGUnlock(settings.sourceDLSSGMFGUnlock);
    TheosRenderPipeline::SourceDLSSG::NeuralOptions options;
    options.runtimePath = settings.neuralRenderingRuntimePath;
    options.enabled = settings.sourceDLSSG.neuralEnabled && !options.runtimePath.empty() &&
        (TheosRenderPipeline::CommunityShaders::Active() || RenderPipeline::GetSingleton()->mUpscaleType == DLSS);
    options.tuning = settings.sourceDLSSG.neuralTuning;
    options.reconstruction = settings.sourceDLSSG.neuralReconstruction;
    options.beforeUpscaling = settings.sourceDLSSG.neuralBeforeUpscaling;
    options.passes = settings.sourceDLSSG.neuralPasses;
    backend.ConfigureNeuralRendering(std::move(options));

    const auto result = TheosRenderPipeline::SourceDLSSG::Backend::Get().CreateSwapChain(
        a_factory, a_device, *a_desc, SourceFrameGeneration::GetSingleton()->settings.sourceDLSSGStreamlineDirectory, a_swapChain);
    if (FAILED(result) || !*a_swapChain)
    {
        status_ = TheosRenderPipeline::SourceDLSSG::Backend::Get().Status();
        return FAILED(result) ? result : E_FAIL;
    }
    innerSwapChain_ = *a_swapChain;
    nativeUIContexts_.ResetAfterRetirement();
    device_.Reset();
    context_.Reset();
    const auto deviceResult = innerSwapChain_->GetDevice(IID_PPV_ARGS(&device_));
    if (FAILED(deviceResult) || !device_)
    {
        status_ = std::format("NVIDIA DLSS-G inner D3D11 device query failed (0x{:08X})", static_cast<std::uint32_t>(deviceResult));
        return E_FAIL;
    }
    device_->GetImmediateContext(&context_);
    if (!CreateGameFacingResources(*a_swapChain))
    {
        status_ = "NVIDIA DLSS-G stable game-facing buffer creation failed";
        (*a_swapChain)->Release();
        *a_swapChain = nullptr;
        return E_FAIL;
    }

    // Transfer the backend-created swapchain into an outer wrapper before the
    // factory hook returns. D3D11CreateDeviceAndSwapChain can cache GetBuffer(0)
    // during its own call, so installing a hook after it returns is too late.
    nativeUIPass_.Frame().NewSession();
    auto* outer = new (std::nothrow) GameSwapChain(*a_swapChain, this);
    if (!outer)
    {
        status_ = "NVIDIA DLSS-G game-facing swapchain wrapper allocation failed";
        (*a_swapChain)->Release();
        *a_swapChain = nullptr;
        return E_OUTOFMEMORY;
    }
    (*a_swapChain)->Release();
    outerSwapChain_ = outer;
    *a_swapChain = outer;
    proxyActive_ = true;
    // Proxy construction can replace or reinitialize the live presentation
    // state. Do not trust the pre-proxy cache across that boundary. Re-issue the
    // disable against the completed proxy before its first game-facing Present.
    frameGenerationStateKnown_ = false;
    SetRuntimeEnabled(false);
    status_ = "NVIDIA DLSS-G proxy active; waiting for complete frame inputs";
    logger::info("[NvidiaHost] source swapchain active format={}", static_cast<std::uint32_t>(a_desc->BufferDesc.Format));
    logger::info("[NvidiaHost] outer stable-buffer swapchain returned during "
                 "factory creation");
    return S_OK;
}

bool NvidiaHost::CreateGameFacingResources(IDXGISwapChain* a_swapChain)
{
    EndNativeUIPass();
    gameTargets_.ResetGameFacingAfterRetirement();
    nativeUIPass_.ResetEvaluation();
    ReleaseSourceUpscaler();
    sourceUpscalerInitializationPending_ = false;
    presentation_.ResetAfterRetirement();
    if (!a_swapChain || !device_ || !context_)
    {
        return false;
    }

    const auto cacheResult = presentation_.CacheAfterRetirement(a_swapChain);
    if (FAILED(cacheResult))
    {
        logger::error("[CoreHost] inner buffer cache failed result=0x{:08X}", static_cast<std::uint32_t>(cacheResult));
        return false;
    }

    D3D11_TEXTURE2D_DESC outputDesc{};
    presentation_.Buffers().front()->GetDesc(&outputDesc);
    outputWidth_ = outputDesc.Width;
    outputHeight_ = outputDesc.Height;
    TheosRenderPipeline::NativeInput::Publish(outputWidth_, outputHeight_);
    auto* settings = RenderPipeline::GetSingleton();
    int queriedRenderWidth = 0;
    int queriedRenderHeight = 0;
    const auto sizeQuery = [](int width, int height, int quality, int* renderWidth, int* renderHeight) {
        if (TheosRenderPipeline::CommunityShaders::Active()) {
            *renderWidth = width; *renderHeight = height; return true;
        }
        return TheosRenderPipeline::SourceDLSSG::QueryRenderSize(width, height, quality, renderWidth, renderHeight);
    };
    if (!sizeQuery(static_cast<int>(outputWidth_), static_cast<int>(outputHeight_), sourceUpscalerSettings_.Startup().AllocationQuality(),
                   &queriedRenderWidth, &queriedRenderHeight) ||
        queriedRenderWidth <= 0 || queriedRenderHeight <= 0 || queriedRenderWidth > static_cast<int>(outputWidth_) ||
        queriedRenderHeight > static_cast<int>(outputHeight_))
    {
        logger::error("[NvidiaHost] QueryRenderSize rejected output={}x{} "
                      "quality={} result={}x{}",
                      outputWidth_, outputHeight_, settings->mQualityLevel, queriedRenderWidth, queriedRenderHeight);
        presentation_.ResetAfterRetirement();
        return false;
    }
    renderWidth_ = static_cast<UINT>(queriedRenderWidth);
    renderHeight_ = static_cast<UINT>(queriedRenderHeight);

    const auto gameFacingDesc = TheosRenderPipeline::GameFacingTargets::GameFacingDesc(outputDesc, renderWidth_, renderHeight_);
    const auto createResult = gameTargets_.CreateGameFacingAfterRetirement(device_.Get(), outputDesc, renderWidth_, renderHeight_);
    if (FAILED(createResult) || !gameTargets_.GameFacing())
    {
        logger::error("[NvidiaHost] stable game-facing texture creation failed "
                      "{}x{} format={} result=0x{:08X}",
                      gameFacingDesc.Width, gameFacingDesc.Height, static_cast<std::uint32_t>(gameFacingDesc.Format),
                      static_cast<std::uint32_t>(createResult));
        ReleaseSourceUpscaler();
        presentation_.ResetAfterRetirement();
        return false;
    }

    if (TheosRenderPipeline::CommunityShaders::Active()) {
        evaluationFailureLogged_ = false;
        sourceUpscalerInitializationPending_ = true;
        logger::info("[CS Adapter] native game-facing buffer prepared {}x{}; CS owns its upscaling resources", outputWidth_, outputHeight_);
        return true;
    }
    const auto inputDesc = TheosRenderPipeline::GameFacingTargets::UpscaleInputDesc(outputDesc, renderWidth_, renderHeight_);
    const auto inputResult = gameTargets_.CreateUpscaleInputAfterRetirement(device_.Get(), outputDesc, renderWidth_, renderHeight_);
    if (FAILED(inputResult) || !gameTargets_.UpscaleInput())
    {
        logger::error("[NvidiaHost] source-upscale input creation failed {}x{} "
                      "format={} result=0x{:08X}",
                      inputDesc.Width, inputDesc.Height, static_cast<std::uint32_t>(inputDesc.Format), static_cast<std::uint32_t>(inputResult));
        gameTargets_.ResetGameFacingAfterRetirement();
        ReleaseSourceUpscaler();
        presentation_.ResetAfterRetirement();
        return false;
    }

    evaluationFailureLogged_ = false;
    sourceUpscalerInitializationPending_ = true;
    logger::info("[NvidiaHost] outer resources prepared render={}x{} "
                 "output={}x{} format={} innerBuffers={}; source upscaler "
                 "deferred until D3D11 device creation returns",
                 gameFacingDesc.Width, gameFacingDesc.Height, outputWidth_, outputHeight_, static_cast<std::uint32_t>(gameFacingDesc.Format),
                 presentation_.Buffers().size());
    return true;
}

bool NvidiaHost::CompleteStartupAfterDeviceCreation()
{
    if (FAILED(FailureResult())) { return false; }
    if (!proxyActive_)
    {
        return false;
    }
    if (upscalerReady_)
    {
        return true;
    }
    if (!sourceUpscalerInitializationPending_ || presentation_.Buffers().empty())
    {
        return false;
    }

    // Preserve the tested startup boundary: publish the stable render-sized
    // buffer during factory creation, then initialize DLSS only after the
    // original D3D11 creation call and its internal Present have returned.
    sourceUpscalerInitializationPending_ = false;
    D3D11_TEXTURE2D_DESC outputDesc{};
    presentation_.Buffers().front()->GetDesc(&outputDesc);
    const auto expectedRenderWidth = renderWidth_;
    const auto expectedRenderHeight = renderHeight_;
    if (!InitializeSourceUpscaler(outputDesc))
    {
        status_ = std::format("Source DLSS startup failed: {}", status_);
        logger::error("[NvidiaHost] {}", status_);
        return false;
    }
    if (renderWidth_ != expectedRenderWidth || renderHeight_ != expectedRenderHeight)
    {
        logger::error("[NvidiaHost] deferred source upscaler extent mismatch "
                      "actual={}x{} prepared={}x{}",
                      renderWidth_, renderHeight_, expectedRenderWidth, expectedRenderHeight);
        // Retain the initialized feature until teardown proves retirement.
        renderWidth_ = expectedRenderWidth;
        renderHeight_ = expectedRenderHeight;
        status_ = "NVIDIA source upscaler disagreed with the prepared render-size "
                  "contract";
        return false;
    }
    ArmFrameGenerationWarmup();
    TheosRenderPipeline::ReShadeIntegration::Get().Configure(device_.Get(), context_.Get(), {outputWidth_, outputHeight_});
    logger::info("[NvidiaHost] source upscaler initialized after D3D11 startup Present");
    return true;
}

bool NvidiaHost::InitializeSourceUpscaler(const D3D11_TEXTURE2D_DESC& a_outputDesc)
{
    if (TheosRenderPipeline::CommunityShaders::Active()) {
        upscalerReady_ = true;
        splitSourceDLSSActive_ = false;
        status_ = "CS owns upscaling; NVIDIA frame adapter initialized";
        return true;
    }
    if (!device_ || !context_ || renderWidth_ == 0 || renderHeight_ == 0)
    {
        status_ = "TheosRenderPipeline DLSS split source prerequisites are incomplete";
        return false;
    }

    // Allocate the native handoff target before creating the DLSS feature.
    // The source backend imports the completed D3D11 output for presentation.
    const auto outputResult = gameTargets_.CreateUpscaleOutputAfterRetirement(device_.Get(), a_outputDesc);
    if (FAILED(outputResult) || !gameTargets_.UpscaleOutput())
    {
        status_ = std::format("TheosRenderPipeline DLSS native handoff texture creation failed (0x{:08X})", static_cast<std::uint32_t>(outputResult));
        return false;
    }

    auto* dlss = DLSSBackend::GetSingleton();
    dlss->SetupDevice(device_.Get(), context_.Get());
    const auto creation = sourceUpscalerSettings_.BeginSubmission();
    if (!dlss->InitUpscale(static_cast<int>(renderWidth_), static_cast<int>(renderHeight_), static_cast<int>(a_outputDesc.Width),
                           static_cast<int>(a_outputDesc.Height), a_outputDesc.Format, creation.sharpening, creation.autoExposure, creation.preset,
                           creation.AllocationQuality()))
    {
        sourceUpscalerSettings_.Completed(false);
        status_ = "TheosRenderPipeline direct DLSS feature initialization failed";
        gameTargets_.ResetUpscaleOutputAfterRetirement();
        return false;
    }
    // These flags also track feature ownership for teardown. A subsequent
    // contract failure must retain the initialized feature until retirement.
    splitSourceDLSSActive_ = true;
    upscalerReady_ = true;
    if (dlss->RenderWidth() != static_cast<int>(renderWidth_) || dlss->RenderHeight() != static_cast<int>(renderHeight_))
    {
        sourceUpscalerSettings_.Completed(false);
        status_ = "TheosRenderPipeline direct DLSS disagreed with the prepared render extent";
        return false;
    }

    sourceUpscalerSettings_.Completed(true);
    AdoptEffectiveSourceUpscalerSettings();
    if (!CreateNativeUIExtractionResources(a_outputDesc))
    {
        logger::warn("[NvidiaHost] explicit native UI extraction unavailable on "
                     "split source; retaining HUD-less-only fallback");
    }
    status_ = "Source DLSS and NVIDIA frame-generation path ready";
    logger::info("[NvidiaHost] split source initialized owner=TheosRenderPipeline-DLSS render={}x{} "
                 "output={}x{} format={} quality={} evaluator=SourceNvidiaFrameEvaluator",
                 renderWidth_, renderHeight_, a_outputDesc.Width, a_outputDesc.Height, static_cast<std::uint32_t>(a_outputDesc.Format),
                 creation.AllocationQuality());
    return true;
}
