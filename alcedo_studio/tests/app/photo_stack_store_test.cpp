//  Copyright 2026 Alcedo Studio contributors
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "app/photo_stack_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_types.hpp"
#include "image/color_label.hpp"
#include "image/image.hpp"
#include "library_search_test_support.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"
#include "support/storage_invariants.hpp"
#include "support/storage_test_access.hpp"
#include "support/test_temp_directory.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

struct TextRow {
  std::string text;
  using Columns = duckorm::RowColumns<&TextRow::text>;
};

/// Real project collaborators; synthetic photos have persisted image bindings and edit roots.
class PhotoStackStoreTest : public ::testing::Test {
 protected:
  test::TestTempDirectory         temp;
  std::filesystem::path           db_path   = temp / "project.db";
  std::filesystem::path           meta_path = temp / "project.json";
  std::unique_ptr<ProjectService> project;
  std::vector<sl_element_id_t>    files;

  void                            SetUp() override {
    TimeProvider::Refresh();
    project = std::make_unique<ProjectService>(db_path, meta_path, ProjectOpenMode::kCreateNew);
    std::vector<library_search_test::SyntheticImageSpec> specs;
    for (int i = 0; i < 8; ++i) {
      specs.push_back({.file_name_   = std::format(L"photo{}.jpg", i),
                       .image_path_  = std::format(L"D:/photos/photo{}.jpg", i),
                       .model_       = "Stack test camera",
                       .rating_      = i % 6,
                       .is_raw_file_ = false});
    }
    files = library_search_test::SyntheticLibraryBuilder(*project).AddFiles(specs);
    ASSERT_EQ(files.size(), 8);
    PipelineMgmtService pipelines(project->GetStorage());
    for (const auto id : files) {
      pipelines.InitializeImageRoot(id, CreateDefaultPipelineDocument(), nullptr);
    }
  }

  void TearDown() override {
    project.reset();
    test::ExpectStoredStorageInvariants(db_path);
  }

  /// Ordered explicit File IDs, expressed by fixture indices for readable expected results.
  auto Files(std::initializer_list<std::size_t> indices) const -> std::vector<sl_element_id_t> {
    std::vector<sl_element_id_t> ids;
    for (const auto index : indices) {
      ids.push_back(files.at(index));
    }
    return ids;
  }

  auto Stacks() -> PhotoStackService& { return *project->GetPhotoStackService(); }
  auto Create(std::initializer_list<std::size_t> indices, std::size_t cover) -> std::string {
    const auto result = Stacks().CreateStack(Files(indices), files.at(cover));
    EXPECT_TRUE(result.Changed());
    EXPECT_EQ(result.stack_id.size(), 36);
    test::ExpectStorageInvariants(*project->GetStorage());
    return result.stack_id;
  }

