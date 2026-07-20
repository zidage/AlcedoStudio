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

}  // namespace

EditorHistoryMaterializer::EditorHistoryMaterializer(std::shared_ptr<StorageService> storage)
    : storage_(std::move(storage)) {
  if (!storage_) {
    throw std::invalid_argument("EditorHistoryMaterializer requires StorageService");
  }
}

auto EditorHistoryMaterializer::LoadRecoveryMetadata(sl_element_id_t element_id) const
    -> std::optional<EditorRecoveryMetadata> {
  return storage_->GetElementController().GetEditorRecoveryMetadata(element_id);
}

auto EditorHistoryMaterializer::Materialize(const EditorMaterializeRequest& request,
                                            EditorTransactionJournal* journal,
                                            const std::shared_ptr<EditHistory>& history,
                                            const nlohmann::json& pipeline_params,
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

  auto& active = ResolveVersion(*history, request.identity);

  const auto stored_metadata =
      storage_->GetElementController().GetEditorRecoveryMetadata(request.identity.element_id);
  const std::uint64_t already_materialized =
      stored_metadata.has_value() ? stored_metadata->materialized_operation_sequence : 0;

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

  const std::uint64_t durable_from_journal = LastDurableOperationSequence(*journal);
  const std::uint64_t target_sequence =
      request.target_operation_sequence == 0 ? durable_from_journal
                                             : request.target_operation_sequence;

  // Reconstruct from the committed journal chain, capped at the requested
  // durable operation sequence so unflushed in-memory batches are excluded.
  JournalTimelineSimulator simulator(request.identity);
  const auto replay =
      simulator.ReplayCommittedThroughOperationSequence(*journal, target_sequence);
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
  metadata.transaction_chain_hash          = simulator.TimelineHash();
  metadata.pipeline_parameter_hash         = ComputePipelineParameterHash(head_params);

  std::string storage_error;
  if (!storage_->GetElementController().MaterializeEditorState(history, pipeline, metadata,
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
    const std::shared_ptr<EditHistory>& history,
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
  if (const auto metadata =
          storage_->GetElementController().GetEditorRecoveryMetadata(identity.element_id)) {
    stored = *metadata;
  } else {
    stored.element_id         = identity.element_id;
    stored.version_id         = identity.version_id;
    stored.journal_generation = identity.journal_generation;
  }

  auto& active = ResolveVersion(*history, identity);

  // Full REDO from the committed journal record chain. DuckDB recovery metadata
  // decides whether the reconstructed head still needs materialization.
  JournalTimelineSimulator recovered(identity);
  const auto replay = recovered.ReplayCommittedRecordChain(*journal);
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

  if (durable_op <= stored.materialized_operation_sequence &&
      (stored.transaction_chain_hash == Hash128{} ||
       stored.transaction_chain_hash == chain_hash)) {
    // Already materialized; keep live history aligned with the reconstructed head.
    nlohmann::json head_params =
        recovered.head_pipeline_params().value_or(stored_pipeline_params.value_or(
            active.GetMaterializedParams().value_or(history->GetImportPipelineParams())));
    WorkingVersion working{identity.element_id, active.GetVersionID(), head_params,
                           recovered.transactions(), recovered.cursor()};
    history->UpdateVersionFromWorkingVersion(active.GetVersionID(), working, head_params);

    result.accepted                        = true;
    result.materialized                    = true;
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
  if (!storage_->GetElementController().MaterializeEditorState(history, pipeline, metadata,
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
