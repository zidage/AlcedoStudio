//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_projection.hpp"

#include <algorithm>
#include <mutex>

#include "app/pipeline_service.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {

EditorHistoryProjection::EditorHistoryProjection(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryProjection::ReadHistorySnapshot(
    const alcedo::EditorHistoryGuardHandle& guard, alcedo::EditorHistorySnapshot* snapshot,
    std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;

  struct ProjectionCommitSource {
    alcedo::EditCommit commit;
    alcedo::EditorHistoryTimelinePosition position;
  };
  alcedo::version_ref_id_t active_version_id{};
  alcedo::head_commit_hash_t active_head = std::nullopt;
  bool recovered_head = false;
  bool can_redo = false;
  std::vector<alcedo::EditorHistoryVersion> versions;
  std::vector<ProjectionCommitSource> commit_sources;
  {
    std::scoped_lock state_lock(state->mutex);
    if (snapshot == nullptr || !state->pipeline_guard || !state->pipeline_guard->commit_graph_ ||
        !state->history) {
      if (error) *error = "Editor history graph is unavailable";
      return false;
    }
    const auto& graph = *state->pipeline_guard->commit_graph_;
    active_version_id = graph.GetActiveVersionId();
    active_head = graph.GetActiveVersionRef().head_commit_hash;
    recovered_head = state->recovered_head;
    can_redo = state->history->redo_count() > 0;

    const auto& all_refs = graph.GetAllVersionRefs();
    versions.reserve(all_refs.size());
    for (const auto& [version_id, version] : all_refs) {
      versions.push_back({version_id, version.display_name, version.head_commit_hash,
                          version.created_at, version.updated_at,
                          version_id == active_version_id});
    }
    const auto redo_suffix = state->history->RedoSuffix();
    std::vector<alcedo::commit_hash_t> chain;
    if (active_head.has_value()) {
      chain = graph.FirstParentChain(active_head);
    }

    commit_sources.reserve(redo_suffix.size() + chain.size());
    for (const auto& hash : redo_suffix) {
      commit_sources.push_back(
          {graph.GetCommit(hash), alcedo::EditorHistoryTimelinePosition::Future});
    }
    if (active_head.has_value()) {
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto position = (it == chain.rbegin())
                                  ? alcedo::EditorHistoryTimelinePosition::Current
                                  : alcedo::EditorHistoryTimelinePosition::Applied;
        commit_sources.push_back({graph.GetCommit(*it), position});
      }
    }
  }

  alcedo::EditorHistorySnapshot projection;
  projection.active_version_id = active_version_id;
  projection.active_head = active_head;
  projection.recovered_head = recovered_head;
  projection.can_undo = active_head.has_value();
  projection.can_redo = can_redo;

  std::sort(versions.begin(), versions.end(), [](const auto& left, const auto& right) {
    if (left.created_at != right.created_at) return left.created_at < right.created_at;
    return left.version_id.ToString() < right.version_id.ToString();
  });
  projection.versions = std::move(versions);

  projection.commits.reserve(commit_sources.size());
  for (const auto& source : commit_sources) {
    projection.commits.push_back(CommitRowFromEdit(source.commit, source.position));
  }
  *snapshot = std::move(projection);
  return true;
}

auto EditorHistoryProjection::ReadAdjustmentSnapshot(
    const alcedo::EditorHistoryGuardHandle& guard,
    alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (snapshot) *snapshot = state->committed_snapshot;
  return true;
}

}  // namespace alcedo::ui
