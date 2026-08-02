//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"

#include <algorithm>
#include <ctime>
#include <optional>
#include <utility>

#include "app/editor_adjustment_pipeline.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"

namespace alcedo::ui {

auto LockLivePipeline(alcedo::CPUPipelineExecutor& executor) -> std::unique_lock<std::mutex> {
  // Sole live-pipeline ownership. History waits here for render to release the
  // lock after the full frame (configure + Apply + present). Do not call this
  // from the GUI thread while that thread is still required for present — the
  // session defers Version ops until render is idle so the GUI never blocks on
  // render, only history queues for ownership.
  return std::unique_lock<std::mutex>(executor.GetRenderLock());
}

const std::array<std::string_view, 22> kEditorSnapshotFields = {
    "exposure",   "contrast",   "white",     "black",      "shadows",     "highlights",
    "curve",      "saturation", "vibrance",  "tint",       "hls",         "color_wheel",
    "lut",        "clarity",    "sharpen",   "odt",        "film_grain",  "halation",
    "crop_rotate", "raw_decode", "lens_calib", "color_temp"};

namespace {

auto IsFullPipelineParamsDocument(const nlohmann::json& params) -> bool {
  if (!params.is_object()) return false;
  return params.contains("Image Loading") || params.contains("Basic Adjustment") ||
         params.contains("Color Adjustment") || params.contains("Output Transform");
}

auto StageNameForSnapshotField(const std::string& field_key) -> std::string {
  const auto spec = alcedo::ResolveEditorAdjustmentField(field_key);
  if (!spec.has_value()) return {};
  switch (spec->stage_name) {
    case alcedo::PipelineStageName::Image_Loading:
      return "Image Loading";
    case alcedo::PipelineStageName::Geometry_Adjustment:
      return "Geometry Adjustment";
    case alcedo::PipelineStageName::To_WorkingSpace:
      return "To Working Space";
    case alcedo::PipelineStageName::Basic_Adjustment:
      return "Basic Adjustment";
    case alcedo::PipelineStageName::Color_Adjustment:
      return "Color Adjustment";
    case alcedo::PipelineStageName::Detail_Adjustment:
      return "Detail Adjustment";
    case alcedo::PipelineStageName::Output_Transform:
      return "Output Transform";
    default:
      return {};
  }
}

auto ScriptNameForSnapshotField(const std::string& field_key) -> std::string {
  if (field_key == "hls") return "HLS";
  if (field_key == "lut") return "ocio_lmt";
  return field_key;
}

auto ReadPipelineOperatorEntry(const nlohmann::json& pipeline_params, const std::string& field_key,
                               nlohmann::json* params, bool* enabled) -> bool {
  const auto spec = alcedo::ResolveEditorAdjustmentField(field_key);
  if (!spec.has_value()) return false;
  const auto stage_name = StageNameForSnapshotField(field_key);
  if (stage_name.empty() || !pipeline_params.contains(stage_name) ||
      !pipeline_params.at(stage_name).is_object()) {
    return false;
  }
  const auto& stage = pipeline_params.at(stage_name);
  if (!stage.contains(stage_name) || !stage.at(stage_name).is_object()) return false;
  for (const auto& [unused_name, entry] : stage.at(stage_name).items()) {
    if (!entry.is_object() || !entry.contains("type") || !entry.contains("params")) continue;
    try {
      if (entry.at("type").get<int>() != static_cast<int>(spec->operator_type)) continue;
      if (params != nullptr) *params = entry.value("params", nlohmann::json::object());
      if (enabled != nullptr) *enabled = entry.value("enable", true);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }
  return false;
}

auto SetSnapshotParamsJson(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                           const nlohmann::json& params) -> void {
  if (snapshot == nullptr) return;
  snapshot->params_json = params.is_null() ? std::string{} : params.dump();
}

}  // namespace

void UpsertCommittedSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                             const std::string& field_key, const nlohmann::json& params,
                             bool enabled) {
  if (snapshot == nullptr) return;
  alcedo::EditorAdjustmentPatch patch{field_key, params.is_null() ? std::string{} : params.dump(),
                                      true, enabled};
  auto existing = std::find_if(
      snapshot->patches.begin(), snapshot->patches.end(),
      [&](const alcedo::EditorAdjustmentPatch& current) { return current.field_key == field_key; });
  if (existing == snapshot->patches.end()) {
    snapshot->patches.push_back(std::move(patch));
  } else {
    *existing = std::move(patch);
  }
  ++snapshot->snapshot_generation;
  if (!IsFullPipelineParamsDocument(
          snapshot->params_json.empty() ? nlohmann::json{} : nlohmann::json::parse(
              snapshot->params_json, nullptr, false))) {
    SetSnapshotParamsJson(snapshot, params);
  }
  snapshot->fingerprint.clear();
  for (const auto& current : snapshot->patches) {
    if (!snapshot->fingerprint.empty()) snapshot->fingerprint += "|";
    snapshot->fingerprint += current.field_key;
  }
}

auto MakeEmptyCompleteAdjustmentSnapshot() -> alcedo::EditorRenderAdjustmentSnapshot {
  alcedo::EditorRenderAdjustmentSnapshot snapshot;
  for (const auto field_key : kEditorSnapshotFields) {
    UpsertCommittedSnapshot(&snapshot, std::string(field_key), nlohmann::json::object(), true);
  }
  snapshot.params_json.clear();
  return snapshot;
}

auto MakePipelineParamsFromSnapshot(
    const alcedo::EditorRenderAdjustmentSnapshot& snapshot, std::string* error)
    -> std::optional<nlohmann::json> {
  if (!IsCompleteAdjustmentSnapshot(snapshot, error)) return std::nullopt;
  nlohmann::json pipeline_params = nlohmann::json::object();
  try {
    for (const auto field_key_view : kEditorSnapshotFields) {
      const std::string field_key(field_key_view);
      const auto spec = alcedo::ResolveEditorAdjustmentField(field_key);
      const auto stage_name = StageNameForSnapshotField(field_key);
      if (!spec.has_value() || stage_name.empty()) {
        if (error) *error = "Unsupported editor adjustment field: " + field_key;
        return std::nullopt;
      }
      alcedo::EditorAdjustmentOperatorState state;
      if (!ReadCommittedAdjustmentState(snapshot, field_key, &state, error)) {
        return std::nullopt;
      }
      pipeline_params[stage_name][stage_name][ScriptNameForSnapshotField(field_key)] = {
          {"params", state.params},
          {"enable", state.enabled},
          {"type", static_cast<int>(spec->operator_type)}};
    }
    return pipeline_params;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return std::nullopt;
  }
}

auto MakeAdjustmentSnapshotFromPipelineParams(
    const nlohmann::json& pipeline_params, alcedo::EditorRenderAdjustmentSnapshot* snapshot,
    std::string* error) -> bool {
  if (snapshot == nullptr) {
    if (error) *error = "Adjustment snapshot output is null";
    return false;
  }
  if (!pipeline_params.is_object()) {
    if (error) *error = "Serialized pipeline parameters must be a JSON object";
    return false;
  }

  try {
    *snapshot = MakeEmptyCompleteAdjustmentSnapshot();
    for (const auto field_key : kEditorSnapshotFields) {
      nlohmann::json params = nlohmann::json::object();
      bool enabled           = true;
      (void)ReadPipelineOperatorEntry(pipeline_params, std::string(field_key), &params, &enabled);
      UpsertCommittedSnapshot(snapshot, std::string(field_key), params, enabled);
    }
    snapshot->params_json = pipeline_params.dump();
    return IsCompleteAdjustmentSnapshot(*snapshot, error);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto MakeAdjustmentSnapshotFromLivePipeline(alcedo::CPUPipelineExecutor& executor,
                                            alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                            std::string* error) -> bool {
  if (snapshot == nullptr) {
    if (error) *error = "Adjustment snapshot output is null";
    return false;
  }
  try {
    *snapshot = MakeEmptyCompleteAdjustmentSnapshot();
    for (const auto field_key_view : kEditorSnapshotFields) {
      const std::string field_key(field_key_view);
      alcedo::EditorAdjustmentOperatorState state;
      std::string local_error;
      if (!alcedo::ReadEditorAdjustmentOperatorState(executor, field_key, &state, &local_error)) {
        if (error) *error = local_error;
        return false;
      }
      // Missing operator → empty params / disabled; still keep the field slot.
      UpsertCommittedSnapshot(snapshot, field_key,
                              state.params.is_null() ? nlohmann::json::object() : state.params,
                              state.enabled);
    }
    // Full pipeline export remains available for checkpoint serialization that
    // still expects a stage document; panel load paths should use field GetParams.
    snapshot->params_json = executor.ExportPipelineParams().dump();
    return IsCompleteAdjustmentSnapshot(*snapshot, error);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto IsCompleteAdjustmentSnapshot(const alcedo::EditorRenderAdjustmentSnapshot& snapshot,
                                  std::string* error) -> bool {
  if (snapshot.patches.size() != kEditorSnapshotFields.size()) {
    if (error) {
      *error = "Committed adjustment snapshot must contain all supported editor fields";
    }
    return false;
  }
  for (const auto field_key : kEditorSnapshotFields) {
    const auto count = std::count_if(
        snapshot.patches.begin(), snapshot.patches.end(), [&](const auto& patch) {
          return patch.field_key == field_key;
        });
    if (count != 1) {
      if (error) {
        *error = "Committed adjustment snapshot field is missing or duplicated: " +
                 std::string(field_key);
      }
      return false;
    }
    if (!alcedo::ResolveEditorAdjustmentField(std::string(field_key)).has_value()) {
      if (error) *error = "Unsupported editor adjustment field: " + std::string(field_key);
      return false;
    }
  }
  return true;
}

auto ReadCommittedAdjustmentState(const alcedo::EditorRenderAdjustmentSnapshot& snapshot,
                                  const std::string& field_key,
                                  alcedo::EditorAdjustmentOperatorState* state,
                                  std::string* error) -> bool {
  if (state == nullptr) {
    if (error) *error = "Adjustment state output is null";
    return false;
  }
  if (!IsCompleteAdjustmentSnapshot(snapshot, error)) return false;
  if (!alcedo::ResolveEditorAdjustmentField(field_key).has_value()) {
    if (error) *error = "Unsupported editor adjustment field: " + field_key;
    return false;
  }
  const auto found = std::find_if(
      snapshot.patches.begin(), snapshot.patches.end(), [&](const auto& patch) {
        return patch.field_key == field_key;
      });
  if (found == snapshot.patches.end()) {
    if (error) *error = "Committed adjustment snapshot is missing field: " + field_key;
    return false;
  }
  try {
    state->params  = found->params_json.empty() ? nlohmann::json::object()
                                                : nlohmann::json::parse(found->params_json);
    state->enabled = found->enabled;
    if (!state->params.is_object()) {
      if (error) *error = "Committed adjustment params must be a JSON object: " + field_key;
      return false;
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
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

namespace {

auto ResolveSnapshotFieldAtHead(const alcedo::CommitGraph& graph,
                                const alcedo::head_commit_hash_t& head,
                                alcedo::OperatorType operator_type,
                                alcedo::PipelineStageName stage_name,
                                alcedo::EditorAdjustmentOperatorState* state,
                                std::string* error) -> bool {
  if (state == nullptr) {
    if (error) *error = "Field resolve output is null";
    return false;
  }
  try {
    const auto chain = graph.FirstParentChain(head);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const auto& commit = graph.GetCommit(*it);
      if (commit.GetKind() == alcedo::EditCommitKind::kEdit) {
        const auto payload = alcedo::OrdinaryEditPayload::FromJSON(commit.GetPayloadJSON());
        if (payload.operator_type == operator_type && payload.stage_name == stage_name) {
          state->params  = payload.after_value.is_null() ? nlohmann::json::object()
                                                         : payload.after_value;
          state->enabled = payload.after_enabled;
          return true;
        }
      } else if (commit.GetKind() == alcedo::EditCommitKind::kMerge) {
        const auto payload = alcedo::MergeEditPayload::FromJSON(commit.GetPayloadJSON());
        for (const auto& field : payload.fields) {
          if (field.operator_type == operator_type && field.stage_name == stage_name) {
            state->params = field.resolved_value.is_null() ? nlohmann::json::object()
                                                           : field.resolved_value;
            state->enabled = field.resolved_enabled;
            return true;
          }
        }
      }
    }
    // The complete root snapshot represents this explicit default. A history commit may refer
    // to a field that has not yet been written on the first-parent chain.
    state->params  = nlohmann::json::object();
    state->enabled = true;
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto ApplySnapshotState(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                        const std::string& field_key,
                        const alcedo::EditorAdjustmentOperatorState& state,
                        std::string* error) -> bool {
  if (snapshot == nullptr) {
    if (error) *error = "Adjustment snapshot output is null";
    return false;
  }
  if (!alcedo::ResolveEditorAdjustmentField(field_key).has_value()) {
    if (error) *error = "Unsupported editor adjustment field: " + field_key;
    return false;
  }
  if (!state.params.is_object() && !state.params.is_null()) {
    if (error) *error = "Committed adjustment params must be a JSON object: " + field_key;
    return false;
  }
  UpsertCommittedSnapshot(snapshot, field_key,
                         state.params.is_null() ? nlohmann::json::object() : state.params,
                         state.enabled);
  snapshot->params_json.clear();
  return true;
}

auto SnapshotFieldForPayload(const alcedo::OrdinaryEditPayload& payload,
                             std::string* error) -> std::optional<std::string> {
  const auto field_key =
      alcedo::EditorAdjustmentFieldKey(payload.stage_name, payload.operator_type);
  if (!field_key.has_value() && error) {
    *error = "Committed adjustment does not map to a supported editor field";
  }
  return field_key;
}

}  // namespace

auto ApplyCommittedPayloadToSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                     const alcedo::OrdinaryEditPayload& payload,
                                     bool use_after_value, std::string* error) -> bool {
  if (snapshot == nullptr) {
    if (error) *error = "Adjustment snapshot output is null";
    return false;
  }
  if (!IsCompleteAdjustmentSnapshot(*snapshot, error)) return false;
  const auto field_key = SnapshotFieldForPayload(payload, error);
  if (!field_key.has_value()) return false;
  alcedo::EditorAdjustmentOperatorState state;
  state.params  = use_after_value ? payload.after_value : payload.before_value;
  state.enabled = use_after_value ? payload.after_enabled : payload.before_enabled;
  return ApplySnapshotState(snapshot, *field_key, state, error);
}

auto ApplyHistoryCommitToSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                  const alcedo::CommitGraph& graph,
                                  const alcedo::EditCommit& commit, bool use_after_value,
                                  std::string* error) -> bool {
  if (snapshot == nullptr || !IsCompleteAdjustmentSnapshot(*snapshot, error)) return false;
  try {
    if (commit.GetKind() == alcedo::EditCommitKind::kEdit) {
      return ApplyCommittedPayloadToSnapshot(
          snapshot, alcedo::OrdinaryEditPayload::FromJSON(commit.GetPayloadJSON()), use_after_value,
          error);
    }
    if (commit.GetKind() != alcedo::EditCommitKind::kMerge) {
      if (error) *error = "Unknown history commit kind";
      return false;
    }

    const auto payload = alcedo::MergeEditPayload::FromJSON(commit.GetPayloadJSON());
    for (const auto& field : payload.fields) {
      alcedo::EditorAdjustmentOperatorState state;
      if (use_after_value) {
        state.params  = field.resolved_value;
        state.enabled = field.resolved_enabled;
      } else {
        state.params  = field.before_value;
        state.enabled = field.before_enabled;
      }
      const auto field_key = alcedo::EditorAdjustmentFieldKey(field.stage_name, field.operator_type);
      if (!field_key.has_value()) {
        if (error) *error = "Merge field does not map to a supported editor field";
        return false;
      }
      if (!ApplySnapshotState(snapshot, *field_key, state, error)) return false;
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto ApplyPreparedHeadMoveToSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                     const alcedo::CommitGraph& graph,
                                     const alcedo::MiniGitPreparedHeadMove& prepared,
                                     std::string* error) -> bool {
  if (snapshot == nullptr) {
    if (error) *error = "Adjustment snapshot output is null";
    return false;
  }
  if (!IsCompleteAdjustmentSnapshot(*snapshot, error)) return false;
  const auto prior = *snapshot;
  for (const auto& commit : prepared.traversed_commits) {
    if (!ApplyHistoryCommitToSnapshot(snapshot, graph, commit, !prepared.backward, error)) {
      *snapshot = prior;
      return false;
    }
  }
  return true;
}

auto SnapshotAtHead(const alcedo::EditorRenderAdjustmentSnapshot& root_snapshot,
                    const alcedo::CommitGraph& graph, const alcedo::head_commit_hash_t& head,
                    alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error) -> bool {
  if (snapshot == nullptr) {
    if (error) *error = "Adjustment snapshot output is null";
    return false;
  }
  if (!IsCompleteAdjustmentSnapshot(root_snapshot, error)) return false;
  *snapshot = root_snapshot;
  try {
    for (const auto& hash : graph.FirstParentChain(head)) {
      if (!ApplyHistoryCommitToSnapshot(snapshot, graph, graph.GetCommit(hash), true, error)) {
        return false;
      }
    }
    // A derived snapshot is represented by its complete field map. Do not carry a stale full
    // pipeline document from the materialized root into a later checkpoint.
    snapshot->params_json.clear();
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto RootSnapshotFromMaterialized(const alcedo::EditorRenderAdjustmentSnapshot& materialized,
                                  const alcedo::CommitGraph& graph,
                                  const alcedo::head_commit_hash_t& materialized_head,
                                  alcedo::EditorRenderAdjustmentSnapshot* root_snapshot,
                                  std::string* error) -> bool {
  if (root_snapshot == nullptr) {
    if (error) *error = "Root adjustment snapshot output is null";
    return false;
  }
  if (!IsCompleteAdjustmentSnapshot(materialized, error)) return false;
  *root_snapshot = materialized;
  try {
    const auto chain = graph.FirstParentChain(materialized_head);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if (!ApplyHistoryCommitToSnapshot(root_snapshot, graph, graph.GetCommit(*it), false, error)) {
        return false;
      }
    }
    root_snapshot->params_json.clear();
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}


}  // namespace alcedo::ui
