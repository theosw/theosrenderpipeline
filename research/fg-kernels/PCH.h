#pragma once
// Standalone logger adapter: the production feature session itself is unchanged.
#include <Windows.h>
#include <format>
#include <iostream>
namespace logger {
template<class... A> void info(std::format_string<A...> f, A&&... a) {
  std::cerr << std::format(f, std::forward<A>(a)...) << std::endl;
}
template<class... A> void warn(std::format_string<A...> f, A&&... a) { info(f, std::forward<A>(a)...); }
template<class... A> void error(std::format_string<A...> f, A&&... a) { info(f, std::forward<A>(a)...); }
}
