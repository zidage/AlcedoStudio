//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"

#include <variant>

#include "edit/history/commit_graph.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/pipeline/pipeline_executor.hpp"

namespace alcedo::ui {

auto LockLivePipeline(alcedo::PipelineExecutor& executor) -> std::unique_lock<std::mutex> {
  // Sole live-pipeline ownership. History waits here for render to release the
  // lock after the full frame (configure + Apply + present). Do not call this
  // from the GUI thread while that thread is still required for present — the
  // session defers Version ops until render is idle so the GUI never blocks on
  // render, only history queues for ownership. Selected-node panel projection
  // must never call this.
  return std::unique_lock<std::mutex>(executor.GetRenderLock());
}

auto CommitFieldKey(const alcedo::EditCommit& commit) -> std::string {
  if (alcedo::IsPipelineEditBatchJson(commit.GetPayloadJSON())) {
    try {
      const auto batch = alcedo::PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
      const auto row   = alcedo::ProjectPipelineEditHistory(batch);
      return row.field_key;
    } catch (...) {
      return {};
    }
  }
  return {};
}

auto CommitRowFromEdit(const alcedo::EditCommit& commit,
                       alcedo::EditorHistoryTimelinePosition position)
    -> alcedo::EditorHistoryCommit {
  alcedo::EditorHistoryCommit row;
  row.commit_hash = commit.GetCommitHash();
  row.first_parent_hash = commit.GetFirstParentHash();
  row.created_at_ns = commit.GetCreatedAtNs();
  row.position = position;
  if (alcedo::IsPipelineEditBatchJson(commit.GetPayloadJSON())) {
    try {
      const auto batch = alcedo::PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
      const auto typed = alcedo::ProjectPipelineEditHistory(batch);
      row.operation_kind = std::string{alcedo::PipelineEditOperationKindText(typed.operation_kind)};
      row.presentation_key = typed.presentation_key;
      row.presentation_args_json =
          typed.presentation_args.is_null() ? std::string{} : typed.presentation_args.dump();
      row.node_id = typed.node_id;
      row.node_display_name = typed.node_display_name;
      row.adjustment_instance_id = typed.adjustment_instance_id;
      row.mask_id = typed.mask_id;
      row.mask_display_name = typed.mask_display_name;
      row.field_key = typed.field_key;
      row.before_value_json = typed.before_display_value.is_null()
                                  ? std::string{}
                                  : typed.before_display_value.dump();
      row.after_value_json =
          typed.after_display_value.is_null() ? std::string{} : typed.after_display_value.dump();
      if (const auto* parameter = std::get_if<alcedo::SetParameterChange>(&batch.changes.front())) {
        row.before_enabled = parameter->before_enabled;
        row.after_enabled  = parameter->after_enabled;
      }
    } catch (...) {
      row.field_key = CommitFieldKey(commit);
    }
  }
  return row;
}

auto VersionNameExists(const alcedo::CommitGraph& graph, const std::string& name,
                       const alcedo::version_ref_id_t* ignored) -> bool {
  for (const auto& [id, version] : graph.GetAllVersionRefs()) {
    if (ignored != nullptr && id == *ignored) continue;
    if (version.display_name == name) return true;
  }
  return false;
}

auto UniqueVersionName(const alcedo::CommitGraph& graph, std::string requested,
                       const alcedo::version_ref_id_t* ignored) -> std::string {
  const auto first = requested.find_first_not_of(" \t\r\n");
  const auto last = requested.find_last_not_of(" \t\r\n");
  requested =
      first == std::string::npos ? std::string{} : requested.substr(first, last - first + 1);
  if (requested.empty()) requested = "Version";
  if (!VersionNameExists(graph, requested, ignored)) return requested;
  for (std::size_t suffix = 2;; ++suffix) {
    auto candidate = requested + " " + std::to_string(suffix);
    if (!VersionNameExists(graph, candidate, ignored)) return candidate;
  }
}

}  // namespace alcedo::ui
