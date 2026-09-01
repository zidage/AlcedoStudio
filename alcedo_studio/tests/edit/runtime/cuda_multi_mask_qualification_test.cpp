//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <memory>

#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "multi_mask_runtime_request_support.hpp"
#include "nm2_qualification_support.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaMultiMaskRequestFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    gpu_dag_test::EnsureTestCameraProfile(*document_);
    renderer_ = std::make_unique<CudaProductRenderer>(document_, nm2_qualification::MakeUnpacker());
    image_    = nm2_qualification::MakeEncodedImage(73);
  }

  std::shared_ptr<PipelineDocument>    document_;
  std::unique_ptr<CudaProductRenderer> renderer_;
  std::shared_ptr<ImageBuffer>         image_;
};

}  // namespace

TEST_F(CudaMultiMaskRequestFixture, BackgroundRenderUsesSettledAssetsOnly) {
  multi_mask_qualification::BackgroundRenderUsesSettledAssetsOnly(*renderer_, *document_, image_);
}

}  // namespace alcedo
