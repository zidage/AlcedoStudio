//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_history_materializer.hpp"

#include <stdexcept>
#include <utility>

#include "edit/history/version.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
namespace {

auto ResolveVersion(EditHistory& history, const EditorJournalIdentity& identity) -> Version& {
  if (identity.version_id != Hash128{}) {
    try {
      return history.GetVersion(identity.version_id);
    } catch (...) {
      // Fall through to the active Version.
    }
  }
  return history.GetActiveVersion();
}

auto LastDurableOperationSequence(const EditorTransactionJournal& journal) -> std::uint64_t {
  const auto decoded = journal.DecodeRecordChain();
  std::uint64_t previous = 0;
  std::uint64_t durable  = 0;
  for (const auto& record : decoded.records) {
    if (record.record_type != EditorJournalRecordType::JournalBatchCommit ||
        !record.batch_commit.has_value()) {
      continue;
    }
    const auto& payload = *record.batch_commit;
    if (payload.previous_batch_commit_sequence != previous ||
        payload.first_covered_sequence != previous + 1 ||
        payload.last_covered_sequence != record.sequence - 1 ||
        payload.first_covered_sequence > payload.last_covered_sequence ||
        payload.last_operation_sequence > payload.last_covered_sequence ||
        ComputeEditorJournalRecordChainHash(decoded.records, payload.last_covered_sequence) !=
            payload.record_chain_hash) {
      continue;
    }
    previous = record.sequence;
    durable  = payload.last_operation_sequence;
  }
  return durable;
}

struct JournalEpochInfo {
  bool          compacted                      = false;
  std::uint64_t generation                     = 0;
  std::uint64_t previous_materialized_sequence = 0;
};

auto InspectJournalEpoch(const EditorTransactionJournal& journal,
                         const EditorJournalIdentity& expected_identity, JournalEpochInfo* info,
                         std::string* error) -> bool {
  if (info == nullptr) {
    return false;
  }
  *info              = {};
  const auto decoded = journal.DecodeRecordChain();
  if (decoded.records.empty()) {
    info->generation = expected_identity.journal_generation;
    return true;
  }
  info->generation = decoded.records.front().identity.journal_generation;
  for (const auto& record : decoded.records) {
    if (record.identity.element_id != expected_identity.element_id ||
        record.identity.journal_generation != info->generation) {
      if (error) {
        *error = "journal record identity or generation mismatch";
      }
      return false;
    }
  }
  if (info->generation != expected_identity.journal_generation) {
    if (error) {
      *error = "journal generation does not match materialize request";
    }
    return false;
  }
  const auto& first = decoded.records.front();
  if (first.record_type == EditorJournalRecordType::CompactionCheckpoint) {
    if (!first.marker.has_value()) {
      if (error) {
        *error = "compaction checkpoint payload is missing";
      }
      return false;
    }
    info->compacted                      = true;
    info->previous_materialized_sequence = first.marker->last_valid_sequence;
  }
  return true;
}

auto ValidateCompactionTransition(const JournalEpochInfo&                      epoch,
                                  const std::optional<EditorRecoveryMetadata>& stored,
                                  std::string*                                 error) -> bool {
  if (!stored.has_value()) {
    if (epoch.compacted) {
      if (error) {
        *error = "compacted journal has no stored materialized base";
      }
      return false;
    }
    return true;
  }
  if (stored->journal_generation == epoch.generation) {
    return true;
  }
  if (!epoch.compacted) {
    if (error) {
      *error = "journal generation changed without a compaction checkpoint";
    }
    return false;
  }
  if (epoch.generation != stored->journal_generation + 1 ||
      epoch.previous_materialized_sequence != stored->materialized_operation_sequence) {
    if (error) {
      *error = "compaction checkpoint does not continue the stored materialized head";
    }
    return false;
  }
  return true;
}

}  // namespace

EditorHistoryMaterializer::EditorHistoryMaterializer(std::shared_ptr<Storage> storage)
    : storage_(std::move(storage)) {
  if (!storage_) {
    throw std::invalid_argument("EditorHistoryMaterializer requires Storage");
  }
}

auto EditorHistoryMaterializer::LoadRecoveryMetadata(sl_element_id_t element_id) const
    -> std::optional<EditorRecoveryMetadata> {
  return storage_->GetElementStore().GetEditorRecoveryMetadata(element_id);
}

