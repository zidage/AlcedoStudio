//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/mask_thumbnail_coordinator.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QImage>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/editor_node_graph_projection.hpp"
#include "app/mask_thumbnail_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"

namespace alcedo::ui {
namespace {

void RunInline(alcedo::MaskThumbnailService* service) {
  service->SetJobRunner([](std::function<void()> job) { job(); });
  service->SetResultDispatcher([](QObject*, std::function<void()> fn) { fn(); });
}

auto DocumentWithMasks(std::vector<MaskModel> masks) -> PipelineDocument {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  for (std::size_t i = 0; i < masks.size(); ++i) {
    grade->AddMask(std::move(masks[i]), i);
  }
  return document;
}

auto GroupsOf(const PipelineDocument& document) -> EditorMaskGroupSnapshot {
  return EditorNodeGraphProjection::BuildMaskGroups(document, 1, 1, 1);
}

}  // namespace

TEST(MaskThumbnailCoordinator, EmptyGradeDoesNotRequestAndDisabledMembersStayBlack) {
  auto service = std::make_shared<alcedo::MaskThumbnailService>(8);
  RunInline(service.get());
  MaskThumbnailCoordinator coordinator;
  auto                     store = std::make_shared<MaskThumbnailImageStore>();
  coordinator.SetImageStore(store);
  coordinator.SetService(service);
  ASSERT_TRUE(coordinator.SetFullReference(Extent2D{128, 128}));

  const auto empty = CreateDefaultPipelineDocument();
  coordinator.Sync(empty, GroupsOf(empty));
  EXPECT_EQ(service->generate_count(), 0u);
  EXPECT_TRUE(coordinator.thumbnailUrl(QStringLiteral("grade.primary"), QString()).isEmpty());

  auto off = grade_mask_test::MakeRadialMask(MaskId{"mask.off"});
  off.enabled = false;
  const auto disabled = DocumentWithMasks({off});
  coordinator.Sync(disabled, GroupsOf(disabled));
  EXPECT_EQ(service->generate_count(), 2u);
  const auto group_id = coordinator.request_id(QStringLiteral("grade.primary"), QString());
  const auto row_id =
      coordinator.request_id(QStringLiteral("grade.primary"), QStringLiteral("mask.off"));
  EXPECT_EQ(qGray(store->Get(group_id).pixel(64, 64)), 0);
  EXPECT_EQ(qGray(store->Get(row_id).pixel(64, 64)), 0);
}

TEST(MaskThumbnailCoordinator, SameContentDifferentMaskIdsShareGeneratedPixels) {
  auto service = std::make_shared<alcedo::MaskThumbnailService>(8);
  RunInline(service.get());
  MaskThumbnailCoordinator coordinator;
  coordinator.SetImageStore(std::make_shared<MaskThumbnailImageStore>());
  coordinator.SetService(service);
  ASSERT_TRUE(coordinator.SetFullReference(Extent2D{128, 128}));

  auto       first    = grade_mask_test::MakeRadialMask(MaskId{"mask.one"});
  auto       second   = grade_mask_test::MakeRadialMask(MaskId{"mask.two"});
  const auto document = DocumentWithMasks({first, second});
  coordinator.Sync(document, GroupsOf(document));
  EXPECT_EQ(service->generate_count(), 2u);
  EXPECT_FALSE(coordinator
                   .thumbnailUrl(QStringLiteral("grade.primary"), QStringLiteral("mask.one"))
                   .isEmpty());
  EXPECT_FALSE(coordinator
                   .thumbnailUrl(QStringLiteral("grade.primary"), QStringLiteral("mask.two"))
                   .isEmpty());
}

TEST(MaskThumbnailCoordinator, StaleRequestIdDoesNotPublish) {
  auto                                 service = std::make_shared<alcedo::MaskThumbnailService>(8);
  std::vector<std::function<void()>> held;
  service->SetJobRunner([&](std::function<void()> job) { held.push_back(std::move(job)); });
  service->SetResultDispatcher([](QObject*, std::function<void()> fn) { fn(); });

  MaskThumbnailCoordinator coordinator;
  coordinator.SetImageStore(std::make_shared<MaskThumbnailImageStore>());
  coordinator.SetService(service);
  ASSERT_TRUE(coordinator.SetFullReference(Extent2D{128, 128}));

  auto       mask     = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"});
  const auto document = DocumentWithMasks({mask});
  coordinator.Sync(document, GroupsOf(document));
  ASSERT_FALSE(held.empty());
  const auto first_id =
      coordinator.request_id(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"));
  coordinator.invalidateTarget(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"));
  const auto second_id =
      coordinator.request_id(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"));
  EXPECT_NE(first_id, second_id);

  for (auto& job : held) {
    job();
  }
  EXPECT_TRUE(coordinator.thumbnailUrl(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"))
                  .isEmpty());
}

TEST(MaskThumbnailCoordinator, RemovedRowDropsLateResultAndLeavesCacheFilled) {
  auto                                 service = std::make_shared<alcedo::MaskThumbnailService>(8);
  std::vector<std::function<void()>> held;
  service->SetJobRunner([&](std::function<void()> job) { held.push_back(std::move(job)); });
  service->SetResultDispatcher([](QObject*, std::function<void()> fn) { fn(); });

  MaskThumbnailCoordinator coordinator;
  coordinator.SetImageStore(std::make_shared<MaskThumbnailImageStore>());
  coordinator.SetService(service);
  ASSERT_TRUE(coordinator.SetFullReference(Extent2D{128, 128}));

  auto       mask     = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"});
  const auto document = DocumentWithMasks({mask});
  coordinator.Sync(document, GroupsOf(document));
  const auto spec =
      coordinator.current_spec(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"));
  ASSERT_TRUE(spec.has_value());
  coordinator.invalidateNode(QStringLiteral("grade.primary"));
  coordinator.Sync(CreateDefaultPipelineDocument(), GroupsOf(CreateDefaultPipelineDocument()));
  for (auto& job : held) {
    job();
  }
  EXPECT_TRUE(coordinator.thumbnailUrl(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"))
                  .isEmpty());
  EXPECT_TRUE(service->Cached(*spec).has_value());
}

TEST(MaskThumbnailCoordinator, SecondSyncOfUnchangedSpecDoesNotGenerateAgain) {
  auto service = std::make_shared<alcedo::MaskThumbnailService>(8);
  RunInline(service.get());
  MaskThumbnailCoordinator coordinator;
  coordinator.SetImageStore(std::make_shared<MaskThumbnailImageStore>());
  coordinator.SetService(service);
  ASSERT_TRUE(coordinator.SetFullReference(Extent2D{128, 128}));

  auto       mask     = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"});
  const auto document = DocumentWithMasks({mask});
  coordinator.Sync(document, GroupsOf(document));
  const auto first_count = service->generate_count();
  coordinator.Sync(document, GroupsOf(document));
  EXPECT_EQ(service->generate_count(), first_count);
}

TEST(MaskThumbnailCoordinator, DestroyedReceiverDropsQueuedCallback) {
  auto                                 service = std::make_shared<alcedo::MaskThumbnailService>(8);
  std::vector<std::function<void()>> held;
  service->SetJobRunner([&](std::function<void()> job) { held.push_back(std::move(job)); });

  auto coordinator = std::make_unique<MaskThumbnailCoordinator>();
  coordinator->SetImageStore(std::make_shared<MaskThumbnailImageStore>());
  coordinator->SetService(service);
  ASSERT_TRUE(coordinator->SetFullReference(Extent2D{128, 128}));
  auto       mask     = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"});
  const auto document = DocumentWithMasks({mask});
  coordinator->Sync(document, GroupsOf(document));
  coordinator.reset();
  for (auto& job : held) {
    job();
  }
  QCoreApplication::processEvents();
  EXPECT_GE(service->generate_count(), 1u);
}

TEST(MaskThumbnailCoordinator, InvalidateThenSyncSameDocumentPublishesFromCache) {
  auto                                 service = std::make_shared<alcedo::MaskThumbnailService>(8);
  std::vector<std::function<void()>> held;
  service->SetJobRunner([&](std::function<void()> job) { held.push_back(std::move(job)); });
  service->SetResultDispatcher([](QObject*, std::function<void()> fn) { fn(); });

  MaskThumbnailCoordinator coordinator;
  coordinator.SetImageStore(std::make_shared<MaskThumbnailImageStore>());
  coordinator.SetService(service);
  ASSERT_TRUE(coordinator.SetFullReference(Extent2D{128, 128}));

  auto       mask     = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"});
  const auto document = DocumentWithMasks({mask});
  coordinator.Sync(document, GroupsOf(document));
  coordinator.invalidateTarget(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"));
  for (auto& job : held) {
    job();
  }
  held.clear();
  EXPECT_TRUE(
      coordinator.thumbnailUrl(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"))
          .isEmpty());
  const auto spec =
      coordinator.current_spec(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"));
  ASSERT_TRUE(spec.has_value());
  EXPECT_TRUE(service->Cached(*spec).has_value());

  const auto generated = service->generate_count();
  EXPECT_GE(generated, 1u);
  RunInline(service.get());
  coordinator.Sync(document, GroupsOf(document));
  EXPECT_FALSE(
      coordinator.thumbnailUrl(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"))
          .isEmpty());
  EXPECT_EQ(service->generate_count(), generated);
}

TEST(MaskThumbnailCoordinator, InvalidateOneMaskLeavesSiblingRequestId) {
  auto service = std::make_shared<alcedo::MaskThumbnailService>(8);
  RunInline(service.get());
  MaskThumbnailCoordinator coordinator;
  coordinator.SetImageStore(std::make_shared<MaskThumbnailImageStore>());
  coordinator.SetService(service);
  ASSERT_TRUE(coordinator.SetFullReference(Extent2D{128, 128}));

  auto       first    = grade_mask_test::MakeRadialMask(MaskId{"mask.one"});
  auto       second   = grade_mask_test::MakeRadialMask(MaskId{"mask.two"});
  const auto document = DocumentWithMasks({first, second});
  coordinator.Sync(document, GroupsOf(document));
  const auto sibling_id =
      coordinator.request_id(QStringLiteral("grade.primary"), QStringLiteral("mask.two"));
  const auto sibling_url =
      coordinator.thumbnailUrl(QStringLiteral("grade.primary"), QStringLiteral("mask.two"));
  coordinator.invalidateTarget(QStringLiteral("grade.primary"), QStringLiteral("mask.one"));
  EXPECT_EQ(coordinator.request_id(QStringLiteral("grade.primary"), QStringLiteral("mask.two")),
            sibling_id);
  EXPECT_EQ(coordinator.thumbnailUrl(QStringLiteral("grade.primary"), QStringLiteral("mask.two")),
            sibling_url);
}

TEST(MaskThumbnailCoordinator, MissingGeometryClearsBindings) {
  auto service = std::make_shared<alcedo::MaskThumbnailService>(8);
  RunInline(service.get());
  MaskThumbnailCoordinator coordinator;
  coordinator.SetImageStore(std::make_shared<MaskThumbnailImageStore>());
  coordinator.SetService(service);
  auto       mask     = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"});
  const auto document = DocumentWithMasks({mask});
  coordinator.Sync(document, GroupsOf(document));
  EXPECT_EQ(service->generate_count(), 0u);
  EXPECT_TRUE(coordinator.thumbnailUrl(QStringLiteral("grade.primary"), QStringLiteral("mask.radial"))
                  .isEmpty());
}

}  // namespace alcedo::ui