  /// Test-only SQL for independent row assertions and deliberate statement failure injection.
  void Execute(const std::string& sql) {
    auto guard = StorageTestAccess::GetDatabase(*project->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    duckorm::Statement statement(guard.conn_, sql);
    statement.Execute();
  }

  auto Number(const duckorm::SqlFragment& query) -> std::int64_t {
    auto guard = StorageTestAccess::GetDatabase(*project->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    return duckorm::select_int64(guard.conn_, query).value();
  }

  auto Cover(const std::string& stack) -> sl_element_id_t {
    return static_cast<sl_element_id_t>(
        Number({"SELECT cover_file_id FROM PhotoStack WHERE stack_id = ?", {stack}}));
  }

  /// Capture only serialized rows for before/after equality assertions; never write them back.
  auto StackRows() -> std::vector<std::string> {
    auto guard = StorageTestAccess::GetDatabase(*project->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    std::vector<std::string> result;
    for (const auto& row : duckorm::query<TextRow>(
             guard.conn_,
             duckorm::expr::raw(
                 "SELECT stack_id || '/cover/' || cover_file_id AS text FROM PhotoStack UNION ALL "
                 "SELECT stack_id || '/member/' || file_id || '/' || member_order "
                 "FROM PhotoStackMember ORDER BY text"))) {
      result.push_back(row.text);
    }
    return result;
  }

  /// Save through ProjectService, release its owners, then load from disk as the next session.
  void Reopen() {
    project->SaveProject(meta_path);
    project.reset();
    project = std::make_unique<ProjectService>(db_path, meta_path, ProjectOpenMode::kLoadExisting);
  }
};

TEST_F(PhotoStackStoreTest, CreateStackPersistsMembersOrderAndSelectedCover) {
  const auto stack = Create({2, 0, 2, 1}, 0);
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({2, 0, 1}));
  EXPECT_EQ(Cover(stack), files[0]);
  const auto rows = StackRows();
  Reopen();
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({2, 0, 1}));
  EXPECT_EQ(Cover(stack), files[0]);
  EXPECT_EQ(StackRows(), rows);
}

TEST_F(PhotoStackStoreTest, ManySourceStacksReportEachAffectedStackAndFileOnce) {
  constexpr std::size_t                                stack_count = 128;
  std::vector<library_search_test::SyntheticImageSpec> specs;
  for (std::size_t i = 0; i < stack_count * 2; ++i) {
    specs.push_back({.file_name_   = std::format(L"many{}.jpg", i),
                     .image_path_  = std::format(L"D:/photos/many{}.jpg", i),
                     .is_raw_file_ = false});
  }
  const auto added = library_search_test::SyntheticLibraryBuilder(*project).AddFiles(specs);
  ASSERT_EQ(added.size(), stack_count * 2);
  std::unordered_set<std::string> expected_stacks;
  std::vector<sl_element_id_t>    selected;
  std::vector<sl_element_id_t>    remaining;
  {
    PipelineMgmtService pipelines(project->GetStorage());
    for (std::size_t i = 0; i < added.size(); ++i) {
      pipelines.InitializeImageRoot(added[i], CreateDefaultPipelineDocument(), nullptr);
    }
  }
  for (std::size_t i = 0; i < added.size(); i += 2) {
    const std::array pair{added[i], added[i + 1]};
    expected_stacks.insert(Stacks().CreateStack(pair, pair[0]).stack_id);
    selected.push_back(pair[0]);
    remaining.push_back(pair[1]);
  }
  // Duplicate command inputs must not duplicate persisted membership or reported changes.
  auto targets = selected;
  targets.insert(targets.end(), selected.begin(), selected.end());
  const auto change = Stacks().CreateStack(targets, selected.front());
  expected_stacks.insert(change.stack_id);
  EXPECT_EQ(change.changed_stack_ids.size(), expected_stacks.size());
  EXPECT_EQ((std::unordered_set<std::string>(change.changed_stack_ids.begin(),
                                             change.changed_stack_ids.end())),
            expected_stacks);
  const std::unordered_set<sl_element_id_t> expected_files(added.begin(), added.end());
  EXPECT_EQ(change.changed_file_ids.size(), expected_files.size());
  EXPECT_EQ((std::unordered_set<sl_element_id_t>(change.changed_file_ids.begin(),
                                                 change.changed_file_ids.end())),
            expected_files);
  EXPECT_EQ(Stacks().ReadMemberIds(change.stack_id), selected);
  const auto appended = Stacks().AddMembers(change.stack_id, added);
  EXPECT_EQ(appended.changed_stack_ids, (std::vector<std::string>{change.stack_id}));
  EXPECT_EQ(appended.changed_file_ids.size(), expected_files.size());
  EXPECT_EQ((std::unordered_set<sl_element_id_t>(appended.changed_file_ids.begin(),
                                                 appended.changed_file_ids.end())),
            expected_files);
  auto order = selected;
  order.insert(order.end(), remaining.begin(), remaining.end());
  EXPECT_EQ(Stacks().ReadMemberIds(change.stack_id), order);
  Reopen();
  EXPECT_EQ(Stacks().ReadMemberIds(change.stack_id), order);
}

TEST_F(PhotoStackStoreTest, MovingSelectedMembersRepairsSourceStacksAtomically) {
  const auto source      = Create({0, 1, 2, 3}, 0);
  const auto dissolved   = Create({4, 5}, 4);
  const auto destination = Create({6, 7}, 6);
  const auto result      = Stacks().AddMembers(destination, Files({0, 4, 0}));
  EXPECT_TRUE(result.Changed());
  EXPECT_EQ(result.changed_stack_ids.size(), 3);
  EXPECT_EQ(Stacks().ReadMemberIds(source), Files({1, 2, 3}));
  EXPECT_EQ(Cover(source), files[1]);
  EXPECT_EQ(Stacks().ReadMemberIds(destination), Files({6, 7, 0, 4}));
  EXPECT_EQ(Cover(destination), files[6]);
  EXPECT_THROW(Stacks().ReadMemberIds(dissolved), std::invalid_argument);
  EXPECT_FALSE(Stacks().RemoveMembers(Files({5})).Changed());
  Reopen();
  EXPECT_EQ(Stacks().ReadMemberIds(source), Files({1, 2, 3}));
  EXPECT_EQ(Stacks().ReadMemberIds(destination), Files({6, 7, 0, 4}));
}

TEST_F(PhotoStackStoreTest, InvalidCoverOrMissingFileLeavesEveryStackUnchanged) {
  const auto source      = Create({0, 1, 2}, 0);
  const auto destination = Create({3, 4}, 3);
  const auto rows        = StackRows();
  EXPECT_THROW(Stacks().CreateStack(Files({0, 1}), files[7]), std::invalid_argument);
  auto invalid = Files({0, 1});
  invalid.push_back(std::numeric_limits<sl_element_id_t>::max());
  EXPECT_THROW(Stacks().CreateStack(invalid, files[0]), std::invalid_argument);
  EXPECT_THROW(Stacks().AddMembers(destination, invalid), std::invalid_argument);
  EXPECT_THROW(Stacks().RemoveMembers(invalid), std::invalid_argument);
  EXPECT_THROW(Stacks().SetCover(destination, files[0]), std::invalid_argument);
  EXPECT_EQ(StackRows(), rows);
  EXPECT_EQ(Stacks().ReadMemberIds(source), Files({0, 1, 2}));
  EXPECT_EQ(Stacks().ReadMemberIds(destination), Files({3, 4}));
}

TEST_F(PhotoStackStoreTest, FailureAfterSourceRemovalRestoresAllMemberships) {
  const auto source           = Create({0, 1, 2}, 0);
  const auto destination      = Create({3, 4}, 3);
  const auto dissolved_source = Create({5, 6}, 5);
  // A real destination INSERT fails after Detach removes source cover 0 and promotes cover 1.
  // Test-only schema writes use the existing StorageTestAccess; production has no failure hook.
  Execute("DROP INDEX idx_photo_stack_member_stack");
  Execute("ALTER TABLE PhotoStackMember RENAME TO MembersBeforeFailure");
  Execute(std::format(
      "CREATE TABLE PhotoStackMember (file_id BIGINT PRIMARY KEY, stack_id VARCHAR NOT NULL, "
      "member_order BIGINT NOT NULL CHECK (member_order >= 0), UNIQUE(stack_id, member_order), "
      "CHECK (NOT (file_id = {} AND stack_id = '{}')))",
      files[0], destination));
  Execute("INSERT INTO PhotoStackMember SELECT * FROM MembersBeforeFailure");
  Execute("DROP TABLE MembersBeforeFailure");
  Execute("CREATE INDEX idx_photo_stack_member_stack ON PhotoStackMember(stack_id)");
  const auto rows = StackRows();
  try {
    Stacks().AddMembers(destination, Files({5, 0}));
    FAIL() << "The destination constraint must reject the inserted membership";
  } catch (const duckorm::StatementError& error) {
    EXPECT_NE(std::string(error.what()).find("CHECK constraint failed"), std::string::npos);
  }
  EXPECT_EQ(StackRows(), rows);
  EXPECT_EQ(Cover(source), files[0]);
  Reopen();
  EXPECT_EQ(StackRows(), rows);
  EXPECT_EQ(Stacks().ReadMemberIds(source), Files({0, 1, 2}));
  EXPECT_EQ(Stacks().ReadMemberIds(destination), Files({3, 4}));
  EXPECT_EQ(Stacks().ReadMemberIds(dissolved_source), Files({5, 6}));
  // A distinct valid command still works after the failed transaction.
  EXPECT_TRUE(Stacks().AddMembers(destination, Files({1})).Changed());
}

TEST_F(PhotoStackStoreTest, CoverAndMemberMovePreserveImageHistoryMarksAndAlbumLinks) {
  const auto folder = project->GetSleeveService()->CreateFolder(L"/", L"Stack album");
  ASSERT_TRUE(folder.second.success_);
  const auto album = folder.first->element_id_;
  for (const auto file : Files({0, 1, 2})) {
    ASSERT_TRUE(project->GetSleeveService()->LinkFileToFolder(file, album).success_);
  }
  const auto photo_rows = project->GetAlbumBrowseService()->ReadAlbumFileRows(Files({0, 1, 2}));
  ASSERT_EQ(photo_rows.size(), 3);
  project->GetImagePoolService()->Write_NoSync<void>(photo_rows[0].image_id_,
                                                     [](std::shared_ptr<Image> image) {
                                                       image->SetColorLabel(ColorLabel::kRed, true);
                                                       image->SetDeleteMark(true);
                                                     });
  project->GetImagePoolService()->SyncWithStorage();
  const auto stack        = Create({0, 1, 2}, 0);
  const auto before_photo = project->GetAlbumBrowseService()->ReadAlbumFileRows(Files({0, 1, 2}));
  ASSERT_EQ(before_photo.at(0).color_labels_, ColorLabelBit(ColorLabel::kRed));
  ASSERT_TRUE(before_photo.at(0).delete_mark_);
  std::vector<root_id_t>          history_roots;
  std::vector<head_commit_hash_t> history_heads;
  for (const auto id : Files({0, 1, 2})) {
    auto       session = project->GetStorage()->OpenSession();
    const auto label   = session.CommitGraph().GetMaterializedHistoryLabel(id).value();
    history_roots.push_back(label.root_id);
    history_heads.push_back(label.head_commit_hash);
  }
  EXPECT_TRUE(Stacks().SetCover(stack, files[2]).Changed());
  EXPECT_TRUE(Stacks().MoveMember(files[2], PhotoStackMove::kEarlier).Changed());
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({0, 2, 1}));
  EXPECT_EQ(Cover(stack), files[2]);
  EXPECT_TRUE(Stacks().MoveMember(files[0], PhotoStackMove::kLater).Changed());
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({2, 0, 1}));
  EXPECT_FALSE(Stacks().MoveMember(files[2], PhotoStackMove::kEarlier).Changed());
  EXPECT_FALSE(Stacks().MoveMember(files[1], PhotoStackMove::kLater).Changed());
  const auto after_photo = project->GetAlbumBrowseService()->ReadAlbumFileRows(Files({0, 1, 2}));
  ASSERT_EQ(after_photo.size(), before_photo.size());
  for (std::size_t i = 0; i < after_photo.size(); ++i) {
    EXPECT_EQ(after_photo[i].image_id_, before_photo[i].image_id_);
    EXPECT_EQ(after_photo[i].rating_, before_photo[i].rating_);
    EXPECT_EQ(after_photo[i].color_labels_, before_photo[i].color_labels_);
    EXPECT_EQ(after_photo[i].delete_mark_, before_photo[i].delete_mark_);
    auto       session = project->GetStorage()->OpenSession();
    const auto label   = session.CommitGraph().GetMaterializedHistoryLabel(files[i]).value();
    EXPECT_EQ(label.root_id, history_roots[i]);
    EXPECT_EQ(label.head_commit_hash, history_heads[i]);
  }
  const auto linked = project->GetSleeveService()->Read<std::vector<sl_element_id_t>>(
      [album](FileSystem& fs) { return fs.ListFolderContent(album); });
  EXPECT_EQ(linked.size(), 3);
  for (const auto id : Files({0, 1, 2})) {
    EXPECT_NE(std::find(linked.begin(), linked.end(), id), linked.end());
  }
}

