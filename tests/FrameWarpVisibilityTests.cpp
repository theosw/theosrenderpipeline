#include "shared/native_visibility.hpp"
#include <cstdio>
int main() {
    int failures = 0;
    const auto expect = [&](bool pass, const char* message) { if (!pass) { ++failures; std::printf("FAIL: %s\n", message); } };
    // First fresh source, before any image/heartbeat exists: prepare but do not
    // cover the game. This was previously a startup dependency cycle.
    auto state = fw::native_visibility(true, false, 1000, 990, 0, 1000);
    expect(state.render && !state.show, "first source can render without output heartbeat");
    state = fw::native_visibility(true, true, 1010, 990, 1000, 1000);
    expect(state.render && state.show, "first completed image becomes visible");
    state = fw::native_visibility(true, true, 2300, 2290, 1000, 1000);
    expect(state.render && !state.show, "stalled helper hides stale output while producer is fresh");
    state = fw::native_visibility(true, true, 1600, 990, 1590, 1000);
    expect(!state.render && !state.show, "stale producer cannot cover native game output");
    state = fw::native_visibility(false, true, 1100, 1090, 1090, 1000);
    expect(!state.render && !state.show, "disabled/minimized/unfocused game pauses helper");
    state = fw::native_visibility(true, false, 2400, 2390, 0, 1000);
    expect(state.render && !state.show, "resume can prepare a first image after suspension");
    state = fw::native_visibility(true, true, 1000, 1100, 1100, 1000);
    expect(!state.render && !state.show, "future/stale-session heartbeat rejected");
    std::printf("Native FrameWarp visibility checks: %d failures\n", failures);
    return failures ? 1 : 0;
}
