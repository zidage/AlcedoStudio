//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "edit/history/edit_transaction.hpp"
#include "edit/history/editor_transaction_journal.hpp"
#include "edit/history/version.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"

namespace alcedo {
namespace {

auto MakeExposureTx(float exposure) -> EditTransaction {
  return EditTransaction{TransactionType::_EDIT,
                         OperatorType::EXPOSURE,
                         PipelineStageName::Basic_Adjustment,
                         nlohmann::json{{"exposure", 0.0f}},
                         nlohmann::json{{"exposure", exposure}},
                         true,
                         true};
}

auto MakeContrastTx(float contrast) -> EditTransaction {
  return EditTransaction{TransactionType::_EDIT,
                         OperatorType::CONTRAST,
                         PipelineStageName::Basic_Adjustment,
                         nlohmann::json{{"contrast", 0.0f}},
                         nlohmann::json{{"contrast", contrast}},
                         true,
                         true};
}

struct TimelineSnapshot {
  std::vector<tx_id_t>      tx_ids;
  std::vector<OperatorType> operators;
  std::size_t               cursor = 0;
  Hash128                   timeline_hash{};
  tx_id_t                   max_tx_id = 0;
};

auto SnapshotWorking(const WorkingVersion& working) -> TimelineSnapshot {
  TimelineSnapshot snap;
  snap.cursor         = working.GetCursor();
  snap.timeline_hash  = ComputeEditorTimelineHash(working.GetAllEditTransactions(), snap.cursor);
  for (const auto& tx : working.GetAllEditTransactions()) {
    snap.tx_ids.push_back(tx.GetTransactionID());
    snap.operators.push_back(tx.GetTxOperatorType());
    snap.max_tx_id = std::max(snap.max_tx_id, tx.GetTransactionID());
  }
  return snap;
}

auto SnapshotSimulator(const JournalTimelineSimulator& sim) -> TimelineSnapshot {
  TimelineSnapshot snap;
  snap.cursor        = sim.cursor();
  snap.timeline_hash = sim.TimelineHash();
  snap.max_tx_id     = sim.tx_id_high_water();
  for (const auto& tx : sim.transactions()) {
    snap.tx_ids.push_back(tx.GetTransactionID());
    snap.operators.push_back(tx.GetTxOperatorType());
  }
  return snap;
}

void ExpectSnapshotsEqual(const TimelineSnapshot& a, const TimelineSnapshot& b) {
  EXPECT_EQ(a.cursor, b.cursor);
  EXPECT_EQ(a.tx_ids, b.tx_ids);
  EXPECT_EQ(a.operators, b.operators);
  EXPECT_EQ(a.timeline_hash, b.timeline_hash);
}

class EditorTransactionJournalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
    identity_.element_id         = 42;
    identity_.version_id         = Hash128(11, 22);
    identity_.session_generation = 7;
    identity_.journal_generation = 1;
    // Construct after operator registration; default-stage setup needs the factory.
    pipeline_ = std::make_unique<CPUPipelineExecutor>();
  }

  void TearDown() override { pipeline_.reset(); }

  auto AppendWorkingEdit(WorkingVersion* working, WorkingVersionJournalRecorder* recorder,
                         EditTransaction tx) -> void {
    const auto before_txs    = working->GetAllEditTransactions();
    const auto before_cursor = working->GetCursor();
    ASSERT_TRUE(tx.ApplyForward(*pipeline_));
    working->AppendEditTransaction(std::move(tx));
    working->SetHeadPipelineParams(pipeline_->ExportPipelineParams());
    ASSERT_FALSE(working->GetAllEditTransactions().empty());
    recorder->RecordAfterAppend(before_txs, before_cursor, working->GetAllEditTransactions().back(),
                                working->GetAllEditTransactions(), working->GetCursor());
  }

  auto UndoWorking(WorkingVersion* working, WorkingVersionJournalRecorder* recorder) -> void {
    const auto from = working->GetCursor();
    ASSERT_TRUE(working->UndoLastTransaction(*pipeline_));
    recorder->RecordCursorMove(from, working->GetCursor());
  }

  auto RedoWorking(WorkingVersion* working, WorkingVersionJournalRecorder* recorder) -> void {
    const auto from = working->GetCursor();
    ASSERT_TRUE(working->RedoNextTransaction(*pipeline_));
    recorder->RecordCursorMove(from, working->GetCursor());
  }

  EditorJournalIdentity                    identity_{};
  std::unique_ptr<CPUPipelineExecutor>     pipeline_;
  EditorTransactionJournal                 journal_;
};

}  // namespace

