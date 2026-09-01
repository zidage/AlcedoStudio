//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>

#include "edit/runtime/metal/metal_renderer.hpp"
#include "multi_mask_runtime_request_support.hpp"
#include "nm2_qualification_support.hpp"

namespace alcedo {
namespace {

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

class MetalMultiMaskRequestFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    gpu_dag_test::EnsureTestCameraProfile(*document_);
    renderer_ = std::make_unique<MetalRenderer>(document_, nm2_qualification::MakeUnpacker());
    image_    = nm2_qualification::MakeEncodedImage(83);
  }

  std::shared_ptr<PipelineDocument> document_;
  std::unique_ptr<MetalRenderer>    renderer_;
  std::shared_ptr<ImageBuffer>      image_;
};

}  // namespace

TEST_F(MetalMultiMaskRequestFixture, BackgroundRenderUsesSettledAssetsOnly) {
  multi_mask_qualification::BackgroundRenderUsesSettledAssetsOnly(*renderer_, *document_, image_);
}

}  // namespace alcedo
