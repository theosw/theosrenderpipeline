// SPDX-License-Identifier: GPL-3.0-only
// Patch facts and pacing algorithm adapted from Coldwood1026's
// OptiScalerDp4aUnlock @ 9eea95bba9fda7121f214d2eba358423be598d7e.
// Original GPL-3.0 source: OptiScaler/proxies/XeFGUnlock.h and XeFGPacing.h.
// TRP adaptation: exact identity, one checked publication transaction, context
// epochs, bounded diagnostics and native forwarding established before writes.
// No implementation copied from PureDark's proprietary backend.
#include "XeFGUnlock.h"
#include "XeFGOptions.h"
#include <bcrypt.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <atomic>

namespace TheosRenderPipeline::XeFGUnlock
{
    namespace
    {
        constexpr std::uint32_t ImageSize = 0x15ed000, PageSize = 4096;
        bool Matches(Memory& memory, const Patch& patch, bool replacement)
        {
            std::array<std::uint8_t, 16> actual{};
            if (patch.original.size() > actual.size()) return false;
            const auto wanted = replacement ? std::span<const std::uint8_t>(patch.replacement)
                                            : std::span<const std::uint8_t>(patch.original);
            return memory.Read(patch.rva, std::span(actual).first(wanted.size())) &&
                std::equal(wanted.begin(), wanted.end(), actual.begin());
        }
        struct Page {
            DWORD original{};
            bool writable{};
        };
    }
    PatchResult Publish(Memory& memory, std::span<const Patch> patches, std::uint32_t size, std::string& reason)
    {
        std::map<std::uint32_t, Page> pages;
        for (std::size_t i = 0; i < patches.size(); ++i) {
            const auto& p = patches[i];
            if (p.original.empty() || p.original.size() > 16 || p.original.size() != p.replacement.size() ||
                p.rva > size || p.original.size() > size - p.rva) {
                reason = "invalid patch range";
                return PatchResult::Refused;
            }
            for (std::size_t j = 0; j < i; ++j)
                if (p.rva < patches[j].rva + patches[j].original.size() && patches[j].rva < p.rva + p.original.size()) {
                    reason = "overlapping patches";
                    return PatchResult::Refused;
                }
            if (!Matches(memory, p, false)) {
                reason = "original bytes mismatch";
                return PatchResult::Refused;
            }
            for (auto page = p.rva & ~(PageSize - 1);
                page <= ((p.rva + static_cast<std::uint32_t>(p.original.size()) - 1) & ~(PageSize - 1));
                page += PageSize)
                pages.try_emplace(page);
        }
        auto restoreProtection = [&]() {
            bool ok = true;
            for (auto it = pages.rbegin(); it != pages.rend(); ++it)
                if (it->second.writable) {
                    DWORD ignored{};
                    const bool restored = memory.Protect(it->first, it->second.original, ignored);
                    ok = restored && memory.ProtectionIs(it->first, it->second.original) && ok;
                    if (restored) it->second.writable = false;
                }
            return ok;
        };
        for (auto& [page, state] : pages) {
            if (!memory.Protect(page, PAGE_EXECUTE_READWRITE, state.original)) {
                reason = "writable protection refused";
                const bool restored = restoreProtection();
                bool originals = true;
                for (const auto& p : patches) originals = Matches(memory, p, false) && originals;
                return restored && originals ? PatchResult::Refused : PatchResult::Unsafe;
            }
            state.writable = true;
        }
        // A second byte check closes the protection-acquisition window.
        for (const auto& p : patches)
            if (!Matches(memory, p, false)) {
                reason = "bytes changed during admission";
                restoreProtection();
                return PatchResult::Unsafe; // Do not overwrite a competing owner's bytes.
            }
        bool wrote = true;
        for (const auto& p : patches)
            if (!memory.Write(p.rva, p.replacement)) {
                wrote = false;
                break;
            }
        bool verified = wrote;
        for (const auto& p : patches) verified = Matches(memory, p, true) && verified;
        const bool flushed = verified && memory.Flush();
        const bool protectedAgain = restoreProtection();
        if (verified && flushed && protectedAgain) {
            reason = "all eight patches published";
            return PatchResult::Applied;
        }
        // Rollback is a verified operation, including a partly failed current write.
        bool writable = true;
        for (auto& [page, state] : pages) {
            DWORD ignored{};
            const bool ok = memory.Protect(page, PAGE_EXECUTE_READWRITE, ignored);
            state.writable = ok;
            writable = ok && writable;
        }
        bool original = writable;
        if (writable) {
            for (const auto& p : patches) {
                // Unknown competing edits must not be silently replaced.
                // Partial writes from this transaction are owned here: no other SDK
                // owner/thread may run while Publish is executing.
                original = memory.Write(p.rva, p.original) && original;
            }
            for (const auto& p : patches) original = Matches(memory, p, false) && original;
        }
        const bool rollbackFlushed = memory.Flush();
        const bool rollbackProtected = restoreProtection();
        reason = original && rollbackFlushed && rollbackProtected ? "publication failed; original image restored"
                                                                  : "unsafe rollback failure";
        return original && rollbackFlushed && rollbackProtected ? PatchResult::Refused : PatchResult::Unsafe;
    }

