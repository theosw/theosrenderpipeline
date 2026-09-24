#include "FrameGen/SourceDLSSGCamera.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace DirectX;
using namespace TheosRenderPipeline::SourceDLSSG;

static void Require(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

static XMMATRIX Matrix(const sl::float4x4& value)
{
    XMFLOAT4X4 copy;
    std::memcpy(&copy, &value, sizeof(copy));
    return XMLoadFloat4x4(&copy);
}

static void SamePoint(FXMVECTOR first, FXMVECTOR second, const char* message)
{
    XMFLOAT4 a, b;
    XMStoreFloat4(&a, XMVectorDivide(first, XMVectorSplatW(first)));
    XMStoreFloat4(&b, XMVectorDivide(second, XMVectorSplatW(second)));
    Require(std::abs(a.x - b.x) < 0.00001f && std::abs(a.y - b.y) < 0.00001f &&
        std::abs(a.z - b.z) < 0.00001f, message);
}

int main()
{
    constexpr float nearPlane = 5.0f, farPlane = 100000.0f;
    XMFLOAT4X4 view; XMStoreFloat4x4(&view, XMMatrixIdentity());
    for (const bool rightHanded : {false, true}) {
        XMFLOAT4X4 forward;
        XMStoreFloat4x4(&forward, rightHanded ? XMMatrixPerspectiveFovRH(1.0f, 2.0f, nearPlane, farPlane) :
            XMMatrixPerspectiveFovLH(1.0f, 2.0f, nearPlane, farPlane));
        auto reversed = forward;
        // The producer reverses clip-space Z while preserving X, Y and W.
        for (unsigned row = 0; row < 4; ++row) { reversed.m[row][2] = forward.m[row][3] - forward.m[row][2]; }
        CameraHistory history;
        sl::Constants constants{};
        unsigned frame = 0;
        for (const bool inverted : {false, true, false}) {
            const auto& projection = inverted ? reversed : forward;
            for (unsigned step = 0; step < 2; ++step) {
                const sl::float3 position{float(step), 0, 0};
                Require(history.Build(projection, view, position, nearPlane, farPlane, 0.25f, -0.5f,
                    1, ++frame, false, constants), "valid producer projection accepted");
                Require((constants.depthInverted == sl::eTrue) == inverted, "depth flag matches near/far ordering");
                Require((constants.reset == sl::eTrue) == (step == 0), "convention changes reset history once");
                Require(constants.cameraNear == nearPlane && constants.cameraFar == farPlane,
                    "physical near/far distances remain ordered");
                const float sign = rightHanded ? -1.0f : 1.0f;
                const auto nearClip = XMVector4Transform(XMVectorSet(0, 0, sign * nearPlane, 1), Matrix(constants.cameraViewToClip));
                const auto farClip = XMVector4Transform(XMVectorSet(0, 0, sign * farPlane, 1), Matrix(constants.cameraViewToClip));
                const float nearDepth = XMVectorGetZ(nearClip) / XMVectorGetW(nearClip);
                const float farDepth = XMVectorGetZ(farClip) / XMVectorGetW(farClip);
                Require(std::abs(nearDepth - (inverted ? 1.0f : 0.0f)) < 0.00001f &&
                    std::abs(farDepth - (inverted ? 0.0f : 1.0f)) < 0.00001f,
                    "camera matrix preserves the producer's actual depth values");
                const auto point = XMVectorSet(3, 2, sign * 100, 1);
                const auto currentClip = XMVector4Transform(point, XMMatrixTranslation(-position.x, 0, 0) * XMLoadFloat4x4(&projection));
                const auto previousClip = XMVector4Transform(point, XMLoadFloat4x4(&projection));
                SamePoint(XMVector4Transform(currentClip, Matrix(constants.clipToPrevClip)), previousClip,
                    "reprojection agrees with the previous camera in either convention");
                SamePoint(XMVector4Transform(nearClip, Matrix(constants.clipToCameraView)),
                    XMVectorSet(0, 0, sign * nearPlane, 1), "inverse projection remains coherent");
            }
        }
        auto invalid = forward; invalid._43 = 0;
        Require(!history.Build(invalid, view, {0, 0, 0}, nearPlane, farPlane, 0, 0, 1, ++frame, false, constants),
            "depth-degenerate projection rejected");
        Require(!history.valid, "invalid projection clears temporal history");
        Require(history.Build(reversed, view, {0, 0, 0}, nearPlane, farPlane, 0, 0, 1, ++frame, false, constants) &&
            constants.reset == sl::eTrue, "valid reversed projection recovers with reset");
        Require(!history.Build(forward, view, {0, 0, 0}, 0, farPlane, 0, 0, 1, ++frame, false, constants),
            "zero near distance rejected");
        Require(!history.Build(forward, view, {0, 0, 0}, farPlane, nearPlane, 0, 0, 1, ++frame, false, constants),
            "physical frustum is not reversed to signal inverted depth");
        invalid = reversed; invalid._33 = std::numeric_limits<float>::quiet_NaN();
        Require(!history.Build(invalid, view, {0, 0, 0}, nearPlane, farPlane, 0, 0, 1, ++frame, false, constants),
            "nonfinite projection rejected");
    }
    std::puts("PASS: forward/reversed LH/RH camera depth, reprojection, convention transitions and invalid input recovery");
}