TEST_F(PhotoStackStoreTest, DuplicateAdditionAndUnchangedCoverPerformNoMembershipChange) {
  const auto stack = Create({2, 0, 1}, 0);
  const auto rows  = StackRows();
  EXPECT_FALSE(Stacks().AddMembers(stack, Files({0, 1, 0, 2})).Changed());
  EXPECT_FALSE(Stacks().AddMembers(stack, {}).Changed());
  EXPECT_FALSE(Stacks().SetCover(stack, files[0]).Changed());
  const auto regroup = Stacks().CreateStack(Files({1, 0, 2, 1}), files[1]);
  EXPECT_FALSE(regroup.Changed());
  EXPECT_EQ(regroup.stack_id, stack);
  EXPECT_EQ(StackRows(), rows);
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({2, 0, 1}));
}

TEST_F(PhotoStackStoreTest, NewStackMovesOnlySelectedFilesAndKeepsTheirExplicitOrder) {
  const auto source = Create({0, 1, 2}, 0);
  // Equal source/selection sizes do not mean the selection is the complete source stack.
  const auto target = Create({1, 5, 4}, 5);
  EXPECT_NE(source, target);
  EXPECT_EQ(Stacks().ReadMemberIds(source), Files({0, 2}));
  EXPECT_EQ(Stacks().ReadMemberIds(target), Files({1, 5, 4}));
  EXPECT_EQ(Cover(target), files[5]);
  EXPECT_TRUE(Stacks().RemoveMembers(Files({0})).Changed());
  EXPECT_THROW(Stacks().ReadMemberIds(source), std::invalid_argument);
  EXPECT_FALSE(Stacks().RemoveMembers(Files({2})).Changed());
  EXPECT_TRUE(Stacks().UngroupStack(target).Changed());
  EXPECT_EQ(Number(duckorm::expr::raw("SELECT COUNT(*) FROM PhotoStackMember")), 0);
  EXPECT_EQ(Number(duckorm::expr::raw("SELECT COUNT(*) FROM FileImage")), 8);
}

