//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "edit/history/edit_transaction.hpp"
#include "edit/history/editor_journal_writer.hpp"
#include "edit/history/editor_transaction_journal.hpp"
#include "editor_journal_fuzz_framework.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"

namespace alcedo::tests {
namespace {

auto LoadRegressionSeeds() -> std::vector<std::uint64_t> {
  std::ifstream              input(EDITOR_JOURNAL_FUZZ_CORPUS_PATH);
  std::vector<std::uint64_t> seeds;
  std::string                line;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    seeds.push_back(std::stoull(line));
  }
  return seeds;
}

auto MakeForcedTerminationTransaction(tx_id_t id, float exposure) -> EditTransaction {
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

[[noreturn]] void TerminateFuzzChild() {
#ifdef _WIN32
  ::TerminateProcess(::GetCurrentProcess(), 86);
#else
  std::_Exit(86);
#endif
  std::abort();
}

}  // namespace

TEST(EditorJournalFuzzFrameworkTest, FixedSeedsRecoverCurrentWalWithinDurabilityBoundary) {
  const auto seeds = LoadRegressionSeeds();
  ASSERT_FALSE(seeds.empty());
  for (const auto seed : seeds) {
    EditorJournalFuzzConfig config;
    config.seed       = seed;
    config.steps      = 48;
    const auto result = RunEditorJournalFuzz(config);
    ASSERT_TRUE(result.passed) << "seed=" << seed << " operations=" << result.operation_sequence
                               << " message=" << result.message
                               << " artifact=" << result.artifact_path.string();
  }
}

TEST(EditorJournalFuzzFrameworkTest, DISABLED_FreshBoundedSeedsRecoverCurrentWal) {
  std::string seed_text;
#ifdef _WIN32
  char*       raw_seed      = nullptr;
  std::size_t raw_seed_size = 0;
  if (_dupenv_s(&raw_seed, &raw_seed_size, "ALCEDO_EDITOR_JOURNAL_FUZZ_SEED_BASE") == 0 &&
      raw_seed != nullptr) {
    seed_text = raw_seed;
  }
  std::free(raw_seed);
#else
  if (const auto* raw_seed = std::getenv("ALCEDO_EDITOR_JOURNAL_FUZZ_SEED_BASE")) {
    seed_text = raw_seed;
  }
#endif
  const auto seed_base = seed_text.empty() ? 0x9e3779b97f4a7c15ULL : std::stoull(seed_text);
  constexpr std::size_t kScheduledSeedCount = 128;
  for (std::size_t index = 0; index < kScheduledSeedCount; ++index) {
    EditorJournalFuzzConfig config;
    config.seed       = seed_base + index;
    config.steps      = 256;
    const auto result = RunEditorJournalFuzz(config);
    ASSERT_TRUE(result.passed) << "seed=" << config.seed
                               << " operations=" << result.operation_sequence
                               << " message=" << result.message
                               << " artifact=" << result.artifact_path.string();
  }
}

TEST(EditorJournalFuzzFrameworkTest, ForcedChildTerminationLeavesLastDurableBatchRecoverable) {
  const auto      directory = std::filesystem::temp_directory_path() / "alcedo_wal_forced_child";
  const auto      path      = directory / "image-42.wal";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  ASSERT_FALSE(error);

  const EditorJournalIdentity identity{42, Hash128(11, 22), 1, 1};
  ASSERT_EXIT(
      {
        EditorJournalWriter writer(identity, path);
        (void)writer.AppendEdit(identity, MakeForcedTerminationTransaction(1, 1.0f));
        if (!writer.CommitQueued().durable) {
          std::_Exit(2);
        }
        // This operation is intentionally not committed. Forced termination
        // must leave the preceding flushed batch as the recovery boundary.
        (void)writer.AppendEdit(identity, MakeForcedTerminationTransaction(2, 2.0f));
        TerminateFuzzChild();
      },
      ::testing::ExitedWithCode(86), "");

  EditorJournalWriter      reopened(identity, path);
  JournalTimelineSimulator recovered(identity);
  const auto               replay = recovered.ReplayCommittedRecordChain(reopened.journal());
  ASSERT_EQ(replay.status, EditorJournalApplyStatus::Applied) << replay.message;
  ASSERT_EQ(recovered.transactions().size(), 1u);
  EXPECT_EQ(recovered.transactions().front().GetTransactionID(), 1u);
  std::filesystem::remove_all(directory, error);
}

TEST(EditorJournalFuzzFrameworkTest, NamedCrashPointsProduceReproducibleRecovery) {
  constexpr std::array<EditorJournalCrashPoint, 9> kPoints = {
      EditorJournalCrashPoint::RecordHeader,
      EditorJournalCrashPoint::RecordPayload,
      EditorJournalCrashPoint::RecordChecksum,
      EditorJournalCrashPoint::Flush,
      EditorJournalCrashPoint::HeadMarker,
      EditorJournalCrashPoint::Materialize,
      EditorJournalCrashPoint::ThumbnailInvalidation,
      EditorJournalCrashPoint::CompactionReplace,
      EditorJournalCrashPoint::ImageSwitch,
  };
  for (const auto point : kPoints) {
    EditorJournalFuzzConfig config;
    config.seed        = 0xabc000ULL + static_cast<std::uint64_t>(point);
    config.steps       = 24;
    config.crash_point = point;
    config.crash_step  = 7;
    const auto first   = RunEditorJournalFuzz(config);
    const auto second  = RunEditorJournalFuzz(config);
    ASSERT_TRUE(first.passed) << ToString(point) << ": " << first.message;
    EXPECT_TRUE(second.passed) << ToString(point) << ": " << second.message;
    EXPECT_EQ(first.operation_sequence, second.operation_sequence);
  }
}

TEST(EditorJournalFuzzFrameworkTest, CompactionNeverRestoresDiscardedRedoTail) {
  auto                     file = std::make_shared<InjectedEditorJournalFile>();
  EditorTransactionJournal journal;
  EditorJournalIdentity    identity{42, Hash128(11, 22), 1, 1};
  EditorJournalWriter      writer(&journal, file);
  const auto               first       = MakeForcedTerminationTransaction(1, 1.0f);
  const auto               discarded   = MakeForcedTerminationTransaction(2, 2.0f);
  const auto               replacement = MakeForcedTerminationTransaction(3, 3.0f);
  ASSERT_NE(writer.AppendEdit(identity, first), 0u);
  ASSERT_TRUE(writer.CommitQueued().durable);
  ASSERT_NE(writer.AppendEdit(identity, discarded), 0u);
  ASSERT_TRUE(writer.CommitQueued().durable);
  ASSERT_NE(writer.AppendCursorMove(identity, 2, 1), 0u);
  const std::vector<EditTransaction> before{first, discarded};
  ASSERT_NE(
      writer.AppendRewriteTimeline(identity, ComputeEditorTimelineHash(before, 1),
                                   ComputeEditorTransactionSpanHash(before, 1, 2), 1, replacement),
      0u);
  ASSERT_TRUE(writer.CommitQueued().durable);

  JournalTimelineSimulator before_compaction(identity);
  ASSERT_EQ(before_compaction.ReplayCommittedRecordChain(writer.journal()).status,
            EditorJournalApplyStatus::Applied);
  ASSERT_EQ(before_compaction.transactions().size(), 2u);
  EXPECT_EQ(before_compaction.transactions()[1].GetTransactionID(), 3u);

  ++identity.journal_generation;
  std::string error;
  ASSERT_TRUE(writer.CompactToMaterializedHead(
      identity, before_compaction.TimelineHash(), before_compaction.cursor(),
      nlohmann::json{{"exposure", 3.0f}}, "active.wal", "active.wal.compact", &error))
      << error;
  const auto after = MakeForcedTerminationTransaction(4, 4.0f);
  ASSERT_NE(writer.AppendEdit(identity, after), 0u);
  ASSERT_TRUE(writer.CommitQueued().durable);

  JournalTimelineSimulator recovered(identity);
  recovered.SeedMaterializedState(identity, before_compaction.transactions(),
                                  before_compaction.cursor(), 0,
                                  nlohmann::json{{"exposure", 3.0f}});
  const auto replay = recovered.ReplayCommittedAfterMaterialized(writer.journal());
  ASSERT_EQ(replay.status, EditorJournalApplyStatus::Applied) << replay.message;
  ASSERT_EQ(recovered.transactions().size(), 3u);
  EXPECT_EQ(recovered.transactions()[0].GetTransactionID(), 1u);
  EXPECT_EQ(recovered.transactions()[1].GetTransactionID(), 3u);
  EXPECT_EQ(recovered.transactions()[2].GetTransactionID(), 4u);
}

}  // namespace alcedo::tests
