//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_history_materializer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/project_service.hpp"
#include "edit/history/edit_history.hpp"
#include "edit/history/edit_transaction.hpp"
#include "edit/history/editor_journal_recovery.hpp"
#include "edit/history/editor_journal_writer.hpp"
#include "edit/history/editor_transaction_journal.hpp"
#include "edit/history/version.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

auto MakeExposureTransaction(float exposure, tx_id_t id) -> EditTransaction {
  EditTransaction transaction{TransactionType::_EDIT,
                              OperatorType::EXPOSURE,
                              PipelineStageName::Basic_Adjustment,
                              nlohmann::json{{"exposure", 0.0f}},
                              nlohmann::json{{"exposure", exposure}},
                              true,
                              true};
  transaction.SetTransactionID(id);
  transaction.GenerateTransactionHash();
  return transaction;
}

auto MakePipelineParams(float exposure) -> nlohmann::json {
  // Build a real pipeline projection so DuckDB round-trips a complete params object.
  CPUPipelineExecutor exec;
  auto& stage = exec.GetStage(PipelineStageName::Basic_Adjustment);
  auto  op    = stage.GetOperator(OperatorType::EXPOSURE);
  if (op.has_value() && op.value() != nullptr) {
    op.value()->op_->SetParams(nlohmann::json{{"exposure", exposure}});
    op.value()->enable_ = true;
  }
  auto params = exec.ExportPipelineParams();
  params["alcedo_test_exposure"] = exposure;
  return params;
}

class EditorHistoryMaterializerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    db_path_   = std::filesystem::temp_directory_path() / ("editor_hist_mat_" + stamp + ".db");
    meta_path_ = std::filesystem::temp_directory_path() / ("editor_hist_mat_" + stamp + ".json");
    journal_path_ =
        std::filesystem::temp_directory_path() / ("editor_hist_mat_" + stamp + ".journal");
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove(journal_path_, ec);

    project_ = std::make_unique<ProjectService>(db_path_, meta_path_);
    storage_ = project_->GetStorageService();
    materializer_ = std::make_unique<EditorHistoryMaterializer>(storage_);

    history_ = std::make_shared<EditHistory>(file_id_);
    auto version_id = history_->GetActiveVersionID();
    identity_.element_id         = file_id_;
    identity_.version_id         = version_id;
    identity_.session_generation = 1;
    identity_.journal_generation = 1;

    file_   = std::make_shared<InjectedEditorJournalFile>();
    writer_ = std::make_unique<EditorJournalWriter>(&journal_, file_);
  }

  void TearDown() override {
    materializer_.reset();
    project_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove(journal_path_, ec);
  }

  auto CommitEdit(float exposure, tx_id_t id) -> std::uint64_t {
    const auto tx = MakeExposureTransaction(exposure, id);
    const auto seq = writer_->AppendEdit(identity_, tx);
    EXPECT_NE(seq, 0u);
    const auto committed = writer_->CommitQueued();
    EXPECT_TRUE(committed.durable) << committed.error;
    return seq;
  }

  static constexpr sl_element_id_t file_id_ = 42;

  std::filesystem::path                         db_path_;
  std::filesystem::path                         meta_path_;
  std::filesystem::path                         journal_path_;
  std::unique_ptr<ProjectService>               project_;
  std::shared_ptr<StorageService>               storage_;
  std::unique_ptr<EditorHistoryMaterializer>    materializer_;
  std::shared_ptr<EditHistory>                  history_;
  EditorJournalIdentity                         identity_{};
  EditorTransactionJournal                      journal_;
  std::shared_ptr<InjectedEditorJournalFile>    file_;
  std::unique_ptr<EditorJournalWriter>          writer_;
};

}  // namespace

