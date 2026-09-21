#pragma once

#include <Windows.h>
#include <filesystem>
#include <memory>

// Standalone-process instrumentation only. Never linked into the Skyrim plugin.
// Installs in the hash-verified NR module's resolver import; no global NVAPI detour.
class LaunchObserver
{
public:
    LaunchObserver(const std::filesystem::path& runtime, const std::filesystem::path& output);
    ~LaunchObserver();
    LaunchObserver(const LaunchObserver&) = delete;
    LaunchObserver& operator=(const LaunchObserver&) = delete;
    void Begin(unsigned frame, bool reset, bool reuse);
    void End(); // Must succeed before submitting the recorded command list.
    void Retired(); // Only after the oracle's completion fence succeeds.
    bool Reusing() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
