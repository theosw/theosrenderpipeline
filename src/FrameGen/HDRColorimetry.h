#pragma once

#include <array>

namespace TheosRenderPipeline::SourceDLSSG::HDRColorimetry
{
    using Vector = std::array<double, 3>;
    using Matrix = std::array<Vector, 3>; // Rows; multiply column RGB vectors.

    constexpr Vector Cross(Vector a, Vector b)
    {
        return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    }
    constexpr double Dot(Vector a, Vector b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    constexpr Matrix Inverse(Matrix m)
    {
        const Matrix columns{Cross(m[1], m[2]), Cross(m[2], m[0]), Cross(m[0], m[1])};
        const double determinant = Dot(m[0], columns[0]);
        Matrix result{};
        for (unsigned row = 0; row < 3; ++row) {
            for (unsigned col = 0; col < 3; ++col) { result[row][col] = columns[col][row] / determinant; }
        }
        return result;
    }
    constexpr Vector XYZ(double x, double y) { return {x / y, 1.0, (1.0 - x - y) / y}; }
    constexpr Matrix RGBToXYZ(Vector red, Vector green, Vector blue, Vector white)
    {
        Matrix basis{};
        for (unsigned row = 0; row < 3; ++row) { basis[row] = {red[row], green[row], blue[row]}; }
        const auto inverse = Inverse(basis);
        const Vector scale{Dot(inverse[0], white), Dot(inverse[1], white), Dot(inverse[2], white)};
        for (auto& row : basis) { for (unsigned col = 0; col < 3; ++col) { row[col] *= scale[col]; } }
        return basis;
    }
    constexpr Matrix Multiply(Matrix a, Matrix b)
    {
        Matrix result{};
        for (unsigned row = 0; row < 3; ++row) {
            for (unsigned col = 0; col < 3; ++col) {
                for (unsigned k = 0; k < 3; ++k) { result[row][col] += a[row][k] * b[k][col]; }
            }
        }
        return result;
    }

    // ITU-R BT.709-6 table 1.3/1.4 and BT.2100-3 table 2; both use D65.
    // Convert through RGB-to-XYZ; shared D65 needs no chromatic adaptation.
    // https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I!!PDF-E.pdf
    // https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-3-202502-I!!PDF-E.pdf
    inline constexpr auto kWhiteD65 = XYZ(0.3127, 0.3290);
    inline constexpr auto k709ToXYZ = RGBToXYZ(XYZ(0.640, 0.330), XYZ(0.300, 0.600), XYZ(0.150, 0.060), kWhiteD65);
    inline constexpr auto k2020ToXYZ = RGBToXYZ(XYZ(0.708, 0.292), XYZ(0.170, 0.797), XYZ(0.131, 0.046), kWhiteD65);
    inline constexpr auto k709To2020 = Multiply(Inverse(k2020ToXYZ), k709ToXYZ);

    // Application white calibration. This is not the
    // Windows scRGB reference white of 80 nits; changing it changes brightness.
    inline constexpr float kReferenceWhiteNits = 10000.0f / 140.0f;
    inline constexpr float kPQPeakNits = 10000.0f;
    struct ShaderParameters
    {
        std::array<float, 4> red, green, blue;
    };
    inline constexpr ShaderParameters kShaderParameters{
        {float(k709To2020[0][0]), float(k709To2020[0][1]), float(k709To2020[0][2]), kReferenceWhiteNits},
        {float(k709To2020[1][0]), float(k709To2020[1][1]), float(k709To2020[1][2]), kPQPeakNits},
        {float(k709To2020[2][0]), float(k709To2020[2][1]), float(k709To2020[2][2]), 0.0f}};
    static_assert(sizeof(ShaderParameters) == 12 * sizeof(float));
}
