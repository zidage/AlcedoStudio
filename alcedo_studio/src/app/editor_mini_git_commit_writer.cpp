//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_commit_writer.hpp"

#include <stdexcept>
#include <utility>

#include "storage/store/edit_history/commit_graph_store.hpp"

namespace alcedo {

EditorMiniGitCommitWriter::EditorMiniGitCommitWriter(std::shared_ptr<Storage> storage)
    : storage_(std::move(storage)) {
  if (!storage_) {
    throw std::invalid_argument("EditorMiniGitCommitWriter requires Storage");
  }
}

auto EditorMiniGitCommitWriter::Write(const CommitGraphMaterialization& materialization,
                                      std::string*                      error) -> WriteResult {
  // Validate the materialization struct before any DuckDB interaction.
  try {
    materialization.Validate();
  } catch (const std::exception& e) {
    WriteResult result;
    result.accepted = false;
    result.error    = e.what();
    if (error) *error = result.error;
    return result;
  }

  // Pre-write hook: failure here simulates a failure before any durable writes.
  if (write_hook_ != nullptr) {
    std::string hook_error;
    if (!write_hook_->OnBeforeWrite(materialization, &hook_error)) {
      WriteResult result;
      result.accepted = false;
      result.error    = hook_error.empty() ? "write hook rejected" : std::move(hook_error);
      if (error) *error = result.error;
      return result;
    }
  }

  try {
    auto               db_guard = storage_->GetDatabase().GetConnectionGuard();
    auto               db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    graph_service.Materialize(materialization);
  } catch (const std::exception& e) {
    WriteResult result;
    result.accepted = false;
    result.error    = e.what();
    if (error) *error = result.error;
    return result;
  } catch (...) {
    WriteResult result;
    result.accepted = false;
    result.error    = "mini-Git commit write failed";
    if (error) *error = result.error;
    return result;
  }

  WriteResult result;
  result.accepted = true;
  return result;
}

}  // namespace alcedo
