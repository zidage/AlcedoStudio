//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file application_module_host_lifecycle_test.cpp
/// @brief Deterministic construction/destruction order coverage for
/// ApplicationModuleHost module composition.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::ui::test {
namespace {

using ApplicationModuleHostLifecycleTests = ApplicationModuleHostTestFixture;

TEST_F(ApplicationModuleHostLifecycleTests,
       ApplicationModuleHostConstructsAndDestroysModulesInDependencyOrder) {
  const auto construction = ApplicationModuleHost::ConstructionOrder();
  const auto destruction  = ApplicationModuleHost::DestructionOrder();

  ASSERT_FALSE(construction.empty());
  ASSERT_EQ(construction.size(), destruction.size());

  // Destruction is the reverse of construction.
  for (size_t i = 0; i < construction.size(); ++i) {
    EXPECT_EQ(destruction[i], construction[construction.size() - 1 - i])
        << "index " << i << " destruction must reverse construction";
  }

  // Required module types appear in dependency-respecting order.
  auto index_of = [&](const std::string& name) -> int {
    for (size_t i = 0; i < construction.size(); ++i) {
      if (construction[i] == name) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  EXPECT_LT(index_of("BackgroundTaskController"), index_of("InteractionPolicyController"));
  EXPECT_LT(index_of("ProjectModule"), index_of("LibraryModule"));
  EXPECT_LT(index_of("LibraryModule"), index_of("FolderController"));
  EXPECT_LT(index_of("FolderController"), index_of("ImageController"));
  EXPECT_LT(index_of("ProjectModule"), index_of("StatsEngine"));
  EXPECT_LT(index_of("StatsEngine"), index_of("SearchController"));
  EXPECT_LT(index_of("BackgroundTaskController"), index_of("ModelDownloadController"));
  EXPECT_LT(index_of("ModelDownloadController"), index_of("SemanticGenerationController"));
  EXPECT_LT(index_of("ProjectDbWriteBarrier"), index_of("ImageAnalysisSink"));
  EXPECT_LT(index_of("ImageAnalysisSink"), index_of("ImageAnalysisController"));
  EXPECT_LT(index_of("ImportExportHandler"), index_of("NikonHeRecoveryController"));
  EXPECT_LT(index_of("LibraryModule"), index_of("EditorController"));
  EXPECT_LT(index_of("ImportExportHandler"), index_of("AdjustmentTransferController"));

  // Live host constructs and exposes every module pointer.
  {
    ApplicationModuleHost host;
    EXPECT_NE(host.project(), nullptr);
    EXPECT_NE(host.library(), nullptr);
    EXPECT_NE(host.folders(), nullptr);
    EXPECT_NE(host.images(), nullptr);
    EXPECT_NE(host.stats(), nullptr);
    EXPECT_NE(host.search(), nullptr);
    EXPECT_NE(host.import_export(), nullptr);
    EXPECT_NE(host.nikon_he_recovery(), nullptr);
    EXPECT_NE(host.editor(), nullptr);
    EXPECT_NE(host.background_tasks(), nullptr);
    EXPECT_NE(host.interaction_policy(), nullptr);
    EXPECT_NE(host.model_download(), nullptr);
    EXPECT_NE(host.semantic_generation(), nullptr);
    EXPECT_NE(host.ai_provider_profiles(), nullptr);
    EXPECT_NE(host.image_analysis(), nullptr);
    EXPECT_NE(host.adjustment_transfer(), nullptr);
    EXPECT_FALSE(host.project()->ServiceReady());
  }
  // Destructor ran without crash (scope exit).
  SUCCEED();
}

}  // namespace
}  // namespace alcedo::ui::test