    std::vector<Patch> MakePlan(std::uintptr_t present, std::uintptr_t scheduler, std::uintptr_t deadline)
    {
        constexpr auto ceiling = static_cast<std::uint8_t>(XeFGMaxGeneratedFrames);
        static_assert(XeFGMaxGeneratedFrames <= 255);
        std::vector<Patch> plan{{0x20da4f, {0x0f, 0x85, 0xcc, 0, 0, 0}, {0xe9, 0xcd, 0, 0, 0, 0x90}},
            {0x1a5de4, {0x74, 0x09}, {0xeb, 0x06}}, {0x1a517d, {0xbb, 3, 0, 0, 0}, {0xbb, ceiling, 0, 0, 0}},
            {0x1a45c2, {0xc7, 0x87, 0x6c, 1, 0, 0, 1, 0, 0, 0}, {0xc7, 0x87, 0x6c, 1, 0, 0, ceiling, 0, 0, 0}},
            {0x20973b, {0xb8, 1, 0, 0, 0}, {0xb8, ceiling, 0, 0, 0}}};
        for (const auto [rva, target, detour] : {std::array<std::uintptr_t, 3>{0x25c0, 0x21f730, present},
                 std::array<std::uintptr_t, 3>{0x3100, 0x21ee30, scheduler},
                 std::array<std::uintptr_t, 3>{0x3430, 0x224b30, deadline}}) {
            Patch p{static_cast<std::uint32_t>(rva), std::vector<std::uint8_t>(16, 0xcc),
                std::vector<std::uint8_t>(16, 0xcc)};
            p.original[0] = 0xe9;
            const auto distance = static_cast<std::int32_t>(target - rva - 5);
            std::memcpy(p.original.data() + 1, &distance, 4);
            p.replacement[0] = 0xff;
            p.replacement[1] = 0x25;
            std::fill(p.replacement.begin() + 2, p.replacement.begin() + 6, std::uint8_t{0});
            std::memcpy(p.replacement.data() + 6, &detour, 8);
            plan.push_back(std::move(p));
        }
        return plan;
    }

