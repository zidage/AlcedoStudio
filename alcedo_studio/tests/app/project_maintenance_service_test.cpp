// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "app/project_maintenance_service.hpp"

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <format>
#include <fstream>

#include "../support/project_maintenance_fixture.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/pipeline/pipeline_mapper.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"

namespace alcedo {
namespace {
using ProjectMaintenanceServiceTest = test::MaintenanceStorageFixture;

TEST_F(ProjectMaintenanceServiceTest, DuplicateSelectionRetainsEarliestEntryWithoutMutation) {
  Add(1);
  Add(2);
  const auto before = Rows("Element");
  ASSERT_TRUE(service_.Scan(*store_));
  ASSERT_EQ(service_.Findings().size(), 2);
  EXPECT_FALSE(Find(1).removable);
  EXPECT_TRUE(Find(2).removable);
  EXPECT_EQ(Find(2).retained_id, 1);
  EXPECT_EQ(Rows("Element"), before);
  const sl_element_id_t id = 2;
  ASSERT_EQ(service_.Selection(std::span(&id, 1)).size(), 1);
}
TEST_F(ProjectMaintenanceServiceTest, MountedEntryDoesNotParticipateInOrdinaryDuplicateReview) {
  Add(1);
  Add(2);
  Execute("UPDATE Element SET mount_id=9 WHERE id=2");
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_TRUE(service_.Findings().empty());
}
TEST_F(ProjectMaintenanceServiceTest, LegacySamePathEntriesAreReviewOnly) {
  Add(1, {}, false);
  Add(2);
  ASSERT_TRUE(service_.Scan(*store_));
  ASSERT_EQ(service_.Findings().size(), 2);
  for (const auto& finding : service_.Findings()) {
    EXPECT_EQ(finding.kind, MaintenanceFindingKind::SourceHistoryUnknown);
    EXPECT_FALSE(finding.removable);
  }
}
TEST_F(ProjectMaintenanceServiceTest, DifferentRecordedSourcesAreProtectedAndOnlyMismatchIsMarked) {
  Add(1);
  std::ofstream(source_) << "replacement photo";
  Add(2);
  ASSERT_TRUE(service_.Scan(*store_, {}, {}, {}, {}, {}, true));
  ASSERT_EQ(service_.Findings().size(), 2);
  EXPECT_EQ(Find(1).kind, MaintenanceFindingKind::SourceChanged);
  EXPECT_FALSE(Find(1).removable);
  EXPECT_FALSE(Find(2).removable);
  EXPECT_EQ(SourceFileStore(*database_).ReadChangedEntryIds(), (std::vector<sl_element_id_t>{1}));
}
TEST_F(ProjectMaintenanceServiceTest, ReviewOnlyScanKeepsMarksAndEnabledScanMarksBothMismatches) {
  Add(1);
  Add(2);
  std::ofstream(source_) << "replacement photo";
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_EQ(Find(1).kind, MaintenanceFindingKind::SourceChanged);
  EXPECT_EQ(Find(2).kind, MaintenanceFindingKind::SourceChanged);
  EXPECT_TRUE(SourceFileStore(*database_).ReadChangedEntryIds().empty());
  ASSERT_TRUE(service_.Scan(*store_, {}, {}, {}, {}, {}, true));
  EXPECT_TRUE(service_.ChangedMarksWritten());
  EXPECT_EQ(SourceFileStore(*database_).ReadChangedEntryIds(),
            (std::vector<sl_element_id_t>{1, 2}));
  EXPECT_FALSE(Find(1).removable);
  EXPECT_FALSE(Find(2).removable);
  std::ofstream(source_) << "source bytes";
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_FALSE(service_.ChangedMarksWritten());
  EXPECT_EQ(SourceFileStore(*database_).ReadChangedEntryIds(),
            (std::vector<sl_element_id_t>{1, 2}));
}
TEST_F(ProjectMaintenanceServiceTest, SavedSingletonMarkReopensAndClearsOnVerifiedMatch) {
  Add(1);
  const auto             original = *ReadMaintenanceFingerprint(source_);
  const SourceChangeMark mark{1, original, true};
  ASSERT_TRUE(SourceFileStore(*database_).UpdateChangeMarks(std::span(&mark, 1)));
  store_.reset();
  database_.reset();
  database_ = std::make_unique<Database>(database_path_);
  store_    = std::make_unique<ProjectMaintenanceStore>(*database_);
  EXPECT_EQ(SourceFileStore(*database_).ReadChangedEntryIds(), (std::vector<sl_element_id_t>{1}));
  bool canceled = false;
  EXPECT_FALSE(service_.Scan(
      *store_, {}, {}, [&] { return canceled; }, {},
      [&](const auto& path) {
        canceled = true;
        return ProjectMaintenanceService::CheckSource(path);
      },
      true));
  EXPECT_EQ(SourceFileStore(*database_).ReadChangedEntryIds(), (std::vector<sl_element_id_t>{1}));
  ASSERT_TRUE(service_.Scan(*store_, {}, {}, {}, {}, {}, true));
  EXPECT_TRUE(service_.ChangedMarksWritten());
  EXPECT_TRUE(service_.Findings().empty());
  EXPECT_TRUE(SourceFileStore(*database_).ReadChangedEntryIds().empty());
}
TEST_F(ProjectMaintenanceServiceTest, UnmarkedSingletonDoesNotGetFingerprinted) {
  Add(1);
  std::ofstream(source_) << "replacement photo";
  ASSERT_TRUE(service_.Scan(*store_, {}, {}, {}, {}, {}, true));
  EXPECT_TRUE(service_.Findings().empty());
  EXPECT_TRUE(SourceFileStore(*database_).ReadChangedEntryIds().empty());
}
TEST_F(ProjectMaintenanceServiceTest, DifferentFoldersWithEqualNamesRemainSeparate) {
  Add(1);
  std::filesystem::create_directory(root_ / "other");
  std::filesystem::copy_file(source_, root_ / "other/photo.raw");
  Add(2, root_ / "other/photo.raw");
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_TRUE(service_.Findings().empty());
}
TEST_F(ProjectMaintenanceServiceTest, RatedUneditedEntryIsProtected) {
  Add(1);
  Add(2);
  Execute("UPDATE Image SET rating=4,metadata='{\"Rating\":4}' WHERE id=2");
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_FALSE(Find(2).removable);
  EXPECT_TRUE(Find(1).removable);
  EXPECT_EQ(Find(1).retained_id, 2);
  const sl_element_id_t id = 2;
  EXPECT_THROW(service_.Selection(std::span(&id, 1)), std::invalid_argument);
}
TEST_F(ProjectMaintenanceServiceTest,
       ColorLabelsAndDeletionMarksProtectDuplicateAndMissingEntries) {
  Add(1);
  Add(2);
  Add(3);
  Execute(
      "UPDATE Image SET color_labels=1 WHERE id=2;"
      "UPDATE Image SET delete_mark=true WHERE id=3");
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_TRUE(Find(1).removable);
  EXPECT_EQ(Find(1).retained_id, 2);
  for (const sl_element_id_t id : {2U, 3U}) {
    EXPECT_FALSE(Find(id).removable);
    EXPECT_THROW(service_.Selection(std::span(&id, 1)), std::invalid_argument);
  }
  std::filesystem::remove(source_);
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_TRUE(Find(1).removable);
  EXPECT_FALSE(Find(2).removable);
  EXPECT_FALSE(Find(3).removable);
}
TEST_F(ProjectMaintenanceServiceTest, RenamedVersionAndMissingSourceRemainProtected) {
  Add(1);
  Execute("UPDATE VersionRef SET display_name='Keep work' WHERE element_id=1");
  std::filesystem::remove(source_);
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_EQ(Find(1).kind, MaintenanceFindingKind::MissingSource);
  EXPECT_FALSE(Find(1).removable);
}
TEST_F(ProjectMaintenanceServiceTest, InactiveVersionProtectsEntry) {
  Add(1);
  Add(2);
  auto guard = database_->GetConnectionGuard();
  {
    auto             lock = guard.Lock();
    CommitGraphStore store(guard.conn_);
    auto             graph = store.LoadGraph(1);
    graph->CreateVersionRefAtRoot("Other");
    store.Materialize(graph->CaptureMaterialization());
  }
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_FALSE(Find(1).removable);
  EXPECT_EQ(Find(1).evidence.version_count, 2);
}
TEST_F(ProjectMaintenanceServiceTest, UndoneCommitProtectsMissingSourceEntry) {
  Add(1);
  {
    auto               guard = database_->GetConnectionGuard();
    auto               lock  = guard.Lock();
    CommitGraphStore   store(guard.conn_);
    auto               graph = store.LoadGraph(1);
    SetParameterChange change;
    change.target.owner_kind             = PipelineParameterOwnerKind::ColorGrade;
    change.target.node_id                = NodeId{"grade.primary"};
    change.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.exposure"};
    change.target.field_key              = "exposure";
    change.before_value                  = CreateDefaultPipelineDocument()
                                               .PrimaryGrade()
                                               ->FindAdjustmentByType(type_ids::Exposure())
                                               ->ToJson();
    change.after_value                   = change.before_value;
    change.after_value["exposure_ev"]    = 2.0;
    change.before_enabled = change.after_enabled = true;
    PipelineEditBatch batch;
    batch.operation_kind   = PipelineEditOperationKind::SetParameter;
    batch.presentation_key = "history.operation.set_parameter";
    batch.changes.push_back(std::move(change));
    graph->InsertCommit(EditCommit::MakePipelineEdit(graph->GetRootId(), {}, std::move(batch)));
    store.Materialize(graph->CaptureMaterialization());  // Default head remains at the empty root.
  }
  std::filesystem::remove(source_);
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_EQ(Find(1).kind, MaintenanceFindingKind::MissingSource);
  EXPECT_EQ(Find(1).evidence.commit_count, 1);
  EXPECT_FALSE(Find(1).removable);
}
TEST_F(ProjectMaintenanceServiceTest, BrokenReferenceProtectsItsEntry) {
  Add(1);
  Add(2);
  Execute("DELETE FROM PipelineRoot WHERE element_id=2");
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_FALSE(Find(2).removable);
  EXPECT_FALSE(Find(2).evidence.protection_reasons.empty());
}
TEST_F(ProjectMaintenanceServiceTest, UniqueRatedAndEditedEntryIsNotAnInvalidFinding) {
  Add(1);
  Execute(
      "UPDATE Image SET rating=4,metadata='{\"Rating\":4}' WHERE id=1;"
      "UPDATE VersionRef SET display_name='Kept version' WHERE element_id=1");
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_TRUE(service_.Findings().empty());
}
TEST_F(ProjectMaintenanceServiceTest, PipelineDifferenceProtectsEntry) {
  Add(1);
  Add(2);
  auto document = CreateDefaultPipelineDocument();
  document.SetNextColorGradeNameNumber(5);
  auto guard = database_->GetConnectionGuard();
  {
    auto lock = guard.Lock();
    PipelineMapper(guard.conn_).UpdatePipelineJsonByFileId(2, document.ToJson());
  }
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_FALSE(Find(2).removable);
}
TEST_F(ProjectMaintenanceServiceTest, PermissionAndUnavailableVolumeAreNotMissingSources) {
  Add(1);
  Add(2);
  ASSERT_TRUE(service_.Scan(*store_, {}, {}, {}, {}, [](const auto&) {
    return MaintenanceSourceCheck{MaintenanceSourceStatus::Error, "Permission denied"};
  }));
  EXPECT_EQ(Find(1).kind, MaintenanceFindingKind::SourceProblem);
  EXPECT_FALSE(Find(1).removable);
  EXPECT_FALSE(Find(2).removable);
  EXPECT_EQ(ProjectMaintenanceService::CheckSource(root_).status, MaintenanceSourceStatus::Error);
}
TEST_F(ProjectMaintenanceServiceTest, ScanLeavesAllProjectRowsUnchanged) {
  Add(1);
  Add(2);
  const std::vector<std::string> tables{
      "Image",         "Element",    "FileImage",      "FolderContent", "PipelineRoot",
      "PipelineParam", "VersionRef", "ImageEditState", "EditCommit",    "MaskAsset"};
  std::vector<std::string> before;
  for (const auto& table : tables) before.push_back(Rows(table));
  ASSERT_TRUE(service_.Scan(*store_));
  for (size_t i = 0; i < tables.size(); ++i) EXPECT_EQ(before[i], Rows(tables[i])) << tables[i];
}
TEST_F(ProjectMaintenanceServiceTest, CanceledScanCannotSelectAndEnumerationPagesAreBounded) {
  for (sl_element_id_t id = 1; id <= 65; ++id) Add(id);
  ProjectMaintenanceStore store(*database_);
  EXPECT_EQ(store.ListEntryIds(0).size(), 64);
  EXPECT_EQ(store.ListEntryIds(64).size(), 1);
  EXPECT_FALSE(service_.Scan(*store_, {}, {}, [] { return true; }));
  const sl_element_id_t id = 2;
  EXPECT_THROW(service_.Selection(std::span(&id, 1)), std::invalid_argument);
}
}  // namespace
}  // namespace alcedo
