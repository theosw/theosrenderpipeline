#pragma once
#include "../../extern/dlssg-turing/src/SourceDLSSGMFGContract.h"
#include "../../extern/dlssg-turing/extern/RTX40MFG/midpoint_fix.h"
#include <Windows.h>
#include <filesystem>
#include <string>

struct ID3D12Device;
namespace TheosRenderPipeline::SourceDLSSG
{

	enum class MFGRoute { Unselected, Native, AdaUnlock, AmpereUnlock, TuringUnlock };
	struct MFGSnapshot
	{
		// requested is the saved permission; route is the effective startup choice.
		bool requested{}, adapterVerified{}, wrapperPatched{}, providerPatched{}, temporalReady{}, wrapperBound{};
		MFGRoute route{ MFGRoute::Unselected };
		midpoint_fix::AdapterKind adapter{ midpoint_fix::AdapterKind::Unavailable };
		bool failed{}, unsafeMemory{};
		std::uint32_t attempts{}, temporalFailure{};
		const char* status{ "waiting for MFG startup selection" };
		bool SelectRoute(midpoint_fix::AdapterKind observed)
		{
			adapter = observed;
			// Only the three verified architectures receive TRP's unlocks, which patch kernels
			// built for them. Anything else, including a GPU CUDA could not identify, runs
			// NVIDIA's own frame generation, whose support check decides what works.
			adapterVerified = adapter == midpoint_fix::AdapterKind::Ada || adapter == midpoint_fix::AdapterKind::Ampere || adapter == midpoint_fix::AdapterKind::Turing;
			route = !requested || !adapterVerified ? MFGRoute::Native :
				adapter == midpoint_fix::AdapterKind::Ampere ? MFGRoute::AmpereUnlock :
                adapter == midpoint_fix::AdapterKind::Turing ? MFGRoute::TuringUnlock : MFGRoute::AdaUnlock;
			return true;
		}
		bool UsesAdaUnlock() const { return route == MFGRoute::AdaUnlock; }
		bool UsesAmpereUnlock() const { return route == MFGRoute::AmpereUnlock; }
		bool UsesTuringUnlock() const { return route == MFGRoute::TuringUnlock; }
        bool UsesProviderBackport() const { return UsesAmpereUnlock() || UsesTuringUnlock(); }
        bool UsesCompatibilityUnlock() const { return UsesAdaUnlock() || UsesProviderBackport(); }
		bool Ready() const { return UsesCompatibilityUnlock() && !failed && !unsafeMemory && adapterVerified && wrapperPatched && providerPatched && temporalReady && wrapperBound; }
	};
	// Used only by the single source-owned Streamline instance. All mutations run
	// on its initialization/render thread. Modules and published allocations stay resident.
	class MFGUnlock
	{
	public:
		void Configure(bool requested);
		// Chooses the route before the Streamline folder is fixed: the RTX 20/30
		// backport needs its own DLSS-G build. BeforeStreamline then prepares it.
		// Both return false when frame generation cannot start but nothing was
		// patched (see MFGStartupPolicy.h): Snapshot().status holds the reason and
		// the backend continues on its startup transport instead of closing Skyrim.
		[[nodiscard]] bool SelectAdapter(ID3D12Device* device);
		// Developer test (Universal): treat an RTX 40 adapter as RTX 30 and run the SM86 path.
		void ConfigureEmulation(bool emulateRTX30) { emulateRTX30_ = emulateRTX30; }
		[[nodiscard]] bool BeforeStreamline(ID3D12Device* device, const std::filesystem::path& directory);
		void EnterStartupScope() noexcept;
		void LeaveStartupScope() noexcept;
		struct StartupScope {
			MFGUnlock& owner;
			explicit StartupScope(MFGUnlock& value) : owner(value) { owner.EnterStartupScope(); }
			~StartupScope() { owner.LeaveStartupScope(); }
			StartupScope(const StartupScope&) = delete;
		};
		void Prepare(ID3D12Device* device, const std::filesystem::path& directory);
		void BindWrapper(const void* setOptions);
		void Tick();
		const MFGSnapshot& Snapshot() const { return state_; }
		bool NvidiaOverride() const { return nvidiaOverride_; }
	private:
		[[noreturn]] void Fail(const char* reason);
		// Fail with this guidance instead of the route's general advice.
		[[noreturn]] void Fail(const char* reason, const std::string& guidance);
		// RTX 20/30 only: refuses a driver too old to compile DLSS-G's kernels.
		bool CheckBackportDriver(ID3D12Device* device);
		// Frame generation refused before any patch: logs, keeps the reason, returns false.
		bool Refuse(std::string reason);
		std::string refusal_;
		// Ada only, while no patch is in an uncertain state: MFG stays off for the
		// session and DLSS-G continues at x2. Anything else is fatal, as before.
		void StandDown(const char* reason);
		std::string standDownStatus_;
		bool nvidiaOverride_{}, emulateRTX30_{};
		MFGSnapshot state_;
		bool started_{};
		HMODULE wrapper_{}, provider_{};
		std::filesystem::path providerPath_;
		std::uint64_t ticks_{};
	};
}
