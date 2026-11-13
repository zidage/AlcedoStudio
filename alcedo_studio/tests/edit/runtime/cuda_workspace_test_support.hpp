//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/parameter_binding.hpp"

namespace alcedo {
namespace cuda_workspace_test {

inline auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaWorkspaceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
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

inline auto BindSharpen(ParameterArena<CudaBackend>& arena, ParameterSlotKey key)
    -> ParameterBinding {
  const auto fields = SharpenFieldBindings();
  return arena.BindSlot(key, static_cast<std::uint32_t>(sizeof(SharpenPayload)), fields);
}

template <class Model>
void WritePackedOwnerBytes(ParameterArena<CudaBackend>& arena, const ParameterSlotKey& key,
                           Model& model) {
  // Read holds the Model lock, so take the revision first.
  const auto revision = model.Revision();
  model.Read([&](const auto& payload) { arena.WritePackedSlot(key, payload, revision); });
}

/**
 * @brief Pack @p model into @p key and upload it.
 * @return true when the slot records the Model's current revision after the upload.
 */
template <class Model>
auto UploadPackedModel(CudaRenderDevice& device, ParameterSlotKey key, Model& model) -> bool {
  auto& arena = device.Workspace().Parameters();
  WritePackedOwnerBytes(arena, key, model);
  arena.UploadDirty(device.CommandContext());
  return arena.AppliedRevision(key) == model.Revision() && !arena.HasPendingUpload();
}

}  // namespace cuda_workspace_test
}  // namespace alcedo
