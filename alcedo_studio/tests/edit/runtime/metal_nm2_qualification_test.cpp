//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>

#include "support/render_snapshot_source.hpp"
#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/metal/metal_renderer.hpp"
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

class MetalNm2QualificationFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    document_ = nm2_qualification::MakeThreeGradeDocument();
    ResetRenderers();
    image_ = nm2_qualification::MakeEncodedImage(81);
  }

  /// Binds a new snapshot source to document_ and creates one renderer of each role.
  void ResetRenderers() {
    source_   = std::make_unique<test::RenderSnapshotSource>(document_);
    renderer_ = std::make_unique<MetalRenderer>(ExecutorRole::Interactive,
                                                nm2_qualification::MakeUnpacker());
    batch_renderer_ =
        std::make_unique<MetalRenderer>(ExecutorRole::Batch, nm2_qualification::MakeUnpacker());
  }

  std::shared_ptr<PipelineDocument>           document_;
  std::unique_ptr<test::RenderSnapshotSource> source_;
  std::unique_ptr<MetalRenderer>              renderer_;
  std::unique_ptr<MetalRenderer>              batch_renderer_;
  std::shared_ptr<ImageBuffer>                image_;
};

}  // namespace

TEST_F(MetalNm2QualificationFixture, ExportRecipeDoesNotChangeNextEditorRender) {
  nm2_qualification::ExportRecipeDoesNotChangeNextEditorRender(*renderer_, *batch_renderer_,
                                                               *source_, image_);
}

TEST_F(MetalNm2QualificationFixture, MultiGradeDocumentRoundTripPreservesOwnersAndEdges) {
  nm2_qualification::MultiGradeDocumentRoundTripPreservesOwnersAndEdges(*renderer_, *source_,
                                                                        image_);
}

TEST_F(MetalNm2QualificationFixture, BackgroundMultiGradeRenderPreservesEditorCache) {
  nm2_qualification::BackgroundMultiGradeRenderPreservesEditorCache(*renderer_, *batch_renderer_,
                                                                    *source_, image_);
}

TEST_F(MetalNm2QualificationFixture, MultiGradeResourceBytesAfterGpuCompletion) {
  document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document_);
  multi_grade_test::ResetGradeLookToIdentity(*document_->PrimaryGrade());
  ResetRenderers();
  nm2_qualification::MultiGradeResourceBytesAfterGpuCompletion(*renderer_, *batch_renderer_,
                                                               *source_, image_);
}

}  // namespace alcedo
