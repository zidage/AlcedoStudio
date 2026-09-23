//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

#include "editor_journal_fuzz_framework.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

#include "edit/history/edit_transaction.hpp"
#include "edit/history/editor_journal_writer.hpp"
#include "edit/history/editor_transaction_journal.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"

namespace alcedo::tests {
namespace {

constexpr sl_element_id_t kElementId = 42;

struct Snapshot {
  std::vector<tx_id_t> ids;
  std::size_t          cursor = 0;
  Hash128              timeline_hash{};
};

auto SnapshotOf(const JournalTimelineSimulator& state) -> Snapshot {
  Snapshot result;
  result.cursor        = state.cursor();
  result.timeline_hash = state.TimelineHash();
  for (const auto& transaction : state.transactions()) {
    result.ids.push_back(transaction.GetTransactionID());
  }
  return result;
}

auto SameSnapshot(const Snapshot& lhs, const Snapshot& rhs) -> bool {
  return lhs.cursor == rhs.cursor && lhs.ids == rhs.ids && lhs.timeline_hash == rhs.timeline_hash;
}

auto DescribeSnapshot(const Snapshot& snapshot) -> std::string {
  std::ostringstream output;
  output << "cursor=" << snapshot.cursor << ";ids=";
  for (std::size_t index = 0; index < snapshot.ids.size(); ++index) {
    if (index != 0) {
      output << ':';
    }
    output << snapshot.ids[index];
  }
  output << ";hash=" << snapshot.timeline_hash.high64() << ':' << snapshot.timeline_hash.low64();
  return output.str();
}

class Rng final {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 1 : seed) {}

  auto Next(std::uint64_t limit) -> std::uint64_t {
    state_ ^= state_ << 7;
    state_ ^= state_ >> 9;
    state_ ^= state_ << 8;
    return limit == 0 ? state_ : state_ % limit;
  }

