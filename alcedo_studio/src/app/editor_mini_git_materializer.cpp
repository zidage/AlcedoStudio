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

auto SharedCoordinator() -> std::shared_ptr<EditorSaveCheckpointCoordinator> {
  static auto coordinator = std::make_shared<EditorSaveCheckpointCoordinator>();
  return coordinator;
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

EditorMiniGitMaterializer::EditorMiniGitMaterializer(std::shared_ptr<StorageService> storage)
    : storage_(std::move(storage)),
      coordinator_(SharedCoordinator()),
      writer_(std::make_unique<EditorMiniGitCommitWriter>(storage_)),
      recovery_(std::make_unique<EditorMiniGitJournalRecovery>(storage_)) {
  if (!storage_) {
    throw std::invalid_argument("EditorMiniGitMaterializer requires StorageService");
  }
}

auto EditorMiniGitMaterializer::Materialize(const EditorMiniGitSaveCapture& capture,
                                            std::string* error) -> EditorMiniGitMaterializeResult {
  EditorMiniGitMaterializeResult result;
  if (capture.element_id == 0) {
    SetError(error, "mini-Git materialize requires an element id");
    result.error = error != nullptr ? *error : "missing element";
    return result;
  }

  auto save_lock = AcquireGlobalSaveLock(*coordinator_, capture.element_id);

  try {
    capture.materialization.Validate();
  } catch (const std::exception& e) {
    SetError(error, e.what());
    result.error = e.what();
    return result;
  }

  bool head_moved = !capture.journal_already_materialized && !capture.journal_records.empty();

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

      if (capture.journal_already_materialized || capture.journal_records.empty()) {
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

  // Truncate only after DuckDB success. Failure here leaves a recoverable
  // redundant journal prefix; recovery skips already-materialized records.
  std::string truncate_error;
  (void)EditorMiniGitJournalRecovery::TruncateJournalFile(capture.journal_path, &truncate_error);

  result.accepted     = true;
  result.materialized = true;
  result.head_moved   = head_moved;
  return result;
}

auto EditorMiniGitMaterializer::RecoverAndMaterialize(sl_element_id_t              element_id,
                                                      const std::filesystem::path& journal_path,
                                                      std::string*                 error)
    -> EditorMiniGitMaterializeResult {
  EditorMiniGitMaterializeResult result;

  auto                           save_lock = AcquireGlobalSaveLock(*coordinator_, element_id);

  auto recovery_result                     = recovery_->Recover(element_id, journal_path, error);
  result.accepted                          = recovery_result.accepted;
  result.materialized                      = recovery_result.materialized;
  result.error                             = recovery_result.error;
  result.head_moved                        = recovery_result.materialized;
  return result;
}

}  // namespace alcedo
