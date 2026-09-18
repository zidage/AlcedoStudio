//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/opencl/opencl_renderer.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace opencl_workspace_test {

inline auto HasOpenClDevice() -> bool { return TryInitializeOpenClRuntime(); }

class OpenClWorkspaceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasOpenClDevice()) {
      GTEST_SKIP() << "No OpenCL device available.";
    }
  }
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

inline auto BindSharpen(ParameterArena<OpenClBackend>& arena, ParameterSlotKey key)
    -> ParameterBinding {
  const auto fields = SharpenFieldBindings();
  return arena.BindSlot(key, static_cast<std::uint32_t>(sizeof(SharpenPayload)), fields);
}

template <class Model>
void WritePackedOwnerBytes(ParameterArena<OpenClBackend>& arena, const ParameterSlotKey& key,
                           Model& model) {
  model.Read([&](const auto& payload) { arena.WritePackedSlot(key, payload); });
}

template <class Model>
auto UploadPackedAndClearDirty(OpenClRenderDevice& device, ParameterSlotKey key, Model& model)
    -> bool {
  auto& arena = device.Workspace().Parameters();
  WritePackedOwnerBytes(arena, key, model);
  arena.UploadDirty(device.CommandContext());
  auto pending = TakePendingDirtyFields(model);
  if (!pending.has_value()) {
    return false;
  }
  pending->Commit();
  return true;
}

}  // namespace opencl_workspace_test
}  // namespace alcedo