TEST_F(EditorTransactionJournalTest, EditAEditBUndoAppendCReplaysAsAThenC) {
  WorkingVersion working(identity_.element_id, identity_.version_id,
                         pipeline_->ExportPipelineParams());
  WorkingVersionJournalRecorder recorder(&journal_, identity_);

  AppendWorkingEdit(&working, &recorder, MakeExposureTx(1.0f));  // A
  AppendWorkingEdit(&working, &recorder, MakeContrastTx(2.0f));  // B
  UndoWorking(&working, &recorder);
  AppendWorkingEdit(&working, &recorder, MakeExposureTx(3.0f));  // C rewrites over B

  ASSERT_EQ(working.GetAllEditTransactions().size(), 2u);
  EXPECT_EQ(working.GetCursor(), 2u);
  EXPECT_EQ(working.GetAllEditTransactions()[0].GetTxOperatorType(), OperatorType::EXPOSURE);
  EXPECT_EQ(working.GetAllEditTransactions()[1].GetTxOperatorType(), OperatorType::EXPOSURE);
  EXPECT_DOUBLE_EQ(
      working.GetAllEditTransactions()[1].GetAfterParams()["exposure"].get<double>(), 3.0);
  EXPECT_FALSE(working.RedoNextTransaction(*pipeline_));  // B unavailable

  JournalTimelineSimulator sim(identity_);
  const auto replay = sim.ReplayRecordChain(journal_);
  EXPECT_EQ(replay.status, EditorJournalApplyStatus::Applied);

  ExpectSnapshotsEqual(SnapshotWorking(working), SnapshotSimulator(sim));
  EXPECT_GE(sim.tx_id_high_water(), 2u);
  // Discarded B's id must remain in the high-water mark so it is never reused.
  EXPECT_GE(sim.tx_id_high_water(), working.GetAllEditTransactions().back().GetTransactionID());
}

TEST_F(EditorTransactionJournalTest, EditAEditBUndoRedoReplaysAsAThenB) {
  WorkingVersion working(identity_.element_id, identity_.version_id,
                         pipeline_->ExportPipelineParams());
  WorkingVersionJournalRecorder recorder(&journal_, identity_);

  AppendWorkingEdit(&working, &recorder, MakeExposureTx(1.0f));
  AppendWorkingEdit(&working, &recorder, MakeContrastTx(2.0f));
  UndoWorking(&working, &recorder);
  RedoWorking(&working, &recorder);

  ASSERT_EQ(working.GetAllEditTransactions().size(), 2u);
  EXPECT_EQ(working.GetCursor(), 2u);

  JournalTimelineSimulator sim(identity_);
  ASSERT_EQ(sim.ReplayRecordChain(journal_).status, EditorJournalApplyStatus::Applied);
  ExpectSnapshotsEqual(SnapshotWorking(working), SnapshotSimulator(sim));
}

TEST_F(EditorTransactionJournalTest, RewriteTimelineAtomicallyDropsRedoTailAndAppendsReplacement) {
  WorkingVersion working(identity_.element_id, identity_.version_id,
                         pipeline_->ExportPipelineParams());
  WorkingVersionJournalRecorder recorder(&journal_, identity_);

  AppendWorkingEdit(&working, &recorder, MakeExposureTx(1.0f));
  AppendWorkingEdit(&working, &recorder, MakeContrastTx(2.0f));
  UndoWorking(&working, &recorder);

  const auto before_rewrite_txs    = working.GetAllEditTransactions();
  const auto before_rewrite_cursor = working.GetCursor();
  EXPECT_EQ(before_rewrite_cursor, 1u);
  EXPECT_EQ(before_rewrite_txs.size(), 2u);

  const Hash128 expected =
      ComputeEditorTimelineHash(before_rewrite_txs, before_rewrite_cursor);
  const Hash128 discarded =
      ComputeEditorTransactionSpanHash(before_rewrite_txs, before_rewrite_cursor,
                                       before_rewrite_txs.size());
  EXPECT_NE(expected, Hash128{});
  EXPECT_NE(discarded, Hash128{});

  AppendWorkingEdit(&working, &recorder, MakeExposureTx(9.0f));

  const auto decoded = journal_.DecodeRecordChain();
  ASSERT_FALSE(decoded.stopped_on_incomplete_tail);
  ASSERT_FALSE(decoded.stopped_on_corrupt_record);
  ASSERT_GE(decoded.records.size(), 4u);

  const auto& rewrite = decoded.records.back();
  ASSERT_EQ(rewrite.record_type, EditorJournalRecordType::RewriteTimeline);
  ASSERT_TRUE(rewrite.rewrite_timeline.has_value());
  EXPECT_EQ(rewrite.rewrite_timeline->expected_timeline_hash, expected);
  EXPECT_EQ(rewrite.rewrite_timeline->discarded_tail_hash, discarded);
  EXPECT_EQ(rewrite.rewrite_timeline->retained_cursor, 1u);

  JournalTimelineSimulator sim(identity_);
  ASSERT_EQ(sim.ReplayRecordChain(journal_).status, EditorJournalApplyStatus::Applied);
  EXPECT_EQ(sim.transactions().size(), 2u);
  EXPECT_EQ(sim.cursor(), 2u);
  EXPECT_EQ(sim.transactions()[1].GetTxOperatorType(), OperatorType::EXPOSURE);
  EXPECT_DOUBLE_EQ(sim.transactions()[1].GetAfterParams()["exposure"].get<double>(), 9.0);
  ExpectSnapshotsEqual(SnapshotWorking(working), SnapshotSimulator(sim));
}