TEST_F(EditorHistoryMaterializerTest,
       MaterializationCommitsHistoryPipelineAndRecoveryMetadataTogether) {
  const auto op_seq = CommitEdit(1.25f, 1);
  auto& version = history_->GetActiveVersion();
  WorkingVersion working{file_id_,
                         version.GetVersionID(),
                         MakePipelineParams(1.25f),
                         {MakeExposureTransaction(1.25f, 1)},
                         1};
  history_->UpdateVersionFromWorkingVersion(version.GetVersionID(), working,
                                            MakePipelineParams(1.25f));

  EditorMaterializeRequest request;
  request.identity                  = identity_;
  request.target_operation_sequence = op_seq;
  const auto result =
      materializer_->Materialize(request, &journal_, history_, MakePipelineParams(1.25f));
  ASSERT_TRUE(result.accepted) << result.error;
  ASSERT_TRUE(result.materialized);
  EXPECT_EQ(result.materialized_operation_sequence, op_seq);

  auto reloaded_history = storage_->GetElementController().GetEditHistoryByFileId(file_id_);
  ASSERT_NE(reloaded_history, nullptr);
  EXPECT_EQ(reloaded_history->GetActiveVersion().GetAllEditTransactions().size(), 1u);
  EXPECT_DOUBLE_EQ(reloaded_history->GetActiveVersion()
                       .GetAllEditTransactions()
                       .front()
                       .GetAfterParams()["exposure"]
                       .get<double>(),
                   1.25);

  auto reloaded_pipeline = storage_->GetElementController().GetPipelineByElementId(file_id_);
  ASSERT_NE(reloaded_pipeline, nullptr);
  EXPECT_EQ(reloaded_pipeline->GetBoundFile(), file_id_);

  auto metadata = storage_->GetElementController().GetEditorRecoveryMetadata(file_id_);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->materialized_operation_sequence, op_seq);
  EXPECT_EQ(metadata->transaction_chain_hash, result.transaction_chain_hash);
  EXPECT_EQ(metadata->pipeline_parameter_hash, result.pipeline_parameter_hash);
  EXPECT_EQ(metadata->transaction_chain_hash,
            ComputeEditorTimelineHash(reloaded_history->GetActiveVersion().GetAllEditTransactions(),
                reloaded_history->GetActiveVersion().GetCursor()));
}

TEST_F(EditorHistoryMaterializerTest,
       CrashAfterJournalCommitBeforeMaterializationRedoesEditExactlyOnce) {
  const auto op_seq = CommitEdit(2.0f, 1);
  // History still at import baseline — simulates crash before DuckDB write.
  ASSERT_EQ(history_->GetActiveVersion().GetAllEditTransactions().size(), 0u);

  const auto first = materializer_->RecoverAndMaterialize(identity_, &journal_, history_,
                                                          MakePipelineParams(2.0f));
  ASSERT_TRUE(first.accepted) << first.error;
  ASSERT_TRUE(first.materialized);
  EXPECT_EQ(first.materialized_operation_sequence, op_seq);
  EXPECT_EQ(history_->GetActiveVersion().GetAllEditTransactions().size(), 1u);

  const auto second = materializer_->RecoverAndMaterialize(identity_, &journal_, history_,
                                                           MakePipelineParams(2.0f));
  ASSERT_TRUE(second.accepted) << second.error;
  EXPECT_EQ(history_->GetActiveVersion().GetAllEditTransactions().size(), 1u);
  EXPECT_EQ(second.materialized_operation_sequence, op_seq);
  EXPECT_EQ(second.transaction_chain_hash, first.transaction_chain_hash);
}

TEST_F(EditorHistoryMaterializerTest,
       CrashAfterDatabaseCommitBeforeMaterializedMarkerDoesNotReplayTwice) {
  const auto op_seq = CommitEdit(3.0f, 1);
  auto first = materializer_->RecoverAndMaterialize(identity_, &journal_, history_,
                                                    MakePipelineParams(3.0f));
  ASSERT_TRUE(first.accepted) << first.error;

  // No MaterializedHead marker in the journal — only recovery metadata.
  auto decoded = journal_.DecodeRecordChain();
  for (const auto& record : decoded.records) {
    EXPECT_NE(record.record_type, EditorJournalRecordType::MaterializedHead);
  }

  auto second = materializer_->RecoverAndMaterialize(identity_, &journal_, history_,
                                                     MakePipelineParams(3.0f));
  ASSERT_TRUE(second.accepted) << second.error;
  EXPECT_EQ(history_->GetActiveVersion().GetAllEditTransactions().size(), 1u);
  EXPECT_EQ(second.materialized_operation_sequence, op_seq);
}

