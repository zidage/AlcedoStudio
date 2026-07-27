//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"

#include <algorithm>
#include <ctime>
#include <mutex>
#include <utility>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"

namespace alcedo::ui {

const std::array<std::string_view, 21> kEditorSnapshotFields = {
    "exposure",   "contrast",   "white",     "black",      "shadows",     "highlights",
    "curve",      "saturation", "vibrance",  "hls",        "color_wheel", "lut",
    "clarity",    "sharpen",    "odt",       "film_grain", "halation",    "crop_rotate",
    "raw_decode", "lens_calib", "color_temp"};

void UpsertCommittedSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                             const std::string& field_key, const nlohmann::json& params) {
  if (snapshot == nullptr) return;
  alcedo::EditorAdjustmentPatch patch{field_key, params.is_null() ? std::string{} : params.dump(),
                                      true};
  auto existing = std::find_if(
      snapshot->patches.begin(), snapshot->patches.end(),
      [&](const alcedo::EditorAdjustmentPatch& current) { return current.field_key == field_key; });
  if (existing == snapshot->patches.end()) {
    snapshot->patches.push_back(std::move(patch));
  } else {
    *existing = std::move(patch);
  }
  ++snapshot->snapshot_generation;
  snapshot->params_json = params.is_null() ? std::string{} : params.dump();
  snapshot->fingerprint.clear();
  for (const auto& current : snapshot->patches) {
    if (!snapshot->fingerprint.empty()) snapshot->fingerprint += "|";
    snapshot->fingerprint += current.field_key;
  }
}

auto EnabledForAdjustmentParams(const nlohmann::json& params) -> bool {
  if (params.is_object() && params.contains("enabled") && params.at("enabled").is_boolean()) {
    return params.at("enabled").get<bool>();
  }
  if (params.is_object() && params.size() == 1 && params.begin().value().is_object()) {
    const auto& nested = params.begin().value();
    if (nested.contains("enabled") && nested.at("enabled").is_boolean()) {
      return nested.at("enabled").get<bool>();
    }
  }
  return true;
}

auto CommitFieldKey(const alcedo::EditCommit& commit) -> std::string {
  if (commit.GetKind() == alcedo::EditCommitKind::kMerge) {
    return "merge";
  }
  try {
    const auto payload = alcedo::OrdinaryEditPayload::FromJSON(commit.GetPayloadJSON());
    const auto key = alcedo::EditorAdjustmentFieldKey(payload.stage_name, payload.operator_type);
    return key.has_value() ? *key : std::string{};
  } catch (...) {
    return {};
  }
}