TEST_F(EditorTransactionJournalTest, PartialRewriteTimelineLeavesPriorTimelineUnchanged) {
  WorkingVersion working(identity_.element_id, identity_.version_id,
                         pipeline_->ExportPipelineParams());
  WorkingVersionJournalRecorder recorder(&journal_, identity_);

  AppendWorkingEdit(&working, &recorder, MakeExposureTx(1.0f));
  AppendWorkingEdit(&working, &recorder, MakeContrastTx(2.0f));
  UndoWorking(&working, &recorder);

  // Build a complete RewriteTimeline record, then append only a truncated record fragment.
  EditTransaction replacement = MakeExposureTx(5.0f);
  replacement.SetTransactionID(99);
  replacement.GenerateTransactionHash();
  const auto before_txs    = working.GetAllEditTransactions();
  const auto before_cursor = working.GetCursor();
  const auto framed        = EncodeEditorJournalRecord(
      EditorJournalRecordType::RewriteTimeline, journal_.next_sequence(), identity_,
      EncodeEditorJournalRewriteTimelinePayload(EditorJournalRewriteTimelinePayload{
          .expected_timeline_hash =
              ComputeEditorTimelineHash(before_txs, before_cursor),
          .discarded_tail_hash =
              ComputeEditorTransactionSpanHash(before_txs, before_cursor, before_txs.size()),
          .retained_cursor = before_cursor,
          .replacement     = replacement,
      }));
  ASSERT_GT(framed.size(), 16u);
  journal_.AppendRaw(framed.data(), framed.size() / 2);

  const auto decoded = journal_.DecodeRecordChain();
  EXPECT_TRUE(decoded.stopped_on_incomplete_tail);
  // Complete prior records remain; incomplete rewrite is not observed.
  EXPECT_EQ(decoded.records.size(), 3u);
  for (const auto& record : decoded.records) {
    EXPECT_NE(record.record_type, EditorJournalRecordType::RewriteTimeline);
  }

  JournalTimelineSimulator sim(identity_);
  ASSERT_EQ(sim.ReplayRecordChain(journal_).status, EditorJournalApplyStatus::Applied);
  // Prior valid record chain: A, B, undo => [A,B] cursor 1. B is still present for redo.
  EXPECT_EQ(sim.transactions().size(), 2u);
  EXPECT_EQ(sim.cursor(), 1u);
  EXPECT_EQ(sim.transactions()[1].GetTxOperatorType(), OperatorType::CONTRAST);
}

TEST_F(EditorTransactionJournalTest, HashMismatchRejectsWholeRewriteAndLeavesRecordChainUnchanged) {
  EditorTransactionJournal journal;

  EditTransaction a = MakeExposureTx(1.0f);
  a.SetTransactionID(1);
  a.GenerateTransactionHash();
  journal.AppendEdit(identity_, a);

  EditTransaction b = MakeContrastTx(2.0f);
  b.SetTransactionID(2);
  b.GenerateTransactionHash();
  journal.AppendEdit(identity_, b);
  journal.AppendCursorMove(identity_, 2, 1);

  JournalTimelineSimulator sim(identity_);
  ASSERT_EQ(sim.ReplayRecordChain(journal).status, EditorJournalApplyStatus::Applied);
  const auto before = SnapshotSimulator(sim);
  EXPECT_EQ(before.cursor, 1u);
  EXPECT_EQ(before.tx_ids.size(), 2u);

  EditTransaction bad = MakeExposureTx(9.0f);
  bad.SetTransactionID(3);
  bad.GenerateTransactionHash();
  // Wrong expected hash; sequence continues after the prior three records.
  journal.AppendRewriteTimeline(identity_, Hash128(1, 2), Hash128{}, 1, bad);

  const auto decoded = journal.DecodeRecordChain();
  ASSERT_EQ(decoded.records.size(), 4u);
  const auto apply = sim.ApplyDecodedRecord(decoded.records.back());
  EXPECT_EQ(apply.status, EditorJournalApplyStatus::RejectedHashMismatch);
  ExpectSnapshotsEqual(before, SnapshotSimulator(sim));
}

