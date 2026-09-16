#pragma once

namespace TheosRenderPipeline
{
constexpr bool SourceGenerationEnabled(int warmupPresents, bool requested, bool inputsPrepared, bool transitionBlocked = false)
{
    return warmupPresents <= 0 && requested && inputsPrepared && !transitionBlocked;
}
} // namespace TheosRenderPipeline
