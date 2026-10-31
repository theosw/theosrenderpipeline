#pragma once
#include <array>
#include <vector>
struct ID3D12Resource;
#include <cstdint>
namespace Fixture {
inline bool peripheral{}, worldOnly{};
inline unsigned sourceWidth{}, sourceHeight{}, guideWidth{}, guideHeight{}, workWidth{}, workHeight{};
inline unsigned secondWidth{}, secondHeight{};
// Model extents by feature index for Passes 3 to 5; zero uses secondWidth/Height.
inline std::array<unsigned,5> passWidth{}, passHeight{};
inline bool transform{};
// The transform's gain by feature index; each pass's own intensity.
inline std::array<float,5> intensity{.8f,.6f,.6f,.6f,.6f};
inline std::array<int,5> networkPreset{-1,1,1,1,1};
inline unsigned creations{}, evaluations{}, resets{};
inline bool objectControls{};
inline unsigned controlEvaluations{};
// The recording frame, set by the test. Passes 3 to 5 are created one per frame
// and first evaluated on a later frame.
inline unsigned frame{};
inline unsigned lateCreationFrame{~0u};
// The feature index NGX refuses to create, as for an unsupported runtime; ~0u none.
inline unsigned refuseIndex{~0u};
// Each evaluation's motion input, in order.
inline std::vector<ID3D12Resource*> motions;
inline std::vector<unsigned> createdFeatures;
inline std::vector<std::pair<unsigned, bool>> evaluatedFeatures;
inline bool refuseRetirement{};
}

namespace Fixture { inline bool uncertainCreation{}; }