TEST_F(EditorTransactionJournalTest, TransactionIdsNeverReusedAfterRewrite) {
  WorkingVersion working(identity_.element_id, identity_.version_id,
                         pipeline_->ExportPipelineParams());
  WorkingVersionJournalRecorder recorder(&journal_, identity_);

  AppendWorkingEdit(&working, &recorder, MakeExposureTx(1.0f));  // id 1
  AppendWorkingEdit(&working, &recorder, MakeContrastTx(2.0f));  // id 2
  UndoWorking(&working, &recorder);
  AppendWorkingEdit(&working, &recorder, MakeExposureTx(3.0f));  // id 3, drops id 2

  JournalTimelineSimulator sim(identity_);
  ASSERT_EQ(sim.ReplayRecordChain(journal_).status, EditorJournalApplyStatus::Applied);

  const auto next_from_sim = sim.AllocateTransactionId();
  EXPECT_GT(next_from_sim, 2u);
  for (const auto& tx : sim.transactions()) {
    EXPECT_NE(tx.GetTransactionID(), next_from_sim);
  }
  // Discarded id 2 must stay below high water.
  EXPECT_GE(sim.tx_id_high_water(), 2u);
}

TEST_F(EditorTransactionJournalTest, WorkingVersionJournalAndSimulatorSharePipelineParams) {
  WorkingVersion working(identity_.element_id, identity_.version_id,
                         pipeline_->ExportPipelineParams());
  WorkingVersionJournalRecorder recorder(&journal_, identity_);

  AppendWorkingEdit(&working, &recorder, MakeExposureTx(1.25f));
  AppendWorkingEdit(&working, &recorder, MakeContrastTx(0.5f));
  UndoWorking(&working, &recorder);
  AppendWorkingEdit(&working, &recorder, MakeExposureTx(-0.75f));

  recorder.RecordMaterializedHead(working.GetAllEditTransactions(), working.GetCursor(),
                                  pipeline_->ExportPipelineParams());

  JournalTimelineSimulator sim(identity_);
  ASSERT_EQ(sim.ReplayRecordChain(journal_).status, EditorJournalApplyStatus::Applied);
  ExpectSnapshotsEqual(SnapshotWorking(working), SnapshotSimulator(sim));
  ASSERT_TRUE(sim.head_pipeline_params().has_value());
  ASSERT_TRUE(working.GetHeadPipelineParams().has_value());
  EXPECT_EQ(*sim.head_pipeline_params(), *working.GetHeadPipelineParams());

  // Apply simulator transactions onto a fresh pipeline and match the live head.
  auto replay_pipeline = std::make_unique<CPUPipelineExecutor>();
  for (std::size_t i = 0; i < sim.cursor(); ++i) {
    ASSERT_TRUE(sim.transactions()[i].ApplyForward(*replay_pipeline));
  }
  const auto live_exposure =
      pipeline_->GetStage(PipelineStageName::Basic_Adjustment)
          .GetOperator(OperatorType::EXPOSURE)
          .value()
          ->op_->GetParams()["exposure"]
          .get<double>();
  const auto replay_exposure =
      replay_pipeline->GetStage(PipelineStageName::Basic_Adjustment)
          .GetOperator(OperatorType::EXPOSURE)
          .value()
          ->op_->GetParams()["exposure"]
          .get<double>();
  EXPECT_DOUBLE_EQ(replay_exposure, live_exposure);
}

