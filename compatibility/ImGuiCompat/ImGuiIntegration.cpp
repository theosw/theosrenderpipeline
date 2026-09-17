#include <SolFGLateOverlayAPI.h>
#include <SolFGStartupOverlayAPI.h>
#include "CursorMapping.h"
#include "ImGuiIntegration.h"
#include "NativeInput.h"
#include "NativeUIBridge.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include "../Shared/FileHash.h"
#include "../Shared/ModuleInfo.h"

#include <Windows.h>
#include <detours/Detours.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace logger = SKSE::log;

namespace
{
	using namespace std::literals;

	// Retain existing adapter settings when upgrading from the companion plugin.
	inline constexpr auto kIniPath = L"Data\\SKSE\\Plugins\\TheosRenderPipelineImGui.ini";
	inline constexpr std::size_t kProducerCount = 8;
	inline constexpr std::size_t kGuardSize = 17;
	inline constexpr std::size_t kCallGuardSize = 5;
	inline constexpr std::size_t kVanityIndex = 5;
	inline constexpr std::size_t kSpellHotbarIndex = 7;

	using RenderDrawData = void (*)(void* a_drawData);
	using UpdateMousePosition = void (*)(void* a_inputManager);
	using VanityProcessMessage = RE::UI_MESSAGE_RESULTS (*)(void* a_menuHost, RE::UIMessage* a_message);
	using GetVanityMenu = void* (*)();

	using TheosRenderPipeline::Compatibility::GetModulePath;
	using TheosRenderPipeline::Compatibility::GetModuleRange;
	using TheosRenderPipeline::Compatibility::HashText;

	struct ProducerProfile
	{
		std::string_view name;
		const wchar_t* moduleName;
		const wchar_t* iniKey;
		std::string_view sha256;
		std::uintptr_t renderDrawDataRva;
		std::array<std::uint8_t, kGuardSize> entryBytes;
	};

	struct ProducerState
	{
		RenderDrawData original{ nullptr };
		UpdateMousePosition originalMouseUpdate{ nullptr };
		VanityProcessMessage originalProcessMessage{ nullptr };
		GetVanityMenu getVanityMenu{ nullptr };
		std::atomic_bool vanityTextDiagnosticsActive{ false };
		std::atomic_bool spellHotbarGeometryDiagnosticsActive{ false };
		std::atomic_uint64_t drawCount{ 0 };
		std::atomic_uint64_t nativeBeginCount{ 0 };
		std::atomic_uint64_t nativeEndCount{ 0 };
		std::atomic_uint64_t rejectCount{ 0 };
		std::atomic_uint64_t mouseRemapCount{ 0 };
		std::atomic_uint64_t cursorSampleCount{ 0 };
		std::atomic_uint64_t cursorNextSampleTick{ 0 };
		std::atomic_uint64_t cursorDrawSampleCount{ 0 };
		std::atomic_uint64_t cursorDrawNextSampleTick{ 0 };
		std::atomic_uint64_t vanityDiagnosticDrawCount{ 0 };
		std::atomic_uint64_t spellHotbarDiagnosticDrawCount{ 0 };
		std::atomic_uint64_t vanityCharacterEventCount{ 0 };
		std::atomic_uint64_t vanityEligibleCharacterCount{ 0 };
		std::atomic_uint64_t vanityBlockedCharacterCount{ 0 };
		std::atomic_uint64_t vanityKeyDownEventCount{ 0 };
		std::atomic_uint64_t vanityMouseDownEventCount{ 0 };
		std::atomic_int vanityLastWantTextInput{ -1 };
		std::atomic_int vanityLastSkyrimTextInputAllowed{ -1 };
	};

	struct CallSiteProfile
	{
		std::uintptr_t rva;
		std::uintptr_t targetRva;
		std::array<std::uint8_t, kCallGuardSize> instructionBytes;
	};

	struct VanityBinaryProfile
	{
		std::string_view label;
		std::string_view sha256;
		std::uintptr_t renderDrawDataRva;
		std::array<std::uint8_t, kGuardSize> renderDrawDataEntryBytes;
		CallSiteProfile mouseUpdateCall;
		bool sourceNormalizesMouse;
		std::uintptr_t processMessageRva;
		std::array<std::uint8_t, 19> processMessageEntryBytes;
		std::uintptr_t getMenuRva;
		std::array<std::uint8_t, 21> getMenuEntryBytes;
	};

