//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"
#include "edit/history/commit_graph.hpp"

namespace alcedo {
class MiniGitJournal;
class MiniGitWorkingHistory;
struct PipelineGuard;
class PipelineMgmtService;
}  // namespace alcedo

namespace alcedo::ui {

class EditorSessionPipelinePort;

/// Per-image history state owned by the queue-thread history unit. The command
/// queue is the sole mutation owner for graph, redo, pending-before, and
/// committed-snapshot fields. The pipeline guard is only a worker hand-off
/// identity here; no history reducer accesses its live executor.
struct HistoryWorkingState {
  std::shared_ptr<alcedo::PipelineGuard> pipeline_guard;
  std::shared_ptr<alcedo::MiniGitJournal> journal;
  std::unique_ptr<alcedo::MiniGitWorkingHistory> history;
  std::unordered_map<std::string, alcedo::EditorAdjustmentOperatorState> pending_before;
  alcedo::EditorRenderAdjustmentSnapshot root_snapshot;
  alcedo::EditorRenderAdjustmentSnapshot committed_snapshot;
  bool recovered_head = false;
};

/// Owns per-image WorkingState acquisition, release, and service-path
/// resolution. Delegated by EditorSessionHistoryPort; does not contain
/// Mini-Git traversal or payload-formatting logic.
class EditorHistoryState {
 public:
  /// Path-resolution services used by state acquisition.
  struct Services {
    std::function<std::filesystem::path(sl_element_id_t)> mini_git_journal_path;
  };

  void SetServices(Services services);
  void SetPipelinePort(std::shared_ptr<EditorSessionPipelinePort> pipeline_port);

  /// Load or create the working history for one image.
  auto EnsureWorkingState(sl_element_id_t element_id, std::string* error)
      -> std::shared_ptr<HistoryWorkingState>;

  /// Drop the working history state for one image.
  void ReleaseState(sl_element_id_t element_id);

  /// Resolve the pipeline port (may be expired).
  [[nodiscard]] auto PipelinePort() const -> std::shared_ptr<EditorSessionPipelinePort>;

  /// Resolve the pipeline service through the current pipeline port.
  [[nodiscard]] auto PipelineService() const -> std::shared_ptr<alcedo::PipelineMgmtService>;

  /// Compare the live working head with the last materialized head.
  auto HasUnmaterializedChanges(sl_element_id_t element_id, std::string* error) -> bool;

  /// Return the journal-path resolver for checkpoint capture.
  [[nodiscard]] auto JournalPathResolver() const
      -> std::function<std::filesystem::path(sl_element_id_t)>;

 private:
  Services services_{};
  mutable std::mutex mutex_;
  std::weak_ptr<EditorSessionPipelinePort> pipeline_port_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<HistoryWorkingState>> working_states_;
};

}  // namespace alcedo::ui
