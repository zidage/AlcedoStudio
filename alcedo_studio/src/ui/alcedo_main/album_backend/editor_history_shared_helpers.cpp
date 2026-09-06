//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"

#include <algorithm>
#include <ctime>
#include <optional>
#include <utility>
#include <variant>

#include "app/editor_adjustment_pipeline.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
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
  alcedo::EditorAdjustmentPatch patch;
  patch.field_key   = field_key;
  patch.params_json = params.is_null() ? std::string{} : params.dump();
  patch.settled     = true;
  patch.enabled     = enabled;
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

namespace {

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

auto PanelSnapshotParams(const std::string& field_key, nlohmann::json params) -> nlohmann::json {
  if (field_key == "exposure" && params.contains("exposure_ev")) {
    params["exposure"] = params.at("exposure_ev");
    params.erase("exposure_ev");
  }
  return params;
}

auto ApplyTypedBatchToSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                               const alcedo::PipelineEditBatch& batch, bool use_after_value,
                               std::string* error) -> bool {
  for (const auto& change : batch.changes) {
    const auto* parameter = std::get_if<alcedo::SetParameterChange>(&change);
    if (parameter == nullptr) {
      continue;
    }
    alcedo::EditorAdjustmentOperatorState state;
    state.params  = PanelSnapshotParams(parameter->target.field_key,
                                       use_after_value ? parameter->after_value
                                                       : parameter->before_value);
    state.enabled = use_after_value ? parameter->after_enabled : parameter->before_enabled;
    if (!ApplySnapshotState(snapshot, parameter->target.field_key, state, error)) {
      return false;
    }
  }
  return true;
}

}  // namespace

auto ApplyHistoryCommitToSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                  const alcedo::CommitGraph& graph,
                                  const alcedo::EditCommit& commit, bool use_after_value,
                                  std::string* error) -> bool {
  (void)graph;
  if (snapshot == nullptr || !IsCompleteAdjustmentSnapshot(*snapshot, error)) return false;
  try {
    if (!alcedo::IsPipelineEditBatchJson(commit.GetPayloadJSON())) {
      if (error) *error = "Commit payload is not a typed batch";
      return false;
    }
    return ApplyTypedBatchToSnapshot(
        snapshot, alcedo::PipelineEditBatch::FromJSON(commit.GetPayloadJSON()), use_after_value,
        error);
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
