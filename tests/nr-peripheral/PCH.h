#pragma once
// Standalone harness: production NR pass and transport, scripted NGX boundary.
#include <algorithm>
#include <cmath>
#include <climits>
#include <format>
#include <string>
#include <Windows.h>
#include <iostream>
namespace logger {
template<class... Args> void info(std::format_string<Args...> text, Args&&... args) {
#ifdef TRP_NR_VENDOR_BENCHMARK
    std::cerr << std::format(text, std::forward<Args>(args)...) << '\n';
#endif
}
template<class... Args> void warn(std::format_string<Args...> text, Args&&... args) { info(text, std::forward<Args>(args)...); }
template<class... Args> void error(std::format_string<Args...> text, Args&&... args) { info(text, std::forward<Args>(args)...); }
}
