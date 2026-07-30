//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_materializer.hpp"

#include <stdexcept>
#include <utility>

#include "app/editor_mini_git_commit_writer.hpp"
#include "app/editor_mini_git_journal_fold.hpp"
#include "app/editor_mini_git_journal_recovery.hpp"
#include "app/editor_save_checkpoint_coordinator.hpp"
#include "storage/service/sleeve/edit_history/commit_graph_service.hpp"

namespace alcedo {
namespace {

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

/// Reject captures whose identity or sequence range is inconsistent before any
/// DuckDB write. Empty journals must not claim a sequence range; non-empty
/// journals must list contiguous records matching first/last.
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

  if (capture.candidate_publication && capture.base_active_version_id == version_ref_id_t{}) {
    SetError(error, "candidate publication is missing its base Version identity");
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

/// Truncate only the captured inclusive sequence range on the journal path so
/// any post-capture append remains durable for the next checkpoint.
auto TruncateCapturedJournalRange(const EditorMiniGitSaveCapture& capture, std::string* error)
    -> bool {
  if (!capture.has_journal_range()) {
    return true;
  }
  if (capture.journal_path.empty()) {
    return true;
  }
  MiniGitJournal journal(capture.journal_path);
  if (!journal.Load(error)) {
    return false;
  }
  return journal.TruncateThroughSequence(*capture.last_journal_sequence, error);
}

}  // namespace

// ── Helpers ─────────────────────────────────────────────────────────────────

auto MakeEditorSerializedPipelineState(const root_id_t& root_id, head_commit_hash_t head,
                                       const transaction_chain_hash_t& chain,
                                       const nlohmann::json& pipeline_params) -> nlohmann::json {
  return nlohmann::json{{"state_format_version", 1},
                        {"root_id", root_id.ToString()},
                        {"head_commit_hash", HeadCommitHashToStorage(head)},
                        {"transaction_chain_hash", chain.ToString()},
                        {"pipeline_params", pipeline_params}};
}

// ── Materializer ────────────────────────────────────────────────────────────

EditorMiniGitMaterializer::EditorMiniGitMaterializer(
    std::shared_ptr<StorageService>                  storage,
    std::shared_ptr<EditorSaveCheckpointCoordinator> coordinator)
    : storage_(std::move(storage)),
      coordinator_(std::move(coordinator)),
      writer_(std::make_unique<EditorMiniGitCommitWriter>(storage_)),
      recovery_(std::make_unique<EditorMiniGitJournalRecovery>(storage_)) {
  if (!storage_) {
    throw std::invalid_argument("EditorMiniGitMaterializer requires StorageService");
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
  // Do not re-acquire here — a second AcquireBlocking would deadlock on the
  // same coordinator while the service still owns the lock.
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

  bool head_moved = !capture.journal_records.empty();

  try {
    auto               db_guard = storage_->GetDBController().GetConnectionGuard();
    auto               db_lock  = db_guard.Lock();
    CommitGraphService graph_service(db_guard.conn_);
    auto               stored_graph = graph_service.LoadGraph(capture.element_id);

    if (!stored_graph.has_value()) {
      // First durable materialization for this image: write the live capture.
      auto write_result = writer_->Write(capture.materialization, error);
      if (!write_result.accepted) {
        result.error = write_result.error;
        return result;
      }
      head_moved = !capture.journal_records.empty();
    } else {
      auto       folded      = *stored_graph;
      const auto prior_head  = folded.GetActiveVersionRef().head_commit_hash;
      const auto prior_chain = folded.ChainHashForHead(prior_head);

      if (capture.candidate_publication) {
        const auto& stored_state = stored_graph->GetImageEditState();
        if (stored_state.active_version_id != capture.base_active_version_id ||
            stored_state.materialized_head_commit_hash != capture.base_materialized_head ||
            stored_state.materialized_transaction_chain_hash !=
                capture.base_materialized_transaction_chain_hash) {
          SetError(error, "candidate publication base no longer matches materialized history");
          result.error = error != nullptr ? *error : "stale candidate publication";
          return result;
        }

        if (capture.journal_records.empty()) {
          if (capture.base_working_head != prior_head ||
              capture.base_working_transaction_chain_hash != prior_chain) {
            SetError(error, "candidate publication working base disagrees with materialized history");
            result.error = error != nullptr ? *error : "candidate working base mismatch";
            return result;
          }
        } else {
          auto candidate_folded = *stored_graph;
          auto fold_result =
              EditorMiniGitJournalFold::Fold(candidate_folded, capture.journal_records, error);
          if (!fold_result.accepted) {
            result.error = fold_result.error;
            return result;
          }
          const auto folded_head = candidate_folded.GetActiveVersionRef().head_commit_hash;
          const auto folded_chain = candidate_folded.ChainHashForHead(folded_head);
          if (folded_head != capture.base_working_head ||
              folded_chain != capture.base_working_transaction_chain_hash) {
            SetError(error, "candidate publication journal fold does not match its working base");
            result.error = error != nullptr ? *error : "candidate journal fold mismatch";
            return result;
          }
        }
        head_moved = prior_head != capture.working_head || prior_chain != capture.transaction_chain_hash ||
                     stored_state.active_version_id != capture.version_id;
        auto write_result = writer_->Write(capture.materialization, error);
        if (!write_result.accepted) {
          result.error = write_result.error;
          return result;
        }
      } else if (capture.journal_records.empty()) {
        // Saving with no journal changes succeeds without moving the Version head.
        if (prior_head != capture.working_head || prior_chain != capture.transaction_chain_hash) {
          SetError(error,
                   "empty mini-Git journal but live head/hash disagree with materialized state");
          result.error = error != nullptr ? *error : "head mismatch on empty journal";
          return result;
        }
        auto materialization                                            = capture.materialization;
        materialization.image_state.materialized_head_commit_hash       = prior_head;
        materialization.image_state.materialized_transaction_chain_hash = prior_chain;
        // Keep Version refs at the stored heads; only refresh serialized state.
        auto write_result = writer_->Write(materialization, error);
        if (!write_result.accepted) {
          result.error = write_result.error;
          return result;
        }
        head_moved = false;
      } else {
        // Pure journal fold — no pipeline replay or mutation.
        auto fold_result = EditorMiniGitJournalFold::Fold(folded, capture.journal_records, error);
        if (!fold_result.accepted) {
          result.error = fold_result.error;
          return result;
        }
        const auto folded_head  = folded.GetActiveVersionRef().head_commit_hash;
        const auto folded_chain = folded.ChainHashForHead(folded_head);
        if (folded_head != capture.working_head || folded_chain != capture.transaction_chain_hash) {
          SetError(error, "mini-Git journal fold does not match the captured pipeline head/hash");
          result.error = error != nullptr ? *error : "journal fold mismatch";
          return result;
        }
        head_moved =
            prior_head != capture.working_head || prior_chain != capture.transaction_chain_hash;
        auto write_result = writer_->Write(capture.materialization, error);
        if (!write_result.accepted) {
          result.error = write_result.error;
          return result;
        }
      }
    }
  } catch (const std::exception& e) {
    SetError(error, e.what());
    result.error = e.what();
    return result;
  } catch (...) {
    SetError(error, "mini-Git materialization failed");
    result.error = error != nullptr ? *error : "materialization failed";
    return result;
  }

  // DuckDB transaction committed successfully.
  result.accepted           = true;
  result.database_committed = true;
  result.head_moved         = head_moved;

  // Truncate only the captured sequence prefix after DuckDB success.
  // Distinguish between DB-committed (safe) and fully materialized (journal
  // also truncated). Failure here means recovery will skip the redundant
  // prefix on the next open.
  bool truncation_succeeded = false;
  if (capture.has_journal_range() && !capture.journal_path.empty()) {
    // Pre-truncation hook: simulate failure to open the journal.
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
    truncation_succeeded = TruncateCapturedJournalRange(capture, &truncate_error);
    if (!truncation_succeeded) {
      if (error != nullptr && error->empty()) {
        *error = truncate_error;
      }
    }

    // Post-truncation hook: simulate flush failure.
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
    // Empty journal or no journal path: nothing to truncate.
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
