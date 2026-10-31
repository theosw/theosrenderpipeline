#include "FrameGen/NeuralRuntimeKernels.h"
#include "FrameGen/NeuralRuntimeKernelsInternal.h"
#include "FrameGen/NeuralRuntimeKernelRegistry.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>

using namespace TheosRenderPipeline::NeuralRendering;
static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

#ifdef TRP_ADA_UNIVERSAL
using namespace KernelPacks;
template<class T> static void Put(std::vector<std::uint8_t>& bytes, std::size_t at, T value)
{
    std::memcpy(bytes.data()+at, &value, sizeof(T));
}
static std::vector<std::uint8_t> Pack()
{
    std::vector<std::uint8_t> pack(PackSize);
    std::memcpy(pack.data(), "TRPNRK1\0", 8);
    Put<std::uint32_t>(pack, 8, 1);
    Put<std::uint32_t>(pack, 12, 15);
    const auto nibble = [](char c) { return c <= '9' ? c-'0' : c-'A'+10; };
    for (std::size_t i = 0; i < 32; ++i) { pack[16+i] = static_cast<std::uint8_t>((nibble(kKernelRuntimeSha256[2*i]) << 4) | nibble(kKernelRuntimeSha256[2*i+1])); }
    constexpr std::uint64_t sizes[]{3119832,1967472,1749664,1872504,1474848,1740184,180936,7152,7728,8240,9408,7640,5200,56808,5928};
    std::uint64_t end = 512;
    for (std::size_t i = 0; i < 15; ++i) {
        const auto offset = (end+255) & ~std::uint64_t(255);
        Put(pack, 48+24*i, Bundles[i].original);
        Put(pack, 52+24*i, Bundles[i].bytes);
        Put(pack, 56+24*i, offset);
        Put(pack, 64+24*i, sizes[i]);
        const std::array<std::uint8_t, 8> magic{0x50,0xED,0x55,0xBA,1,0,0x10,0};
        std::memcpy(pack.data()+offset, magic.data(), magic.size());
        Put(pack, static_cast<std::size_t>(offset)+8, sizes[i]-16);
        end = offset+sizes[i];
    }
    Require(end == pack.size(), "synthetic pack matches qualified layout");
    return pack;
}
struct FakeMemory final : Memory
{
    std::map<std::uint32_t, std::uint8_t> bytes;
    std::map<std::uint32_t, DWORD> pages;
    unsigned writes{}, protects{}, flushes{}, failWrite{}, failProtect{}, failFlush{};
    bool alwaysFailWrites{}, race{}, silentWrite{};
    explicit FakeMemory(const std::vector<Patch>& plan)
    {
        for (const auto& p : plan) {
            for (std::size_t i = 0; i < p.original.size(); ++i) { bytes[p.rva+static_cast<std::uint32_t>(i)] = p.original[i]; }
            pages.try_emplace(p.rva & ~(PageSize-1), p.rva < 0x100000 ? PAGE_EXECUTE_READ : PAGE_READONLY);
        }
    }
    bool Read(std::uint32_t at, std::span<std::uint8_t> out) override
    {
        for (auto& b : out) { const auto it = bytes.find(at++); if (it == bytes.end()) { return false; } b = it->second; }
        return true;
    }
    bool Write(std::uint32_t at, std::span<const std::uint8_t> source) override
    {
        ++writes;
        if (alwaysFailWrites && writes > 1) { return false; }
        if (silentWrite && writes == 1) { return true; }
        const bool fail = writes == failWrite;
        const auto size = fail ? source.size()/2 : source.size();
        for (std::size_t i = 0; i < size; ++i) {
            Require(pages[at & ~(PageSize-1)] == PAGE_EXECUTE_READWRITE, "write only while page writable");
            bytes[at++] = source[i];
        }
        return !fail;
    }
    bool Protect(std::uint32_t page, DWORD value, DWORD& old) override
    {
        ++protects;
        if (protects == failProtect) { return false; }
        old = pages[page]; pages[page] = value;
        if (race && protects == 1) { bytes.begin()->second ^= 1; }
        return true;
    }
    bool ProtectionIs(std::uint32_t page, DWORD value) override { return pages[page] == value; }
    bool Flush() override { return ++flushes != failFlush; }
};
static void UnitTests()
{
    Require(ProtectionEquivalent(PAGE_READWRITE, PAGE_WRITECOPY) && ProtectionEquivalent(PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY),
        "private copy-on-write image pages can report equivalent writable access");
    Require(!ProtectionEquivalent(PAGE_EXECUTE_READWRITE, PAGE_READONLY) && !ProtectionEquivalent(PAGE_READWRITE, PAGE_EXECUTE_READ) &&
        !ProtectionEquivalent(PAGE_READWRITE, PAGE_WRITECOPY | PAGE_GUARD), "protection equivalence never broadens other access or drops modifiers");
    auto pack = Pack();
    std::vector<Patch> plan;
    std::string reason;
    constexpr std::uintptr_t module = 0x7FF800000000, storage = module+ImageSize;
    Require(MakePlan(pack, module, storage, plan, reason) && plan.size() == 38, "38 checked patches include CRT-cached lengths and all four architecture entries");
    for (const int capability : {75, 86, 89}) { Require(NeedsRuntimeKernels(capability), "RTX 20/30/40 install the qualified pack"); }
    for (const int capability : {0, 80, 90, 120, 121}) { Require(!NeedsRuntimeKernels(capability), "unknown and native RTX 50 adapters do not install a pack"); }
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& entry = plan[34+i];
        std::uint32_t target{}; std::memcpy(&target, entry.replacement.data(), 4);
        Require(entry.rva == 0x18144+4*i && target == 0x17C2B, "each audited architecture entry reaches initialization");
    }
    for (std::size_t i = 0; i < 13; ++i) {
        std::int32_t relative{}; std::memcpy(&relative, plan[2*i].replacement.data()+3, 4);
        std::uint64_t offset{}; std::memcpy(&offset, pack.data()+56+24*i, 8);
        Require(module+Bundles[i].site+7+relative == storage+offset, "RIP-relative target reaches its bundle after ASLR");
    }
    FakeMemory applied(plan);
    const auto originalPages = applied.pages;
    Require(Publish(applied, plan, reason) == Publication::Applied && Matches(applied, plan, true), "successful publication");
    for (std::size_t i = 13; i < 15; ++i) {
        std::array<std::uint8_t, 4> cached{};
        Require(applied.Read(Bundles[i].site+8, cached), "driver descriptor includes its CRT-cached length");
        std::uint32_t length{}; std::uint64_t expected{};
        std::memcpy(&length, cached.data(), 4); std::memcpy(&expected, pack.data()+64+24*i, 8);
        Require(length == expected, "driver reads exactly the replacement fatbin length, preventing overrun");
    }
    Require(applied.pages == originalPages && applied.flushes == 1, "original page protections and instruction cache restored");
    Require(Publish(applied, plan, reason) == Publication::Refused, "raw transaction refuses repeated publication");
    for (unsigned i = 1; i <= plan.size(); ++i) {
        FakeMemory failed(plan); failed.failWrite = i;
        const auto bytes = failed.bytes;
        Require(Publish(failed, plan, reason) == Publication::Refused, "partial write refusal rolls back");
        Require(failed.bytes == bytes && failed.pages == originalPages, "every partial write restores bytes and protection");
    }
    const auto pageCount = static_cast<unsigned>(originalPages.size());
    for (unsigned i = 1; i <= pageCount*2; ++i) {
        FakeMemory failed(plan); failed.failProtect = i;
        const auto bytes = failed.bytes;
        Require(Publish(failed, plan, reason) == Publication::Refused, "acquire or restore protection failure rolls back");
        Require(failed.bytes == bytes && failed.pages == originalPages, "protection failure leaves original image");
    }
    for (const bool silent : {false, true}) {
        FakeMemory failed(plan); failed.failFlush = silent ? 0 : 1; failed.silentWrite = silent;
        const auto bytes = failed.bytes;
        Require(Publish(failed, plan, reason) == Publication::Refused && failed.bytes == bytes && failed.pages == originalPages,
            "failed flush or false-success write rolls back");
    }
    FakeMemory unsafe(plan); unsafe.failWrite = 2; unsafe.alwaysFailWrites = true;
    Require(Publish(unsafe, plan, reason) == Publication::Unsafe, "failed rollback is unsafe");
    FakeMemory competing(plan); competing.race = true;
    Require(Publish(competing, plan, reason) == Publication::Unsafe && competing.writes == 0 && competing.pages == originalPages,
        "competing edit is preserved and refused");
    FakeMemory mismatch(plan); mismatch.bytes.begin()->second ^= 1;
    Require(Publish(mismatch, plan, reason) == Publication::Refused && mismatch.protects == 0, "original-byte mismatch makes no protection changes");
    auto overlap = plan; overlap.push_back(plan[0]); FakeMemory bad(plan);
    Require(Publish(bad, overlap, reason) == Publication::Refused && bad.protects == 0, "overlapping patches refused");
    auto outside = plan; outside[0].rva = ImageSize;
    Require(Publish(bad, outside, reason) == Publication::Refused && bad.protects == 0, "out-of-image patch refused");
    // Exercise the actual pinned-module registry and publication transaction.
    // A clean rollback frees storage, but retains ownership for the next Apply.
    {
        InstallationRegistry registry;
        const auto handle = reinterpret_cast<HMODULE>(module);
        FakeMemory memory(plan);
        memory.failFlush = 1;
        unsigned attempts{}, allocations{}, pins{};
        auto apply = [&](bool fresh) {
            return registry.Apply(handle, fresh, [](Installation&) -> std::optional<RuntimeKernelResult> { return {}; },
                [&](Installation*) -> RuntimeKernelResult {
                    ++attempts;
                    Require(Matches(memory, plan, false), "retry revalidates originals before preparing new storage");
                    Installation pending;
                    pending.plan = plan;
                    auto* entry = registry.Retain(handle, std::move(pending), [&] { ++pins; return true; });
                    ++allocations;
                    const auto publication = Publish(memory, entry->plan, entry->result.status);
                    entry->result.applied = publication == Publication::Applied;
                    entry->result.unsafe = publication == Publication::Unsafe;
                    return entry->result;
                });
        };
        const auto refused = apply(true);
        Require(!refused.applied && !refused.unsafe && registry.Find(handle), "clean refusal retains the pinned registry entry");
        Require(Matches(memory, plan, false), "clean refusal restored original bytes");
        Require(apply(false).applied, "resident pinned module retries successfully after clean refusal");
        Require(attempts == 2 && allocations == 2 && pins == 1, "retry reacquires storage without requiring a fresh module");
        Require(apply(false).applied && attempts == 2, "successful installation stays cached");
        registry.Find(handle)->result = {false, true, "unsafe"};
        Require(apply(false).unsafe && attempts == 2, "unsafe installation is permanently retained without another preparation");
        InstallationRegistry foreign;
        Require(!foreign.Apply(handle, false, [](Installation&) -> std::optional<RuntimeKernelResult> { return {}; },
            [&](Installation*) -> RuntimeKernelResult { Require(false, "foreign resident module must never prepare"); return {}; }).applied,
            "a resident module without registry ownership still refuses");
    }
    for (const auto at : {0u,8u,12u,16u,48u,52u,56u,64u,512u,520u}) {
    auto corrupt = pack; corrupt[at] ^= 1;
        Require(!MakePlan(corrupt, module, storage, plan, reason) && plan.empty(), "malformed pack rejected before publication");
    }
    auto corrupt = pack; Put<std::uint64_t>(corrupt, 56, UINT64_MAX-255);
    Require(!MakePlan(corrupt, module, storage, plan, reason), "overflowing table rejected");
    Require(!MakePlan(std::span(pack).first(24), module, storage, plan, reason), "truncated pack rejected");
    Require(!MakePlan(pack, module, module+0x90000000, plan, reason), "out-of-reach allocation rejected");
    Require(!MakePlan(pack, UINTPTR_MAX-1, storage, plan, reason), "address overflow rejected");
    Require(!ApplyRuntimeKernels(nullptr, {}, true).applied, "null module refused");
    Require(!ApplyRuntimeKernels(reinterpret_cast<HMODULE>(module), {}, false).applied, "unowned resident module refused before access");
    std::puts("PASS Universal RTX 20/30/40 planning, address reach, malformed packs, all partial writes/protection failures, rollback and competing edits");
}
static void ManualProbe(const wchar_t* runtime, const wchar_t* pack, bool expectRefusal)
{
    auto module = LoadLibraryExW(runtime, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Require(module != nullptr, "load the identified manual NR runtime");
    std::vector<Patch> plan; std::string reason;
    Require(MakePlan(Pack(), reinterpret_cast<std::uintptr_t>(module), reinterpret_cast<std::uintptr_t>(module)+ImageSize, plan, reason), "plan real image snapshots");
    std::map<std::uint32_t, DWORD> protections;
    for (const auto& p : plan) {
        Require(std::memcmp(reinterpret_cast<const std::uint8_t*>(module)+p.rva, p.original.data(), p.original.size()) == 0,
            "all original patch bytes, including CRT-cached lengths, match the real loaded image");
        const auto page = p.rva & ~(PageSize-1); MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(reinterpret_cast<const std::uint8_t*>(module)+page, &info, sizeof(info)) != 0, "snapshot real page protection");
        protections.emplace(page, info.Protect);
    }
    Require(!ApplyRuntimeKernels(module, pack, false).applied, "a resident image without installer ownership is refused");
    const auto result = ApplyRuntimeKernels(module, pack, true);
    std::printf("INSTALL applied=%d unsafe=%d status=%s\n", result.applied, result.unsafe, result.status.c_str());
    if (expectRefusal) {
        Require(!result.applied && !result.unsafe, "bad or missing pack is refused safely");
        for (const auto& p : plan) { Require(std::memcmp(reinterpret_cast<const std::uint8_t*>(module)+p.rva, p.original.data(), p.original.size()) == 0, "refusal preserves all original bytes"); }
        FreeLibrary(module);
        std::puts("PASS real image refusal preserves original bytes");
        return;
    }
    Require(result.applied && !result.unsafe, "publish real NR image");
    for (const auto [page, before] : protections) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(reinterpret_cast<const std::uint8_t*>(module)+page, &info, sizeof(info)) && ProtectionEquivalent(info.Protect, before),
            "all real image page protections restored with copy-on-write normalization");
    }
    Require(FreeLibrary(module) != 0, "release caller reference");
    HMODULE resident{};
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(module), &resident) && resident == module, "installed image remains pinned");
    std::array<RuntimeKernelResult, 4> results;
    std::array<std::thread, 4> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) { threads[i] = std::thread([&, i] { results[i] = ApplyRuntimeKernels(module, L"missing-pack.trpk", false); }); }
    for (auto& t : threads) { t.join(); }
    for (const auto& r : results) { Require(r.applied && !r.unsafe, "concurrent sessions reuse installation without reading pack file"); }
    std::puts("PASS real image patching, page restoration, lifetime pin and concurrent idempotent reuse");
    // A third party can still corrupt a protected allocation after changing its
    // permissions. Reuse must detect content changes even if protection is back.
    std::int32_t relative{};
    std::memcpy(&relative, reinterpret_cast<const std::uint8_t*>(module)+Bundles[0].site+3, 4);
    auto* storage = reinterpret_cast<std::uint8_t*>(module)+Bundles[0].site+7+relative-512;
    DWORD old{};
    Require(VirtualProtect(storage, PackSize, PAGE_READWRITE, &old) != 0, "simulate competing storage change");
    storage[1000] ^= 1;
    Require(VirtualProtect(storage, PackSize, old, &old) != 0, "restore storage read-only");
    const auto changed = ApplyRuntimeKernels(module, pack, false);
    Require(!changed.applied && changed.unsafe, "tampered installed storage blocks future runtime entry");
    std::puts("PASS installed pack tampering is detected before reuse");
}
#endif

int wmain(int argc, wchar_t** argv)
{
#ifdef TRP_ADA_UNIVERSAL
    Require(RuntimeKernelsSupported(), "Universal supplies installer");
    if (argc == 3 || argc == 4) { ManualProbe(argv[1], argv[2], argc == 4); }
    else { Require(argc == 1, "usage: AdaTests [absolute-runtime absolute-pack [expect-refusal]]"); UnitTests(); }
#else
    (void)argc; (void)argv;
    Require(!RuntimeKernelsSupported(), "Standard does not supply installer");
    const auto result = ApplyRuntimeKernels(reinterpret_cast<HMODULE>(1), L"missing", true);
    Require(!result.applied && !result.unsafe, "Standard refuses without accessing image or pack");
    std::puts("PASS Standard leaves runtime unchanged");
#endif
}