auto CommitRowFromEdit(const alcedo::EditCommit& commit,
                       alcedo::EditorHistoryTimelinePosition position)
    -> alcedo::EditorHistoryCommit {
  alcedo::EditorHistoryCommit row;
  row.commit_hash = commit.GetCommitHash();
  row.first_parent_hash = commit.GetFirstParentHash();
  row.second_parent_hash = commit.GetSecondParentHash();
  row.kind = commit.GetKind();
  row.created_at_ns = commit.GetCreatedAtNs();
  row.position = position;
  if (commit.GetKind() == alcedo::EditCommitKind::kMerge) {
    row.field_key = "merge";
    try {
      const auto merge_payload = alcedo::MergeEditPayload::FromJSON(commit.GetPayloadJSON());
      row.merge_field_keys.reserve(merge_payload.fields.size());
      for (const auto& delta : merge_payload.fields) {
        const auto key = alcedo::EditorAdjustmentFieldKey(delta.stage_name, delta.operator_type);
        row.merge_field_keys.push_back(key.value_or(delta.field_name));
      }
    } catch (...) {
    }
    return row;
  }
  try {
    const auto payload = alcedo::OrdinaryEditPayload::FromJSON(commit.GetPayloadJSON());
    const auto key = alcedo::EditorAdjustmentFieldKey(payload.stage_name, payload.operator_type);
    row.field_key = key.value_or(std::string{});
    row.before_value_json =
        payload.before_value.is_null() ? std::string{} : payload.before_value.dump();
    row.after_value_json =
        payload.after_value.is_null() ? std::string{} : payload.after_value.dump();
    row.before_enabled = payload.before_enabled;
    row.after_enabled = payload.after_enabled;
  } catch (...) {
    row.field_key = CommitFieldKey(commit);
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

auto ReadPipelineSnapshot(alcedo::PipelineGuard& guard,
                          alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
    -> bool {
  if (snapshot == nullptr || !guard.pipeline_) {
    if (error) *error = "Editor pipeline is unavailable for history operation";
    return false;
  }
  return InitializeCommittedSnapshotFromPipeline(guard, snapshot, error);
}

auto InitializeCommittedSnapshotFromPipeline(alcedo::PipelineGuard& guard,
                                             alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                             std::string* error) -> bool {
  if (snapshot == nullptr || !guard.pipeline_) {
    if (error) *error = "Editor pipeline is unavailable for snapshot initialization";
    return false;
  }

  std::unique_lock<std::mutex> render_lock(guard.pipeline_->GetRenderLock());
  *snapshot = {};
  for (const auto field_key : kEditorSnapshotFields) {
    alcedo::EditorAdjustmentOperatorState state;
    if (!alcedo::ReadEditorAdjustmentOperatorState(*guard.pipeline_, std::string(field_key), &state,
                                                   error)) {
      return false;
    }
    if (state.params.is_object()) {
      UpsertCommittedSnapshot(snapshot, std::string(field_key), state.params);
    }
  }
  snapshot->params_json = guard.pipeline_->ExportPipelineParams().dump();
  return true;
}

auto ApplyCommittedPayload(alcedo::PipelineGuard& guard,
                           alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                           const alcedo::OrdinaryEditPayload& payload, bool use_after_value,
                           std::string* error) -> bool {
  if (!guard.pipeline_) {
    if (error) *error = "Editor pipeline is unavailable";
    return false;
  }
  const auto field_key =
      alcedo::EditorAdjustmentFieldKey(payload.stage_name, payload.operator_type);
  if (!field_key.has_value()) {
    if (error) *error = "Committed adjustment does not map to a QML editor field";
    return false;
  }
  const auto spec = alcedo::ResolveEditorAdjustmentField(*field_key);
  if (!spec.has_value()) {
    if (error) *error = "Committed adjustment field mapping is unavailable";
    return false;
  }
  alcedo::EditorAdjustmentOperatorState state;
  state.params = use_after_value ? payload.after_value : payload.before_value;
  state.enabled = use_after_value ? payload.after_enabled : payload.before_enabled;
  std::unique_lock<std::mutex> render_lock(guard.pipeline_->GetRenderLock());
  if (!alcedo::ApplyEditorAdjustmentOperatorState(*guard.pipeline_, *spec, state, error)) {
    return false;
  }
  UpsertCommittedSnapshot(snapshot, *field_key, state.params);
  return true;
}

auto ApplyRecoveredRecord(alcedo::PipelineGuard& guard,
                          alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                          alcedo::CommitGraph* replay_graph,
                          const alcedo::MiniGitJournalRecord& record, std::string* error) -> bool {
  if (replay_graph == nullptr) {
    if (error) *error = "Recovery commit graph is unavailable";
    return false;
  }
  if (record.kind == alcedo::MiniGitJournalRecordKind::kEditCommit && record.edit_commit) {
    const auto payload =
        alcedo::OrdinaryEditPayload::FromJSON(record.edit_commit->GetPayloadJSON());
    if (!ApplyCommittedPayload(guard, snapshot, payload, true, error)) return false;
  } else if (record.kind == alcedo::MiniGitJournalRecordKind::kHeadMove) {
    const auto source_head = replay_graph->GetActiveVersionRef().head_commit_hash;
    bool backward = false;
    if (record.target_head.has_value()) {
      if (source_head.has_value()) {
        const auto source_chain = replay_graph->FirstParentChain(source_head);
        backward = std::find(source_chain.begin(), source_chain.end(), *record.target_head) !=
                   source_chain.end();
      }
    } else {
      backward = source_head.has_value();
    }
    if (backward) {
      if (source_head.has_value()) {
        const auto chain = replay_graph->FirstParentChain(source_head);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
          if (record.target_head.has_value() && *it == *record.target_head) break;
          const auto& commit = replay_graph->GetCommit(*it);
          if (commit.GetKind() == alcedo::EditCommitKind::kMerge) continue;
          const auto payload = alcedo::OrdinaryEditPayload::FromJSON(commit.GetPayloadJSON());
          if (!ApplyCommittedPayload(guard, snapshot, payload, false, error)) return false;
        }
      }
    } else if (record.target_head.has_value()) {
      const auto chain = replay_graph->FirstParentChain(*record.target_head);
      std::vector<alcedo::commit_hash_t> forward;
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (source_head.has_value() && *it == *source_head) break;
        forward.push_back(*it);
      }
      std::reverse(forward.begin(), forward.end());
      for (const auto& hash : forward) {
        const auto& commit = replay_graph->GetCommit(hash);
        if (commit.GetKind() == alcedo::EditCommitKind::kMerge) continue;
        const auto payload = alcedo::OrdinaryEditPayload::FromJSON(commit.GetPayloadJSON());
        if (!ApplyCommittedPayload(guard, snapshot, payload, true, error)) return false;
      }
    }
  }
  return alcedo::MiniGitWorkingHistory::Replay(*replay_graph, {record}, error);
}

void RestoreGraphAndPipeline(alcedo::CommitGraph& graph, const alcedo::CommitGraph& prior_graph,
                             alcedo::PipelineMgmtService& pipeline_service,
                             const std::shared_ptr<alcedo::PipelineGuard>& pipeline_guard,
                             const alcedo::head_commit_hash_t& prior_head,
                             const alcedo::transaction_chain_hash_t& prior_chain,
                             bool prior_dirty, bool prior_serialized_state_needs_writeback) {
  graph = prior_graph;
  std::string ignored_error;
  (void)pipeline_service.RebuildActiveEditorPipeline(pipeline_guard, &ignored_error);
  pipeline_guard->working_head_commit_hash_ = prior_head;
  pipeline_guard->transaction_chain_hash_ = prior_chain;
  pipeline_guard->dirty_ = prior_dirty;
  pipeline_guard->serialized_state_needs_writeback_ = prior_serialized_state_needs_writeback;
}

}  // namespace alcedo::ui