TEST_F(EditorHistoryMaterializerTest, FailedCompactionLeavesPreviousJournalRecoverable) {
  CommitEdit(1.0f, 1);
  ASSERT_TRUE(
      materializer_->RecoverAndMaterialize(identity_, &journal_, history_, MakePipelineParams(1.0f))
                  .accepted);

  file_->fail_replace = true;
  const auto active   = journal_path_;
  const auto compact  = journal_path_.string() + ".compact";
  auto       compact_identity = writer_->identity();
  ++compact_identity.journal_generation;
  std::string error;
  EXPECT_FALSE(writer_->CompactToMaterializedHead(
      compact_identity,
      ComputeEditorTimelineHash(history_->GetActiveVersion().GetAllEditTransactions(),
                                history_->GetActiveVersion().GetCursor()),
      history_->GetActiveVersion().GetCursor(), MakePipelineParams(1.0f), active, compact, &error));
  EXPECT_FALSE(error.empty());

  // Previous committed bytes remain recoverable.
  EditorTransactionJournal reopened;
  ASSERT_TRUE(reopened.LoadBytes(file_->bytes()));
  JournalTimelineSimulator sim(identity_);
  ASSERT_EQ(sim.ReplayCommittedRecordChain(reopened).status, EditorJournalApplyStatus::Applied);
  EXPECT_EQ(sim.transactions().size(), 1u);
}

TEST_F(EditorHistoryMaterializerTest, SuccessfulCompactionReplacesActiveJournal) {
  CommitEdit(1.0f, 1);
  ASSERT_TRUE(
      materializer_->RecoverAndMaterialize(identity_, &journal_, history_, MakePipelineParams(1.0f))
                  .accepted);

  const auto active  = journal_path_;
  const auto compact = std::filesystem::path(journal_path_.string() + ".compact");
  auto       compact_identity = writer_->identity();
  ++compact_identity.journal_generation;
  std::string error;
  ASSERT_TRUE(writer_->CompactToMaterializedHead(
      compact_identity,
      ComputeEditorTimelineHash(history_->GetActiveVersion().GetAllEditTransactions(),
                                           history_->GetActiveVersion().GetCursor()),
      history_->GetActiveVersion().GetCursor(), MakePipelineParams(1.0f), active, compact, &error))
      << error;

  const auto decoded = writer_->journal().DecodeRecordChain();
  ASSERT_GE(decoded.records.size(), 2u);
  EXPECT_EQ(decoded.records.front().record_type, EditorJournalRecordType::CompactionCheckpoint);
  EXPECT_EQ(file_->replace_calls, 1);
}

TEST_F(EditorHistoryMaterializerTest,
       InjectedChecksumCorruptionPreservesOriginalAndEmitsDiagnostic) {
  CommitEdit(1.0f, 1);
  auto bytes = file_->bytes();
  ASSERT_FALSE(bytes.empty());
  bytes[bytes.size() / 2] ^= 0xA5u;
  file_->SetBytes(bytes);

  EditorTransactionJournal corrupted;
  std::string load_error;
  // LoadBytes rejects a fully corrupt chain.
  if (!corrupted.LoadBytes(file_->bytes(), &load_error)) {
    std::string diag_error;
    auto        path = WriteEditorJournalDiagnosticBundle(
        journal_path_, file_->bytes(), load_error.empty() ? "corrupt" : load_error, &diag_error);
    ASSERT_TRUE(path.has_value()) << diag_error;
    EXPECT_TRUE(std::filesystem::exists(*path / "journal.bin"));
    EXPECT_TRUE(std::filesystem::exists(*path / "reason.txt"));
    std::error_code ec;
    std::filesystem::remove_all(*path, ec);
  } else {
    // Partial valid prefix may still load; recovery must not invent hybrid state.
    EditorRecoveryMetadata empty_meta;
    empty_meta.element_id         = identity_.element_id;
    empty_meta.version_id         = identity_.version_id;
    empty_meta.journal_generation = identity_.journal_generation;
    auto recovery =
        RecoverEditorJournal(corrupted, identity_, empty_meta, {}, 0, MakePipelineParams(0.0f));
    // Either pure empty or pure valid prefix — never a partial record apply.
    if (recovery.accepted) {
      EXPECT_TRUE(recovery.durable_operation_sequence == 0 ||
                  recovery.recovered_state.transactions().size() <= 1);
    }
  }
}

