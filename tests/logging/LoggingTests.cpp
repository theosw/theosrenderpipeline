#include "LoggingPolicy.h"
#include <spdlog/sinks/base_sink.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
static void Require(bool ok) { if (!ok) { std::fputs("logging check failed\n", stderr); std::exit(1); } }
struct Sink : spdlog::sinks::base_sink<std::mutex> {
    unsigned records{}, flushes{};
    void sink_it_(const spdlog::details::log_msg&) override { ++records; }
    void flush_() override { ++flushes; }
};
int main(int argc, char** argv) {
    Require(argc == 2);
    const std::string_view value = argv[1];
    SetEnvironmentVariableA("TRP_TRACE_NATIVE_UI", value == "unset" ? nullptr : argv[1]);
    Require(TheosRenderPipeline::Logging::NativeUITraceEnabled() == (value == "1"));
    auto sink = std::make_shared<Sink>();
    spdlog::logger logger("test", sink);
    TheosRenderPipeline::Logging::ConfigureFlush(logger);
    for (unsigned i = 0; i < 48; ++i) logger.info("routine record");
    Require(sink->records == 48 && sink->flushes == 0);
    logger.warn("warning"); Require(sink->records == 49 && sink->flushes == 1);
    logger.error("error"); Require(sink->records == 50 && sink->flushes == 2);
    logger.flush(); Require(sink->flushes == 3);
    std::puts("logging policy PASS");
}