TEST_F(PhotoStackStoreTest, RemoveCoverPromotesEarliestRemainingSavedMemberAndKeepsOrderGaps) {
  const auto stack = Create({0, 1, 2, 3}, 0);
  EXPECT_TRUE(Stacks().RemoveMembers(Files({0, 2, 0})).Changed());
  EXPECT_EQ(Cover(stack), files[1]);
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({1, 3}));
  EXPECT_TRUE(Stacks().MoveMember(files[3], PhotoStackMove::kEarlier).Changed());
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({3, 1}));
  EXPECT_TRUE(Stacks().AddMembers(stack, Files({4, 4, 5})).Changed());
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({3, 1, 4, 5}));
  EXPECT_EQ(Cover(stack), files[1]);
  Reopen();
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({3, 1, 4, 5}));
}

TEST_F(PhotoStackStoreTest, MalformedUnknownAndDuplicateStackIdsAreRejectedWithoutWrites) {
  const auto stack = Create({0, 1}, 0);
  const auto rows  = StackRows();
  EXPECT_THROW(Stacks().ReadMemberIds("bad' OR TRUE"), std::invalid_argument);
  EXPECT_THROW(Stacks().UngroupStack("00000000-0000-0000-0000-000000000000"),
               std::invalid_argument);
  EXPECT_THROW(
      project->GetStorage()->GetPhotoStackStore().CreateStack(stack, Files({2, 3}), files[2]),
      std::invalid_argument);
  EXPECT_THROW(
      project->GetStorage()->GetPhotoStackStore().CreateStack("invalid", Files({2, 3}), files[2]),
      std::invalid_argument);
  EXPECT_THROW(Stacks().CreateStack(Files({2, 2}), files[2]), std::invalid_argument);
  EXPECT_THROW(Stacks().MoveMember(files[0], static_cast<PhotoStackMove>(99)),
               std::invalid_argument);
  EXPECT_THROW(Stacks().CreateStack({}, files[0]), std::invalid_argument);
  EXPECT_THROW(PhotoStackService(nullptr), std::invalid_argument);
  EXPECT_EQ(StackRows(), rows);
}

