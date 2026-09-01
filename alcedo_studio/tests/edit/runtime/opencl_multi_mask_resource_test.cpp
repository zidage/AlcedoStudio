//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>

#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_pass_encoder.hpp"
#include "multi_mask_runtime_resource_support.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

auto HasOpenClImageDevice() -> bool {
  if (!TryInitializeOpenClRuntime()) {
    return false;
  }
  return OpenClContext::Instance().Capabilities().image_support;
}

class OpenClMultiMaskResourceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasOpenClImageDevice()) {
      GTEST_SKIP() << "No OpenCL image device available.";
    }
    device_ = std::make_unique<OpenClRenderDevice>();
  }

  std::unique_ptr<OpenClRenderDevice> device_;
};

}  // namespace

TEST_F(OpenClMultiMaskResourceFixture, EnabledMaskCountsScaleWithMaskList) {
  multi_mask_qualification::ExpectEnabledMaskCountsScaleWithMaskList(*device_);
}

TEST_F(OpenClMultiMaskResourceFixture, DisabledMasksSkipSourceEvaluation) {
  multi_mask_qualification::ExpectDisabledMasksSkipSourceEvaluation(*device_);
}

TEST_F(OpenClMultiMaskResourceFixture, PartialBrushUploadTransfersOnlyDirtyRectangle) {
  multi_mask_qualification::ExpectPartialBrushUploadTransfersOnlyDirtyRectangle(*device_);
}

TEST_F(OpenClMultiMaskResourceFixture, FeatheredBrushDirtyUpdateRecomputesFullSignedDistance) {
  multi_mask_qualification::ExpectFeatheredBrushDirtyUpdateRecomputesFullSignedDistance(*device_);
}

TEST_F(OpenClMultiMaskResourceFixture, MaskUploadFailureKeepsPriorPublishedResults) {
  multi_mask_qualification::ExpectMaskUploadFailureKeepsPriorPublishedResults(*device_);
}

}  // namespace alcedo
