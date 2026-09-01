//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>

#include "edit/runtime/opencl/opencl_renderer.hpp"
#include "multi_mask_runtime_request_support.hpp"
#include "nm2_qualification_support.hpp"
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

class OpenClMultiMaskRequestFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasOpenClImageDevice()) {
      GTEST_SKIP() << "No OpenCL image device available.";
    }
    document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    gpu_dag_test::EnsureTestCameraProfile(*document_);
    renderer_ = std::make_unique<OpenClRenderer>(document_, nm2_qualification::MakeUnpacker());
    image_    = nm2_qualification::MakeEncodedImage(93);
  }

  std::shared_ptr<PipelineDocument> document_;
  std::unique_ptr<OpenClRenderer>   renderer_;
  std::shared_ptr<ImageBuffer>      image_;
};

}  // namespace

TEST_F(OpenClMultiMaskRequestFixture, BackgroundRenderUsesSettledAssetsOnly) {
  multi_mask_qualification::BackgroundRenderUsesSettledAssetsOnly(*renderer_, *document_, image_);
}

}  // namespace alcedo
