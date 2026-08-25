//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/metal/metal_renderer.hpp"
#include "edit/runtime/parameter_binding.hpp"

namespace alcedo {
namespace metal_workspace_test {

inline auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

class MetalWorkspaceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    presentation_device_ = BindSystemDefaultMetalPresentationDevice();
  }

  void* presentation_device_ = nullptr;
};

inline auto SharpenFieldBindings() -> std::vector<ParameterFieldBinding> {
  return {
      ParameterFieldBinding{DirtyFieldMask{SharpenDirty::Amount},
                            static_cast<std::uint32_t>(offsetof(SharpenPayload, amount)), 0, 4},
      ParameterFieldBinding{DirtyFieldMask{SharpenDirty::Radius},
                            static_cast<std::uint32_t>(offsetof(SharpenPayload, radius)), 4, 4},
      ParameterFieldBinding{DirtyFieldMask{SharpenDirty::Threshold},
                            static_cast<std::uint32_t>(offsetof(SharpenPayload, threshold)), 8, 4},
  };
}

inline auto ExposureFieldBindings() -> std::vector<ParameterFieldBinding> {
  return {ParameterFieldBinding{DirtyFieldMask{ExposureTraits::Dirty::Value}, 0, 0, 4}};
}

inline auto BindSharpen(ParameterArena<MetalBackend>& arena, ParameterSlotKey key)
    -> ParameterBinding {
  const auto fields = SharpenFieldBindings();
  return arena.BindSlot(key, static_cast<std::uint32_t>(sizeof(SharpenPayload)), fields);
}

inline auto UploadFullAndClearDirty(MetalRenderDevice& device, ParameterSlotKey key,
                                    IOperatorModel& model) -> bool {
  auto& arena = device.Workspace().Parameters();
  arena.InitializeFromFullDto(key, model.MakeFullDto());
  arena.UploadDirty(device.CommandContext());
  auto pending = TakePendingParameterPatch(model);
  if (!pending.has_value()) {
    return false;
  }
  pending->Commit();
  return true;
}

}  // namespace metal_workspace_test
}  // namespace alcedo
