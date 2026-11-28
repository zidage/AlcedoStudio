//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The reference backend of the Vulkan pass tests (#1036, decision B9) and the comparison of two
// RGBA32F images (phases VK6 and VK7). The reference backend is OpenCL when the build has OpenCL
// (Windows), else Metal (macOS, where the build has no OpenCL). Both sides run the product pass
// functions.

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/curve_input_histogram.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"

#if defined(HAVE_OPENCL)
#include "../../vulkan/vulkan_opencl_comparison.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_develop_pass.hpp"
#include "edit/runtime/opencl/opencl_diffusion_filter_pass.hpp"
#include "edit/runtime/opencl/opencl_drt_pass.hpp"
#include "edit/runtime/opencl/opencl_primary_grade_pass.hpp"
#elif defined(HAVE_METAL)
#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/metal/metal_develop_pass.hpp"
#include "edit/runtime/metal/metal_diffusion_filter_pass.hpp"
#include "edit/runtime/metal/metal_drt_pass.hpp"
#include "edit/runtime/metal/metal_primary_grade_pass.hpp"
#else
#error "The Vulkan pass tests need OpenCL or Metal as their reference backend."
#endif

namespace alcedo::vulkan_reference {

#if defined(HAVE_OPENCL)
using ReferenceRenderDevice                    = OpenClRenderDevice;
inline constexpr const char* kReferenceBackend = "OpenCL";

inline void ExecuteReferenceDevelop(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                                    const PreparedRawInput& input,
                                    const PipelineDocument& document) {
  ExecuteOpenClDevelop(device, plan, input, document);
}

inline void ExecuteReferenceGeometryResample(ReferenceRenderDevice& device,
                                             const ExecutionPlan&   plan) {
  ExecuteOpenClGeometryResample(device, plan);
}

inline void ExecuteReferenceCameraColor(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                                        const PipelineDocument& document) {
  ExecuteOpenClCameraColor(device, plan, document);
}

inline void ExecuteReferenceDisplayToAp1(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                                         const PreparedRawInput& input,
                                         const PipelineDocument& document) {
  ExecuteOpenClDisplayToAp1(device, plan, input, document);
}

/// Every Color Grade of @p plan; the scene location of the last Grade output.
inline auto ExecuteReferencePrimaryGrade(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                                         const PreparedRawInput&    input,
                                         const PipelineDocument&    document,
                                         CurveInputHistogramRecord* histogram = nullptr)
    -> FrameSceneBinding {
  return ExecuteOpenClPrimaryGrade(device, plan, input, document, histogram).output_binding;
}

inline auto ExecuteReferenceDiffusionFilter(ReferenceRenderDevice&   device,
                                            const ExecutionPlan&     plan,
                                            const PipelineDocument&  document,
                                            const FrameSceneBinding& scene) -> FrameSceneBinding {
  return ExecuteOpenClDiffusionFilter(device, plan, document, scene);
}

/// The display transform and the DRT finish; the display output of @p plan.
inline auto ExecuteReferenceDrt(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                                const PipelineDocument& document, const FrameSceneBinding& scene)
    -> GraphValueId {
  return ExecuteOpenClDrt(device, plan, document, scene).output;
}

/// OpenCL on the GPU of the Vulkan context; an empty text when it is available.
inline auto PrepareReferenceBackend() -> std::string {
  return vulkan_opencl_comparison::PrepareOpenClOnTheVulkanDevice();
}
#else
using ReferenceRenderDevice                    = MetalRenderDevice;
inline constexpr const char* kReferenceBackend = "Metal";

inline void ExecuteReferenceDevelop(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                                    const PreparedRawInput& input,
                                    const PipelineDocument& document) {
  ExecuteMetalDevelop(device, plan, input, document);
}

inline void ExecuteReferenceGeometryResample(ReferenceRenderDevice& device,
                                             const ExecutionPlan&   plan) {
  ExecuteMetalGeometryResample(device, plan);
}

inline void ExecuteReferenceCameraColor(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                                        const PipelineDocument& document) {
  ExecuteMetalCameraColor(device, plan, document);
}

inline void ExecuteReferenceDisplayToAp1(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                                         const PreparedRawInput& input,
                                         const PipelineDocument& document) {
  ExecuteMetalDisplayToAp1(device, plan, input, document);
}

/// Every Color Grade of @p plan; the scene location of the last Grade output.
inline auto ExecuteReferencePrimaryGrade(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                                         const PreparedRawInput&    input,
                                         const PipelineDocument&    document,
                                         CurveInputHistogramRecord* histogram = nullptr)
    -> FrameSceneBinding {
  return ExecuteMetalPrimaryGrade(device, plan, input, document, histogram).output_binding;
}

inline auto ExecuteReferenceDiffusionFilter(ReferenceRenderDevice&   device,
                                            const ExecutionPlan&     plan,
                                            const PipelineDocument&  document,
                                            const FrameSceneBinding& scene) -> FrameSceneBinding {
  return ExecuteMetalDiffusionFilter(device, plan, document, scene);
}

/// The display transform and the DRT finish; the display output of @p plan.
inline auto ExecuteReferenceDrt(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                                const PipelineDocument& document, const FrameSceneBinding& scene)
    -> GraphValueId {
  return ExecuteMetalDrt(device, plan, document, scene).output;
}

/// The system default Metal device: the GPU of MoltenVK on a computer with one GPU.
inline auto PrepareReferenceBackend() -> std::string {
  try {
    if (BindSystemDefaultMetalPresentationDevice() == nullptr) {
      return "no Metal device";
    }
  } catch (const std::exception& error) {
    return std::string("no Metal device: ") + error.what();
  }
  return {};
}
#endif

/// The RGBA32F values of the image @p id of @p device after the render.
template <class Device>
auto DownloadImage(Device& device, const GraphValueId& id) -> std::vector<float> {
  auto* lease = device.Workspace().Images().Find(id);
  if (lease == nullptr || lease->Empty()) {
    ADD_FAILURE() << "the render has no image " << id.producer.Value();
    return {};
  }
  const auto&        texture = lease->Texture();
  std::vector<float> values(static_cast<std::size_t>(texture.Width()) * texture.Height() * 4);
  device.Workspace().Device().DownloadTexture2D(
      texture, std::as_writable_bytes(std::span<float>(values)), device.CommandContext());
  return values;
}

/// The RGBA32F values of the scene location @p binding of @p device after the render: a cached
/// image, or a scene-work member (a buffer on Vulkan and OpenCL, a texture on Metal).
template <class Device>
auto DownloadScene(Device& device, const FrameSceneBinding& binding) -> std::vector<float> {
  if (!binding.IsWorkImage()) {
    return DownloadImage(device, binding.graph_id);
  }
  auto&              member = device.Workspace().SceneWork().Member(binding.member);
  std::vector<float> values(static_cast<std::size_t>(member.Width()) * member.Height() * 4);
  auto               bytes = std::as_writable_bytes(std::span<float>(values));
  if constexpr (requires { member.Storage(); }) {
    device.Workspace().Device().DownloadBufferRange(member.Storage(), 0, bytes,
                                                    device.CommandContext());
  } else {
    device.Workspace().Device().DownloadTexture2D(member, bytes, device.CommandContext());
  }
  return values;
}

/// The distance bands of a texel to the nearest image edge, for the report of a difference.
inline constexpr std::array<std::uint32_t, 3> kBorderBands = {4, 16, 64};

struct Difference {
  double                     largest     = 0.0;
  std::size_t                largest_at  = 0;
  std::size_t                above       = 0;
  std::size_t                differing   = 0;
  double                     sum_squares = 0.0;
  /// Values above the tolerance with an edge distance below 4, 16, 64, and from 64.
  std::array<std::size_t, 4> above_by_band{};
};

/// The largest difference of a value: @p border for a texel whose edge distance is below
/// @p border_band, @p interior for the others.
/// @p rms above 0 also bounds the root mean square of the differences of all values.
struct Tolerance {
  double        interior    = 0.0;
  double        border      = 0.0;
  std::uint32_t border_band = 0;
  double        rms         = 0.0;
};

constexpr auto Uniform(double tolerance) -> Tolerance { return {tolerance, tolerance, 0, 0.0}; }

/// |vulkan - reference| / max(1, |reference|) for each value; NaN on one side only is above.
/// @p width of 0: no border bands, the interior tolerance for each value.
inline auto    Compare(std::span<const float> reference, std::span<const float> vulkan,
                       const Tolerance& tolerance, std::uint32_t width = 0) -> Difference {
  Difference        result;
  const std::size_t height = width == 0 ? 0 : reference.size() / 4 / width;
  for (std::size_t i = 0; i < reference.size(); ++i) {
    const double a = reference[i];
    const double b = vulkan[i];
    double       d = 0.0;
    if (std::isnan(a) || std::isnan(b)) {
      d = (std::isnan(a) && std::isnan(b)) ? 0.0 : INFINITY;
    } else if (a != b) {
      d = std::abs(b - a) / std::max(1.0, std::abs(a));
    }
    if (d > 0.0) {
      ++result.differing;
      result.sum_squares += d * d;
    }
    if (d > result.largest) {
      result.largest    = d;
      result.largest_at = i;
    }
    if (d == 0.0) {
      continue;
    }
    std::size_t edge = SIZE_MAX;
    if (width != 0) {
      const std::size_t x = (i / 4) % width;
      const std::size_t y = (i / 4) / width;
      edge                = std::min({x, y, width - 1 - x, height - 1 - y});
    }
    if (d > (edge < tolerance.border_band ? tolerance.border : tolerance.interior)) {
      ++result.above;
      std::size_t band = 0;
      while (band < kBorderBands.size() && edge >= kBorderBands[band]) {
        ++band;
      }
      ++result.above_by_band[band];
    }
  }
  return result;
}

/// Prints the difference of @p vulkan from @p reference and expects it in @p tolerance.
/// @p reference_name names the reference in the report (the reference backend by default).
inline void ExpectEqualToReference(const std::string& label, std::span<const float> reference,
                                   std::span<const float> vulkan, std::uint32_t width,
                                   const Tolerance& tolerance,
                                   const char*      reference_name = kReferenceBackend) {
  ASSERT_FALSE(reference.empty()) << label;
  ASSERT_EQ(reference.size(), vulkan.size()) << label;
  const auto        result = Compare(reference, vulkan, tolerance, width);
  const std::size_t texel  = result.largest_at / 4;
  const auto        rms    = std::sqrt(result.sum_squares / static_cast<double>(reference.size()));
  std::printf(
      "%s: %s against Vulkan, %zu values, %zu differ, largest %.3g at (%zu, %zu) channel %zu "
      "(%s %.9g, Vulkan %.9g), rms %.3g, %zu above %.3g (border %.3g below %u) (edge distance "
      "<4: %zu, <16: %zu, <64: %zu, >=64: %zu)\n",
      label.c_str(), reference_name, reference.size(), result.differing, result.largest,
      texel % width, texel / width, result.largest_at % 4, reference_name,
      static_cast<double>(reference[result.largest_at]),
      static_cast<double>(vulkan[result.largest_at]), rms, result.above, tolerance.interior,
      tolerance.border, tolerance.border_band, result.above_by_band[0], result.above_by_band[1],
      result.above_by_band[2], result.above_by_band[3]);
  EXPECT_EQ(result.above, 0U) << label << ": largest difference " << result.largest;
  if (tolerance.rms > 0.0) {
    EXPECT_LE(rms, tolerance.rms) << label;
  }
}

}  // namespace alcedo::vulkan_reference
