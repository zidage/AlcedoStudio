//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/editor_journal_recovery.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <utility>

namespace alcedo {
namespace {

auto FindLastValidBatchCommit(const EditorJournalDecodeRecordChainResult& decoded)
    -> std::optional<EditorJournalDecodedRecord> {
  std::optional<EditorJournalDecodedRecord> last;
  std::uint64_t                             previous_valid_commit = 0;
  for (const auto& record : decoded.records) {
    if (record.record_type != EditorJournalRecordType::JournalBatchCommit ||
        !record.batch_commit.has_value()) {
      continue;
    }
    const auto& payload        = *record.batch_commit;
    const auto  expected_first = previous_valid_commit + 1;
    if (payload.previous_batch_commit_sequence != previous_valid_commit ||
        payload.first_covered_sequence != expected_first ||
        payload.last_covered_sequence != record.sequence - 1 ||
        payload.first_covered_sequence > payload.last_covered_sequence ||
        payload.last_operation_sequence > payload.last_covered_sequence ||
        ComputeEditorJournalRecordChainHash(decoded.records, payload.last_covered_sequence) !=
            payload.record_chain_hash) {
      continue;
    }
    std::uint64_t last_operation_sequence = 0;
    for (const auto& covered : decoded.records) {
      if (covered.sequence < payload.first_covered_sequence ||
          covered.sequence > payload.last_covered_sequence ||
          !IsEditorJournalEditHistoryRecord(covered.record_type)) {
        continue;
      }
      last_operation_sequence = covered.sequence;
    }
    if (last_operation_sequence != payload.last_operation_sequence) {
      continue;
    }
    previous_valid_commit = record.sequence;
    last                  = record;
  }
  return last;
}

}  // namespace

auto ComputePipelineParameterHash(const nlohmann::json& params) -> Hash128 {
  const auto text = params.dump();
  return Hash128::Compute(text.data(), text.size());
}

auto WriteEditorJournalDiagnosticBundle(const std::filesystem::path& journal_path,
                                        const std::vector<std::uint8_t>& journal_bytes,
                                        const std::string& reason, std::string* error)
    -> std::optional<std::filesystem::path> {
  const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
  const auto parent =
      journal_path.has_parent_path() ? journal_path.parent_path() : std::filesystem::path(".");
  const auto stem = journal_path.filename().string();
  const auto diagnostic_dir =
      parent / (stem + ".diagnostic." + std::to_string(static_cast<long long>(stamp)));
  std::error_code ec;
  std::filesystem::create_directories(diagnostic_dir, ec);
  if (ec) {
    if (error) {
      *error = "failed to create journal diagnostic directory: " + ec.message();
    }
    return std::nullopt;
  }

  const auto journal_copy = diagnostic_dir / "journal.bin";
  {
    std::ofstream out(journal_copy, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) {
        *error = "failed to write journal diagnostic copy";
      }
      return std::nullopt;
    }
    if (!journal_bytes.empty()) {
      out.write(reinterpret_cast<const char*>(journal_bytes.data()),
                static_cast<std::streamsize>(journal_bytes.size()));
    }
  }

  const auto reason_path = diagnostic_dir / "reason.txt";
  {
    std::ofstream out(reason_path, std::ios::trunc);
    if (!out) {
      if (error) {
        *error = "failed to write journal diagnostic reason";
      }
      return std::nullopt;
    }
    out << reason << '\n';
  }
  return diagnostic_dir;
}

auto RecoverEditorJournal(const EditorTransactionJournal& journal,
                          const EditorJournalIdentity& identity,
                          const EditorRecoveryMetadata& stored_metadata,
                          std::vector<EditTransaction> materialized_transactions,
                          std::size_t                  materialized_cursor,
                          std::optional<nlohmann::json> materialized_pipeline_params)
    -> EditorJournalRecoveryResult {
  EditorJournalRecoveryResult result;
  result.recovered_state.Reset(identity);

  if (stored_metadata.element_id != 0 && stored_metadata.element_id != identity.element_id) {
    result.message = "recovery metadata element identity mismatch";
    return result;
  }
  if (stored_metadata.version_id != Hash128{} && stored_metadata.version_id != identity.version_id) {
    result.message = "recovery metadata version identity mismatch";
    return result;
  }
  if (stored_metadata.journal_generation != 0 &&
      stored_metadata.journal_generation != identity.journal_generation) {
    result.message = "recovery metadata journal generation mismatch";
    return result;
  }

  const auto decoded = journal.DecodeRecordChain();
  if (decoded.stopped_on_corrupt_record && decoded.records.empty()) {
    result.message = decoded.message.empty() ? "corrupt journal with no valid prefix"
                                             : decoded.message;
    return result;
  }

  const auto last_commit = FindLastValidBatchCommit(decoded);
  if (!last_commit.has_value() &&
      std::any_of(decoded.records.begin(), decoded.records.end(), [](const auto& record) {
        return record.record_type == EditorJournalRecordType::JournalBatchCommit;
      })) {
    result.message = "journal has no valid batch commit";
    return result;
  }

  if (last_commit.has_value()) {
    result.durable_batch_commit_sequence = last_commit->sequence;
    result.durable_operation_sequence    = last_commit->batch_commit->last_operation_sequence;
    result.last_valid_sequence           = last_commit->sequence;
  }

  const Hash128 seeded_hash =
      ComputeEditorTimelineHash(materialized_transactions, materialized_cursor);
  if (stored_metadata.materialized_operation_sequence != 0 &&
      stored_metadata.transaction_chain_hash != Hash128{} &&
      stored_metadata.transaction_chain_hash != seeded_hash) {
    result.message = "stored transaction-chain hash does not match materialized Version";
    return result;
  }

  if (materialized_pipeline_params.has_value() &&
      stored_metadata.pipeline_parameter_hash != Hash128{} &&
      stored_metadata.pipeline_parameter_hash !=
          ComputePipelineParameterHash(*materialized_pipeline_params)) {
    result.message = "stored pipeline-parameter hash does not match materialized pipeline";
    return result;
  }

  result.recovered_state.SeedMaterializedState(
      identity, std::move(materialized_transactions), materialized_cursor,
      stored_metadata.materialized_operation_sequence, std::move(materialized_pipeline_params));

  const auto replay = result.recovered_state.ReplayCommittedAfterMaterialized(journal);
  if (replay.status != EditorJournalApplyStatus::Applied &&
      replay.status != EditorJournalApplyStatus::IgnoredAlreadyMaterialized) {
    result.message = replay.message.empty() ? "journal recovery REDO failed" : replay.message;
    return result;
  }

  result.accepted = true;
  result.requires_materialize =
      result.durable_operation_sequence > stored_metadata.materialized_operation_sequence;
  if (result.requires_materialize) {
    result.message = "recovered unmaterialized journal-committed operations";
  } else {
    result.message = "journal head already matches materialized state";
  }
  return result;
}

}  // namespace alcedo