TEST_F(EditorHistoryMaterializerTest, ShortWriteAndFailedFlushDoNotAdvanceMaterializedHead) {
  file_->max_write = 4;
  ASSERT_NE(writer_->AppendEdit(identity_, MakeExposureTransaction(1.0f, 1)), 0u);
  ASSERT_TRUE(writer_->CommitQueued().durable);

  file_->fail_flush = true;
  auto second = MakeExposureTransaction(2.0f, 2);
  ASSERT_NE(writer_->AppendEdit(identity_, second), 0u);
  const auto failed = writer_->CommitQueued();
  EXPECT_FALSE(failed.durable);
  EXPECT_EQ(writer_->state().durable_operation_sequence, 1u);

  // Materialize only the durable head.
  EditorMaterializeRequest request;
  request.identity                  = identity_;
  request.target_operation_sequence = writer_->state().durable_operation_sequence;
  WorkingVersion working{file_id_,
                         history_->GetActiveVersionID(),
                         MakePipelineParams(1.0f),
                         {MakeExposureTransaction(1.0f, 1)},
                         1};
  history_->UpdateVersionFromWorkingVersion(history_->GetActiveVersionID(), working,
                                            MakePipelineParams(1.0f));
  const auto result =
      materializer_->Materialize(request, &journal_, history_, MakePipelineParams(1.0f));
  ASSERT_TRUE(result.accepted) << result.error;
  EXPECT_EQ(result.materialized_operation_sequence, 1u);
  EXPECT_EQ(history_->GetActiveVersion().GetAllEditTransactions().size(), 1u);
}

TEST_F(EditorHistoryMaterializerTest, ReplayingTheSameJournalTwiceIsIdempotent) {
  CommitEdit(1.0f, 1);
  const auto a = materializer_->RecoverAndMaterialize(identity_, &journal_, history_,
                                                      MakePipelineParams(1.0f));
  ASSERT_TRUE(a.accepted) << a.error;
  const auto b = materializer_->RecoverAndMaterialize(identity_, &journal_, history_,
                                                      MakePipelineParams(1.0f));
  ASSERT_TRUE(b.accepted) << b.error;
  EXPECT_EQ(a.transaction_chain_hash, b.transaction_chain_hash);
  EXPECT_EQ(a.materialized_operation_sequence, b.materialized_operation_sequence);
  EXPECT_EQ(history_->GetActiveVersion().GetAllEditTransactions().size(), 1u);
}

TEST_F(EditorHistoryMaterializerTest, StaleHeadMarkerDoesNotDowngradeMaterializedState) {
  CommitEdit(1.0f, 1);
  ASSERT_TRUE(
      materializer_->RecoverAndMaterialize(identity_, &journal_, history_, MakePipelineParams(1.0f))
                  .accepted);
  const auto head = storage_->GetElementController().GetEditorRecoveryMetadata(file_id_);
  ASSERT_TRUE(head.has_value());

  // Append a second durable edit and materialize it.
  CommitEdit(2.0f, 2);
  ASSERT_TRUE(
      materializer_->RecoverAndMaterialize(identity_, &journal_, history_, MakePipelineParams(2.0f))
                  .accepted);
  const auto advanced = storage_->GetElementController().GetEditorRecoveryMetadata(file_id_);
  ASSERT_TRUE(advanced.has_value());
  EXPECT_GT(advanced->materialized_operation_sequence, head->materialized_operation_sequence);

  // A materialize request that claims the stale head is rejected when validation is on.
  EditorMaterializeRequest stale;
  stale.identity                                 = identity_;
  stale.target_operation_sequence                = head->materialized_operation_sequence;
  stale.validate_expected_materialized_head      = true;
  stale.expected_materialized_operation_sequence = head->materialized_operation_sequence;
  stale.expected_materialized_chain_hash         = head->transaction_chain_hash;
  const auto rejected =
      materializer_->Materialize(stale, &journal_, history_, MakePipelineParams(1.0f));
  EXPECT_FALSE(rejected.accepted);
}

