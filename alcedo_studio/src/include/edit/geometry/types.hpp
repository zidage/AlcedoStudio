//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace alcedo {

inline constexpr float kGeometryMinNormalizedSize = 1e-4f;
inline constexpr float kGeometryMatrixEpsilon     = 1e-4f;

struct Extent2D {
  std::uint32_t width  = 0;
  std::uint32_t height = 0;

  [[nodiscard]] auto Empty() const -> bool { return width == 0 || height == 0; }
};

inline auto operator==(const Extent2D& a, const Extent2D& b) -> bool {
  return a.width == b.width && a.height == b.height;
}

inline auto operator!=(const Extent2D& a, const Extent2D& b) -> bool { return !(a == b); }

/**
 * @brief Integer rectangle in left-closed, right-open form [x, x+width) x [y, y+height).
 */
struct RectI {
  std::int32_t x      = 0;
  std::int32_t y      = 0;
  std::int32_t width  = 0;
  std::int32_t height = 0;

  [[nodiscard]] auto X1() const -> std::int32_t { return x + width; }
  [[nodiscard]] auto Y1() const -> std::int32_t { return y + height; }

  [[nodiscard]] auto Contains(std::int32_t px, std::int32_t py) const -> bool {
    return px >= x && px < X1() && py >= y && py < Y1();
  }
};

inline auto operator==(const RectI& a, const RectI& b) -> bool {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

struct Vector2 {
  float x = 0.0f;
  float y = 0.0f;
};

/**
 * @brief Row-major 3x3 matrix mapping homogeneous 2D points (column vectors).
 *
 * Pixel-center coordinates use (x + 0.5, y + 0.5). Affine maps keep the last row [0, 0, 1].
 */
struct Matrix3x3 {
  float m[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

  static auto Identity() -> Matrix3x3 { return {}; }

  static auto Translate(float tx, float ty) -> Matrix3x3 {
    Matrix3x3 r;
    r.m[2] = tx;
    r.m[5] = ty;
    return r;
  }

  static auto Scale(float sx, float sy) -> Matrix3x3 {
    Matrix3x3 r;
    r.m[0] = sx;
    r.m[4] = sy;
    return r;
  }

  /// Counterclockwise rotation about the origin, @p radians.
  static auto Rotate(float radians) -> Matrix3x3 {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Matrix3x3   r;
    r.m[0] = c;
    r.m[1] = -s;
    r.m[3] = s;
    r.m[4] = c;
    return r;
  }
};

inline auto operator*(const Matrix3x3& a, const Matrix3x3& b) -> Matrix3x3 {
  Matrix3x3 c{};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      c.m[row * 3 + col] = a.m[row * 3 + 0] * b.m[0 * 3 + col] + a.m[row * 3 + 1] * b.m[1 * 3 + col] +
                           a.m[row * 3 + 2] * b.m[2 * 3 + col];
    }
  }
  return c;
}

/**
 * @brief Maps a 2D point with implicit w = 1. Does not divide by w (affine maps keep w = 1).
 */
inline auto TransformPoint(const Matrix3x3& matrix, Vector2 point) -> Vector2 {
  return Vector2{matrix.m[0] * point.x + matrix.m[1] * point.y + matrix.m[2],
                 matrix.m[3] * point.x + matrix.m[4] * point.y + matrix.m[5]};
}

/**
 * @brief Analytic inverse of an affine 3x3 (last row [0, 0, 1]).
 * @throws std::runtime_error if the upper 2x2 is singular.
 */
inline auto InvertAffine(const Matrix3x3& matrix) -> Matrix3x3 {
  const float det = matrix.m[0] * matrix.m[4] - matrix.m[1] * matrix.m[3];
  if (!std::isfinite(det) || std::fabs(det) < 1e-12f) {
    throw std::runtime_error("InvertAffine: singular matrix");
  }
  const float inv_det = 1.0f / det;
  Matrix3x3   inv;
  inv.m[0] = matrix.m[4] * inv_det;
  inv.m[1] = -matrix.m[1] * inv_det;
  inv.m[2] = (matrix.m[1] * matrix.m[5] - matrix.m[4] * matrix.m[2]) * inv_det;
  inv.m[3] = -matrix.m[3] * inv_det;
  inv.m[4] = matrix.m[0] * inv_det;
  inv.m[5] = (matrix.m[3] * matrix.m[2] - matrix.m[0] * matrix.m[5]) * inv_det;
  inv.m[6] = 0.0f;
  inv.m[7] = 0.0f;
  inv.m[8] = 1.0f;
  return inv;
}

[[nodiscard]] inline auto MatrixApproxEqual(const Matrix3x3& a, const Matrix3x3& b,
                                            float epsilon = kGeometryMatrixEpsilon) -> bool {
  for (int i = 0; i < 9; ++i) {
    if (std::fabs(a.m[i] - b.m[i]) > epsilon) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline auto IsApproxIdentity(const Matrix3x3& matrix,
                                           float            epsilon = kGeometryMatrixEpsilon) -> bool {
  return MatrixApproxEqual(matrix, Matrix3x3::Identity(), epsilon);
}

struct NormalizedRect {
  float x = 0.0f;
  float y = 0.0f;
  float w = 1.0f;
  float h = 1.0f;

  [[nodiscard]] auto IsFullFrame() const -> bool {
    return x == 0.0f && y == 0.0f && w == 1.0f && h == 1.0f;
  }
};

enum class TextureFilter : std::uint8_t {
  Bilinear = 0,
  Bicubic  = 1,
};

enum class RenderQuality : std::uint8_t {
  Preview = 0,
  Export  = 1,
};

[[nodiscard]] inline auto PixelCenter(std::uint32_t x, std::uint32_t y) -> Vector2 {
  return {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
}

[[nodiscard]] inline auto NormalizedFromPixelCenter(Vector2 center, Extent2D extent) -> Vector2 {
  return {center.x / static_cast<float>(extent.width), center.y / static_cast<float>(extent.height)};
}

}  // namespace alcedo
