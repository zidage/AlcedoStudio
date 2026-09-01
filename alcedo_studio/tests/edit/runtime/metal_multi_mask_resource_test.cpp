//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#include "multi_mask_runtime_resource_support.hpp"

namespace alcedo {
namespace {

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

class MetalMultiMaskResourceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    (void)BindSystemDefaultMetalPresentationDevice();
  }

  MetalRenderDevice device_;
};

}  // namespace

TEST_F(MetalMultiMaskResourceFixture, EnabledMaskCountsScaleWithMaskList) {
  multi_mask_qualification::ExpectEnabledMaskCountsScaleWithMaskList(device_);
}

TEST_F(MetalMultiMaskResourceFixture, DisabledMasksSkipSourceEvaluation) {
  multi_mask_qualification::ExpectDisabledMasksSkipSourceEvaluation(device_);
}

TEST_F(MetalMultiMaskResourceFixture, PartialBrushUploadTransfersOnlyDirtyRectangle) {
  multi_mask_qualification::ExpectPartialBrushUploadTransfersOnlyDirtyRectangle(device_);
}

TEST_F(MetalMultiMaskResourceFixture, FeatheredBrushDirtyUpdateRecomputesFullSignedDistance) {
  multi_mask_qualification::ExpectFeatheredBrushDirtyUpdateRecomputesFullSignedDistance(device_);
}

TEST_F(MetalMultiMaskResourceFixture, MaskUploadFailureKeepsPriorPublishedResults) {
  multi_mask_qualification::ExpectMaskUploadFailureKeepsPriorPublishedResults(device_);
}

}  // namespace alcedo
