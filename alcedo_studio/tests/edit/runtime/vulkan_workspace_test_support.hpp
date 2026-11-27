//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Support of the Vulkan workspace tests: a fixture that creates the Vulkan context of the run with
// the validation layer, and parameter helpers for the ParameterArena of VulkanBackend.

#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../vulkan/vulkan_test_device.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo::vulkan_workspace_test {

/**
 * @brief Creates the Vulkan context of the run before each test; the base class destroys it and
 * checks that the validation layer reported no error.
 *
 * Each test destroys its render devices before `TearDown` (they are locals of the test body).
 */
class VulkanWorkspaceFixture : public vulkan::test_support::VulkanContextTestBase {
 protected:
  void SetUp() override {
    VulkanContextTestBase::SetUp();
    vulkan::VulkanContext::Create(vulkan::test_support::OptionsOfThisRun());
  }
};

inline auto ExposureFieldBindings() -> std::vector<ParameterFieldBinding> {
  return {ParameterFieldBinding{DirtyFieldMask{ExposureTraits::Dirty::Value}, 0, 0, 4}};
}

inline auto BindSharpen(ParameterArena<VulkanBackend>& arena, ParameterSlotKey key)
    -> ParameterBinding {
  const std::vector<ParameterFieldBinding> fields = {
      ParameterFieldBinding{DirtyFieldMask{SharpenDirty::Amount},
                            static_cast<std::uint32_t>(offsetof(SharpenPayload, amount)), 0, 4},
      ParameterFieldBinding{DirtyFieldMask{SharpenDirty::Radius},
                            static_cast<std::uint32_t>(offsetof(SharpenPayload, radius)), 4, 4},
      ParameterFieldBinding{DirtyFieldMask{SharpenDirty::Threshold},
                            static_cast<std::uint32_t>(offsetof(SharpenPayload, threshold)), 8, 4},
  };
  return arena.BindSlot(key, static_cast<std::uint32_t>(sizeof(SharpenPayload)), fields);
}

template <class Model>
void WritePackedOwnerBytes(ParameterArena<VulkanBackend>& arena, const ParameterSlotKey& key,
                           Model& model) {
  // Read holds the Model lock, so take the revision first.
  const auto revision = model.Revision();
  model.Read([&](const auto& payload) { arena.WritePackedSlot(key, payload, revision); });
}

/// Packs @p model into @p key and records its upload. True when the slot has the Model revision.
template <class Model>
auto UploadPackedModel(VulkanRenderDevice& device, ParameterSlotKey key, Model& model) -> bool {
  auto& arena = device.Workspace().Parameters();
  WritePackedOwnerBytes(arena, key, model);
  arena.UploadDirty(device.CommandContext());
  return arena.AppliedRevision(key) == model.Revision() && !arena.HasPendingUpload();
}

/// Packed RGBA32F pixels with a different value in each channel of each texel.
inline auto RampRgba32f(std::uint32_t width, std::uint32_t height, float scale)
    -> std::vector<float> {
  std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<float>(i) * scale - 3.25F;
  }
  return pixels;
}

}  // namespace alcedo::vulkan_workspace_test