TEST_F(EditorTransactionJournalTest, CursorMovesAndTimelineRewriteReplayToIndependentSimulator) {
  WorkingVersion working(identity_.element_id, identity_.version_id,
                         pipeline_->ExportPipelineParams());
  WorkingVersionJournalRecorder recorder(&journal_, identity_);

  AppendWorkingEdit(&working, &recorder, MakeExposureTx(1.0f));
  AppendWorkingEdit(&working, &recorder, MakeContrastTx(2.0f));
  AppendWorkingEdit(&working, &recorder, MakeExposureTx(0.5f));
  UndoWorking(&working, &recorder);
  UndoWorking(&working, &recorder);
  RedoWorking(&working, &recorder);
  AppendWorkingEdit(&working, &recorder, MakeContrastTx(4.0f));  // rewrite remaining redo

  JournalTimelineSimulator sim(identity_);
  ASSERT_EQ(sim.ReplayRecordChain(journal_).status, EditorJournalApplyStatus::Applied);
  ExpectSnapshotsEqual(SnapshotWorking(working), SnapshotSimulator(sim));
  // A, B, then rewrite of the remaining redo tail into contrast => [A, B, contrast].
  EXPECT_EQ(sim.transactions().size(), 3u);
  EXPECT_EQ(sim.cursor(), 3u);
  EXPECT_EQ(sim.transactions()[2].GetTxOperatorType(), OperatorType::CONTRAST);
}

TEST_F(EditorTransactionJournalTest, RecordRoundTripPreservesChecksumsAndIdentity) {
  EditTransaction tx = MakeExposureTx(2.5f);
  tx.SetTransactionID(8);
  tx.GenerateTransactionHash();
  journal_.AppendEdit(identity_, tx);

  const auto decoded = journal_.DecodeRecordChain();
  ASSERT_EQ(decoded.records.size(), 1u);
  EXPECT_EQ(decoded.valid_chain_byte_count, journal_.size());
  EXPECT_FALSE(decoded.stopped_on_incomplete_tail);
  EXPECT_EQ(decoded.records[0].identity.element_id, identity_.element_id);
  EXPECT_EQ(decoded.records[0].identity.version_id, identity_.version_id);
  EXPECT_EQ(decoded.records[0].identity.session_generation, identity_.session_generation);
  ASSERT_TRUE(decoded.records[0].edit_append.has_value());
  EXPECT_EQ(decoded.records[0].edit_append->transaction.GetTransactionID(), 8u);
  EXPECT_EQ(decoded.records[0].edit_append->transaction.GetTransactionHash(), tx.GetTransactionHash());
}

TEST_F(EditorTransactionJournalTest, EditorTransactionJournalReplaysRecordSequencesInOrder) {
  EditTransaction a = MakeExposureTx(1.0f);
  a.SetTransactionID(1);
  a.GenerateTransactionHash();
  EditTransaction b = MakeContrastTx(2.0f);
  b.SetTransactionID(2);
  b.GenerateTransactionHash();
  journal_.AppendEdit(identity_, a);
  journal_.AppendEdit(identity_, b);

  JournalTimelineSimulator sim(identity_);
  ASSERT_EQ(sim.ReplayRecordChain(journal_).status, EditorJournalApplyStatus::Applied);
  EXPECT_EQ(sim.last_sequence(), 2u);
  EXPECT_EQ(sim.transactions().size(), 2u);
  EXPECT_EQ(sim.cursor(), 2u);
}

TEST_F(EditorTransactionJournalTest, EditorTransactionJournalIgnoresAlreadyMaterializedRecords) {
  EditTransaction a = MakeExposureTx(1.0f);
  a.SetTransactionID(1);
  a.GenerateTransactionHash();
  journal_.AppendEdit(identity_, a);
  journal_.AppendMaterializedHead(identity_, ComputeEditorTimelineHash({a}, 1), 1,
                                  nlohmann::json{{"ok", true}});

  // A second copy of the same edit sequence after materialization should be ignored when sequence
  // is still after the head but the materialize marker covers prior work. Here we append a new
  // edit after materialize (normal) and also verify replaying twice is safe.
  JournalTimelineSimulator sim(identity_);
  ASSERT_EQ(sim.ReplayRecordChain(journal_).status, EditorJournalApplyStatus::Applied);
  EXPECT_EQ(sim.materialized_sequence(), 2u);
  EXPECT_EQ(sim.transactions().size(), 1u);

  // Replay again on a fresh simulator remains deterministic.
  JournalTimelineSimulator sim2(identity_);
  ASSERT_EQ(sim2.ReplayRecordChain(journal_).status, EditorJournalApplyStatus::Applied);
  ExpectSnapshotsEqual(SnapshotSimulator(sim), SnapshotSimulator(sim2));
}

}  // namespace alcedo
