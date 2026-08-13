//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file application_module_host_lifecycle_test.cpp
/// @brief Deterministic construction/destruction order coverage for
/// ApplicationModuleHost module composition.

#include <gtest/gtest.h>

#include <QMetaProperty>
#include <algorithm>
#include <string>
#include <vector>

#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"

namespace alcedo::ui::test {
namespace {

using ApplicationModuleHostLifecycleTests = ApplicationModuleHostTestFixture;

TEST_F(ApplicationModuleHostLifecycleTests,
       ApplicationModuleHostConstructsAndDestroysModulesInDependencyOrder) {
  std::vector<ApplicationModuleHost::LifecycleEvent> events;
  {
    ApplicationModuleHost host(nullptr, [&events](const auto& event) { events.push_back(event); });
    ASSERT_FALSE(events.empty());
    ASSERT_TRUE(std::all_of(events.begin(), events.end(), [](const auto& event) {
      return event.kind == ApplicationModuleHost::LifecycleEvent::Kind::Constructed;
    }));
    EXPECT_NE(host.editor_session(), nullptr);
    EXPECT_NE(host.workspace_router(), nullptr);
  }

  const auto first_destroy = std::find_if(events.begin(), events.end(), [](const auto& event) {
    return event.kind == ApplicationModuleHost::LifecycleEvent::Kind::Destroyed;
  });
  ASSERT_NE(first_destroy, events.end());
  const auto construction_end = static_cast<size_t>(first_destroy - events.begin());
  ASSERT_GT(construction_end, 0u);
  ASSERT_EQ(events.size() - construction_end, construction_end);

  for (size_t i = 0; i < construction_end; ++i) {
    EXPECT_EQ(events[i].kind, ApplicationModuleHost::LifecycleEvent::Kind::Constructed);
    EXPECT_NE(events[i].object, nullptr);
    const auto& destroyed = events[construction_end + i];
    EXPECT_EQ(destroyed.kind, ApplicationModuleHost::LifecycleEvent::Kind::Destroyed);
    EXPECT_EQ(destroyed.type_name, events[construction_end - 1 - i].type_name);
    EXPECT_EQ(destroyed.object, events[construction_end - 1 - i].object);
  }

  // Required module types appear in dependency-respecting order.
  auto index_of = [&](const std::string& name) -> int {
    for (size_t i = 0; i < construction_end; ++i) {
      if (events[i].type_name == name) {
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
  EXPECT_LT(index_of("ImportExportHandler"), index_of("AdjustmentTransferController"));

  // Live host constructs and exposes every module pointer with its concrete
  // QMetaProperty type, so QML tooling cannot silently regress to QObject*.
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
    EXPECT_NE(host.background_tasks(), nullptr);
    EXPECT_NE(host.interaction_policy(), nullptr);
    EXPECT_NE(host.model_download(), nullptr);
    EXPECT_NE(host.updates(), nullptr);
    EXPECT_NE(host.semantic_generation(), nullptr);
    EXPECT_NE(host.ai_provider_profiles(), nullptr);
    EXPECT_NE(host.image_analysis(), nullptr);
    EXPECT_NE(host.adjustment_transfer(), nullptr);
    EXPECT_NE(host.editor_session(), nullptr);
    EXPECT_NE(host.workspace_router(), nullptr);
    EXPECT_FALSE(host.project()->ServiceReady());

    const auto*                                            meta           = host.metaObject();
    const std::vector<std::pair<const char*, const char*>> property_types = {
        {"project", "alcedo::ui::ProjectModule*"},
        {"library", "alcedo::ui::LibraryModule*"},
        {"folders", "alcedo::ui::FolderController*"},
        {"images", "alcedo::ui::ImageController*"},
        {"stats", "alcedo::ui::StatsEngine*"},
        {"search", "alcedo::ui::SearchController*"},
        {"importExport", "alcedo::ui::ImportExportHandler*"},
        {"nikonHeRecovery", "alcedo::ui::NikonHeRecoveryController*"},
        {"backgroundTasks", "alcedo::ui::BackgroundTaskController*"},
        {"interactionPolicy", "alcedo::ui::InteractionPolicyController*"},
        {"modelDownload", "alcedo::ui::ModelDownloadController*"},
        {"updates", "alcedo::UpdateService*"},
        {"semanticGeneration", "alcedo::ui::SemanticGenerationController*"},
        {"aiProviderProfiles", "alcedo::AiProviderProfileController*"},
        {"imageAnalysis", "alcedo::ui::ImageAnalysisController*"},
        {"adjustmentTransfer", "alcedo::ui::AdjustmentTransferController*"},
        {"workspaceRouter", "alcedo::ui::WorkspaceRouter*"},
        {"editorSession", "alcedo::ui::EditorSessionController*"},
    };
    for (const auto& [name, type_name] : property_types) {
      const int index = meta->indexOfProperty(name);
      ASSERT_GE(index, 0) << name;
      EXPECT_STREQ(meta->property(index).typeName(), type_name) << name;
    }
  }
}

TEST_F(ApplicationModuleHostLifecycleTests, ModulesCanBeConstructedWithoutTheCompositionRoot) {
  class NullStatusSink final : public IUiStatusSink {
   public:
    void SetServiceMessage(const i18n::LocalizedText&) override {}
    void SetTaskState(const i18n::LocalizedText&, int, bool) override {}
    void ScheduleIdleTaskStateReset(int) override {}
  } status;

  ProjectDbWriteBarrier barrier;
  ProjectModule         project;
  LibraryModule         library(nullptr);
  FolderController      folders(nullptr, nullptr, &status);
  ImageController       images(nullptr, nullptr, nullptr, &status);
  StatsEngine           stats(nullptr, nullptr, nullptr);
  SearchController      search(nullptr, nullptr, nullptr, nullptr);
  ImportExportHandler   import_export(nullptr, nullptr, nullptr, &status, &barrier);

  EXPECT_EQ(project.parent(), nullptr);
  EXPECT_EQ(library.parent(), nullptr);
  EXPECT_EQ(folders.parent(), nullptr);
  EXPECT_EQ(images.parent(), nullptr);
  EXPECT_EQ(stats.parent(), nullptr);
  EXPECT_EQ(search.parent(), nullptr);
  EXPECT_EQ(import_export.parent(), nullptr);
  EXPECT_EQ(stats.FormatPhotoInfo(0, 0), QStringLiteral("No images loaded."));
}

}  // namespace
}  // namespace alcedo::ui::test
