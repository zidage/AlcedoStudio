//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>

#include "support/render_snapshot_source.hpp"
#include "edit/runtime/executor_role.hpp"
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
    ResetRenderers();
    image_ = nm2_qualification::MakeEncodedImage(91);
  }

  /// Binds a new snapshot source to document_ and creates one renderer of each role.
  void ResetRenderers() {
    source_   = std::make_unique<test::RenderSnapshotSource>(document_);
    renderer_ = std::make_unique<OpenClRenderer>(ExecutorRole::Interactive,
                                                 nm2_qualification::MakeUnpacker());
    batch_renderer_ =
        std::make_unique<OpenClRenderer>(ExecutorRole::Batch, nm2_qualification::MakeUnpacker());
  }

  std::shared_ptr<PipelineDocument>           document_;
  std::unique_ptr<test::RenderSnapshotSource> source_;
  std::unique_ptr<OpenClRenderer>             renderer_;
  std::unique_ptr<OpenClRenderer>             batch_renderer_;
  std::shared_ptr<ImageBuffer>                image_;
};

}  // namespace

TEST_F(OpenClNm2QualificationFixture, ExportRecipeDoesNotChangeNextEditorRender) {
  nm2_qualification::ExportRecipeDoesNotChangeNextEditorRender(*renderer_, *batch_renderer_,
                                                               *source_, image_);
}

TEST_F(OpenClNm2QualificationFixture, MultiGradeDocumentRoundTripPreservesOwnersAndEdges) {
  nm2_qualification::MultiGradeDocumentRoundTripPreservesOwnersAndEdges(*renderer_, *source_,
                                                                        image_);
}

TEST_F(OpenClNm2QualificationFixture, BackgroundMultiGradeRenderPreservesEditorCache) {
  nm2_qualification::BackgroundMultiGradeRenderPreservesEditorCache(*renderer_, *batch_renderer_,
                                                                    *source_, image_);
}

TEST_F(OpenClNm2QualificationFixture, MultiGradeResourceBytesAfterGpuCompletion) {
  document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document_);
  multi_grade_test::ResetGradeLookToIdentity(*document_->PrimaryGrade());
  ResetRenderers();
  nm2_qualification::MultiGradeResourceBytesAfterGpuCompletion(*renderer_, *batch_renderer_,
                                                               *source_, image_);
}

}  // namespace alcedo
