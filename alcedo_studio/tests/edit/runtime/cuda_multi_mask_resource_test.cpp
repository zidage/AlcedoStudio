//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "multi_mask_runtime_resource_support.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaMultiMaskResourceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }

  CudaRenderDevice device_;
};

}  // namespace

TEST_F(CudaMultiMaskResourceFixture, EnabledMaskCountsScaleWithMaskList) {
  multi_mask_qualification::ExpectEnabledMaskCountsScaleWithMaskList(device_);
}

TEST_F(CudaMultiMaskResourceFixture, DisabledMasksSkipSourceEvaluation) {
  multi_mask_qualification::ExpectDisabledMasksSkipSourceEvaluation(device_);
}

TEST_F(CudaMultiMaskResourceFixture, PartialBrushUploadTransfersOnlyDirtyRectangle) {
  multi_mask_qualification::ExpectPartialBrushUploadTransfersOnlyDirtyRectangle(device_);
}

TEST_F(CudaMultiMaskResourceFixture, FeatheredBrushDirtyUpdateRecomputesFullSignedDistance) {
  multi_mask_qualification::ExpectFeatheredBrushDirtyUpdateRecomputesFullSignedDistance(device_);
}

TEST_F(CudaMultiMaskResourceFixture, MaskUploadFailureKeepsPriorPublishedResults) {
  multi_mask_qualification::ExpectMaskUploadFailureKeepsPriorPublishedResults(device_);
}

}  // namespace alcedo
