//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_journal_writer_port.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "app/editor_session_bootstrap.hpp"
#include "app/project_service.hpp"
#include "edit/history/edit_transaction.hpp"
#include "edit/history/editor_journal_writer.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "type/type.hpp"

namespace alcedo::ui {
namespace {

auto MakeExposureTransaction(float exposure, tx_id_t id) -> alcedo::EditTransaction {
  alcedo::EditTransaction transaction{alcedo::TransactionType::_EDIT,
                                      alcedo::OperatorType::EXPOSURE,
                                      alcedo::PipelineStageName::Basic_Adjustment,
                                      nlohmann::json{{"exposure", 0.0f}},
                                      nlohmann::json{{"exposure", exposure}},
                                      true,
                                      true};
  transaction.SetTransactionID(id);
  transaction.GenerateTransactionHash();
  return transaction;
}

class EditorSessionJournalWriterPortTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path();
    db_path_        = temp / ("session_writer_" + stamp + ".db");
    meta_path_      = temp / ("session_writer_" + stamp + ".json");
    journal_dir_    = temp / ("session_writer_" + stamp);
    std::error_code ec;
    std::filesystem::create_directories(journal_dir_, ec);
    project_ = std::make_unique<alcedo::ProjectService>(db_path_, meta_path_);
    port_    = std::make_shared<EditorSessionJournalWriterPort>(
        EditorSessionJournalWriterPort::Services{[this](sl_element_id_t id) {
          return journal_dir_ /
                 ("image-" + std::to_string(static_cast<std::uint64_t>(id)) + ".wal");
        }});
  }

  void TearDown() override {
    port_.reset();
    project_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove_all(journal_dir_, ec);
  }

  std::filesystem::path                           db_path_;
  std::filesystem::path                           meta_path_;
  std::filesystem::path                           journal_dir_;
  std::unique_ptr<alcedo::ProjectService>         project_;
  std::shared_ptr<EditorSessionJournalWriterPort> port_;
};

TEST_F(EditorSessionJournalWriterPortTest, FinalizedEditIsDurablyAppended) {
  std::string error;
  ASSERT_TRUE(port_->RecordEdit(42, 1, MakeExposureTransaction(1.5f, 1), &error)) << error;
  const auto committed = port_->CommitJournal(42, 1, &error);
  ASSERT_TRUE(committed.accepted) << committed.error;
  EXPECT_TRUE(committed.durable);
  EXPECT_GT(committed.durable_operation_sequence, 0u);
}

TEST_F(EditorSessionJournalWriterPortTest, CursorAndTimelineOperationsPreserveOrder) {
  std::string error;
  const auto  first       = MakeExposureTransaction(1.0f, 1);
  const auto  replacement = MakeExposureTransaction(2.0f, 2);
  ASSERT_TRUE(port_->RecordEdit(42, 1, first, &error)) << error;
  ASSERT_TRUE(port_->RecordCursorMove(42, 1, 1, 0, &error)) << error;
  ASSERT_TRUE(port_->RecordRewriteTimeline(42, 1, alcedo::Hash128{}, alcedo::Hash128{}, 0,
                                           replacement, &error))
      << error;
  ASSERT_TRUE(port_->CommitJournal(42, 1, &error).durable) << error;

  alcedo::EditorJournalWriter reopened({42, {}, 1, 1}, journal_dir_ / "image-42.wal");
  const auto                  decoded = reopened.journal().DecodeRecordChain();
  ASSERT_GE(decoded.records.size(), 4u);
  EXPECT_EQ(decoded.records[0].record_type, alcedo::EditorJournalRecordType::EditAppend);
  EXPECT_EQ(decoded.records[1].record_type, alcedo::EditorJournalRecordType::CursorMove);
  EXPECT_EQ(decoded.records[2].record_type, alcedo::EditorJournalRecordType::RewriteTimeline);
  EXPECT_EQ(decoded.records[3].record_type, alcedo::EditorJournalRecordType::JournalBatchCommit);
}

TEST_F(EditorSessionJournalWriterPortTest, DiscardRemovesQueuedRecordsBeforeDurability) {
  std::string error;
  ASSERT_TRUE(port_->RecordEdit(42, 1, MakeExposureTransaction(1.5f, 1), &error));
  ASSERT_TRUE(port_->DiscardUnflushed(42, &error)) << error;
  const auto committed = port_->CommitJournal(42, 1, &error);
  EXPECT_TRUE(committed.accepted);
  EXPECT_EQ(committed.durable_operation_sequence, 0u);
}

}  // namespace
}  // namespace alcedo::ui
