#pragma once
#include "SourceDLSSGMFGContract.h"
#include "../../extern/RTX40MFG/midpoint_fix.h"
#include <Windows.h>
#include <filesystem>

struct ID3D12Device;
namespace TheosRenderPipeline::SourceDLSSG
{
	enum class MFGRoute { Unselected, Native, AdaUnlock, AmpereUnlock };
	struct MFGSnapshot
	{
		// requested is the saved permission; route is the effective startup choice.
		bool requested{}, adapterVerified{}, wrapperPatched{}, providerPatched{}, temporalReady{}, wrapperBound{};
		MFGRoute route{ MFGRoute::Unselected };
		bool failed{}, unsafeMemory{};
		std::uint32_t attempts{}, temporalFailure{};
		const char* status{ "waiting for MFG startup selection" };
		bool SelectRoute(midpoint_fix::AdapterKind adapter)
		{
			if (requested && adapter == midpoint_fix::AdapterKind::Unavailable) { return false; }
			adapterVerified = adapter == midpoint_fix::AdapterKind::Ada || adapter == midpoint_fix::AdapterKind::Ampere;
			route = !requested || !adapterVerified ? MFGRoute::Native :
				adapter == midpoint_fix::AdapterKind::Ampere ? MFGRoute::AmpereUnlock : MFGRoute::AdaUnlock;
			return true;
		}
		bool UsesAdaUnlock() const { return route == MFGRoute::AdaUnlock; }
		bool UsesAmpereUnlock() const { return route == MFGRoute::AmpereUnlock; }
		bool UsesCompatibilityUnlock() const { return UsesAdaUnlock() || UsesAmpereUnlock(); }
		bool Ready() const { return UsesCompatibilityUnlock() && !failed && !unsafeMemory && adapterVerified && wrapperPatched && providerPatched && temporalReady && wrapperBound; }
	};
	// Used only by the single source-owned Streamline instance. All mutations run
	// on its initialization/render thread. Modules and published allocations stay resident.
	class MFGUnlock
	{
	public:
		void Configure(bool requested);
		void BeforeStreamline(ID3D12Device* device, const std::filesystem::path& directory);
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
	private:
		[[noreturn]] void Fail(const char* reason);
		MFGSnapshot state_;
		bool started_{};
		HMODULE wrapper_{}, provider_{};
		std::filesystem::path providerPath_;
		std::uint64_t ticks_{};
	};
}