TEST_F(PhotoStackStoreTest, DamagedCoverIsReportedWithoutImplicitRepair) {
  const auto stack = Create({0, 1, 2}, 0);
  Execute(std::format("UPDATE PhotoStack SET cover_file_id = {}", files[7]));
  const auto rows = StackRows();
  EXPECT_THROW(Stacks().ReadMemberIds(stack), std::runtime_error);
  EXPECT_THROW(Stacks().SetCover(stack, files[0]), std::runtime_error);
  EXPECT_THROW(Stacks().AddMembers(stack, Files({3})), std::runtime_error);
  EXPECT_EQ(StackRows(), rows);
  EXPECT_FALSE(test::FindStorageInvariantViolations(*project->GetStorage()).empty());
  Execute(std::format("UPDATE PhotoStack SET cover_file_id = {}", files[0]));
}

TEST_F(PhotoStackStoreTest, MissingBindingAndFolderTargetsAreRejectedBeforeMembershipWrites) {
  const auto stack = Create({0, 1}, 0);
  const auto rows  = StackRows();
  const auto image =
      project->GetAlbumBrowseService()->ReadAlbumFileRows(Files({2})).at(0).image_id_;
  Execute(std::format("DELETE FROM FileImage WHERE file_id = {}", files[2]));
  EXPECT_THROW(Stacks().AddMembers(stack, Files({2})), std::invalid_argument);
  EXPECT_EQ(StackRows(), rows);
  Execute(std::format("INSERT INTO FileImage VALUES ({}, {})", files[2], image));
  const std::vector<sl_element_id_t> with_folder{files[2], 0};
  EXPECT_THROW(Stacks().CreateStack(with_folder, files[2]), std::invalid_argument);
  EXPECT_EQ(StackRows(), rows);
  EXPECT_FALSE(Stacks().RemoveMembers({}).Changed());
  EXPECT_FALSE(Stacks().MoveMember(files[7], PhotoStackMove::kEarlier).Changed());
}

