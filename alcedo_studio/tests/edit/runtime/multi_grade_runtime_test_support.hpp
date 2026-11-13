//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../graph/test_camera_profile.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "oklab_contrast_reference.hpp"

namespace alcedo::multi_grade_test {

using AcesccRgb = oklab_contrast_reference::Rgb;

template <class Pixel>
auto Rgb(const Pixel& pixel) -> AcesccRgb {
  return {pixel.r, pixel.g, pixel.b};
}

inline auto ApplyExposureAcescc(float value, float exposure_ev) -> float {
  return value + exposure_ev / 17.52f;
}

inline auto ApplyExposureAcescc(const AcesccRgb& value, float exposure_ev) -> AcesccRgb {
  return {ApplyExposureAcescc(value[0], exposure_ev), ApplyExposureAcescc(value[1], exposure_ev),
          ApplyExposureAcescc(value[2], exposure_ev)};
}

/// Contrast mixes channels (OkLab lightness), so it takes and returns the full ACEScc RGB.
inline auto ApplyContrastAcescc(const AcesccRgb& value, float contrast) -> AcesccRgb {
  return oklab_contrast_reference::ApplyContrastAcescc(value, contrast);
}

inline auto MixToward(float input, float adjusted, float mix, float coverage = 1.0f) -> float {
  const float weight = std::clamp(coverage * mix, 0.0f, 1.0f);
  return input + weight * (adjusted - input);
}

inline auto MixToward(const AcesccRgb& input, const AcesccRgb& adjusted, float mix,
                      float coverage = 1.0f) -> AcesccRgb {
  return {MixToward(input[0], adjusted[0], mix, coverage),
          MixToward(input[1], adjusted[1], mix, coverage),
          MixToward(input[2], adjusted[2], mix, coverage)};
}

/// Every RGB channel of @p pixel is within @p tolerance of @p expected.
template <class Pixel>
auto RgbNear(const Pixel& pixel, const AcesccRgb& expected, float tolerance)
    -> ::testing::AssertionResult {
  const AcesccRgb actual = Rgb(pixel);
  for (std::size_t i = 0; i < actual.size(); ++i) {
    if (!(std::fabs(actual[i] - expected[i]) <= tolerance)) {
      return ::testing::AssertionFailure()
             << "channel " << i << ": actual " << actual[i] << ", expected " << expected[i]
             << ", tolerance " << tolerance;
    }
  }
  return ::testing::AssertionSuccess();
}

inline void ResetGradeLookToIdentity(ColorGradeNodeModel& grade) {
  auto* exposure =
      dynamic_cast<ExposureModel*>(grade.FindAdjustmentByType(type_ids::Exposure()));
  auto* saturation =
      dynamic_cast<SaturationModel*>(grade.FindAdjustmentByType(type_ids::Saturation()));
  auto* contrast = dynamic_cast<ContrastModel*>(grade.FindAdjustmentByType(type_ids::Contrast()));
  if (exposure != nullptr) {
    exposure->SetValue(0.0f);
  }
  if (saturation != nullptr) {
    saturation->SetValue(1.0f);
  }
  if (contrast != nullptr) {
    contrast->SetValue(0.0f);
  }
}

inline auto GradeNode(PipelineDocument& document, std::string_view id) -> ColorGradeNodeModel* {
  return dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{std::string{id}}));
}

template <class Model>
auto GradeAdjustment(PipelineDocument& document, const NodeId& node_id, const OperatorTypeId& type)
    -> Model& {
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(node_id));
  auto* model = grade == nullptr ? nullptr : dynamic_cast<Model*>(grade->FindAdjustmentByType(type));
  if (model == nullptr) {
    throw std::runtime_error("multi_grade_test: missing adjustment on " +
                             std::string{node_id.Value()});
  }
  return *model;
}

inline auto MakeIdentityGradeDocument() -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  ResetGradeLookToIdentity(*document.PrimaryGrade());
  return document;
}

inline void AddCleanGradesBeforeDrt(PipelineDocument& document,
                                    std::initializer_list<const char*> ids) {
  for (const char* id : ids) {
    const auto errors = AddCleanColorGrade(document, NodeId{"drt"}, NodeId{id});
    if (!errors.empty()) {
      throw std::runtime_error("AddCleanColorGrade failed for " + std::string{id});
    }
  }
}

inline auto MakeNeighborhoodRgbaPlane(std::uint32_t width, std::uint32_t height, float surroundings,
                                      float center) -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = {width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  auto  storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                 [](std::byte* p) { delete[] p; });
  auto* pixels       = reinterpret_cast<float*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float value = x == width / 2 && y == height / 2 ? center : surroundings;
      const auto  index = (static_cast<std::size_t>(y) * width + x) * 4;
      pixels[index + 0] = value;
      pixels[index + 1] = value;
      pixels[index + 2] = value;
      pixels[index + 3] = 1.0f;
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

/**
 * @brief Colored plane with a bright warm center on dark surroundings.
 *
 * Distinct channel values make chroma operators (Saturation, Vibrance, HLS)
 * visible, and the center detail gives Local Laplacian real edge contrast.
 */
inline auto MakeColoredNeighborhoodRgbaPlane(std::uint32_t width, std::uint32_t height)
    -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = {width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  auto  storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                 [](std::byte* p) { delete[] p; });
  auto* pixels       = reinterpret_cast<float*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const bool center = x == width / 2 && y == height / 2;
      const auto index  = (static_cast<std::size_t>(y) * width + x) * 4;
      pixels[index + 0] = center ? 0.55f : 0.06f;
      pixels[index + 1] = center ? 0.30f : 0.035f;
      pixels[index + 2] = center ? 0.08f : 0.02f;
      pixels[index + 3] = 1.0f;
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

inline void WriteConstantRgbCube(const std::filesystem::path& path, float r, float g, float b) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "LUT_3D_SIZE 2\n";
  for (int i = 0; i < 8; ++i) {
    out << r << ' ' << g << ' ' << b << '\n';
  }
}

}  // namespace alcedo::multi_grade_test
