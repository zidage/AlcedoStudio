//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>

#include "edit/runtime/opencl/opencl_renderer.hpp"
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

class OpenClNm2QualificationFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasOpenClImageDevice()) {
      GTEST_SKIP() << "No OpenCL image device available.";
    }
    document_ = nm2_qualification::MakeThreeGradeDocument();
    renderer_ = std::make_unique<OpenClRenderer>(document_, nm2_qualification::MakeUnpacker());
    image_    = nm2_qualification::MakeEncodedImage(91);
  }

  std::shared_ptr<PipelineDocument> document_;
  std::unique_ptr<OpenClRenderer>   renderer_;
  std::shared_ptr<ImageBuffer>      image_;
};

}  // namespace

TEST_F(OpenClNm2QualificationFixture, ExportRecipeDoesNotChangeNextEditorRender) {
  nm2_qualification::ExportRecipeDoesNotChangeNextEditorRender(*renderer_, *document_, image_);
}

TEST_F(OpenClNm2QualificationFixture, MultiGradeDocumentRoundTripPreservesOwnersAndEdges) {
  nm2_qualification::MultiGradeDocumentRoundTripPreservesOwnersAndEdges(
      *renderer_, *document_, image_, [](std::shared_ptr<PipelineDocument> loaded) {
        return std::make_unique<OpenClRenderer>(std::move(loaded),
                                                nm2_qualification::MakeUnpacker());
      });
}

TEST_F(OpenClNm2QualificationFixture, BackgroundMultiGradeRenderPreservesEditorCache) {
  nm2_qualification::BackgroundMultiGradeRenderPreservesEditorCache(*renderer_, image_);
}

TEST_F(OpenClNm2QualificationFixture, MultiGradeResourceBytesAfterGpuCompletion) {
  document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document_);
  multi_grade_test::ResetGradeLookToIdentity(*document_->PrimaryGrade());
  renderer_ = std::make_unique<OpenClRenderer>(document_, nm2_qualification::MakeUnpacker());
  nm2_qualification::MultiGradeResourceBytesAfterGpuCompletion(*renderer_, *document_, image_);
}

}  // namespace alcedo
