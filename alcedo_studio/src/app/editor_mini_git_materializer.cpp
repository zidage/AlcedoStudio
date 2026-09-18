//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_materializer.hpp"

#include <stdexcept>
#include <utility>

#include "app/editor_mini_git_commit_writer.hpp"
#include "app/editor_mini_git_journal_recovery.hpp"
#include "app/editor_save_checkpoint_coordinator.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"

namespace alcedo {
namespace {

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

/// Reject captures whose identity is inconsistent before any DuckDB write.
/// Journal records may be present for diagnostics; normal save never folds them
/// into DuckDB — history + pipeline already live in capture.materialization.
auto ValidateSaveCapture(const EditorMiniGitSaveCapture& capture, std::string* error) -> bool {
  if (capture.element_id == 0) {
    SetError(error, "mini-Git materialize requires an element id");
    return false;
  }
  if (capture.materialization.image_state.element_id != capture.element_id) {
    SetError(error, "mini-Git capture element id does not match materialization state");
    return false;
  }
  if (capture.root_id != capture.materialization.image_state.root_id) {
    SetError(error, "mini-Git capture root id does not match materialization state");
    return false;
  }
  if (capture.version_id != capture.materialization.image_state.active_version_id) {
    SetError(error, "mini-Git capture version id does not match materialization state");
    return false;
  }
  if (capture.materialization.image_state.materialized_head_commit_hash != capture.working_head) {
    SetError(error, "mini-Git capture working head does not match materialization head");
    return false;
  }
  if (capture.materialization.image_state.materialized_transaction_chain_hash !=
      capture.transaction_chain_hash) {
    SetError(error, "mini-Git capture chain hash does not match materialization chain");
    return false;
  }

  if (capture.journal_records.empty()) {
    if (capture.first_journal_sequence.has_value() || capture.last_journal_sequence.has_value()) {
      SetError(error, "empty mini-Git journal must not claim a sequence range");
      return false;
    }
    return true;
  }

  if (!capture.first_journal_sequence.has_value() || !capture.last_journal_sequence.has_value()) {
    SetError(error, "non-empty mini-Git journal requires first and last sequence numbers");
    return false;
  }
  if (*capture.first_journal_sequence == 0 || *capture.last_journal_sequence == 0 ||
      *capture.first_journal_sequence > *capture.last_journal_sequence) {
    SetError(error, "mini-Git journal sequence range is invalid");
    return false;
  }
  if (capture.journal_records.front().sequence != *capture.first_journal_sequence ||
      capture.journal_records.back().sequence != *capture.last_journal_sequence) {
    SetError(error, "mini-Git journal records do not match the captured sequence range");
    return false;
  }
  std::uint64_t expected = *capture.first_journal_sequence;
  for (const auto& record : capture.journal_records) {
    if (record.sequence != expected) {
      SetError(error, "mini-Git journal capture sequence numbers are not contiguous");
      return false;
    }
    ++expected;
  }
  if (expected - 1 != *capture.last_journal_sequence) {
    SetError(error, "mini-Git journal capture sequence range length mismatch");
    return false;
  }
  return true;
}

/// Clear the entire WAL after history + pipeline checkpoint both succeed.
auto ClearEntireJournal(const EditorMiniGitSaveCapture& capture, std::string* error) -> bool {
  if (capture.journal_path.empty()) {
    return true;
  }
  return EditorMiniGitJournalRecovery::TruncateJournalFile(capture.journal_path, error);
}

}  // namespace

// ── Helpers ─────────────────────────────────────────────────────────────────

auto MakeEditorSerializedPipelineState(const root_id_t& root_id, head_commit_hash_t head,
                                       const transaction_chain_hash_t& chain,
                                       const PipelineDocument& document) -> nlohmann::json {
  return EncodePipelineDocumentCheckpoint(root_id, head, chain, document);
}

// ── Materializer ────────────────────────────────────────────────────────────

EditorMiniGitMaterializer::EditorMiniGitMaterializer(
    std::shared_ptr<Storage>                  storage,
    std::shared_ptr<EditorSaveCheckpointCoordinator> coordinator)
    : storage_(std::move(storage)),
      coordinator_(std::move(coordinator)),
      writer_(std::make_unique<EditorMiniGitCommitWriter>(storage_)),
      recovery_(std::make_unique<EditorMiniGitJournalRecovery>(storage_)) {
  if (!storage_) {
    throw std::invalid_argument("EditorMiniGitMaterializer requires Storage");
  }
  if (!coordinator_) {
    throw std::invalid_argument(
        "EditorMiniGitMaterializer requires a project-owned EditorSaveCheckpointCoordinator");
  }
}

auto EditorMiniGitMaterializer::Materialize(const EditorMiniGitSaveCapture& capture,
                                            std::string* error) -> EditorMiniGitMaterializeResult {
  // Precondition: the project-owned SaveCheckpointLock is already held by
  // EditorSaveCheckpointService (or a focused test that serializes access).
  EditorMiniGitMaterializeResult result;
  if (!ValidateSaveCapture(capture, error)) {
    result.error = error != nullptr ? *error : "invalid capture";
    return result;
  }

  try {
    capture.materialization.Validate();
  } catch (const std::exception& e) {
    SetError(error, e.what());
    result.error = e.what();
    return result;
  }

  bool head_moved = false;

  try {
    auto               db_guard = storage_->GetDatabase().GetConnectionGuard();
    auto               db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    auto               stored_graph = graph_service.LoadGraph(capture.element_id);

    head_commit_hash_t       prior_head  = std::nullopt;
    transaction_chain_hash_t prior_chain{};
    if (stored_graph.has_value()) {
      prior_head  = stored_graph->GetActiveVersionRef().head_commit_hash;
      prior_chain = stored_graph->ChainHashForHead(prior_head);
    }

    // Normal save: persist the unique in-memory history + pipeline checkpoint.
    // Never decode WAL records into DuckDB commits — capture.materialization is
    // already the complete durable snapshot of the unique history instance.
    auto write_result = writer_->Write(capture.materialization, error);
    if (!write_result.accepted) {
      result.error = write_result.error;
      return result;
    }

    // Verify DB HEAD == capture memory logical HEAD after write.
    auto reloaded = graph_service.LoadGraph(capture.element_id);
    if (!reloaded.has_value()) {
      SetError(error, "mini-Git materialize could not reload history after write");
      result.error = error != nullptr ? *error : "reload failed after write";
      return result;
    }
    const auto db_head  = reloaded->GetActiveVersionRef().head_commit_hash;
    const auto db_chain = reloaded->ChainHashForHead(db_head);
    if (db_head != capture.working_head || db_chain != capture.transaction_chain_hash) {
      SetError(error, "mini-Git materialize DB HEAD does not match memory logical HEAD");
      result.error = error != nullptr ? *error : "HEAD mismatch after write";
      return result;
    }
    const auto& image_state = reloaded->GetImageEditState();
    if (image_state.materialized_head_commit_hash != capture.working_head ||
        image_state.materialized_transaction_chain_hash != capture.transaction_chain_hash) {
      SetError(error, "mini-Git materialize checkpoint HEAD does not match memory logical HEAD");
      result.error = error != nullptr ? *error : "checkpoint HEAD mismatch";
      return result;
    }

    head_moved = prior_head != capture.working_head || prior_chain != capture.transaction_chain_hash;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    result.error = e.what();
    return result;
  } catch (...) {
    SetError(error, "mini-Git materialization failed");
    result.error = error != nullptr ? *error : "materialization failed";
    return result;
  }

  // DuckDB history + pipeline checkpoint committed successfully.
  result.accepted           = true;
  result.database_committed = true;
  result.head_moved         = head_moved;

  // Clear the entire WAL only after both history and pipeline checkpoint succeed.
  // On failure leave WAL intact for recovery.
  bool truncation_succeeded = false;
  if (!capture.journal_path.empty()) {
    if (truncation_hook_ != nullptr) {
      std::string hook_error;
      if (!truncation_hook_->OnBeforeTruncate(capture.journal_path, &hook_error)) {
        if (error != nullptr && error->empty()) {
          *error = hook_error;
        }
        result.materialized = false;
        return result;
      }
    }

    std::string truncate_error;
    truncation_succeeded = ClearEntireJournal(capture, &truncate_error);
    if (!truncation_succeeded) {
      if (error != nullptr && error->empty()) {
        *error = truncate_error;
      }
    }

    if (truncation_succeeded && truncation_hook_ != nullptr) {
      std::string hook_error;
      if (!truncation_hook_->OnAfterTruncate(capture.journal_path, &hook_error)) {
        if (error != nullptr && error->empty()) {
          *error = hook_error;
        }
        truncation_succeeded = false;
        result.materialized  = false;
        return result;
      }
    }
  } else {
    truncation_succeeded = true;
  }

  result.materialized = truncation_succeeded;
  return result;
}

auto EditorMiniGitMaterializer::RecoverAndMaterialize(sl_element_id_t              element_id,
                                                      const std::filesystem::path& journal_path,
                                                      std::string*                 error)
    -> EditorMiniGitMaterializeResult {
  EditorMiniGitMaterializeResult result;

  // Recovery is not driven by EditorSaveCheckpointService, so it must take the
  // project-owned lock itself. AcquireBlocking exits cleanly after Shutdown().
  auto save_lock = coordinator_->AcquireBlocking(element_id);
  if (!save_lock.owns_lock()) {
    SetError(error, "project save checkpoint coordinator is shutting down");
    result.error = error != nullptr ? *error : "save coordinator shutting down";
    return result;
  }

  auto recovery_result = recovery_->Recover(element_id, journal_path, error);
  result.accepted      = recovery_result.accepted;
  result.materialized  = recovery_result.materialized;
  result.error         = recovery_result.error;
  result.head_moved    = recovery_result.materialized;
  return result;
}

}  // namespace alcedo