 private:
  std::uint64_t state_;
};

auto MakeTransaction(tx_id_t id, float value) -> EditTransaction {
  EditTransaction transaction{TransactionType::_EDIT,
                              OperatorType::EXPOSURE,
                              PipelineStageName::Basic_Adjustment,
                              nlohmann::json{{"exposure", 0.0f}},
                              nlohmann::json{{"exposure", value}},
                              true,
                              true};
  transaction.SetTransactionID(id);
  transaction.GenerateTransactionHash();
  return transaction;
}

auto MakeIdentity(std::uint64_t session_generation = 1) -> EditorJournalIdentity {
  return EditorJournalIdentity{kElementId, Hash128(11, 22), session_generation, 1};
}

auto ApplyAppend(JournalTimelineSimulator* state, EditTransaction transaction) -> void {
  auto       transactions = state->transactions();
  const auto cursor       = state->cursor();
  if (cursor < transactions.size()) {
    transactions.erase(transactions.begin() + static_cast<std::ptrdiff_t>(cursor),
                       transactions.end());
  }
  transactions.push_back(std::move(transaction));
  // Rebuild through the public journal-free state by applying a synthetic journal.
  EditorTransactionJournal journal;
  const auto               identity = state->identity();
  for (const auto& item : transactions) {
    journal.AppendEdit(identity, item);
  }
  JournalTimelineSimulator rebuilt(identity);
  (void)rebuilt.ReplayRecordChain(journal);
  *state = std::move(rebuilt);
}

auto ApplyCursor(JournalTimelineSimulator* state, std::size_t to_cursor) -> void {
  EditorTransactionJournal journal;
  const auto               identity = state->identity();
  for (const auto& item : state->transactions()) {
    journal.AppendEdit(identity, item);
  }
  journal.AppendCursorMove(identity, state->transactions().size(), to_cursor);
  JournalTimelineSimulator rebuilt(identity);
  (void)rebuilt.ReplayRecordChain(journal);
  *state = std::move(rebuilt);
}

auto WriteArtifact(const EditorJournalFuzzConfig& config, const EditorJournalFuzzResult& result,
                   const std::vector<std::uint8_t>& bytes) -> std::filesystem::path {
  auto directory = config.artifact_directory;
  if (directory.empty()) {
    directory = std::filesystem::temp_directory_path() / "alcedo_editor_journal_fuzz";
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const auto    path = directory / ("seed-" + std::to_string(config.seed) + ".txt");
  std::ofstream output(path, std::ios::trunc);
  output << "seed=" << result.seed << "\n"
         << "crash_point=" << ToString(result.crash_point) << "\n"
         << "crash_step=" << result.crash_step << "\n"
         << "steps_executed=" << result.steps_executed << "\n"
         << "operations=" << result.operation_sequence << "\n"
         << "message=" << result.message << "\n"
         << "journal_bytes=" << bytes.size() << "\n"
         << "journal_hex=";
  constexpr char kHex[] = "0123456789abcdef";
  for (const auto byte : bytes) {
    output << kHex[byte >> 4] << kHex[byte & 0x0f];
  }
  output << "\n";
  return path;
}

}  // namespace

auto ToString(EditorJournalCrashPoint point) -> const char* {
  switch (point) {
    case EditorJournalCrashPoint::None:
      return "none";
    case EditorJournalCrashPoint::RecordHeader:
      return "record-header";
    case EditorJournalCrashPoint::RecordPayload:
      return "record-payload";
    case EditorJournalCrashPoint::RecordChecksum:
      return "record-checksum";
    case EditorJournalCrashPoint::Flush:
      return "flush";
    case EditorJournalCrashPoint::HeadMarker:
      return "head-marker";
    case EditorJournalCrashPoint::Materialize:
      return "materialize";
    case EditorJournalCrashPoint::ThumbnailInvalidation:
      return "thumbnail-invalidation";
    case EditorJournalCrashPoint::CompactionReplace:
      return "compaction-replace";
    case EditorJournalCrashPoint::ImageSwitch:
      return "image-switch";
  }
  return "unknown";
}

auto RunEditorJournalFuzz(const EditorJournalFuzzConfig& config) -> EditorJournalFuzzResult {
  EditorJournalFuzzResult result;
  result.seed                       = config.seed;
  result.crash_point                = config.crash_point;
  result.crash_step                 = config.crash_step;

  const auto               identity = MakeIdentity();
  auto                     file     = std::make_shared<InjectedEditorJournalFile>();
  EditorTransactionJournal journal;
  EditorJournalWriter      writer(&journal, file);
  JournalTimelineSimulator committed(identity);
  JournalTimelineSimulator pending(identity);
  tx_id_t                  next_transaction_id = 1;
  Rng                      rng(config.seed);
  bool                     crashed = false;

  std::ostringstream       operations;
  for (std::size_t step = 0; step < config.steps && !crashed; ++step) {
    result.steps_executed = step + 1;
    if (step != 0) {
      operations << ',';
    }
    const auto operation = rng.Next(12);
    const bool at_crash_step =
        config.crash_point != EditorJournalCrashPoint::None && step == config.crash_step;
    std::string operation_name;
    bool        queued          = false;
    bool        writer_rejected = false;
    if (operation <= 2 || pending.transactions().empty()) {
      auto transaction =
          MakeTransaction(next_transaction_id++, static_cast<float>(rng.Next(500)) / 100.0f);
      const auto before = pending.transactions();
      if (pending.cursor() < before.size()) {
        const auto expected_hash = pending.TimelineHash();
        const auto discarded_hash =
            ComputeEditorTransactionSpanHash(before, pending.cursor(), before.size());
        queued           = writer.AppendRewriteTimeline(identity, expected_hash, discarded_hash,
                                                        pending.cursor(), transaction) != 0;
        writer_rejected  = !queued;
        auto replacement = before;
        replacement.erase(replacement.begin() + static_cast<std::ptrdiff_t>(pending.cursor()),
                          replacement.end());
        replacement.push_back(transaction);
        pending.Reset(identity);
        EditorTransactionJournal model;
        for (const auto& item : replacement) {
          model.AppendEdit(identity, item);
        }
        JournalTimelineSimulator rebuilt(identity);
        (void)rebuilt.ReplayRecordChain(model);
        pending        = std::move(rebuilt);
        operation_name = "rewrite";
      } else {
        queued          = writer.AppendEdit(identity, transaction) != 0;
        writer_rejected = !queued;
        ApplyAppend(&pending, std::move(transaction));
        operation_name = "edit";
      }
    } else if (operation == 3 && pending.cursor() > 0) {
      const auto from = pending.cursor();
      queued          = writer.AppendCursorMove(identity, from, from - 1) != 0;
      writer_rejected = !queued;
      ApplyCursor(&pending, from - 1);
      operation_name = "undo";
    } else if (operation == 4 && pending.cursor() < pending.transactions().size()) {
      const auto from = pending.cursor();
      queued          = writer.AppendCursorMove(identity, from, from + 1) != 0;
      writer_rejected = !queued;
      ApplyCursor(&pending, from + 1);
      operation_name = "redo";
    } else if (operation == 5) {
      operation_name = "search-replacement";
    } else if (operation == 6) {
      operation_name = "workspace-change";
    } else if (operation == 7) {
      operation_name = "image-switch";
    } else if (operation == 8) {
      operation_name = "autosave";
    } else if (operation == 9) {
      operation_name = "materialize";
    } else if (operation == 10) {
      operation_name = "compact";
    } else {
      operation_name = "shutdown";
    }
    operations << operation_name;

    if (writer_rejected) {
      result.message            = "writer rejected a generated operation: " + operation_name;
      result.operation_sequence = operations.str();
      result.artifact_path      = WriteArtifact(config, result, file->bytes());
      return result;
    }

    if (!queued) {
      if (at_crash_step) {
        crashed = true;
      }
      continue;
    }

    if (at_crash_step && config.crash_point == EditorJournalCrashPoint::Materialize) {
      crashed = true;
      break;
    }
    if (at_crash_step && config.crash_point == EditorJournalCrashPoint::Flush) {
      file->fail_flush = true;
    } else if (at_crash_step && config.crash_point == EditorJournalCrashPoint::RecordHeader) {
      file->max_total_append_bytes = file->bytes().size() + 8;
    } else if (at_crash_step && config.crash_point == EditorJournalCrashPoint::RecordPayload) {
      file->max_total_append_bytes = file->bytes().size() + 64;
    } else if (at_crash_step && config.crash_point == EditorJournalCrashPoint::RecordChecksum) {
      file->max_total_append_bytes = file->bytes().size() + 160;
    }

    const auto commit = writer.CommitQueued();
    if (commit.durable) {
      committed = pending;
    }
    if (at_crash_step) {
      crashed = true;
    }
  }
  result.operation_sequence          = operations.str();

  auto                recovered_file = std::make_shared<InjectedEditorJournalFile>(file->bytes());
  EditorJournalWriter reopened(recovered_file);
  if (reopened.identity().element_id == 0) {
    (void)reopened.SetIdentity(identity);
  }
  JournalTimelineSimulator recovered(identity);
  const auto               replay = recovered.ReplayCommittedRecordChain(reopened.journal());
  if (replay.status != EditorJournalApplyStatus::Applied &&
      replay.status != EditorJournalApplyStatus::IgnoredAlreadyMaterialized) {
    result.message       = "recovery rejected the committed prefix: " + replay.message;
    result.artifact_path = WriteArtifact(config, result, file->bytes());
    return result;
  }

  const auto recovered_snapshot = SnapshotOf(recovered);
  const auto committed_snapshot = SnapshotOf(committed);
  const auto pending_snapshot   = SnapshotOf(pending);
  const bool allowed            = SameSnapshot(recovered_snapshot, committed_snapshot) ||
                       (crashed && SameSnapshot(recovered_snapshot, pending_snapshot));
  if (!allowed) {
    result.message = "expected_pre={" + DescribeSnapshot(committed_snapshot) + "};expected_post={" +
                     DescribeSnapshot(pending_snapshot) + "};actual={" +
                     DescribeSnapshot(recovered_snapshot) + "}";
    result.artifact_path = WriteArtifact(config, result, file->bytes());
    return result;
  }
  result.passed = true;
  return result;
}

}  // namespace alcedo::tests
