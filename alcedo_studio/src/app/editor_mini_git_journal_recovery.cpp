//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_journal_recovery.hpp"

#include <fstream>
#include <utility>

#include "app/editor_mini_git_commit_writer.hpp"
#include "app/editor_mini_git_journal_fold.hpp"
#include "storage/service/sleeve/edit_history/commit_graph_service.hpp"

namespace alcedo {
namespace {

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

// ── Construction ────────────────────────────────────────────────────────────

EditorMiniGitJournalRecovery::EditorMiniGitJournalRecovery(std::shared_ptr<StorageService> storage)
    : storage_(std::move(storage)) {
  if (!storage_) {
    throw std::invalid_argument("EditorMiniGitJournalRecovery requires StorageService");
  }
}

// ── Recovery ────────────────────────────────────────────────────────────────

auto EditorMiniGitJournalRecovery::Recover(sl_element_id_t              element_id,
                                           const std::filesystem::path& journal_path,
                                           std::string*                 error) -> RecoveryResult {
  RecoveryResult result;

  MiniGitJournal journal(journal_path);
  if (!journal.Load(error)) {
    result.error = error != nullptr ? *error : "journal load failed";
    return result;
  }

  if (journal.records().empty()) {
    result.accepted     = true;
    result.materialized = false;
    return result;
  }

  try {
    auto               db_guard = storage_->GetDBController().GetConnectionGuard();
    auto               db_lock  = db_guard.Lock();
    CommitGraphService graph_service(db_guard.conn_);
    auto               stored_graph = graph_service.LoadGraph(element_id);
    if (!stored_graph.has_value()) {
      SetError(error, "mini-Git recovery requires a durable commit graph");
      result.error = error != nullptr ? *error : "missing graph";
      return result;
    }

    auto       graph       = *stored_graph;
    const auto prior_head  = graph.GetActiveVersionRef().head_commit_hash;
    const auto prior_chain = graph.ChainHashForHead(prior_head);

    auto       fold_result = EditorMiniGitJournalFold::Fold(graph, journal.records(), error);
    if (!fold_result.accepted) {
      result.error = fold_result.error;
      return result;
    }

    const auto folded_head  = graph.GetActiveVersionRef().head_commit_hash;
    const auto folded_chain = graph.ChainHashForHead(folded_head);

    if (prior_head == folded_head && prior_chain == folded_chain) {
      // Already fully materialized — only truncate leftover journal bytes.
      std::string truncate_error;
      (void)TruncateJournalFile(journal_path, &truncate_error);
      result.accepted     = true;
      result.materialized = false;
      return result;
    }

    // Recovery without a live pipeline: keep the previous serialized state.
    auto materialization = graph.CaptureMaterializationWithSerializedPipelineState(
        graph.GetImageEditState().serialized_pipeline_state);

    EditorMiniGitCommitWriter writer(storage_);
    auto                      write_result = writer.Write(materialization, error);
    if (!write_result.accepted) {
      result.error = write_result.error;
      return result;
    }

    result.materialized = true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    result.error = e.what();
    return result;
  } catch (...) {
    SetError(error, "mini-Git recovery failed");
    result.error = error != nullptr ? *error : "recovery failed";
    return result;
  }

  std::string truncate_error;
  (void)TruncateJournalFile(journal_path, &truncate_error);

  result.accepted = true;
  return result;
}

// ── Static helpers ──────────────────────────────────────────────────────────

auto EditorMiniGitJournalRecovery::TruncateJournalFile(const std::filesystem::path& path,
                                                       std::string*                 error) -> bool {
  try {
    if (path.empty() || !std::filesystem::exists(path)) {
      return true;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      SetError(error, "mini-Git journal file could not be truncated");
      return false;
    }
    output.flush();
    if (!output.good()) {
      SetError(error, "mini-Git journal file truncate failed");
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal truncate failed");
  }
  return false;
}

}  // namespace alcedo