TEST_F(PhotoStackStoreTest, ExhaustedMemberOrderRejectsAdditionWithoutRemovingSourceMembership) {
  const auto target = Create({0, 1}, 0);
  const auto source = Create({2, 3}, 2);
  Execute(std::format("UPDATE PhotoStackMember SET member_order = {} WHERE file_id = {}",
                      std::numeric_limits<std::int64_t>::max(), files[1]));
  const auto rows = StackRows();
  EXPECT_THROW(Stacks().AddMembers(target, Files({2})), std::invalid_argument);
  EXPECT_EQ(StackRows(), rows);
  EXPECT_EQ(Stacks().ReadMemberIds(source), Files({2, 3}));
}

TEST_F(PhotoStackStoreTest, DamagedMemberBindingIsReportedBeforeAnyAffectedStackWrite) {
  const auto stack = Create({0, 1, 2}, 0);
  const auto image =
      project->GetAlbumBrowseService()->ReadAlbumFileRows(Files({1})).at(0).image_id_;
  Execute(std::format("DELETE FROM FileImage WHERE file_id = {}", files[1]));
  const auto rows = StackRows();
  EXPECT_THROW(Stacks().ReadMemberIds(stack), std::runtime_error);
  EXPECT_THROW(Stacks().AddMembers(stack, Files({3})), std::runtime_error);
  EXPECT_THROW(Stacks().CreateStack(Files({0, 3}), files[3]), std::runtime_error);
  EXPECT_EQ(StackRows(), rows);
  EXPECT_FALSE(test::FindStorageInvariantViolations(*project->GetStorage()).empty());
  Execute(std::format("INSERT INTO FileImage VALUES ({}, {})", files[1], image));
  // One valid binding plus one dangling binding is also damaged, not a valid single binding.
  const auto missing_image = std::numeric_limits<image_id_t>::max();
  Execute(std::format("INSERT INTO FileImage VALUES ({}, {})", files[1], missing_image));
  EXPECT_THROW(Stacks().ReadMemberIds(stack), std::runtime_error);
  EXPECT_THROW(Stacks().AddMembers(stack, Files({3})), std::runtime_error);
  EXPECT_EQ(StackRows(), rows);
  const auto violations = test::FindStorageInvariantViolations(*project->GetStorage());
  EXPECT_TRUE(std::any_of(violations.begin(), violations.end(), [](const auto& violation) {
    return violation.rule == "stack member names a live File with an image binding";
  }));
  Execute(std::format("DELETE FROM FileImage WHERE file_id = {} AND image_id = {}", files[1],
                      missing_image));
}

TEST_F(PhotoStackStoreTest, StackSchemaRejectsDuplicateMembershipAndInvalidPositions) {
  const auto stack = Create({0, 1}, 0);
  const auto other = Create({2, 3}, 2);
  const auto rows  = StackRows();
  EXPECT_THROW(
      Execute(std::format("INSERT INTO PhotoStackMember VALUES ({}, '{}', 2)", files[0], other)),
      duckorm::StatementError);
  EXPECT_THROW(
      Execute(std::format("INSERT INTO PhotoStackMember VALUES ({}, '{}', 0)", files[4], stack)),
      duckorm::StatementError);
  EXPECT_THROW(
      Execute(std::format("INSERT INTO PhotoStackMember VALUES ({}, '{}', -1)", files[4], stack)),
      duckorm::StatementError);
  EXPECT_EQ(StackRows(), rows);
}

}  // namespace
}  // namespace alcedo
