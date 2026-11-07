#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <filesystem>
#include <functional>
#include <string>
#include "SourceDLSSGNeuralState.h"
#include "RuntimeStateDiagnostics.h"
#include "../../third_party/amd-nr/dlss5nr.h"
#include "../../third_party/amd-nr/trp_adapter_selection.h"

namespace TheosRenderPipeline::SourceDLSSG {
    // Private experimental AMD owner. The host must Drain before destruction.
    // process records on the host queue; Submitted must follow that actual submission.
    class AmdNeuralPass final {
    public:
        struct API {
            decltype(&dlss5nr_create) create{};
            decltype(&dlss5nr_destroy) destroy{};
            decltype(&dlss5nr_default_settings) defaults{};
            decltype(&dlss5nr_set_settings) settings{};
            decltype(&dlss5nr_process) process{};
            decltype(&dlss5nr_present) present{};
            decltype(&dlss5nr_get_status) status{};
            decltype(&dlss5nr_trp_adapter_selection_version) adapterSelectionVersion{};
        };
        AmdNeuralPass() = default;
#if defined(TRP_AMD_NR_TESTING)
        explicit AmdNeuralPass(API testAPI) : api_(testAPI), testAPI_(true) {}
#endif
        ~AmdNeuralPass();
        AmdNeuralPass(const AmdNeuralPass&) = delete;
        AmdNeuralPass& operator=(const AmdNeuralPass&) = delete;
        static std::filesystem::path Directory();
        static std::filesystem::path RuntimePath();
        static std::filesystem::path ModelPath();
        static const char* Availability(bool amdAdapter);
        static const char* PrerequisiteReason();
        static bool IsAMD(ID3D12Device* device);
        static const char* AdmissionReason(const NeuralOptions& options, bool xeSSReady);
        bool RecoverySafe() const { return !pending_ && !unsafeRecording_; }
        static dlss5nr_settings Settings(const NeuralOptions& options);
        bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                    const NeuralOptions& options, bool reset, bool depthInverted,
                    float scaleX, float scaleY, ID3D12Resource* motion,
                    ID3D12Resource* depth, ID3D12Resource* color);
        bool Submitted(ID3D12CommandQueue* queue);
        bool NeedsRecreation(const NeuralOptions& options, UINT guideWidth, UINT guideHeight,
                             ID3D12Resource* color) const;
        ID3D12Resource* Output() const { return output_.Get(); }
        const std::string& Status() const { return message_; }
        bool Failed() const { return failed_; }
        int ModelState() const { return status_.state; }
        std::uint64_t Submissions() const { return submissions_; }
        std::uint64_t ComposedResults() const { return status_.results; }
        bool Running() const { return AMDOutputActive(status_.state == DLSS5NR_STATE_RUNNING, status_.results, failed_); }
        std::function<void(int, const char*)> log;
    private:
        bool Initialize(ID3D12Device* device);
        bool VerifyHipAdapter(ID3D12Device* device);
        void Refresh();
        void CopyOriginal(ID3D12GraphicsCommandList* list, ID3D12Resource* color);
        static void Log(void* self, int level, const char* text);
        HMODULE module_{}, hip_{};
        std::uint64_t adapterLUID_{};
        std::string adapterPCI_;
        API api_{};
        dlss5nr_context* context_{};
        dlss5nr_status status_{};
        Microsoft::WRL::ComPtr<ID3D12Resource> output_;
        NeuralRendering::Reconstruction reconstruction_{};
        UINT guideWidth_{}, guideHeight_{};
        bool initialized_{}, failed_{}, pending_{}, testAPI_{}, unsafeRecording_{};
        std::string message_{"AMD HIP/ZLUDA NR has not started"};
        std::uint64_t submissions_{}, lastTelemetryTick_{};
        int waitMode_{-1}, lastState_{-1};
        bool telemetryLogged_{};
    };
}