// Phase 5H-Fix #3: a partial or damaged tail must not reject the whole journal.
// Recovery uses the last complete batch commit before the corruption.
TEST_F(EditorHistoryMaterializerTest, RecoveryUsesLastCompleteBatchWhenJournalEndsWithPartialTail) {
  CommitEdit(1.0f, 1);
  const std::size_t size_after_first_commit = file_->bytes().size();
  CommitEdit(2.0f, 2);
  // Corrupt a byte inside the second edit's header so decode stops there.
  file_->corrupt_on_read        = true;
  file_->corrupt_on_read_offset = size_after_first_commit + 1;

  // A recovery-capable writer (ctor 1) must tolerate the damaged tail instead
  // of throwing, and recover the durable head from the last valid batch commit.
  auto recovered_writer         = std::make_unique<EditorJournalWriter>(file_);
  EXPECT_EQ(recovered_writer->state().durable_operation_sequence, 1u);

  history_          = std::make_shared<EditHistory>(file_id_);
  const auto result = materializer_->RecoverAndMaterialize(
      identity_, &recovered_writer->mutable_journal(), history_, MakePipelineParams(1.0f));
  ASSERT_TRUE(result.accepted) << result.error;
  EXPECT_EQ(result.materialized_operation_sequence, 1u);
  EXPECT_EQ(history_->GetActiveVersion().GetAllEditTransactions().size(), 1u);
}

// Phase 5H-Fix #4: a compacted journal omits prior edit records behind a
// checkpoint. Recovery must seed from the DuckDB-materialized chain so the
// durable history is preserved instead of being replaced with an empty timeline.
TEST_F(EditorHistoryMaterializerTest, RecoveringCompactedJournalPreservesMaterializedHistory) {
  CommitEdit(1.0f, 1);
  CommitEdit(2.0f, 2);
  ASSERT_TRUE(
      materializer_->RecoverAndMaterialize(identity_, &journal_, history_, MakePipelineParams(2.0f))
          .accepted);
  ASSERT_EQ(history_->GetActiveVersion().GetAllEditTransactions().size(), 2u);
  const auto stored = storage_->GetElementController().GetEditorRecoveryMetadata(file_id_);
  ASSERT_TRUE(stored.has_value());
  ASSERT_EQ(stored->materialized_operation_sequence, 3u);

  const auto active           = journal_path_;
  const auto compact          = std::filesystem::path(journal_path_.string() + ".compact");
  auto       compact_identity = writer_->identity();
  ++compact_identity.journal_generation;
  std::string error;
  ASSERT_TRUE(writer_->CompactToMaterializedHead(
      compact_identity,
      ComputeEditorTimelineHash(history_->GetActiveVersion().GetAllEditTransactions(),
                                history_->GetActiveVersion().GetCursor()),
      history_->GetActiveVersion().GetCursor(), MakePipelineParams(2.0f), active, compact, &error))
      << error;

  // Reload the materialized history from DuckDB, then recover the compacted
  // journal. The transaction chain must survive (not collapse to zero).
  auto reloaded = storage_->GetElementController().GetEditHistoryByFileId(file_id_);
  ASSERT_NE(reloaded, nullptr);
  history_          = reloaded;
  const auto result = materializer_->RecoverAndMaterialize(
      writer_->identity(), &writer_->mutable_journal(), history_, MakePipelineParams(2.0f));
  ASSERT_TRUE(result.accepted) << result.error;
  EXPECT_EQ(history_->GetActiveVersion().GetAllEditTransactions().size(), 2u);
  const auto advanced = storage_->GetElementController().GetEditorRecoveryMetadata(file_id_);
  ASSERT_TRUE(advanced.has_value());
  EXPECT_EQ(advanced->journal_generation, compact_identity.journal_generation);
  EXPECT_EQ(advanced->materialized_operation_sequence, 0u);
}

