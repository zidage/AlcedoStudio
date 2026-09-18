//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <memory>

#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "nm2_qualification_support.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaNm2QualificationFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    document_ = nm2_qualification::MakeThreeGradeDocument();
    renderer_ = std::make_unique<CudaProductRenderer>(document_, nm2_qualification::MakeUnpacker());
    image_    = nm2_qualification::MakeEncodedImage(71);
  }

  std::shared_ptr<PipelineDocument>    document_;
  std::unique_ptr<CudaProductRenderer> renderer_;
  std::shared_ptr<ImageBuffer>         image_;
};

}  // namespace

TEST_F(CudaNm2QualificationFixture, ExportRecipeDoesNotChangeNextEditorRender) {
  nm2_qualification::ExportRecipeDoesNotChangeNextEditorRender(*renderer_, *document_, image_);
}

TEST_F(CudaNm2QualificationFixture, MultiGradeDocumentRoundTripPreservesOwnersAndEdges) {
  nm2_qualification::MultiGradeDocumentRoundTripPreservesOwnersAndEdges(
      *renderer_, *document_, image_, [](std::shared_ptr<PipelineDocument> loaded) {
        return std::make_unique<CudaProductRenderer>(std::move(loaded),
                                                     nm2_qualification::MakeUnpacker());
      });
}

TEST_F(CudaNm2QualificationFixture, BackgroundMultiGradeRenderPreservesEditorCache) {
  nm2_qualification::BackgroundMultiGradeRenderPreservesEditorCache(*renderer_, image_);
}

TEST_F(CudaNm2QualificationFixture, MultiGradeResourceBytesAfterGpuCompletion) {
  document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document_);
  multi_grade_test::ResetGradeLookToIdentity(*document_->PrimaryGrade());
  renderer_ = std::make_unique<CudaProductRenderer>(document_, nm2_qualification::MakeUnpacker());
  nm2_qualification::MultiGradeResourceBytesAfterGpuCompletion(*renderer_, *document_, image_);
}

}  // namespace alcedo
