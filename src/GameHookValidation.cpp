#include <PCH.h>
#include "GameHookValidation.h"
#include "HookSafety.h"
#include "SkyrimRuntime.h"
#include <Psapi.h>

namespace TheosRenderPipeline
{
    void ValidateGameHooks(bool communityShaders)
    {
        const auto* profile = SkyrimRuntime::Find(REL::Module::get().version());
        if (!profile) { util::report_and_fail("No verified hook profile for this Skyrim runtime."); }
        const auto& offsets = profile->hooks;
        const auto base = REL::Module::get().base();
        MODULEINFO image{};
        if (!K32GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &image, sizeof(image))) {
            util::report_and_fail("Could not identify Skyrim's executable before installing renderer hooks.");
        }
        const auto require = [&](bool valid, const char* name, std::uintptr_t site) {
            if (!valid) {
                util::report_and_fail(std::format("Theo's Render Pipeline: {} does not match the verified Skyrim {} hook contract (RVA 0x{:X}). "
                    "No renderer game-code patches were installed. Check conflicting mods and see TheosRenderPipeline.log.",
                    name, profile->version.string(), site - base));
            }
        };
        const auto call = [&](const char* name, REL::RelocationID source, std::uintptr_t offset, REL::RelocationID target) {
            const auto site = source.address() + offset;
            require(HookSafety::DirectCall(site, target.address(), base, image.SizeOfImage), name, site);
        };
        call("InitD3D call", REL::RelocationID{75595,77226}, offsets.initD3D, REL::RelocationID{75827,77649});
        constexpr std::array<std::uint8_t, 19> artwork{0x40,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x40,0x48,0xC7,0x44,0x24,0x38,0xFE,0xFF,0xFF,0xFF};
        const auto artworkSite = REL::RelocationID(13214,13363).address();
        require(HookSafety::Bytes(artworkSite, artwork), "Loading artwork sender", artworkSite);
        const auto creationSlot = HookSafety::ImportSlot(base, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
        std::uintptr_t creation{};
        require(creationSlot && HookSafety::Read(reinterpret_cast<std::uintptr_t>(creationSlot), &creation, sizeof(creation)) &&
            HookSafety::Executable(creation), "D3D11 device import", reinterpret_cast<std::uintptr_t>(creationSlot));
        if (communityShaders) {
            const auto site = REL::RelocationID(100430,107148).address() + offsets.csPostProcessing;
            require(HookSafety::DirectCallTarget(site) != 0, "CS postprocessing call", site);
            logger::info("[Hooks] CS startup sites verified before installation");
            return;
        }
        call("DRS call", REL::RelocationID{35556,36555}, offsets.drs, REL::RelocationID{75706,77515});
        call("Cursor bounds call", REL::RelocationID{50604,51498}, offsets.cursorBounds, REL::RelocationID{80427,82539});
        call("Screen size call", REL::RelocationID{75590,77397}, offsets.screenSize, REL::RelocationID{75637,77444});
        call("Engine dimensions call", REL::RelocationID{99938,106583}, offsets.engineDimensions, REL::RelocationID{99963,106609});
        call("Mist background call", REL::RelocationID{51855,52727}, offsets.mistBackground, REL::RelocationID{99023,105674});
        call("World completion call", REL::RelocationID{79947,82084}, offsets.worldCompletion, REL::RelocationID{75462,77247});
        call("Update jitter call", REL::RelocationID{75460,77245}, offsets.updateJitter, REL::RelocationID{75709,77518});
        call("Render world call", REL::RelocationID{35560,36559}, offsets.renderWorld, REL::RelocationID{100424,107142});
        const auto clientSite = REL::RelocationID(75460,77245).address() + offsets.rendererClientRect;
        const auto clientSlot = HookSafety::ImportSlot(base, "user32.dll", "GetClientRect");
        std::uintptr_t client{};
        require(clientSlot && HookSafety::ImportCallSlot(clientSite) == reinterpret_cast<std::uintptr_t>(clientSlot) &&
            HookSafety::Read(reinterpret_cast<std::uintptr_t>(clientSlot), &client, sizeof(client)) && HookSafety::Executable(client),
            "Renderer GetClientRect import call", clientSite);
        const auto jitterSite = REL::RelocationID(75709,77518).address() + offsets.jitterBranch;
        const auto cameraSite = REL::RelocationID(75711,77520).address() + offsets.cameraBranch;
        require(HookSafety::Bytes(jitterSite, profile->bytes.jitterBranch), "Jitter branch patch", jitterSite);
        require(HookSafety::Bytes(cameraSite, profile->bytes.cameraBranch), "Camera branch patch", cameraSite);
        const auto interfaceSite = REL::RelocationID(79947,82084).address();
        require(HookSafety::Entry(interfaceSite, profile->bytes.drawInterface, base, image.SizeOfImage), "Draw interface entry", interfaceSite);
        constexpr std::array<std::uint8_t, 6> inventory{0x40,0x53,0x48,0x83,0xEC,0x20};
        const auto inventorySite = REL::RelocationID(50882,51755).address();
        require(HookSafety::Entry(inventorySite, inventory, base, image.SizeOfImage), "Inventory 3D entry", inventorySite);
        logger::info("[Hooks] native startup sites verified before installation");
    }
}