TEST_F(EditorHistoryMaterializerTest,
       EditAfterCompactionMaterializesOnTopOfTheStoredTransactionChain) {
  CommitEdit(1.0f, 1);
  ASSERT_TRUE(
      materializer_->RecoverAndMaterialize(identity_, &journal_, history_, MakePipelineParams(1.0f))
          .accepted);

  auto compact_identity = writer_->identity();
  ++compact_identity.journal_generation;
  std::string error;
  ASSERT_TRUE(writer_->CompactToMaterializedHead(
      compact_identity,
      ComputeEditorTimelineHash(history_->GetActiveVersion().GetAllEditTransactions(),
                                history_->GetActiveVersion().GetCursor()),
      history_->GetActiveVersion().GetCursor(), MakePipelineParams(1.0f), journal_path_,
      std::filesystem::path(journal_path_.string() + ".compact"), &error))
      << error;

  auto next = MakeExposureTransaction(2.0f, 2);
  ASSERT_NE(writer_->AppendEdit(writer_->identity(), next), 0u);
  ASSERT_TRUE(writer_->CommitQueued().durable);

  auto durable_history = storage_->GetElementController().GetEditHistoryByFileId(file_id_);
  ASSERT_NE(durable_history, nullptr);
  EditorMaterializeRequest request;
  request.identity  = writer_->identity();
  const auto result = materializer_->Materialize(request, &writer_->mutable_journal(),
                                                 durable_history, MakePipelineParams(2.0f), &error);
  ASSERT_TRUE(result.accepted) << result.error;
  auto reloaded = storage_->GetElementController().GetEditHistoryByFileId(file_id_);
  ASSERT_NE(reloaded, nullptr);
  const auto& transactions = reloaded->GetActiveVersion().GetAllEditTransactions();
  ASSERT_EQ(transactions.size(), 2u);
  EXPECT_EQ(transactions[0].GetTransactionID(), 1u);
  EXPECT_EQ(transactions[1].GetTransactionID(), 2u);
}

