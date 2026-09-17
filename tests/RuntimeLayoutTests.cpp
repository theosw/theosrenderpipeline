#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include "RE/BSGraphics.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    }
}

int main(int argc, char** argv)
{
    Require(argc == 2, "select one runtime per process (CommonLib caches layouts)");
    const bool se = std::strcmp(argv[1], "1.5.97") == 0;
    const bool ae640 = std::strcmp(argv[1], "1.6.640") == 0;
    const bool ae1170 = std::strcmp(argv[1], "1.6.1170") == 0;
    const bool ae17104 = std::strcmp(argv[1], "1.7.104") == 0;
    Require(se || ae640 || ae1170 || ae17104, "known runtime");
    const REL::Version version = se ? REL::Version{ 1, 5, 97, 0 }
        : ae640 ? REL::Version{ 1, 6, 640, 0 }
        : ae1170 ? REL::Version{ 1, 6, 1170, 0 } : REL::Version{ 1, 7, 104, 0 };
    Require(REL::Module::mock(version), "mock executable identity");
    Require(REL::Module::IsSE() == se && REL::Module::IsAE() == !se,
        "1.7 uses AE relocations while 1.5 keeps SE relocations");

    alignas(16) std::array<std::byte, 0x200> bytes{};
    auto* state = reinterpret_cast<BSGraphics::State*>(bytes.data());
    const auto* constState = state;
    auto& runtime = state->GetRuntimeData();
    const auto expectedRuntime = se ? 0x58 : ae17104 ? 0x70 : 0x60;
    const auto expectedRatio = se ? 0xFC : ae17104 ? 0x114 : 0x104;
    Require(reinterpret_cast<std::byte*>(&runtime) - bytes.data() == expectedRuntime,
        "custom graphics data selects the engine layout");
    Require(&constState->GetRuntimeData() == &runtime, "const and writable views agree");
    Require(reinterpret_cast<std::byte*>(&runtime.kCameraDataCacheA) - bytes.data() == expectedRuntime + 0x48,
        "camera array follows the selected data base");
    runtime.dynamicResolutionWidthRatio = 0.625f;
    float observed{};
    std::memcpy(&observed, bytes.data() + expectedRatio, sizeof(observed));
    Require(observed == 0.625f, "DRS write reaches the actual engine ratio field");

    // 1.7's former counter location is a UI projection scale. Ensure camera
    // history uses +0x54 and world jitter does not overwrite the UI scales.
    constexpr std::uint32_t legacyCounter = 0x3F000000, newCounter = 8127;
    std::memcpy(bytes.data() + 0x4C, &legacyCounter, sizeof(legacyCounter));
    std::memcpy(bytes.data() + 0x54, &newCounter, sizeof(newCounter));
    state->jitter[0] = 0.125f;
    state->jitter[1] = -0.25f;
    Require(state->GetFrameCount() == (ae17104 ? newCounter : legacyCounter),
        "camera history reads frame count, not 1.7 UI projection scale");
    std::uint32_t preserved{};
    std::memcpy(&preserved, bytes.data() + 0x4C, sizeof(preserved));
    Require(preserved == legacyCounter, "world jitter preserves the following engine field");

    auto* commonState = reinterpret_cast<RE::BSGraphics::State*>(bytes.data());
    Require(reinterpret_cast<std::byte*>(&commonState->GetRuntimeData()) - bytes.data() == expectedRuntime,
        "dependency and custom wrapper agree on data base");
    Require(commonState->GetFrameCount() == state->GetFrameCount(),
        "dependency and camera history agree on frame count");

    alignas(16) std::array<std::byte, 0x3000> rendererBytes{};
    auto* renderer = reinterpret_cast<RE::BSGraphics::Renderer*>(rendererBytes.data());
    Require(reinterpret_cast<std::byte*>(&renderer->GetDepthStencilData()) - rendererBytes.data() ==
        (se || ae640 ? 0x1FB8 : 0x2018), "depth diagnostic uses versioned renderer accessor");
    static_assert(offsetof(RE::BSGraphics::RendererData, renderWindows) == 0x48);
    static_assert(offsetof(RE::BSGraphics::RendererWindow, windowWidth) == 0x10);
    static_assert(offsetof(RE::BSGraphics::RendererWindow, windowHeight) == 0x14);
    static_assert(offsetof(RE::BSGraphics::State, screenWidth) == 0x24);
    static_assert(offsetof(RE::BSGraphics::State, screenHeight) == 0x28);
    static_assert(offsetof(RE::BSGraphics::State, frameBufferViewport) == 0x2C);

    alignas(16) std::array<std::byte, 0x160> controlBytes{};
    auto* controls = reinterpret_cast<RE::ControlMap*>(controlBytes.data());
    const auto enabledOffset = se || ae640 ? 0x118 : 0x120;
    const auto mask = static_cast<std::uint32_t>(RE::ControlMap::UEFlag::kFighting);
    std::memcpy(controlBytes.data() + enabledOffset, &mask, sizeof(mask));
    Require(controls->IsFightingControlsEnabled() && !controls->IsLookingControlsEnabled(),
        "overlay reads the actual enabled-controls field on each runtime");
    Require(reinterpret_cast<std::byte*>(&controls->GetRuntimeData().textEntryCount) - controlBytes.data() ==
        enabledOffset + 8, "control and text-capture layouts agree with the engine functions");

    // Exercise the linked dependency's auto-detecting loader, not just the
    // offline Python parser. A synthetic revision avoids legacy shared-map
    // collisions with an installed game or a real-table audit in another process.
    auto fixtureVersion = version;
    fixtureVersion[3] = 65534;
    Require(REL::Module::mock(fixtureVersion), "mock isolated fixture revision");
    const auto fixture = std::filesystem::path(argv[0]).parent_path() /
        (std::string("address-fixture-") + argv[1] + ".bin");
    {
        std::ofstream file(fixture, std::ios::binary | std::ios::trunc);
        const auto write32 = [&](std::uint32_t value) {
            file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        };
        const auto write64 = [&](std::uint64_t value) {
            file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        };
        write32(se ? 1 : ae17104 ? 5 : 2);
        for (std::size_t i = 0; i < 4; ++i) { write32(fixtureVersion[i]); }
        constexpr char imageName[64] = "SkyrimSE.exe";
        if (ae17104) {
            file.write(imageName, sizeof(imageName));
        } else {
            write32(12);
            file.write(imageName, 12);
        }
        write32(8);
        if (ae17104) {
            write32(0);  // reserved data format
            write32(4);  // dense entries; IDs 0 and 2 are holes
            write32(0); write32(0x1230); write32(0); write32(0x5670);
        } else {
            write32(2);  // sparse entries, absolute IDs/RVAs
            file.put(0); write64(1); write64(0x1230);
            file.put(0); write64(3); write64(0x5670);
        }
        file.close();
        Require(static_cast<bool>(file), "write isolated address fixture");
    }
    Require(REL::IDDB::inject(fixture.wstring(), fixtureVersion), "auto-detect address format");
    Require(REL::ID(1).offset() == 0x1230 && REL::ID(3).offset() == 0x5670,
        "loader resolves sparse IDs and dense indexes after holes");
    Require(REL::RelocationID(1, 3).offset() == (se ? 0x1230 : 0x5670),
        "runtime classification selects the correct relocation family");
    auto wrongVersion = fixtureVersion;
    --wrongVersion[3];
    Require(!REL::IDDB::inject(fixture.wstring(), wrongVersion), "loader rejects version mismatch");
    REL::IDDB::reset();
    Require(std::filesystem::remove(fixture), "remove address fixture after closing mapping");
    std::printf("PASS: %s runtime classification, graphics/frame/camera and renderer layouts\n", argv[1]);
}
