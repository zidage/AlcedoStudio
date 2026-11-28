//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The inputs of the Vulkan Mask tests (#1036, phase VK11): the documents and the Masks of the
// cases, the upload of a Grade input, and the R8 download, for vulkan_mask_test.cpp (against the
// reference backend) and vulkan_mask_harness_test.cpp (the shared checks and the Vulkan rules).

#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/texture_format.hpp"
#include "mask_range_reference.hpp"

namespace alcedo::vulkan_mask_test {

using mask_range_reference::ScenePixel;

/// The Grade input of a ranged Mask that a case uploads as a cached image.
inline const GraphValueId kRangeSceneId{NodeId{"test.range_scene"}, PortId{"image"}};

template <class Device>
using BackendOf = std::remove_reference_t<decltype(std::declval<Device&>().Workspace().Device())>;

inline auto MakeInput(std::uint32_t width, std::uint32_t height) -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(width, height),
                                       gpu_dag_test::FullSensor(width, height));
}

inline auto MakeDocument() -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  return document;
}

inline auto Compile(const PipelineDocument& document, const PreparedRawInput& input,
                    const RenderRequest& request = RenderRequest{}) -> ExecutionPlan {
  return GraphCompiler::Compile(document, input.CompileSource(), request);
}

inline void AddMask(PipelineDocument& document, MaskModel mask) {
  (void)grade_mask_test::AddMask(*document.PrimaryGrade(), std::move(mask));
  document.MarkTopologyChanged();
}

/// A soft Radial, rotated, with feathers, inverted, at opacity 0.6.
inline auto MakeRotatedRadial(std::string id) -> MaskModel {
  RadialMaskSource radial;
  radial.center_x      = 0.42F;
  radial.center_y      = 0.57F;
  radial.major_radius  = 0.38F;
  radial.minor_radius  = 0.21F;
  radial.rotation      = 0.7F;
  radial.inner_feather = 0.55F;
  radial.outer_feather = 0.35F;
  auto mask            = grade_mask_test::MakeRadialMask(MaskId{std::move(id)}, radial, true);
  mask.opacity         = 0.6F;
  return mask;
}

/// A diagonal Linear Gradient from 0.1 to 0.9.
inline auto MakeDiagonalGradient(std::string id) -> MaskModel {
  LinearGradientMaskSource gradient;
  gradient.origin_x            = 0.55F;
  gradient.origin_y            = 0.45F;
  gradient.normal_x            = 0.6F;
  gradient.normal_y            = -0.8F;
  gradient.transition_distance = 0.3F;
  gradient.start_value         = 0.1F;
  gradient.end_value           = 0.9F;
  return grade_mask_test::MakeLinearGradientMask(MaskId{std::move(id)}, gradient);
}

/// Where a case puts the Grade input of its ranged Masks.
enum class SceneStorage { kCachedImage, kSceneWork };

template <class Device>
inline auto DownloadR8(Device& device, const GraphValueId& id) -> std::vector<std::uint8_t> {
  auto* lease = device.Workspace().Images().Find(id);
  if (lease == nullptr || lease->Empty()) {
    ADD_FAILURE() << "the render has no R8 image " << id.producer.Value();
    return {};
  }
  std::vector<std::uint8_t> pixels(lease->Texture().Bytes());
  device.Workspace().Device().DownloadTexture2D(
      lease->Texture(), std::as_writable_bytes(std::span<std::uint8_t>(pixels)),
      device.CommandContext());
  return pixels;
}

/// Uploads @p scene as the Grade input of the ranged Masks; the binding of that input.
template <class Device>
inline auto UploadScene(Device& device, std::span<const ScenePixel> scene, SceneStorage storage,
                        Extent2D extent) -> FrameSceneBinding {
  auto& workspace = device.Workspace();
  if (storage == SceneStorage::kSceneWork) {
    workspace.EnsureSceneWorkImages({extent.width, extent.height});
    const auto binding = FrameSceneBinding::WorkImage(SceneWorkMember::Member0);
    auto&      member  = workspace.SceneWork().Member(binding.member);
    if constexpr (requires { member.Storage(); }) {
      workspace.Device().UploadBufferRange(member.Storage(), 0, std::as_bytes(scene),
                                           device.CommandContext());
    } else {
      workspace.Device().UploadTexture2D(member, std::as_bytes(scene), device.CommandContext());
    }
    return binding;
  }
  auto& lease = workspace.AcquireImageForWrite(
      kRangeSceneId, {extent.width, extent.height, TextureFormat::Rgba32f});
  workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(scene),
                                     device.CommandContext());
  return FrameSceneBinding::CachedImage(kRangeSceneId);
}

}  // namespace alcedo::vulkan_mask_test
