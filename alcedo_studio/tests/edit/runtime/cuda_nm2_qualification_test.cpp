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
    document_       = nm2_qualification::MakeThreeGradeDocument();
    source_         = std::make_unique<test::RenderSnapshotSource>(document_);
    renderer_       = MakeRenderer(ExecutorRole::Interactive);
    batch_renderer_ = MakeRenderer(ExecutorRole::Batch);
    image_          = nm2_qualification::MakeEncodedImage(71);
  }

  static auto MakeRenderer(ExecutorRole role) -> std::unique_ptr<CudaProductRenderer> {
    return std::make_unique<CudaProductRenderer>(role, nm2_qualification::MakeUnpacker());
  }

  std::shared_ptr<PipelineDocument>           document_;
  std::unique_ptr<test::RenderSnapshotSource> source_;
  std::unique_ptr<CudaProductRenderer>        renderer_;
  std::unique_ptr<CudaProductRenderer>        batch_renderer_;
  std::shared_ptr<ImageBuffer>                image_;
};

}  // namespace

TEST_F(CudaNm2QualificationFixture, ExportRecipeDoesNotChangeNextEditorRender) {
  nm2_qualification::ExportRecipeDoesNotChangeNextEditorRender(*renderer_, *batch_renderer_,
                                                               *source_, image_);
}

TEST_F(CudaNm2QualificationFixture, MultiGradeDocumentRoundTripPreservesOwnersAndEdges) {
  nm2_qualification::MultiGradeDocumentRoundTripPreservesOwnersAndEdges(*renderer_, *source_,
                                                                        image_);
}

TEST_F(CudaNm2QualificationFixture, BackgroundMultiGradeRenderPreservesEditorCache) {
  nm2_qualification::BackgroundMultiGradeRenderPreservesEditorCache(*renderer_, *batch_renderer_,
                                                                    *source_, image_);
}

TEST_F(CudaNm2QualificationFixture, MultiGradeResourceBytesAfterGpuCompletion) {
  document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document_);
  multi_grade_test::ResetGradeLookToIdentity(*document_->PrimaryGrade());
  source_   = std::make_unique<test::RenderSnapshotSource>(document_);
  renderer_ = MakeRenderer(ExecutorRole::Interactive);
  nm2_qualification::MultiGradeResourceBytesAfterGpuCompletion(*renderer_, *batch_renderer_,
                                                               *source_, image_);
}

}  // namespace alcedo