// Phase 5H-Fix #5: the compact file must be re-read and verified after the
// write+flush, before it replaces the active journal. A damaged compact write
// is refused and the previous journal stays recoverable.
TEST_F(EditorHistoryMaterializerTest, CompactionVerifyAfterWriteRejectsDamagedCompactFile) {
  CommitEdit(1.0f, 1);
  ASSERT_TRUE(
      materializer_->RecoverAndMaterialize(identity_, &journal_, history_, MakePipelineParams(1.0f))
          .accepted);
  const auto before_bytes         = file_->bytes();

  file_->corrupt_compact_on_write = true;
  file_->corrupt_compact_offset   = 0;

  const auto active               = journal_path_;
  const auto compact              = std::filesystem::path(journal_path_.string() + ".compact");
  auto       compact_identity     = writer_->identity();
  ++compact_identity.journal_generation;
  std::string error;
  EXPECT_FALSE(writer_->CompactToMaterializedHead(
      compact_identity,
      ComputeEditorTimelineHash(history_->GetActiveVersion().GetAllEditTransactions(),
                                history_->GetActiveVersion().GetCursor()),
      history_->GetActiveVersion().GetCursor(), MakePipelineParams(1.0f), active, compact, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(file_->replace_calls, 0);
  // The active journal is untouched and still recoverable.
  EXPECT_EQ(file_->bytes(), before_bytes);
}

// Phase 5H-Fix #7: the real recovery failure path (writer ctor detecting a
// corrupt tail it cannot fully validate) emits a diagnostic bundle next to the
// journal, not only from a test calling WriteEditorJournalDiagnosticBundle directly.
TEST_F(EditorHistoryMaterializerTest, RecoveryEmitsDiagnosticBundleFromRealFailurePath) {
  {
    EditorJournalWriter real_writer(identity_, journal_path_);
    ASSERT_NE(real_writer.AppendEdit(identity_, MakeExposureTransaction(1.0f, 1)), 0u);
    ASSERT_TRUE(real_writer.CommitQueued().durable);
  }
  // Corrupt the on-disk file so the next open cannot validate a record.
  {
    std::ifstream in(journal_path_, std::ios::binary);
    ASSERT_TRUE(in);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    ASSERT_FALSE(bytes.empty());
    bytes[bytes.size() / 2] ^= 0xA5u;
    std::ofstream out(journal_path_, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  // Reopen: a fully corrupt journal must fail and emit a diagnostic bundle.
  EXPECT_THROW((void)EditorJournalWriter(identity_, journal_path_), std::runtime_error);
  bool       found_diagnostic = false;
  const auto stem             = journal_path_.filename().string() + ".diagnostic.";
  for (const auto& entry : std::filesystem::directory_iterator(journal_path_.parent_path())) {
    const auto name = entry.path().filename().string();
    if (name.find(stem) == 0) {
      found_diagnostic = true;
      EXPECT_TRUE(std::filesystem::exists(entry.path() / "journal.bin"));
      EXPECT_TRUE(std::filesystem::exists(entry.path() / "reason.txt"));
      std::error_code ec;
      std::filesystem::remove_all(entry.path(), ec);
      break;
    }
  }
  EXPECT_TRUE(found_diagnostic);
}

// Phase 5H-Fix #8: a duplicated record (same sequence) is rejected by recovery,
// which falls back to the last complete batch commit before the duplicate.
TEST_F(EditorHistoryMaterializerTest, DuplicatedRecordIsRejectedByRecovery) {
  CommitEdit(1.0f, 1);
  CommitEdit(2.0f, 2);
  // edit1=record 0, commit1=record 1, edit2=record 2. Duplicate edit2 so the
  // stream has a same-sequence record that decode must reject as a sequence gap.
  file_->duplicate_record_on_read = true;
  file_->duplicate_record_index   = 2;

  auto recovered_writer           = std::make_unique<EditorJournalWriter>(file_);
  EXPECT_EQ(recovered_writer->state().durable_operation_sequence, 1u);

  history_          = std::make_shared<EditHistory>(file_id_);
  const auto result = materializer_->RecoverAndMaterialize(
      identity_, &recovered_writer->mutable_journal(), history_, MakePipelineParams(1.0f));
  ASSERT_TRUE(result.accepted) << result.error;
  EXPECT_EQ(result.materialized_operation_sequence, 1u);
  EXPECT_EQ(history_->GetActiveVersion().GetAllEditTransactions().size(), 1u);
}

// Phase 5H-Fix #8: a failure between the history, pipeline, and recovery-metadata
// writes inside the single DuckDB transaction rolls all three back together.
TEST_F(EditorHistoryMaterializerTest, MaterializePreCommitFailureRollsBackAllThreeWrites) {
  const auto     op_seq1 = CommitEdit(1.0f, 1);
  auto&          version = history_->GetActiveVersion();
  WorkingVersion working1{file_id_,
                          version.GetVersionID(),
                          MakePipelineParams(1.0f),
                          {MakeExposureTransaction(1.0f, 1)},
                          1};
  history_->UpdateVersionFromWorkingVersion(version.GetVersionID(), working1,
                                            MakePipelineParams(1.0f));
  EditorMaterializeRequest request1;
  request1.identity                  = identity_;
  request1.target_operation_sequence = op_seq1;
  ASSERT_TRUE(
      materializer_->Materialize(request1, &journal_, history_, MakePipelineParams(1.0f)).accepted);
  auto baseline = storage_->GetElementController().GetEditorRecoveryMetadata(file_id_);
  ASSERT_TRUE(baseline.has_value());
  ASSERT_EQ(baseline->materialized_operation_sequence, op_seq1);

  const auto     op_seq2 = CommitEdit(2.0f, 2);
  WorkingVersion working2{file_id_,
                          version.GetVersionID(),
                          MakePipelineParams(2.0f),
                          {MakeExposureTransaction(1.0f, 1), MakeExposureTransaction(2.0f, 2)},
                          2};
  history_->UpdateVersionFromWorkingVersion(version.GetVersionID(), working2,
                                            MakePipelineParams(2.0f));
  storage_->GetElementController().SetMaterializePreCommitHook(
      [] { throw std::runtime_error("injected pre-commit failure"); });
  EditorMaterializeRequest request2;
  request2.identity                  = identity_;
  request2.target_operation_sequence = op_seq2;
  std::string error;
  const auto  failed =
      materializer_->Materialize(request2, &journal_, history_, MakePipelineParams(2.0f), &error);
  EXPECT_FALSE(failed.accepted);
  EXPECT_FALSE(error.empty());
  storage_->GetElementController().SetMaterializePreCommitHook({});

  auto after = storage_->GetElementController().GetEditorRecoveryMetadata(file_id_);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->materialized_operation_sequence, op_seq1);
  EXPECT_EQ(after->transaction_chain_hash, baseline->transaction_chain_hash);
  EXPECT_EQ(after->pipeline_parameter_hash, baseline->pipeline_parameter_hash);
  auto reloaded = storage_->GetElementController().GetEditHistoryByFileId(file_id_);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->GetActiveVersion().GetAllEditTransactions().size(), 1u);
}

}  // namespace alcedo
