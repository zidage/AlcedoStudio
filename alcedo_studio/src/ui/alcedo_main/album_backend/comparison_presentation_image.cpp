//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/comparison_presentation_image.hpp"

#include <cmath>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>

#include "edit/operators/utils/color_utils.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"

namespace alcedo::ui {
namespace {

auto ExtentText(Extent2D extent) -> std::string {
  return std::to_string(extent.width) + "x" + std::to_string(extent.height);
}

}  // namespace

auto ComparisonImagePlacement::ToVariantMap() const -> QVariantMap {
  const auto& m = render_to_reference.m;
  return QVariantMap{
      {QStringLiteral("referenceWidth"), static_cast<double>(reference_extent.width)},
      {QStringLiteral("referenceHeight"), static_cast<double>(reference_extent.height)},
      {QStringLiteral("renderWidth"), static_cast<double>(render_extent.width)},
      {QStringLiteral("renderHeight"), static_cast<double>(render_extent.height)},
      {QStringLiteral("m11"), static_cast<double>(m[0])},
      {QStringLiteral("m12"), static_cast<double>(m[1])},
      {QStringLiteral("dx"), static_cast<double>(m[2])},
      {QStringLiteral("m21"), static_cast<double>(m[3])},
      {QStringLiteral("m22"), static_cast<double>(m[4])},
      {QStringLiteral("dy"), static_cast<double>(m[5])},
  };
}

auto ComparisonPlacementFromGeometry(const ResolvedRenderGeometry& geometry)
    -> ComparisonImagePlacement {
  if (geometry.full_reference_extent.Empty()) {
    throw std::invalid_argument("Comparison image has an empty reference extent.");
  }
  if (geometry.render_extent.Empty()) {
    throw std::invalid_argument("Comparison image has an empty render extent.");
  }
  const auto& m = geometry.render_to_reference.m;
  for (const float value : m) {
    if (!std::isfinite(value)) {
      throw std::invalid_argument("Comparison image placement is not finite.");
    }
  }
  if (m[6] != 0.0f || m[7] != 0.0f || m[8] != 1.0f) {
    throw std::invalid_argument("Comparison image placement is not an affine map.");
  }
  const float determinant = m[0] * m[4] - m[1] * m[3];
  if (std::fabs(determinant) < 1e-12f) {
    throw std::invalid_argument("Comparison image placement is singular.");
  }
  return ComparisonImagePlacement{geometry.full_reference_extent, geometry.render_extent,
                                  geometry.render_to_reference};
}

auto ConvertRenderedImageForComparison(const RenderedPipelineImage& rendered)
    -> ComparisonPresentationImage {
  if (!rendered.pixels || !rendered.pixels->cpu_data_valid_) {
    throw std::invalid_argument("Comparison image has no host pixels.");
  }
  const cv::Mat& pixels = rendered.pixels->GetCPUData();
  if (pixels.empty() || pixels.type() != CV_32FC4) {
    throw std::invalid_argument("Comparison image pixels are not RGBA32F.");
  }
  const Extent2D pixel_extent{static_cast<std::uint32_t>(pixels.cols),
                              static_cast<std::uint32_t>(pixels.rows)};
  if (pixel_extent != rendered.geometry.render_extent) {
    throw std::invalid_argument("Comparison image is " + ExtentText(pixel_extent) +
                                " pixels, but its render geometry is " +
                                ExtentText(rendered.geometry.render_extent) + ".");
  }
  auto       placement = ComparisonPlacementFromGeometry(rendered.geometry);

  const auto eotf      = rendered.display.encoding_eotf;
  if (eotf == ColorUtils::EOTF::ST2084 || eotf == ColorUtils::EOTF::HLG) {
    throw std::invalid_argument("HDR comparison is unavailable: the image uses " +
                                std::string(eotf == ColorUtils::EOTF::ST2084 ? "ST 2084" : "HLG") +
                                " output.");
  }
  if (!cv::checkRange(pixels, /*quiet=*/true)) {
    throw std::invalid_argument("Comparison image contains a non-finite pixel value.");
  }

  // The user-approved SDR boundary: OpenCV saturate_cast rounds to the nearest 8-bit value and
  // clamps to [0, 255]. The result is a deep copy.
  QImage image = album_util::MatRgba32fToQImageCopy(pixels);
  if (image.isNull() || image.format() != QImage::Format_RGBA8888) {
    throw std::invalid_argument("Comparison image conversion to RGBA8 failed.");
  }
  return ComparisonPresentationImage{std::move(image), placement};
}

}  // namespace alcedo::ui