	// Each profile identifies an exact supported producer build. Its file hash and an
	// instruction-aligned prologue must match before Detours can write memory.
	inline constexpr std::array<ProducerProfile, kProducerCount> kProfiles{
		ProducerProfile{
			"KreatE 1.5.0"sv,
			L"KreatE.dll",
			L"KreatE",
			"24129130C02B5B1709FAB77C54CAEADA971551FCE530456F701637D10F834413"sv,
			0x32530,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x08, 0xE5, 0xFF, 0xFF, 0xB8, 0xF8, 0x1B, 0x00, 0x00 } },
		ProducerProfile{
			"Immersive Equipment Displays 1.7.4"sv,
			L"ImmersiveEquipmentDisplays.dll",
			L"ImmersiveEquipmentDisplays",
			"E12DA86BE6C3412E1AD91EF7C12D51067F19E023A8D4EF8EC703EE43ACEDC38C"sv,
			0xCBA00,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xE8, 0xE4, 0xFF, 0xFF, 0xB8, 0x18, 0x1C, 0x00, 0x00 } },
		ProducerProfile{
			"Open Animation Replacer 3.1.5"sv,
			L"OpenAnimationReplacer.dll",
			L"OpenAnimationReplacer",
			"8BC22F077C0984BAAC135C94E58643F473C5BEA18648D499116A8A5585C6CCC0"sv,
			0x299DC0,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x08, 0xE5, 0xFF, 0xFF, 0xB8, 0xF8, 0x1B, 0x00, 0x00 } },
		ProducerProfile{
			"Photo Mode 2.0.2"sv,
			L"po3_PhotoMode.dll",
			L"PhotoMode",
			"FF7458FAEAF102037C199E3FE8E34D33429E88F76D32983682D85608D4EF8D9D"sv,
			0x107710,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xB8, 0xE4, 0xFF, 0xFF, 0xB8, 0x48, 0x1C, 0x00, 0x00 } },
		ProducerProfile{
			"dMenu NG 1.3.0"sv,
			L"dmenu.dll",
			L"dMenu",
			"1C0210C500388FAE145D4979F57CFD6192A3A64DE25050C240F67CFF5DD3C3F1"sv,
			0xE5630,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xD8, 0xE4, 0xFF, 0xFF, 0xB8, 0x28, 0x1C, 0x00, 0x00 } },
		ProducerProfile{
			"Skyrim Vanity System 1.4.10"sv,
			L"SkyrimVanitySystem.dll",
			L"SkyrimVanitySystem",
			"58B42886F1C7B441678A5823A39AFB9838F2BF293637157F5220D630712322FA"sv,
			0x163590,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xA8, 0xE4, 0xFF, 0xFF, 0xB8, 0x58, 0x1C, 0x00, 0x00 } },
		ProducerProfile{
			"Detection Meter AE 1.0.8"sv,
			L"MaxsuDetectionMeter.dll",
			L"MaxsuDetectionMeter",
			"A4803DA251251268C42C7986037635DAF502FA697EBAECCD0ADEB12A41550959"sv,
			0x87920,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xF8, 0xE4, 0xFF, 0xFF, 0xB8, 0x08, 0x1C, 0x00, 0x00 } },
		ProducerProfile{
			"SpellHotbar2 0.0.14"sv,
			L"SpellHotbar2.dll",
			L"SpellHotbar2",
			"AB8F82DBA9F8673E3486C783B5910C82B40A5E6630E24B120CFD9936E4113E4B"sv,
			0x2C00D0,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xD8, 0xE4, 0xFF, 0xFF, 0xB8, 0x28, 0x1C, 0x00, 0x00 } }
	};

	// Skyrim Vanity System feeds its private ImGui context from Skyrim's
	// MenuCursor while CursorMenu is open. The native host already aligns its
	// coordinates with Vanity's Win32 backend. The fallback is used only when
	// native input is inactive. Patch only Menu::Draw's existing direct call to
	// UpdateMousePosition. The original function remains byte-for-byte intact,
	// avoiding a prologue trampoline whose stolen instructions contain a
	// PC-relative call.
	// Patched builds can normalize MenuCursor coordinates in Vanity itself. Such
	// profiles still need native ImGui rendering, but must not receive TheosRenderPipeline's
	// legacy mouse call-site patch or the cursor would be scaled twice.
	inline constexpr std::array<VanityBinaryProfile, 3> kVanityBinaryProfiles{
		VanityBinaryProfile{
			"upstream 1.4.10"sv,
			"58B42886F1C7B441678A5823A39AFB9838F2BF293637157F5220D630712322FA"sv,
			0x163590,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xA8, 0xE4, 0xFF, 0xFF, 0xB8, 0x58, 0x1C, 0x00, 0x00 },
			CallSiteProfile{ 0xD4C32, 0x68460, { 0xE8, 0x29, 0x38, 0xF9, 0xFF } },
			false,
			0xE2B80,
			{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x42, 0x08 },
			0xBC2E0,
			{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x0D, 0x64, 0x6B, 0x15, 0x00, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00 } },
		VanityBinaryProfile{
			"1.4.10 live-equipment-labels.1"sv,
			"F1AAE8FBD5E281804B0A488C20D74CA13403D025A30F08502598DBB9EB8B6093"sv,
			0x165F50,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xA8, 0xE4, 0xFF, 0xFF, 0xB8, 0x58, 0x1C, 0x00, 0x00 },
			CallSiteProfile{ 0xD5CA2, 0x68A00, { 0xE8, 0x59, 0x2D, 0xF9, 0xFF } },
			false,
			0xE3B30,
			{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x42, 0x08 },
			0xBD560,
			{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x0D, 0xE4, 0x98, 0x15, 0x00, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00 } },
		VanityBinaryProfile{
			"1.4.10 live-equipment-labels.1 source cursor"sv,
			"EA60F7DBBA438BEDFD27E17D100934AD8285C1162EA3656A640F3D8D9719BFE8"sv,
			0x1D50,
			{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xA8, 0xE4, 0xFF, 0xFF, 0xB8, 0x58, 0x1C, 0x00, 0x00 },
			CallSiteProfile{ 0, 0, {} },
			true,
			0x149110,
			{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x42, 0x08 },
			0x148E00,
			{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x0D, 0x54, 0x00, 0x0D, 0x00, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00 } }
	};
	inline constexpr std::size_t kVanityWantTextInputOffset = 0xB62;
	inline constexpr std::size_t kVanitySkyrimTextInputAllowedOffset = 0xB63;
	inline constexpr std::size_t kVanityFontSizePixelsOffset = 0xAC8;

	std::array<ProducerState, kProducerCount>& GetStates()
	{
		static std::array<ProducerState, kProducerCount> states;
		return states;
	}

	const SolFGLateOverlayAPI::BridgeV1*& GetBridge()
	{
		static const SolFGLateOverlayAPI::BridgeV1* bridge = nullptr;
		return bridge;
	}

	SKSE::Trampoline& GetVanityCallTrampoline()
	{
		static SKSE::Trampoline trampoline{ "TheosRenderPipelineImGui.Vanity" };
		return trampoline;
	}

	using TheosRenderPipeline::Compatibility::Sha256File;



	bool IsProducerEnabled(const ProducerProfile& a_profile)
	{
		const auto allEnabled = ::GetPrivateProfileIntW(L"Adapters", L"EnableAll", 1, kIniPath) != 0;
		return allEnabled && ::GetPrivateProfileIntW(L"Adapters", a_profile.iniKey, 1, kIniPath) != 0;
	}

	bool VanityTextDiagnosticsEnabled()
	{
		return ::GetPrivateProfileIntW(L"Diagnostics", L"VanityTextInput", 0, kIniPath) != 0;
	}

	bool SpellHotbarGeometryDiagnosticsEnabled()
	{
		return ::GetPrivateProfileIntW(L"Diagnostics", L"SpellHotbarGeometry", 0, kIniPath) != 0;
	}

	bool ResolveBridge()
	{
		const auto bridge = TheosRenderPipeline::NativeUIBridge::LateOverlay(SolFGLateOverlayAPI::kVersion1);
		if (!bridge || bridge->structSize < sizeof(SolFGLateOverlayAPI::BridgeV1) ||
			bridge->version != SolFGLateOverlayAPI::kVersion1 ||
			!bridge->query || !bridge->begin || !bridge->end) {
			logger::warn("compatibility layer disabled: TheosRenderPipeline late-overlay bridge v1 is unavailable");
			return false;
		}
		GetBridge() = bridge;
		return true;
	}

	// The first four fields of ImDrawData have been stable across all profiled
	// backends. Reading only this prefix lets us skip empty frames without
	// linking to or assuming the producer's private ImGui version.
	struct DrawDataHeader
	{
		bool valid{ false };
		std::array<std::byte, 3> padding{};
		std::int32_t commandListCount{ 0 };
		std::int32_t totalIndexCount{ 0 };
		std::int32_t totalVertexCount{ 0 };
	};
	static_assert(sizeof(DrawDataHeader) == 16);

	bool HasDrawCommands(const void* a_drawData)
	{
		if (!a_drawData) {
			return false;
		}
		DrawDataHeader header{};
		std::memcpy(std::addressof(header), a_drawData, sizeof(header));
		return header.valid && header.commandListCount > 0 && header.commandListCount < 65536 &&
			header.totalIndexCount >= 0 && header.totalVertexCount >= 0;
	}

	// Skyrim Vanity System 1.4.10 pins Dear ImGui 1.92.7 WIP at commit
	// 994ca12b29381d6e33b771cfad6bfefa5d0ced90. This diagnostic-only prefix is
	// read only after the complete Vanity DLL hash and renderer guard match.
	struct VanityDrawDataSnapshot
	{
		DrawDataHeader header{};
		std::int32_t commandListsSize{ 0 };
		std::int32_t commandListsCapacity{ 0 };
		const void* commandListsData{ nullptr };
		float displayPosX{ 0.0F };
		float displayPosY{ 0.0F };
		float displaySizeX{ 0.0F };
		float displaySizeY{ 0.0F };
		float framebufferScaleX{ 0.0F };
		float framebufferScaleY{ 0.0F };
	};
	static_assert(sizeof(VanityDrawDataSnapshot) == 56);

	// SpellHotbar2 0.0.14 embeds Dear ImGui 1.91.6. These read-only prefixes
	// mirror the exact tagged layouts and are accessed only after the DLL hash
	// and RenderDrawData prologue both match. No producer memory is modified.
	using SpellHotbarDrawDataSnapshot = VanityDrawDataSnapshot;

	struct SpellHotbarVectorSnapshot
	{
		std::int32_t size{ 0 };
		std::int32_t capacity{ 0 };
		const void* data{ nullptr };
	};
	static_assert(sizeof(SpellHotbarVectorSnapshot) == 16);

	struct SpellHotbarDrawListSnapshot
	{
		SpellHotbarVectorSnapshot commands{};
		SpellHotbarVectorSnapshot indices{};
		SpellHotbarVectorSnapshot vertices{};
	};
	static_assert(sizeof(SpellHotbarDrawListSnapshot) == 48);

	struct SpellHotbarDrawVertex
	{
		float positionX{ 0.0F };
		float positionY{ 0.0F };
		float uvX{ 0.0F };
		float uvY{ 0.0F };
		std::uint32_t color{ 0 };
	};
	static_assert(sizeof(SpellHotbarDrawVertex) == 20);

	struct SpellHotbarDrawCommand
	{
		float clipLeft{ 0.0F };
		float clipTop{ 0.0F };
		float clipRight{ 0.0F };
		float clipBottom{ 0.0F };
		std::uint64_t textureId{ 0 };
		std::uint32_t vertexOffset{ 0 };
		std::uint32_t indexOffset{ 0 };
		std::uint32_t elementCount{ 0 };
		std::uint32_t padding{ 0 };
		const void* callback{ nullptr };
		const void* callbackData{ nullptr };
		std::int32_t callbackDataSize{ 0 };
		std::int32_t callbackDataOffset{ 0 };
	};
	static_assert(sizeof(SpellHotbarDrawCommand) == 64);

	void LogSpellHotbarGeometry(const void* a_drawData, const std::uint64_t a_draw)
	{
		auto& state = GetStates()[kSpellHotbarIndex];
		if (!state.spellHotbarGeometryDiagnosticsActive.load(std::memory_order_acquire) || !a_drawData) {
			return;
		}

		const auto sample = ++state.spellHotbarDiagnosticDrawCount;
		if (sample > 8) {
			return;
		}

		SpellHotbarDrawDataSnapshot drawData{};
		std::memcpy(std::addressof(drawData), a_drawData, sizeof(drawData));
		const bool validGeometry =
			std::isfinite(drawData.displayPosX) && std::isfinite(drawData.displayPosY) &&
			std::isfinite(drawData.displaySizeX) && std::isfinite(drawData.displaySizeY) &&
			std::isfinite(drawData.framebufferScaleX) && std::isfinite(drawData.framebufferScaleY) &&
			drawData.displaySizeX > 0.0F && drawData.displaySizeX <= 16384.0F &&
			drawData.displaySizeY > 0.0F && drawData.displaySizeY <= 16384.0F &&
			drawData.framebufferScaleX > 0.0F && drawData.framebufferScaleX <= 8.0F &&
			drawData.framebufferScaleY > 0.0F && drawData.framebufferScaleY <= 8.0F &&
			drawData.commandListsSize == drawData.header.commandListCount &&
			drawData.commandListsSize > 0 && drawData.commandListsSize < 4096 &&
			drawData.commandListsData;
		if (!validGeometry) {
			logger::warn("SpellHotbar2 geometry sample {} rejected: invalid ImDrawData prefix", sample);
			return;
		}

		float vertexMinX = std::numeric_limits<float>::max();
		float vertexMinY = std::numeric_limits<float>::max();
		float vertexMaxX = std::numeric_limits<float>::lowest();
		float vertexMaxY = std::numeric_limits<float>::lowest();
		float clipMinX = std::numeric_limits<float>::max();
		float clipMinY = std::numeric_limits<float>::max();
		float clipMaxX = std::numeric_limits<float>::lowest();
		float clipMaxY = std::numeric_limits<float>::lowest();
		std::uint64_t commands = 0;
		std::uint64_t imageCommands = 0;
		std::uint64_t malformedLists = 0;

		const auto lists = reinterpret_cast<const void* const*>(drawData.commandListsData);
		for (std::int32_t listIndex = 0; listIndex < drawData.commandListsSize; ++listIndex) {
			if (!lists[listIndex]) {
				++malformedLists;
				continue;
			}
			SpellHotbarDrawListSnapshot list{};
			std::memcpy(std::addressof(list), lists[listIndex], sizeof(list));
			const bool validList =
				list.commands.size >= 0 && list.commands.size < 65536 &&
				list.vertices.size >= 0 && list.vertices.size <= drawData.header.totalVertexCount &&
				(list.commands.size == 0 || list.commands.data) &&
				(list.vertices.size == 0 || list.vertices.data);
			if (!validList) {
				++malformedLists;
				continue;
			}

			const auto vertices = reinterpret_cast<const SpellHotbarDrawVertex*>(list.vertices.data);
			for (std::int32_t vertexIndex = 0; vertexIndex < list.vertices.size; ++vertexIndex) {
				const auto& vertex = vertices[vertexIndex];
				if (!std::isfinite(vertex.positionX) || !std::isfinite(vertex.positionY)) {
					continue;
				}
				vertexMinX = (std::min)(vertexMinX, vertex.positionX);
				vertexMinY = (std::min)(vertexMinY, vertex.positionY);
				vertexMaxX = (std::max)(vertexMaxX, vertex.positionX);
				vertexMaxY = (std::max)(vertexMaxY, vertex.positionY);
			}

			const auto drawCommands = reinterpret_cast<const SpellHotbarDrawCommand*>(list.commands.data);
			for (std::int32_t commandIndex = 0; commandIndex < list.commands.size; ++commandIndex) {
				const auto& command = drawCommands[commandIndex];
				if (!std::isfinite(command.clipLeft) || !std::isfinite(command.clipTop) ||
					!std::isfinite(command.clipRight) || !std::isfinite(command.clipBottom)) {
					continue;
				}
				++commands;
				imageCommands += command.textureId != 0 ? 1U : 0U;
				clipMinX = (std::min)(clipMinX, command.clipLeft);
				clipMinY = (std::min)(clipMinY, command.clipTop);
				clipMaxX = (std::max)(clipMaxX, command.clipRight);
				clipMaxY = (std::max)(clipMaxY, command.clipBottom);
			}
		}

		SolFGLateOverlayAPI::FrameV1 nativeFrame{};
		const auto bridge = GetBridge();
		const bool nativeAvailable = bridge && bridge->query(std::addressof(nativeFrame)) != 0;
		const auto renderSize = RE::BSGraphics::Renderer::GetScreenSize();
		logger::info(
			"SpellHotbar2 geometry sample {} draw {}: display=({:.1f},{:.1f}) {:.1f}x{:.1f} framebufferScale=({:.3f},{:.3f}) render={}x{} native={}x{} lists={} commands={} textured={} malformed={} vertices=({:.1f},{:.1f})-({:.1f},{:.1f}) clips=({:.1f},{:.1f})-({:.1f},{:.1f})",
			sample,
			a_draw,
			drawData.displayPosX,
			drawData.displayPosY,
			drawData.displaySizeX,
			drawData.displaySizeY,
			drawData.framebufferScaleX,
			drawData.framebufferScaleY,
			renderSize.width,
			renderSize.height,
			nativeAvailable ? nativeFrame.width : 0,
			nativeAvailable ? nativeFrame.height : 0,
			drawData.commandListsSize,
			commands,
			imageCommands,
			malformedLists,
			vertexMinX,
			vertexMinY,
			vertexMaxX,
			vertexMaxY,
			clipMinX,
			clipMinY,
			clipMaxX,
			clipMaxY);
	}

	struct VanityMenuTextSnapshot
	{
		bool available{ false };
		bool wantTextInput{ false };
		bool skyrimTextInputAllowed{ false };
		std::int32_t fontSizePixels{ 0 };
	};

	VanityMenuTextSnapshot ReadVanityMenuTextSnapshot()
	{
		const auto& state = GetStates()[kVanityIndex];
		if (!state.getVanityMenu) {
			return {};
		}

		const auto menu = reinterpret_cast<const std::byte*>(state.getVanityMenu());
		if (!menu) {
			return {};
		}

		VanityMenuTextSnapshot snapshot{};
		std::memcpy(std::addressof(snapshot.wantTextInput), menu + kVanityWantTextInputOffset, sizeof(snapshot.wantTextInput));
		std::memcpy(std::addressof(snapshot.skyrimTextInputAllowed), menu + kVanitySkyrimTextInputAllowedOffset, sizeof(snapshot.skyrimTextInputAllowed));
		std::memcpy(std::addressof(snapshot.fontSizePixels), menu + kVanityFontSizePixelsOffset, sizeof(snapshot.fontSizePixels));
		if (snapshot.fontSizePixels < 6 || snapshot.fontSizePixels > 256) {
			return {};
		}
		snapshot.available = true;
		return snapshot;
	}

	void LogVanityFrameDiagnostics(const void* a_drawData, const std::uint64_t a_draw)
	{
		auto& state = GetStates()[kVanityIndex];
		if (!state.vanityTextDiagnosticsActive.load(std::memory_order_acquire) || !a_drawData || !state.getVanityMenu) {
			return;
		}

		VanityDrawDataSnapshot drawData{};
		std::memcpy(std::addressof(drawData), a_drawData, sizeof(drawData));
		const bool validGeometry =
			std::isfinite(drawData.displayPosX) && std::isfinite(drawData.displayPosY) &&
			std::isfinite(drawData.displaySizeX) && std::isfinite(drawData.displaySizeY) &&
			std::isfinite(drawData.framebufferScaleX) && std::isfinite(drawData.framebufferScaleY) &&
			drawData.displaySizeX > 0.0F && drawData.displaySizeX <= 16384.0F &&
			drawData.displaySizeY > 0.0F && drawData.displaySizeY <= 16384.0F &&
			drawData.framebufferScaleX > 0.0F && drawData.framebufferScaleX <= 8.0F &&
			drawData.framebufferScaleY > 0.0F && drawData.framebufferScaleY <= 8.0F;
		const auto menu = ReadVanityMenuTextSnapshot();
		if (!validGeometry || !menu.available) {
			const auto sample = ++state.vanityDiagnosticDrawCount;
			if (sample <= 3) {
				logger::warn("Vanity text diagnostics rejected private state sample {}: drawGeometryValid={} menuStateValid={}", sample, validGeometry, menu.available);
			}
			return;
		}

		const int wantText = menu.wantTextInput ? 1 : 0;
		const int textAllowed = menu.skyrimTextInputAllowed ? 1 : 0;
		const int previousWantText = state.vanityLastWantTextInput.exchange(wantText, std::memory_order_relaxed);
		const int previousTextAllowed = state.vanityLastSkyrimTextInputAllowed.exchange(textAllowed, std::memory_order_relaxed);
		const bool stateChanged = previousWantText != wantText || previousTextAllowed != textAllowed;
		const auto sample = ++state.vanityDiagnosticDrawCount;
		if (sample > 4 && !stateChanged) {
			return;
		}

		SolFGLateOverlayAPI::FrameV1 nativeFrame{};
		const auto bridge = GetBridge();
		const bool nativeAvailable = bridge && bridge->query(std::addressof(nativeFrame)) != 0;
		const auto renderSize = RE::BSGraphics::Renderer::GetScreenSize();
		logger::info(
			"Vanity text frame {} draw {}: display=({:.1f},{:.1f}) {:.1f}x{:.1f} framebufferScale=({:.3f},{:.3f}) render={}x{} native={}x{} font={} wantText={} SkyrimTextAllowed={} chars={}/{} blocked={} keys={} clicks={}",
			sample,
			a_draw,
			drawData.displayPosX,
			drawData.displayPosY,
			drawData.displaySizeX,
			drawData.displaySizeY,
			drawData.framebufferScaleX,
			drawData.framebufferScaleY,
			renderSize.width,
			renderSize.height,
			nativeAvailable ? nativeFrame.width : 0,
			nativeAvailable ? nativeFrame.height : 0,
			menu.fontSizePixels,
			menu.wantTextInput,
			menu.skyrimTextInputAllowed,
			state.vanityEligibleCharacterCount.load(std::memory_order_relaxed),
			state.vanityCharacterEventCount.load(std::memory_order_relaxed),
			state.vanityBlockedCharacterCount.load(std::memory_order_relaxed),
			state.vanityKeyDownEventCount.load(std::memory_order_relaxed),
			state.vanityMouseDownEventCount.load(std::memory_order_relaxed));
	}

	RE::UI_MESSAGE_RESULTS VanityProcessMessageThunk(void* a_menuHost, RE::UIMessage* a_message)
	{
		auto& state = GetStates()[kVanityIndex];
		const auto original = state.originalProcessMessage;
		if (!original) {
			return RE::UI_MESSAGE_RESULTS::kPassOn;
		}

		const auto menu = ReadVanityMenuTextSnapshot();
		if (a_message && a_message->type.get() == RE::UI_MESSAGE_TYPE::kScaleformEvent && a_message->data) {
			const auto scaleformData = reinterpret_cast<const RE::BSUIScaleformData*>(a_message->data);
			const auto event = scaleformData->scaleformEvent;
			if (event) {
				switch (event->type.get()) {
				case RE::GFxEvent::EventType::kCharEvent:
				{
					std::uint32_t codePoint = 0;
					std::memcpy(std::addressof(codePoint), reinterpret_cast<const std::byte*>(event) + sizeof(RE::GFxEvent), sizeof(codePoint));
					const bool printable = codePoint >= 0x20U && codePoint != 0x7FU;
					const bool eligible = menu.available && menu.wantTextInput && printable;
					const auto count = ++state.vanityCharacterEventCount;
					if (eligible) {
						++state.vanityEligibleCharacterCount;
					} else {
						++state.vanityBlockedCharacterCount;
					}
					if (count <= 12) {
						logger::info(
							"Vanity character event {}: printable={} wantText={} SkyrimTextAllowed={} eligible={}",
							count,
							printable,
							menu.available && menu.wantTextInput,
							menu.available && menu.skyrimTextInputAllowed,
							eligible);
					}
					break;
				}
				case RE::GFxEvent::EventType::kKeyDown:
				{
					const auto count = ++state.vanityKeyDownEventCount;
					if (count <= 6) {
						logger::info("Vanity key-down event {}: wantText={} SkyrimTextAllowed={}", count, menu.available && menu.wantTextInput, menu.available && menu.skyrimTextInputAllowed);
					}
					break;
				}
				case RE::GFxEvent::EventType::kMouseDown:
				{
					const auto count = ++state.vanityMouseDownEventCount;
					if (count <= 6) {
						logger::info("Vanity mouse-down event {}: wantText={} SkyrimTextAllowed={}", count, menu.available && menu.wantTextInput, menu.available && menu.skyrimTextInputAllowed);
					}
					break;
				}
				default:
					break;
				}
			}
		}

		return original(a_menuHost, a_message);
	}

	thread_local bool nativeScopeActive = false;

	class NativeDrawScope
	{
	public:
		NativeDrawScope(
			ProducerState& a_state,
			const ProducerProfile& a_profile,
			const SolFGLateOverlayAPI::BridgeV1& a_bridge,
			const SolFGLateOverlayAPI::FrameV1& a_frame) :
			state_(a_state), profile_(a_profile), bridge_(a_bridge)
		{
			nativeScopeActive = true;
			const auto begin = ++state_.nativeBeginCount;
			if (begin <= 3) {
				logger::info("{} native draw {} began at {}x{} on frame {}", profile_.name, begin, a_frame.width, a_frame.height, a_frame.frameId);
			}
		}

		~NativeDrawScope()
		{
			bridge_.end();
			nativeScopeActive = false;
			const auto ended = ++state_.nativeEndCount;
			if (ended <= 3) {
				logger::info("{} native draw {} completed", profile_.name, ended);
			}
		}

		NativeDrawScope(const NativeDrawScope&) = delete;
		NativeDrawScope& operator=(const NativeDrawScope&) = delete;

	private:
		ProducerState& state_;
		const ProducerProfile& profile_;
		const SolFGLateOverlayAPI::BridgeV1& bridge_;
	};

	bool HasNativeInputCoordinates()
	{
		return TheosRenderPipeline::NativeInput::Active();
	}

	void LogVanityCursorCoordinates(void* a_drawData, const char* a_route)
	{
		// Observe both sides of Vanity's mouse-update/NewFrame boundary. Limit
		// each stage to twelve samples, one second apart, without changing input.
		auto& state = GetStates()[kVanityIndex];
		auto& samples = a_drawData ? state.cursorDrawSampleCount : state.cursorSampleCount;
		auto& nextTick = a_drawData ? state.cursorDrawNextSampleTick : state.cursorNextSampleTick;
		if (samples.load(std::memory_order_relaxed) >= 12) { return; }
		const auto now = ::GetTickCount64();
		auto next = nextTick.load(std::memory_order_relaxed);
		if (now < next || !nextTick.compare_exchange_strong(next, now + 1000, std::memory_order_relaxed)) { return; }
		const auto sample = samples.fetch_add(1, std::memory_order_relaxed) + 1;
		auto* ui = RE::UI::GetSingleton();
		const auto* cursor = RE::MenuCursor::GetSingleton();
		const bool cursorMenu = ui && ui->IsMenuOpen(RE::CursorMenu::MENU_NAME);
		const auto screen = RE::BSGraphics::Renderer::GetScreenSize();
		const auto* renderer = RE::BSGraphics::Renderer::GetRendererDataSingleton();
		const auto window = renderer ? reinterpret_cast<HWND>(renderer->renderWindows[0].hWnd) : nullptr;
		RECT client{};
		const bool clientValid = window && ::GetClientRect(window, &client);
		POINT osCursor{};
		const bool osValid = window && ::GetCursorPos(&osCursor) && ::ScreenToClient(window, &osCursor);
		VanityDrawDataSnapshot draw{};
		if (a_drawData) { std::memcpy(&draw, a_drawData, sizeof(draw)); }
		logger::info(
			"Vanity cursor sample {} stage={} route={} nativeOwner={} cursorAvailable={} cursorMenu={} gamePos=({:.1f},{:.1f}) bounds={:.1f}x{:.1f} screen={}x{} clientValid={} client={}x{} osClientValid={} osClient=({},{}) drawOrigin=({:.1f},{:.1f}) drawExtent={:.1f}x{:.1f} framebufferScale=({:.3f},{:.3f})",
			sample, a_drawData ? "draw" : "mouse-update", a_route, HasNativeInputCoordinates(), cursor != nullptr, cursorMenu,
			cursor ? cursor->cursorPosX : 0.0F, cursor ? cursor->cursorPosY : 0.0F,
			cursor ? cursor->screenWidthX : 0.0F, cursor ? cursor->screenWidthY : 0.0F,
			screen.width, screen.height, clientValid, client.right - client.left, client.bottom - client.top,
			osValid, osCursor.x, osCursor.y, draw.displayPosX, draw.displayPosY,
			draw.displaySizeX, draw.displaySizeY, draw.framebufferScaleX, draw.framebufferScaleY);
	}

	template <std::size_t Index>
	void RenderDrawDataThunk(void* a_drawData)
	{
		auto& state = GetStates()[Index];
		const auto original = state.original;
		if (!original) {
			return;
		}

		const auto draw = state.drawCount.fetch_add(1, std::memory_order_relaxed) + 1;
		if (!HasDrawCommands(a_drawData)) {
			original(a_drawData);
			return;
		}
		if constexpr (Index == kVanityIndex) {
			LogVanityCursorCoordinates(a_drawData, "producer-draw");
			LogVanityFrameDiagnostics(a_drawData, draw);
		}
		if constexpr (Index == kSpellHotbarIndex) {
			LogSpellHotbarGeometry(a_drawData, draw);
		}

		auto bridge = GetBridge();
		if (!bridge || nativeScopeActive) {
			++state.rejectCount;
			original(a_drawData);
			return;
		}

		SolFGLateOverlayAPI::FrameV1 frame{};
		// Optional source-host startup service. Existing layout/input queries keep
		// using the original bridge; only this bounded draw opts into the layer.
		const auto startup = TheosRenderPipeline::NativeUIBridge::Startup(SolFGStartupOverlayAPI::kVersion1);
		const bool startupBegan = startup && startup->structSize >= sizeof(*startup) &&
			startup->version == SolFGStartupOverlayAPI::kVersion1 && startup->begin && startup->end &&
			startup->begin(std::addressof(frame)) != 0;
		if (startupBegan) { bridge = startup; }
		const bool began = startupBegan || bridge->begin(std::addressof(frame)) != 0;
		if (!began) {
			const auto rejected = ++state.rejectCount;
			if (rejected <= 3) {
				logger::info("{} draw {} used its original target; TheosRenderPipeline native frame was unavailable", kProfiles[Index].name, draw);
			}
			original(a_drawData);
			return;
		}

		if (frame.width == 0 || frame.height == 0 || frame.width > 16384 || frame.height > 16384) {
			bridge->end();
			++state.rejectCount;
			logger::error("{} received invalid native dimensions {}x{}; using the original target", kProfiles[Index].name, frame.width, frame.height);
			original(a_drawData);
			return;
		}

		NativeDrawScope scope{ state, kProfiles[Index], *bridge, frame };
		original(a_drawData);
	}

	inline constexpr std::array<RenderDrawData, kProducerCount> kThunks{
		&RenderDrawDataThunk<0>,
		&RenderDrawDataThunk<1>,
		&RenderDrawDataThunk<2>,
		&RenderDrawDataThunk<3>,
		&RenderDrawDataThunk<4>,
		&RenderDrawDataThunk<5>,
		&RenderDrawDataThunk<6>,
		&RenderDrawDataThunk<7>
	};

	void VanityUpdateMousePositionThunk(void* a_inputManager)
	{
		auto& state = GetStates()[kVanityIndex];
		const auto original = state.originalMouseUpdate;
		if (!original) {
			return;
		}

		const bool nativeInput = HasNativeInputCoordinates();
		LogVanityCursorCoordinates(nullptr, nativeInput ? "native-bypass" : "legacy-fallback-candidate");
		if (nativeInput) {
			original(a_inputManager);
			return;
		}

		const auto bridge = GetBridge();
		const auto ui = RE::UI::GetSingleton();
		const auto cursor = RE::MenuCursor::GetSingleton();
		if (!ui || !cursor || !ui->IsMenuOpen(RE::CursorMenu::MENU_NAME)) {
			original(a_inputManager);
			return;
		}

		SolFGLateOverlayAPI::FrameV1 frame{};
		TheosRenderPipeline::ImGuiCompat::CursorExtent bridgeExtent{}, clientExtent{};
		if (bridge && bridge->query(std::addressof(frame))) {
			bridgeExtent = { frame.width, frame.height };
		}
		if (!bridgeExtent.Valid()) {
			const auto* rendererData = RE::BSGraphics::Renderer::GetRendererDataSingleton();
			const auto window = rendererData ? reinterpret_cast<HWND>(rendererData->renderWindows[0].hWnd) : nullptr;
			RECT client{};
			if (window && ::GetClientRect(window, &client) && client.right > client.left && client.bottom > client.top) {
				clientExtent = { static_cast<std::uint32_t>(client.right - client.left),
					static_cast<std::uint32_t>(client.bottom - client.top) };
			}
		}
		const auto renderSize = RE::BSGraphics::Renderer::GetScreenSize();
		const auto mapping = TheosRenderPipeline::ImGuiCompat::ResolveCursorMapping(
			{ renderSize.width, renderSize.height }, bridgeExtent, clientExtent);
		if (!mapping || (mapping->scaleX == 1.0F && mapping->scaleY == 1.0F)) {
			original(a_inputManager);
			return;
		}

		const float scaleX = mapping->scaleX;
		const float scaleY = mapping->scaleY;

		const float savedX = cursor->cursorPosX;
		const float savedY = cursor->cursorPosY;
		cursor->cursorPosX = savedX * scaleX;
		cursor->cursorPosY = savedY * scaleY;
		original(a_inputManager);
		cursor->cursorPosX = savedX;
		cursor->cursorPosY = savedY;

		const auto remapped = ++state.mouseRemapCount;
		if (remapped <= 3) {
			logger::info(
				"{} mouse remap {}: game {}x{} -> native {}x{} ({:.4f}, {:.4f}); extent={}",
				kProfiles[kVanityIndex].name,
				remapped,
				renderSize.width,
				renderSize.height,
				mapping->target.width,
				mapping->target.height,
				scaleX,
				scaleY,
				mapping->usesClientExtent ? "window-client" : "legacy-bridge");
		}
	}

	bool InstallProducer(const std::size_t a_index)
	{
		const auto& profile = kProfiles[a_index];
		auto& state = GetStates()[a_index];
		if (!IsProducerEnabled(profile)) {
			logger::info("{} adapter disabled in {}", profile.name, std::filesystem::path(kIniPath).string());
			return false;
		}

		const auto module = ::GetModuleHandleW(profile.moduleName);
		if (!module) {
			logger::info("{} adapter inactive: {} is not loaded", profile.name, std::filesystem::path(profile.moduleName).string());
			return false;
		}
		const auto path = GetModulePath(module);
		const auto hash = path ? Sha256File(*path) : std::nullopt;
		if (!hash) {
			logger::error("{} adapter rejected: module could not be hashed", profile.name);
			return false;
		}
		const auto actualHash = HashText(*hash);
		const VanityBinaryProfile* vanityBinary = nullptr;
		if (a_index == kVanityIndex) {
			for (const auto& candidate : kVanityBinaryProfiles) {
				if (actualHash == candidate.sha256) {
					vanityBinary = std::addressof(candidate);
					break;
				}
			}
		}
		if ((a_index == kVanityIndex && !vanityBinary) || (a_index != kVanityIndex && actualHash != profile.sha256)) {
			logger::warn("{} adapter rejected: unsupported DLL sha256={}", profile.name, actualHash);
			return false;
		}

		const auto range = GetModuleRange(module);
		const auto renderDrawDataRva = vanityBinary ? vanityBinary->renderDrawDataRva : profile.renderDrawDataRva;
		const auto& renderDrawDataEntryBytes = vanityBinary ? vanityBinary->renderDrawDataEntryBytes : profile.entryBytes;
		const auto entry = range ? range->base + renderDrawDataRva : 0;
		if (!range || !range->Contains(entry, renderDrawDataEntryBytes.size()) ||
			std::memcmp(reinterpret_cast<const void*>(entry), renderDrawDataEntryBytes.data(), renderDrawDataEntryBytes.size()) != 0) {
			logger::error("{} adapter rejected: RenderDrawData entry guard mismatch at RVA 0x{:X}", profile.name, renderDrawDataRva);
			return false;
		}

		std::uintptr_t vanityMouseCall = 0;
		std::uintptr_t vanityProcessMessageEntry = 0;
		const bool vanityTextDiagnostics = a_index == kVanityIndex && VanityTextDiagnosticsEnabled();
		if (a_index == kVanityIndex) {
			if (!vanityBinary->sourceNormalizesMouse) {
				const auto& mouseUpdateCall = vanityBinary->mouseUpdateCall;
				vanityMouseCall = range->base + mouseUpdateCall.rva;
				if (!range->Contains(vanityMouseCall, mouseUpdateCall.instructionBytes.size()) ||
					std::memcmp(
						reinterpret_cast<const void*>(vanityMouseCall),
						mouseUpdateCall.instructionBytes.data(),
						mouseUpdateCall.instructionBytes.size()) != 0) {
					logger::error("{} adapter rejected: UpdateMousePosition call guard mismatch at RVA 0x{:X}", profile.name, mouseUpdateCall.rva);
					return false;
				}

				std::int32_t displacement = 0;
				std::memcpy(
					std::addressof(displacement),
					reinterpret_cast<const void*>(vanityMouseCall + 1),
					sizeof(displacement));
				const auto resolvedTarget = vanityMouseCall + kCallGuardSize + displacement;
				if (resolvedTarget != range->base + mouseUpdateCall.targetRva) {
					logger::error(
						"{} adapter rejected: UpdateMousePosition call target mismatch at RVA 0x{:X}",
						profile.name,
						mouseUpdateCall.rva);
					return false;
				}
			}

			if (vanityTextDiagnostics) {
				vanityProcessMessageEntry = range->base + vanityBinary->processMessageRva;
				const auto vanityGetMenuEntry = range->base + vanityBinary->getMenuRva;
				const bool processMessageGuardMatches =
					range->Contains(vanityProcessMessageEntry, vanityBinary->processMessageEntryBytes.size()) &&
					std::memcmp(
						reinterpret_cast<const void*>(vanityProcessMessageEntry),
						vanityBinary->processMessageEntryBytes.data(),
						vanityBinary->processMessageEntryBytes.size()) == 0;
				const bool getMenuGuardMatches =
					range->Contains(vanityGetMenuEntry, vanityBinary->getMenuEntryBytes.size()) &&
					std::memcmp(
						reinterpret_cast<const void*>(vanityGetMenuEntry),
						vanityBinary->getMenuEntryBytes.data(),
						vanityBinary->getMenuEntryBytes.size()) == 0;
				if (!processMessageGuardMatches || !getMenuGuardMatches) {
					logger::warn(
						"{} text diagnostics disabled: ProcessMessageGuard={} GetMenuGuard={}",
						profile.name,
						processMessageGuardMatches,
						getMenuGuardMatches);
					vanityProcessMessageEntry = 0;
				} else {
					state.getVanityMenu = reinterpret_cast<GetVanityMenu>(vanityGetMenuEntry);
				}
			}
		}

		const auto trampoline = Detours::X64::DetourFunction(
			entry,
			reinterpret_cast<std::uintptr_t>(kThunks[a_index]),
			Detours::X64Option::USE_RAX_JUMP);
		if (!trampoline) {
			state.original = nullptr;
			logger::error("{} adapter rejected: detour trampoline creation failed", profile.name);
			return false;
		}
		state.original = reinterpret_cast<RenderDrawData>(trampoline);

		if (a_index == kVanityIndex) {
			if (vanityBinary->sourceNormalizesMouse) {
				logger::info(
					"{} uses source-normalized mouse coordinates for {}; mouse adapter skipped",
					profile.name,
					vanityBinary->label);
			} else {
				auto& branchTrampoline = GetVanityCallTrampoline();
				branchTrampoline.create(14, reinterpret_cast<void*>(vanityMouseCall + kCallGuardSize));
				const auto originalTarget = branchTrampoline.write_call<5>(
					vanityMouseCall,
					reinterpret_cast<std::uintptr_t>(&VanityUpdateMousePositionThunk));
				state.originalMouseUpdate = reinterpret_cast<UpdateMousePosition>(originalTarget);
				logger::info(
					"{} mouse adapter installed for {}; Menu::Draw call RVA=0x{:X} -> UpdateMousePosition RVA=0x{:X}",
					profile.name,
					vanityBinary->label,
					vanityBinary->mouseUpdateCall.rva,
					vanityBinary->mouseUpdateCall.targetRva);
			}

			if (vanityProcessMessageEntry && state.getVanityMenu) {
				const auto processMessageTrampoline = Detours::X64::DetourFunction(
					vanityProcessMessageEntry,
					reinterpret_cast<std::uintptr_t>(&VanityProcessMessageThunk),
					Detours::X64Option::USE_RAX_JUMP);
				if (!processMessageTrampoline) {
					state.getVanityMenu = nullptr;
					logger::warn("{} text diagnostics disabled: ProcessMessage detour creation failed", profile.name);
				} else {
					state.originalProcessMessage = reinterpret_cast<VanityProcessMessage>(processMessageTrampoline);
					state.vanityTextDiagnosticsActive.store(true, std::memory_order_release);
					logger::info(
						"{} text diagnostics installed; ProcessMessage RVA=0x{:X}, Menu::GetSingleton RVA=0x{:X}",
						profile.name,
						vanityBinary->processMessageRva,
						vanityBinary->getMenuRva);
				}
			} else if (!vanityTextDiagnostics) {
				logger::info("{} text diagnostics are disabled in {}", profile.name, std::filesystem::path(kIniPath).string());
			}
		}
		if (a_index == kSpellHotbarIndex) {
			const bool geometryDiagnostics = SpellHotbarGeometryDiagnosticsEnabled();
			state.spellHotbarGeometryDiagnosticsActive.store(geometryDiagnostics, std::memory_order_release);
			logger::info(
				"{} geometry diagnostics are {} in {}",
				profile.name,
				geometryDiagnostics ? "enabled" : "disabled",
				std::filesystem::path(kIniPath).string());
		}

		logger::info("{} adapter installed; sha256={}, RenderDrawData RVA=0x{:X}", profile.name, actualHash, renderDrawDataRva);
		return true;
	}

	void InstallProducers()
	{
		if (!ResolveBridge()) {
			return;
		}
		std::size_t installed = 0;
		for (std::size_t index = 0; index < kProfiles.size(); ++index) {
			installed += InstallProducer(index) ? 1u : 0u;
		}
		logger::info("[ImGuiIntegration] profile installation complete: {}/{} producer adapters active", installed, kProfiles.size());
	}

}

void TheosRenderPipeline::ImGuiIntegration::Install()
{
	InstallProducers();
}