auto EditorHistoryMaterializer::Materialize(const EditorMaterializeRequest&     request,
                                            EditorTransactionJournal*           journal,
                                            const std::shared_ptr<EditHistory>& history,
                                            const nlohmann::json&               pipeline_params,
                                            std::string* error) -> EditorMaterializeResult {
  EditorMaterializeResult result;
  if (!history || !journal) {
    result.error = "materialize requires history and journal";
    if (error) {
      *error = result.error;
    }
    return result;
  }
  if (history->GetBoundImage() != request.identity.element_id) {
    result.error = "history bound image does not match journal identity";
    if (error) {
      *error = result.error;
    }
    return result;
  }

  auto&      active = ResolveVersion(*history, request.identity);

  const auto stored_metadata =
      storage_->GetElementStore().GetEditorRecoveryMetadata(request.identity.element_id);
  JournalEpochInfo epoch;
  if (!InspectJournalEpoch(*journal, request.identity, &epoch, error) ||
      !ValidateCompactionTransition(epoch, stored_metadata, error)) {
    result.error = error != nullptr && !error->empty() ? *error : "invalid journal epoch";
    return result;
  }
  const bool same_generation =
      stored_metadata.has_value() &&
      stored_metadata->journal_generation == request.identity.journal_generation;
  const std::uint64_t already_materialized =
      same_generation ? stored_metadata->materialized_operation_sequence : 0;

  if (request.validate_expected_materialized_head) {
    if (already_materialized != request.expected_materialized_operation_sequence) {
      result.error = "expected materialized operation sequence mismatch";
      if (error) {
        *error = result.error;
      }
      return result;
    }
    if (request.expected_materialized_chain_hash != Hash128{} && stored_metadata.has_value() &&
        stored_metadata->transaction_chain_hash != request.expected_materialized_chain_hash) {
      result.error = "expected transaction-chain hash mismatch";
      if (error) {
        *error = result.error;
      }
      return result;
    }
  }

  const std::uint64_t      durable_from_journal = LastDurableOperationSequence(*journal);
  const std::uint64_t      target_sequence      = request.target_operation_sequence == 0
                                                      ? durable_from_journal
                                                      : request.target_operation_sequence;

  // A compacted journal is a new generation whose record sequence restarts at
  // one. Seed it from the DuckDB projection and REDO only operations in the new
  // generation; otherwise replay the original generation from its beginning.
  JournalTimelineSimulator simulator(request.identity);
  EditorJournalApplyResult replay;
  if (epoch.compacted) {
    simulator.SeedMaterializedState(request.identity, active.GetAllEditTransactions(),
                                    active.GetCursor(), already_materialized, pipeline_params);
    replay = simulator.ReplayCommittedAfterMaterialized(*journal);
  } else {
    replay = simulator.ReplayCommittedThroughOperationSequence(*journal, target_sequence);
  }
  if (replay.status != EditorJournalApplyStatus::Applied &&
      replay.status != EditorJournalApplyStatus::IgnoredAlreadyMaterialized) {
    result.error = replay.message.empty() ? "materialize journal REDO failed" : replay.message;
    if (error) {
      *error = result.error;
    }
    return result;
  }
  if (target_sequence < already_materialized) {
    result.error = "cannot materialize behind the stored materialized head";
    if (error) {
      *error = result.error;
    }
    return result;
  }

  nlohmann::json head_params = pipeline_params;
  if (simulator.head_pipeline_params().has_value()) {
    head_params = *simulator.head_pipeline_params();
  }

  const Hash128 chain_hash = simulator.TimelineHash();
  if (same_generation && stored_metadata.has_value() && target_sequence <= already_materialized &&
      (stored_metadata->transaction_chain_hash == Hash128{} ||
       stored_metadata->transaction_chain_hash == chain_hash)) {
    result.accepted                        = true;
    result.materialized                    = false;
    result.materialized_operation_sequence = already_materialized;
    result.transaction_chain_hash          = chain_hash;
    result.pipeline_parameter_hash         = ComputePipelineParameterHash(head_params);
    return result;
  }

  WorkingVersion working{request.identity.element_id, active.GetVersionID(), head_params,
                         simulator.transactions(), simulator.cursor()};
  history->UpdateVersionFromWorkingVersion(active.GetVersionID(), working, head_params);

  auto pipeline = std::make_shared<CPUPipelineExecutor>();
  pipeline->SetBoundFile(request.identity.element_id);
  pipeline->ImportPipelineParams(head_params);

  EditorRecoveryMetadata metadata;
  metadata.element_id                      = request.identity.element_id;
  metadata.version_id                      = active.GetVersionID();
  metadata.journal_generation              = request.identity.journal_generation;
  metadata.materialized_operation_sequence = target_sequence;
  metadata.transaction_chain_hash          = chain_hash;
  metadata.pipeline_parameter_hash         = ComputePipelineParameterHash(head_params);

  std::string storage_error;
  if (!storage_->GetElementStore().MaterializeEditorState(history, pipeline, metadata,
                                                               &storage_error)) {
    result.error = storage_error.empty() ? "atomic materialize failed" : storage_error;
    if (error) {
      *error = result.error;
    }
    return result;
  }

  result.accepted                        = true;
  result.materialized                    = true;
  result.materialized_operation_sequence = metadata.materialized_operation_sequence;
  result.transaction_chain_hash          = metadata.transaction_chain_hash;
  result.pipeline_parameter_hash         = metadata.pipeline_parameter_hash;
  return result;
}

