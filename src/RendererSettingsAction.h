#pragma once
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace TheosRenderPipeline
{
struct RendererSettingsResult
{
    std::string message;
    bool error{};
    bool applied{};
};
template<class Log> RendererSettingsResult RejectSettingsAction(bool save, std::string message, Log&& log)
{
    log(std::format("[Overlay] {} rejected: {}", save ? "Save as default" : "Apply", message));
    return {std::move(message), true, false};
}
enum class SettingsStatusKind { Neutral, Pending, Success, Error };
struct SettingsActionStatus { std::string text; SettingsStatusKind kind{}; };
inline SettingsActionStatus SettingsStatus(int pending, std::string_view message, bool error)
{
    if (error && !message.empty()) { return {std::string(message), SettingsStatusKind::Error}; }
    if (pending > 0) { return {std::format("{} pending change{}", pending, pending == 1 ? "" : "s"), SettingsStatusKind::Pending}; }
    if (!message.empty()) { return {std::string(message), SettingsStatusKind::Success}; }
    return {"No changes", SettingsStatusKind::Neutral};
}
}