    namespace
    {
        bool Hash(std::span<const std::uint8_t> bytes, std::array<std::uint8_t, 32>& result)
        {
            BCRYPT_ALG_HANDLE alg{};
            BCRYPT_HASH_HANDLE hash{};
            if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
            bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
            if (ok)
                ok = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0;
            if (ok) ok = BCryptFinishHash(hash, result.data(), 32, 0) >= 0;
            if (hash) BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            return ok;
        }
        const std::array<std::uint8_t, 32> ExpectedHash{0xec, 0x5e, 0x0c, 0x65, 0xe0, 0x75, 0x57, 0x0c, 0x6e, 0xde,
            0x72, 0x61, 0x8b, 0xb6, 0x66, 0xd0, 0xbe, 0x0c, 0x2e, 0x10, 0xb2, 0xea, 0x97, 0x62, 0xc0, 0xfe, 0x8c, 0xb8,
            0xe3, 0x75, 0xab, 0x27};
        bool ReadSafe(const void* src, void* dst, std::size_t size)
        {
            __try {
                std::memcpy(dst, src, size);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        struct WinMemory final : Memory {
            std::uint8_t* base;
            explicit WinMemory(HMODULE module) : base(reinterpret_cast<std::uint8_t*>(module)) {}
            bool Read(std::uint32_t rva, std::span<std::uint8_t> out) override
            {
                return ReadSafe(base + rva, out.data(), out.size());
            }
            bool Write(std::uint32_t rva, std::span<const std::uint8_t> bytes) override
            {
                __try {
                    std::memcpy(base + rva, bytes.data(), bytes.size());
                    return true;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    return false;
                }
            }
            bool Protect(std::uint32_t page, DWORD value, DWORD& old) override
            {
                return VirtualProtect(base + page, PageSize, value, &old) != 0;
            }
            bool ProtectionIs(std::uint32_t page, DWORD value) override
            {
                MEMORY_BASIC_INFORMATION info{};
                return VirtualQuery(base + page, &info, sizeof(info)) && info.Protect == value;
            }
            bool Flush() override { return FlushInstructionCache(GetCurrentProcess(), base, ImageSize) != 0; }
        };
        using PresentFn = std::int64_t (*)(
            void*, std::uint32_t, std::uint32_t, std::uint64_t, void*, void*, std::uint64_t);
        using SchedulerFn = bool (*)(void*, void*, std::uint8_t, void*, std::uint32_t);
        using RingFn = void* (*)(void*, void*);
        using DeadlineFn = void* (*)(void*, std::int64_t*, void*, void*, std::uint32_t, std::uint32_t);
        PresentFn nativePresent{};
        SchedulerFn nativeScheduler{};
        RingFn nativeRing{};
        DeadlineFn nativeDeadline{};
        std::uint8_t* moduleBase{};
        Observer observer{};
        bool forceSchedulerRefusal{}; // Explicit fixture injection; never compiled into the renderer.
        LARGE_INTEGER frequency{};
        std::uint64_t epoch{};
        struct AtomicStats {
            std::atomic<std::uint64_t> presents{}, scheduled{}, refused{}, wallWaits{}, deadlines{}, contextChanges{};
        };
        AtomicStats stats{};
#if defined(TRP_XEFG_TESTING)
        std::mutex diagnosticsMutex;
        std::vector<Event> events;
#endif
        struct Timing {
            void* context{};
            std::uint64_t epoch{};
            std::int64_t lastBurst{}, period{}, nextDeadline{}, step{};
            bool fallbackBurst{};
            std::uint32_t lastIndex{}, lastCount{};
            std::array<std::int64_t, 15> samples{};
            unsigned size{}, position{};
        };
        thread_local Timing timing;
        std::int64_t Qpc()
        {
            LARGE_INTEGER q{};
            QueryPerformanceCounter(&q);
            return q.QuadPart;
        }
        std::int64_t Ns(std::int64_t ticks) { return ticks * 1000000000LL / frequency.QuadPart; }
        std::int64_t Ticks(std::int64_t ns) { return ns * frequency.QuadPart / 1000000000LL; }
        void Context(void* ctx)
        {
            if (timing.context != ctx || timing.epoch != epoch) {
                timing = {};
                timing.context = ctx;
                timing.epoch = epoch;
                stats.contextChanges.fetch_add(1, std::memory_order_relaxed);
            }
        }
        void Period(std::uint64_t index, std::int64_t now)
        {
            if (index != 1) return;
            if (timing.lastBurst && now > timing.lastBurst) {
                const auto ns = Ns(now - timing.lastBurst);
                // Do not keep pacing history through a loading-size gap.
                if (ns > 500000000LL) {
                    timing.size = timing.position = 0;
                    timing.period = 0;
                } else {
                    timing.samples[timing.position++ % 15] = ns;
                    timing.size = (std::min)(timing.size + 1, 15u);
                    auto sorted = timing.samples;
                    std::sort(sorted.begin(), sorted.begin() + timing.size);
                    timing.period = sorted[timing.size / 2];
                }
            }
            timing.lastBurst = now;
        }
        void WallWait(Event& e)
        {
            if (timing.period <= 0) return;
            const auto step = Ticks(timing.period / static_cast<std::int64_t>(e.count + 1));
            // Derive by index, not by how many waits happened: mixed successful/refused
            // scheduling must not compress the remaining frames into one time slot.
            const auto target = (std::min)(
                timing.lastBurst + step * static_cast<std::int64_t>(e.index), e.entryQpc + Ticks(timing.period));
            auto now = Qpc();
            while (now < target) {
                if (target - now > Ticks(200000)) Sleep(0);
                else YieldProcessor();
                now = Qpc();
            }
            stats.wallWaits.fetch_add(1, std::memory_order_relaxed);
        }
        bool Burst(void* arg5, void* arg6, std::uint64_t flag, bool last, Event& e)
        {
            if (!arg5 || ((static_cast<std::uint8_t>(flag) == 1) == last)) return false;
            auto* burst = static_cast<std::uint8_t*>(arg5) - 0x38;
            std::uint64_t count{}, frames{};
            if (!ReadSafe(burst + 8, &count, 8) || count < 2 || count > XeFGMaxGeneratedFrames)
                return false; // x2 forwarded untouched.
            e.count = count;
            e.index = count;
            e.finalFrame = last;
            e.burst = reinterpret_cast<std::uintptr_t>(burst);
            if (!last) {
                if (!ReadSafe(burst, &frames, 8) || !frames || reinterpret_cast<std::uintptr_t>(arg6) < frames)
                    return false;
                const auto offset = reinterpret_cast<std::uintptr_t>(arg6) - frames;
                e.index = offset / 8;
                if (offset % 8 || e.index < 1 || e.index >= count) return false;
            }
            std::uintptr_t resource{};
            if (arg6) ReadSafe(arg6, &resource, sizeof(resource));
            e.resource = resource;
            return true;
        }
        std::int64_t Present(
            void* ctx, std::uint32_t a2, std::uint32_t a3, std::uint64_t a4, void* a5, void* a6, std::uint64_t a7)
        {
            const auto caller = static_cast<std::uint8_t*>(_ReturnAddress());
            const bool intermediate = caller == moduleBase + 0x2202ed, last = caller == moduleBase + 0x220467;
            Event event{};
            bool capture = (intermediate || last) && Burst(a5, a6, a7, last, event);
            if (capture) {
                Context(ctx);
                event.context = reinterpret_cast<std::uintptr_t>(ctx);
                event.entryQpc = Qpc();
                Period(event.index, event.entryQpc);
                if (event.index == 1) timing.fallbackBurst = false;
                auto* bytes = static_cast<std::uint8_t*>(ctx);
                auto* burst = reinterpret_cast<std::uint8_t*>(event.burst);
                std::uint8_t limiter{}, enabled{}, gate{};
                std::uint32_t limiterField{};
                ReadSafe(bytes + 0x340, &limiter, 1);
                ReadSafe(bytes + 0x341, &enabled, 1);
                event.scheduler = limiter == 0 && enabled != 0;
                if (event.scheduler && !last) {
                    alignas(16) std::array<std::uint8_t, 32> snapshot{};
                    nativeRing(bytes + 0x168, snapshot.data());
                    ReadSafe(burst + 0xc0, &gate, 1);
// Fixture-only injection: the pinned scheduler rejects index zero
// before waiting or updating history (0x21ee74 -> 0x21efa7).
// A zero gate only skips its optional fence wait, not scheduling.
#if defined(TRP_XEFG_TESTING)
                    const auto schedulerIndex = forceSchedulerRefusal ? 0 : static_cast<std::uint32_t>(event.index);
#else
                    const auto schedulerIndex = static_cast<std::uint32_t>(event.index);
#endif
                    event.scheduled = nativeScheduler(ctx, burst, gate & 1, snapshot.data(), schedulerIndex);
                    // The gate may say yes while history/fence lookup still refuses.
                    if (!event.scheduled) {
                        timing.fallbackBurst = true;
                        WallWait(event);
                    }
                    if (event.scheduled) stats.scheduled.fetch_add(1, std::memory_order_relaxed);
                    else stats.refused.fetch_add(1, std::memory_order_relaxed);
                } else if (last && event.scheduler && timing.fallbackBurst) {
                    // Without intermediate deadline calls, the SDK's final scheduler
                    // may lack a usable anchor. A past target adds no duplicate wait.
                    WallWait(event);
                } else if (!event.scheduler) {
                    ReadSafe(burst + 0x28, &limiterField, 4);
                    if (!last || !(event.count > 2 && limiter != 0 && limiterField == 0)) WallWait(event);
                }
                event.pacedQpc = Qpc();
                event.deadlineNs = timing.nextDeadline;
            }
// Optional diagnostic readback runs before Present: FLIP_DISCARD content
// after presentation is not evidence of the image that was submitted.
#if defined(TRP_XEFG_TESTING)
            if (capture && observer) observer(event);
#endif
            const auto result = nativePresent(ctx, a2, a3, a4, a5, a6, a7);
            if (capture) {
                stats.presents.fetch_add(1, std::memory_order_relaxed);
#if defined(TRP_XEFG_TESTING)
                event.result = result;
                event.exitQpc = Qpc();
                {
                    std::lock_guard lock(diagnosticsMutex);
                    if (events.size() < 20000) events.push_back(event);
                }
#endif
            }
            return result;
        }
        bool Scheduler(void* ctx, void* burst, std::uint8_t gate, void* snapshot, std::uint32_t index)
        {
            Context(ctx);
            return nativeScheduler(ctx, burst, gate, snapshot, index);
        }
        void* Deadline(
            void* ring, std::int64_t* out, void* lookup, void* snapshot, std::uint32_t index, std::uint32_t countPlus1)
        {
            void* result = nativeDeadline(ring, out, lookup, snapshot, index, countPlus1);
            if (!out || !snapshot || index < 1 || index > XeFGMaxGeneratedFrames || countPlus1 < 3 ||
                countPlus1 > XeFGMaxGeneratedFrames + 1)
                return result;
            Context(static_cast<std::uint8_t*>(ring) - 0x168);
            std::int64_t median{};
            ReadSafe(static_cast<std::uint8_t*>(snapshot) + 8, &median, 8);
            const auto unit = median / static_cast<std::int64_t>(countPlus1);
            if (unit <= 0) return result;
            const bool fresh = index == 1 || index <= timing.lastIndex || countPlus1 != timing.lastCount;
            if (fresh) {
                float measured{};
                ReadSafe(static_cast<std::uint8_t*>(ring) + 0x1b8, &measured, 4);
                if (!std::isfinite(measured) || measured < 0.125f || measured > 500) measured = 500;
                const auto clamped = static_cast<std::int64_t>(measured * 1000000.0f) / countPlus1;
                timing.nextDeadline = *out + static_cast<std::int64_t>(index) * (unit - (std::min)(unit, clamped));
                timing.step = unit;
            } else timing.nextDeadline += timing.step;
            timing.lastIndex = index;
            timing.lastCount = countPlus1;
            *out = timing.nextDeadline;
            stats.deadlines.fetch_add(1, std::memory_order_relaxed);
            return result;
        }
        HMODULE installed{};
        bool unsafePublication{};
    }
    PatchResult Install(HMODULE runtime, Observer callback, std::string& reason, bool forceRefusal)
    {
        if (unsafePublication) {
            reason = "prior unsafe publication; restart required";
            return PatchResult::Unsafe;
        }
        if (installed) {
            reason = "repeated installation refused";
            return PatchResult::Refused;
        }
        if (!runtime) {
            reason = "null runtime";
            return PatchResult::Refused;
        }
        wchar_t path[32768]{};
        const auto len = GetModuleFileNameW(runtime, path, 32768);
        if (!len || len >= 32768) {
            reason = "module path unavailable";
            return PatchResult::Refused;
        }
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(file)), {});
        std::array<std::uint8_t, 32> hash{};
        if (!Hash(raw, hash) || hash != ExpectedHash) {
            reason = "runtime SHA256 not allowlisted";
            return PatchResult::Refused;
        }
        auto* base = reinterpret_cast<std::uint8_t*>(runtime);
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!ReadSafe(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
            dos.e_lfanew > 4096 || !ReadSafe(base + dos.e_lfanew, &nt, sizeof(nt)) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.FileHeader.TimeDateStamp != 0x69cb0f4d || nt.OptionalHeader.SizeOfImage != ImageSize) {
            reason = "mapped PE identity not allowlisted";
            return PatchResult::Refused;
        }
        // Compare every original .text byte against this hash-identified file;
        // applying PE base relocations handles ASLR before comparing mapped code.
        std::vector<std::uint8_t> expected(ImageSize);
        const auto* diskNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos.e_lfanew);
        const auto* sections = IMAGE_FIRST_SECTION(diskNt);
        for (WORD i = 0; i < diskNt->FileHeader.NumberOfSections; ++i) {
            const auto& s = sections[i];
            if (s.VirtualAddress > ImageSize || s.SizeOfRawData > ImageSize - s.VirtualAddress ||
                s.PointerToRawData > raw.size() || s.SizeOfRawData > raw.size() - s.PointerToRawData) {
                reason = "invalid section extent";
                return PatchResult::Refused;
            }
            std::memcpy(expected.data() + s.VirtualAddress, raw.data() + s.PointerToRawData, s.SizeOfRawData);
        }
        const auto delta = reinterpret_cast<std::uintptr_t>(base) - diskNt->OptionalHeader.ImageBase;
        const auto reloc = diskNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        std::size_t offset = reloc.VirtualAddress;
        while (offset < static_cast<std::size_t>(reloc.VirtualAddress) + reloc.Size) {
            const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(expected.data() + offset);
            if (block->SizeOfBlock < sizeof(*block) || block->SizeOfBlock > reloc.Size ||
                offset + block->SizeOfBlock > expected.size()) {
                reason = "invalid relocation block";
                return PatchResult::Refused;
            }
            const auto* items = reinterpret_cast<const WORD*>(block + 1);
            for (std::size_t i = 0; i < (block->SizeOfBlock - sizeof(*block)) / 2; ++i)
                if (items[i] >> 12 == IMAGE_REL_BASED_DIR64) {
                    const auto target = block->VirtualAddress + (items[i] & 0xfff);
                    if (target > ImageSize - 8) {
                        reason = "invalid relocation target";
                        return PatchResult::Refused;
                    }
                    std::uint64_t value{};
                    std::memcpy(&value, expected.data() + target, 8);
                    value += delta;
                    std::memcpy(expected.data() + target, &value, 8);
                }
            offset += block->SizeOfBlock;
        }
        bool text = false;
        for (WORD i = 0; i < diskNt->FileHeader.NumberOfSections; ++i)
            if (std::memcmp(sections[i].Name, ".text", 5) == 0) {
                const auto& s = sections[i];
                std::vector<std::uint8_t> mapped(s.SizeOfRawData);
                if (!ReadSafe(base + s.VirtualAddress, mapped.data(), mapped.size()) ||
                    std::memcmp(mapped.data(), expected.data() + s.VirtualAddress, mapped.size()) != 0) {
                    reason = "mapped text changed or foreign hook present";
                    return PatchResult::Refused;
                }
                text = true;
            }
        if (!text || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            reason = "text/clock unavailable";
            return PatchResult::Refused;
        }
        moduleBase = base;
        observer = callback;
        forceSchedulerRefusal = forceRefusal;
        nativePresent = reinterpret_cast<PresentFn>(base + 0x21f730);
        nativeScheduler = reinterpret_cast<SchedulerFn>(base + 0x21ee30);
        nativeRing = reinterpret_cast<RingFn>(base + 0x224cf0);
        nativeDeadline = reinterpret_cast<DeadlineFn>(base + 0x224b30);
        const auto plan = MakePlan(reinterpret_cast<std::uintptr_t>(&Present),
            reinterpret_cast<std::uintptr_t>(&Scheduler), reinterpret_cast<std::uintptr_t>(&Deadline));
        WinMemory memory(runtime);
        const auto result = Publish(memory, plan, ImageSize, reason);
        if (result == PatchResult::Applied) {
            installed = runtime;
            NewContextEpoch();
        } else if (result == PatchResult::Unsafe) {
            unsafePublication = true;
        } else if (result == PatchResult::Refused) {
            moduleBase = nullptr;
            nativePresent = nullptr;
            nativeScheduler = nullptr;
            nativeDeadline = nullptr;
        }
        return result;
    }
    PatchResult EnsureInstalled(HMODULE runtime, std::string& reason)
    {
        if (!installed) return Install(runtime, nullptr, reason);
        if (installed != runtime) {
            reason = "runtime owner changed after publication";
            return PatchResult::Unsafe;
        }
        WinMemory memory(runtime);
        const auto plan = MakePlan(reinterpret_cast<std::uintptr_t>(&Present),
            reinterpret_cast<std::uintptr_t>(&Scheduler), reinterpret_cast<std::uintptr_t>(&Deadline));
        for (const auto& patch : plan)
            if (!Matches(memory, patch, true)) {
                reason = "published code changed";
                return PatchResult::Unsafe;
            }
        reason = "checked patches already resident";
        return PatchResult::Applied;
    }
    void NewContextEpoch() { ++epoch; }
    Stats Snapshot()
    {
        return {stats.presents.load(), stats.scheduled.load(), stats.refused.load(), stats.wallWaits.load(),
            stats.deadlines.load(), stats.contextChanges.load()};
    }
    void SaveEvents(const char* path)
    {
#if defined(TRP_XEFG_TESTING)
        std::lock_guard lock(diagnosticsMutex);
        std::ofstream out(path);
        out << "context,resource,burst,index,count,entryQpc,pacedQpc,exitQpc,result,deadlineNs,scheduler,scheduled,finalFrame,frequency\n";
        for (const auto& e : events)
            out << e.context << ',' << e.resource << ',' << e.burst << ',' << e.index << ',' << e.count << ','
                << e.entryQpc << ',' << e.pacedQpc << ',' << e.exitQpc << ',' << e.result << ',' << e.deadlineNs << ','
                << e.scheduler << ',' << e.scheduled << ',' << e.finalFrame << ',' << frequency.QuadPart << '\n';
#else
        (void)path;
#endif
    }
}