auto EditorHistoryMaterializer::RecoverAndMaterialize(
    const EditorJournalIdentity& identity, EditorTransactionJournal* journal,
    const std::shared_ptr<EditHistory>&  history,
    const std::optional<nlohmann::json>& stored_pipeline_params, std::string* error)
    -> EditorMaterializeResult {
  EditorMaterializeResult result;
  if (!history || !journal) {
    result.error = "recover requires history and journal";
    if (error) {
      *error = result.error;
    }
    return result;
  }

  EditorRecoveryMetadata stored{};
  const auto             stored_metadata =
      storage_->GetElementStore().GetEditorRecoveryMetadata(identity.element_id);
  if (stored_metadata.has_value()) {
    stored = *stored_metadata;
  } else {
    stored.element_id         = identity.element_id;
    stored.version_id         = identity.version_id;
    stored.journal_generation = identity.journal_generation;
  }

  auto&            active = ResolveVersion(*history, identity);

  JournalEpochInfo epoch;
  if (!InspectJournalEpoch(*journal, identity, &epoch, error) ||
      !ValidateCompactionTransition(epoch, stored_metadata, error)) {
    result.error = error != nullptr && !error->empty() ? *error : "invalid journal epoch";
    return result;
  }
  const bool same_generation =
      !stored_metadata.has_value() || stored.journal_generation == identity.journal_generation;
  const std::uint64_t local_materialized_sequence =
      same_generation ? stored.materialized_operation_sequence : 0;

  // Seed the simulator from the DuckDB-materialized Version projection so a
  // compacted journal (which omits prior edit records behind a checkpoint)
  // preserves the durable transaction chain instead of replacing it with an
  // empty timeline, and so recovery REDOs only journal-committed operations
  // after the stored materialized head. The caller must pass a history loaded
  // from DuckDB so the seed reflects the durable state.
  JournalTimelineSimulator recovered(identity);
  recovered.SeedMaterializedState(
      identity, active.GetAllEditTransactions(), active.GetCursor(), local_materialized_sequence,
      stored_pipeline_params.has_value() ? std::optional<nlohmann::json>(*stored_pipeline_params)
                                         : std::nullopt);

  const auto replay = recovered.ReplayCommittedAfterMaterialized(*journal);
  if (replay.status != EditorJournalApplyStatus::Applied &&
      replay.status != EditorJournalApplyStatus::IgnoredAlreadyMaterialized) {
    result.error = replay.message.empty() ? "journal recovery REDO failed" : replay.message;
    if (error) {
      *error = result.error;
    }
    return result;
  }

  const std::uint64_t durable_op = LastDurableOperationSequence(*journal);
  const Hash128       chain_hash = recovered.TimelineHash();

  if (same_generation && durable_op <= stored.materialized_operation_sequence &&
      (stored.transaction_chain_hash == Hash128{} || stored.transaction_chain_hash == chain_hash)) {
    // Already materialized; keep live history aligned with the reconstructed head.
    // This branch does not write DuckDB, so materialized is false: callers gate
    // thumbnail invalidation (and compaction) on a real DuckDB advance, not on a
    // no-op recovery that confirmed the durable head already matches.
    nlohmann::json head_params =
        recovered.head_pipeline_params().value_or(stored_pipeline_params.value_or(
            active.GetMaterializedParams().value_or(history->GetImportPipelineParams())));
    WorkingVersion working{identity.element_id, active.GetVersionID(), head_params,
                           recovered.transactions(), recovered.cursor()};
    history->UpdateVersionFromWorkingVersion(active.GetVersionID(), working, head_params);

    result.accepted                        = true;
    result.materialized                    = false;
    result.materialized_operation_sequence = stored.materialized_operation_sequence;
    result.transaction_chain_hash          = chain_hash;
    result.pipeline_parameter_hash         = ComputePipelineParameterHash(head_params);
    return result;
  }

  nlohmann::json head_params =
      recovered.head_pipeline_params().value_or(stored_pipeline_params.value_or(
          active.GetMaterializedParams().value_or(history->GetImportPipelineParams())));

  WorkingVersion working{identity.element_id, active.GetVersionID(), head_params,
                         recovered.transactions(), recovered.cursor()};
  history->UpdateVersionFromWorkingVersion(active.GetVersionID(), working, head_params);

  auto pipeline = std::make_shared<CPUPipelineExecutor>();
  pipeline->SetBoundFile(identity.element_id);
  pipeline->ImportPipelineParams(head_params);

  EditorRecoveryMetadata metadata;
  metadata.element_id                      = identity.element_id;
  metadata.version_id                      = active.GetVersionID();
  metadata.journal_generation              = identity.journal_generation;
  metadata.materialized_operation_sequence = durable_op;
  metadata.transaction_chain_hash          = chain_hash;
  metadata.pipeline_parameter_hash         = ComputePipelineParameterHash(head_params);

  std::string storage_error;
  if (!storage_->GetElementStore().MaterializeEditorState(history, pipeline, metadata,
                                                               &storage_error)) {
    result.error = storage_error.empty() ? "atomic materialize failed" : storage_error;
    if (error) {
      *error = result.error;
    }
    return result;
  }

  result.accepted                        = true;
  result.materialized                    = true;
  result.materialized_operation_sequence = metadata.materialized_operation_sequence;
  result.transaction_chain_hash          = metadata.transaction_chain_hash;
  result.pipeline_parameter_hash         = metadata.pipeline_parameter_hash;
  return result;
}

}  // namespace alcedo
