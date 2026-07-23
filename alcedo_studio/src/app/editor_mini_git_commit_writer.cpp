//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_commit_writer.hpp"

#include <stdexcept>
#include <utility>

#include "storage/service/sleeve/edit_history/commit_graph_service.hpp"

namespace alcedo {

EditorMiniGitCommitWriter::EditorMiniGitCommitWriter(std::shared_ptr<StorageService> storage)
    : storage_(std::move(storage)) {
  if (!storage_) {
    throw std::invalid_argument("EditorMiniGitCommitWriter requires StorageService");
  }
}

auto EditorMiniGitCommitWriter::Write(const CommitGraphMaterialization& materialization,
                                      std::string*                      error) -> WriteResult {
  try {
    materialization.Validate();
  } catch (const std::exception& e) {
    WriteResult result;
    result.accepted = false;
    result.error    = e.what();
    if (error) *error = result.error;
    return result;
  }

  try {
    auto               db_guard = storage_->GetDBController().GetConnectionGuard();
    auto               db_lock  = db_guard.Lock();
    CommitGraphService graph_service(db_guard.conn_);
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
