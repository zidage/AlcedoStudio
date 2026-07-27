//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <string_view>
#include <utility>

#include "app/adjustment_transfer_service.hpp"
#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_mini_git_materializer.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"

namespace alcedo::ui {

struct EditorSessionHistoryPort::WorkingState {
  std::mutex                                                             mutex;
  std::shared_ptr<alcedo::PipelineGuard>                                 pipeline_guard;
  std::shared_ptr<alcedo::MiniGitJournal>                                journal;
  std::unique_ptr<alcedo::MiniGitWorkingHistory>                         history;
  std::unordered_map<std::string, alcedo::EditorAdjustmentOperatorState> pending_before;
  alcedo::EditorRenderAdjustmentSnapshot                                 committed_snapshot;
  bool                                                                   recovered_head = false;
};

namespace {

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

// These are the stable field names shared by the editor models and the
// adjustment-transfer/history services. The loaded pipeline is the durable
// source of truth after a checkpoint; journal replay only contains edits made
// after that checkpoint.
constexpr std::array<std::string_view, 21> kEditorSnapshotFields = {
    "exposure",   "contrast",   "white",     "black",      "shadows",     "highlights",
    "curve",      "saturation", "vibrance",  "hls",        "color_wheel", "lut",
    "clarity",    "sharpen",    "odt",       "film_grain", "halation",    "crop_rotate",
    "raw_decode", "lens_calib", "color_temp"};

void UpsertCommittedSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                             const std::string& field_key, const nlohmann::json& params) {
  if (snapshot == nullptr) return;
  alcedo::EditorAdjustmentPatch patch{field_key, params.is_null() ? std::string{} : params.dump(),
                                      true};
  auto                          existing = std::find_if(
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

auto CommitLabel(const alcedo::EditCommit& commit) -> std::string {
  if (commit.GetKind() == alcedo::EditCommitKind::kMerge) {
    return "Merge";
  }
  const auto key = CommitFieldKey(commit);
  return key.empty() ? "Edit" : key;
}

auto VersionNameExists(const alcedo::CommitGraph& graph, const std::string& name,
                       const alcedo::version_ref_id_t* ignored = nullptr) -> bool {
  for (const auto& [id, version] : graph.GetAllVersionRefs()) {
    if (ignored != nullptr && id == *ignored) continue;
    if (version.display_name == name) return true;
  }
  return false;
}

auto UniqueVersionName(const alcedo::CommitGraph& graph, std::string requested,
                       const alcedo::version_ref_id_t* ignored = nullptr) -> std::string {
  const auto first = requested.find_first_not_of(" \t\r\n");
  const auto last  = requested.find_last_not_of(" \t\r\n");
  requested =
      first == std::string::npos ? std::string{} : requested.substr(first, last - first + 1);
  if (requested.empty()) requested = "Version";
  if (!VersionNameExists(graph, requested, ignored)) return requested;
  for (std::size_t suffix = 2;; ++suffix) {
    auto candidate = requested + " " + std::to_string(suffix);
    if (!VersionNameExists(graph, candidate, ignored)) return candidate;
  }
}

auto InitializeCommittedSnapshotFromPipeline(alcedo::PipelineGuard&                  guard,
                                             alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                             std::string*                            error) -> bool;

void RestoreGraphAndPipeline(alcedo::CommitGraph& graph, const alcedo::CommitGraph& prior_graph,
                             alcedo::PipelineMgmtService&                  pipeline_service,
                             const std::shared_ptr<alcedo::PipelineGuard>& pipeline_guard,
                             const alcedo::head_commit_hash_t&             prior_head,
                             const alcedo::transaction_chain_hash_t& prior_chain, bool prior_dirty,
                             bool prior_serialized_state_needs_writeback) {
  graph = prior_graph;
  std::string ignored_error;
  (void)pipeline_service.RebuildActiveEditorPipeline(pipeline_guard, &ignored_error);
  pipeline_guard->working_head_commit_hash_         = prior_head;
  pipeline_guard->transaction_chain_hash_           = prior_chain;
  pipeline_guard->dirty_                            = prior_dirty;
  pipeline_guard->serialized_state_needs_writeback_ = prior_serialized_state_needs_writeback;
}

auto ReadPipelineSnapshot(alcedo::PipelineGuard&                  guard,
                          alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
    -> bool {
  if (snapshot == nullptr || !guard.pipeline_) {
    if (error) *error = "Editor pipeline is unavailable for history operation";
    return false;
  }
  return InitializeCommittedSnapshotFromPipeline(guard, snapshot, error);
}

auto ApplyCommittedPayload(alcedo::PipelineGuard&                  guard,
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
  state.params  = use_after_value ? payload.after_value : payload.before_value;
  state.enabled = use_after_value ? payload.after_enabled : payload.before_enabled;
  std::unique_lock<std::mutex> render_lock(guard.pipeline_->GetRenderLock());
  if (!alcedo::ApplyEditorAdjustmentOperatorState(*guard.pipeline_, *spec, state, error)) {
    return false;
  }
  UpsertCommittedSnapshot(snapshot, *field_key, state.params);
  return true;
}

auto ApplyRecoveredRecord(alcedo::PipelineGuard&                  guard,
                          alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                          alcedo::CommitGraph*                    replay_graph,
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
    if (source_head) {
      const auto& source = replay_graph->GetCommit(*source_head);
      if (record.target_head == source.GetFirstParentHash()) {
        const auto payload = alcedo::OrdinaryEditPayload::FromJSON(source.GetPayloadJSON());
        if (!ApplyCommittedPayload(guard, snapshot, payload, false, error)) return false;
      } else if (record.target_head) {
        const auto& target  = replay_graph->GetCommit(*record.target_head);
        const auto  payload = alcedo::OrdinaryEditPayload::FromJSON(target.GetPayloadJSON());
        if (!ApplyCommittedPayload(guard, snapshot, payload, true, error)) return false;
      }
    } else if (record.target_head) {
      const auto& target  = replay_graph->GetCommit(*record.target_head);
      const auto  payload = alcedo::OrdinaryEditPayload::FromJSON(target.GetPayloadJSON());
      if (!ApplyCommittedPayload(guard, snapshot, payload, true, error)) return false;
    }
  }
  return alcedo::MiniGitWorkingHistory::Replay(*replay_graph, {record}, error);
}

auto InitializeCommittedSnapshotFromPipeline(alcedo::PipelineGuard&                  guard,
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
    // Missing optional operators are left out; the QML model then retains its
    // declared baseline instead of receiving an empty JSON value.
    if (state.params.is_object()) {
      UpsertCommittedSnapshot(snapshot, std::string(field_key), state.params);
    }
  }
  // Keep a complete serialized state as a fallback for render consumers. The
  // field patches above are what the QML panel projection reads.
  snapshot->params_json = guard.pipeline_->ExportPipelineParams().dump();
  return true;
}

}  // namespace

void EditorSessionHistoryPort::SetServices(Services services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

void EditorSessionHistoryPort::SetPipelinePort(
    std::shared_ptr<EditorSessionPipelinePort> pipeline_port) {
  std::scoped_lock lock(mutex_);
  pipeline_port_ = std::move(pipeline_port);
}

auto EditorSessionHistoryPort::EnsureWorkingState(sl_element_id_t element_id, std::string* error)
    -> std::shared_ptr<WorkingState> {
  std::shared_ptr<EditorSessionPipelinePort>            pipeline_port;
  std::function<std::filesystem::path(sl_element_id_t)> journal_path;
  {
    std::scoped_lock lock(mutex_);
    const auto       existing = working_states_.find(element_id);
    if (existing != working_states_.end()) return existing->second;
    pipeline_port = pipeline_port_.lock();
    journal_path  = services_.mini_git_journal_path;
  }
  if (!pipeline_port) {
    if (error) *error = "Editor pipeline port is unavailable";
    return nullptr;
  }
  auto guard = pipeline_port->EnsureLoaded(element_id, error);
  if (!guard || !guard->pipeline_ || !guard->commit_graph_) {
    if (error && error->empty()) *error = "Editor Mini-Git pipeline state is unavailable";
    return nullptr;
  }

  std::filesystem::path path;
  try {
    if (journal_path) path = journal_path(element_id);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return nullptr;
  } catch (...) {
    if (error) *error = "Failed to resolve Mini-Git journal path";
    return nullptr;
  }
  auto journal = std::make_shared<alcedo::MiniGitJournal>(std::move(path));
  if (!journal->Load(error)) return nullptr;

  auto state            = std::make_shared<WorkingState>();
  state->pipeline_guard = guard;
  state->journal        = journal;
  if (!InitializeCommittedSnapshotFromPipeline(*guard, &state->committed_snapshot, error)) {
    return nullptr;
  }
  auto       replay_graph    = *guard->commit_graph_;
  auto       validated_graph = replay_graph;
  const auto journal_records = journal->records();
  if (!alcedo::MiniGitWorkingHistory::Replay(validated_graph, journal_records, error)) {
    return nullptr;
  }
  for (const auto& record : journal_records) {
    if (!ApplyRecoveredRecord(*guard, &state->committed_snapshot, &replay_graph, record, error)) {
      return nullptr;
    }
  }
  *guard->commit_graph_            = std::move(replay_graph);
  guard->working_head_commit_hash_ = guard->commit_graph_->GetActiveVersionRef().head_commit_hash;
  guard->transaction_chain_hash_ =
      guard->commit_graph_->ChainHashForHead(guard->working_head_commit_hash_);
  guard->dirty_         = !journal_records.empty();
  state->recovered_head = !journal_records.empty();
  if (!journal_records.empty()) guard->pipeline_->SetExecutionStages();
  state->history = std::make_unique<alcedo::MiniGitWorkingHistory>(guard->commit_graph_, journal);

  std::scoped_lock lock(mutex_);
  const auto [it, inserted] = working_states_.emplace(element_id, state);
  return inserted ? state : it->second;
}

auto EditorSessionHistoryPort::Acquire(sl_element_id_t element_id, std::string* error)
    -> alcedo::EditorHistoryGuardHandle {
  std::function<std::filesystem::path(sl_element_id_t)> journal_path;
  {
    std::scoped_lock lock(mutex_);
    journal_path = services_.mini_git_journal_path;
  }
  if (journal_path) {
    std::string prepare_error;
    if (!EnsureWorkingState(element_id, &prepare_error)) {
      if (error)
        *error = prepare_error.empty() ? "Editor Mini-Git history initialization failed"
                                       : std::move(prepare_error);
      return {};
    }
  }
  return {element_id, true};
}

void EditorSessionHistoryPort::Release(const alcedo::EditorHistoryGuardHandle& guard) {
  if (!guard.valid) return;
  std::scoped_lock lock(mutex_);
  working_states_.erase(guard.element_id);
}

auto EditorSessionHistoryPort::CaptureAdjustmentBeforePreview(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::EditorAdjustmentPatch& patch,
    std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (state->pending_before.contains(patch.field_key)) return true;

  // Prefer the committed snapshot for "before" so the GUI-thread submit path
  // never blocks on pipeline_->GetRenderLock(). The scheduler holds that lock
  // for the entire Apply (including frame present). Blocking here while a
  // worker also needs the GUI for present is a classic multi-slider deadlock:
  // finish slider A / start slider B freezes with the render spinner still on.
  alcedo::EditorAdjustmentOperatorState before;
  bool                                  resolved = false;
  for (const auto& committed : state->committed_snapshot.patches) {
    if (committed.field_key != patch.field_key) {
      continue;
    }
    try {
      before.params = committed.params_json.empty() ? nlohmann::json(nullptr)
                                                    : nlohmann::json::parse(committed.params_json);
    } catch (const std::exception& ex) {
      if (error) *error = ex.what();
      return false;
    }
    before.enabled = EnabledForAdjustmentParams(before.params);
    resolved       = true;
    break;
  }

  if (!resolved) {
    if (!state->pipeline_guard || !state->pipeline_guard->pipeline_) {
      if (error) *error = "Editor pipeline is unavailable for adjustment capture";
      return false;
    }
    // Fallback only when the field was never snapshotted. Use try_lock — never
    // wait for a mid-flight Apply on the GUI thread.
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock(),
                                             std::try_to_lock);
    if (render_lock.owns_lock()) {
      if (!alcedo::ReadEditorAdjustmentOperatorState(*state->pipeline_guard->pipeline_,
                                                     patch.field_key, &before, error)) {
        return false;
      }
    } else {
      // Pipeline is rendering; record an empty-object before rather than freeze.
      // The next settled commit still pairs with whatever after-params arrive.
      before.params  = nlohmann::json::object();
      before.enabled = true;
    }
  }

  state->pending_before.emplace(patch.field_key, std::move(before));
  return true;
}

auto EditorSessionHistoryPort::CommitAdjustment(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::EditorAdjustmentPatch&    patch,
                                                std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  const auto       spec = alcedo::ResolveEditorAdjustmentField(patch.field_key);
  if (!spec) {
    if (error) *error = "Unknown editor adjustment field: " + patch.field_key;
    return false;
  }
  auto before = state->pending_before.find(patch.field_key);
  if (before == state->pending_before.end()) {
    if (error) *error = "Settled adjustment has no captured committed state";
    return false;
  }
  nlohmann::json after_params;
  try {
    after_params = nlohmann::json::parse(patch.params_json);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  if (!after_params.is_object()) {
    if (error) *error = "Editor adjustment params must be a JSON object";
    return false;
  }

  alcedo::OrdinaryEditPayload payload;
  payload.operator_type  = spec->operator_type;
  payload.stage_name     = spec->stage_name;
  payload.field_name     = "$operator_params";
  payload.before_value   = before->second.params;
  payload.after_value    = after_params;
  payload.before_enabled = before->second.enabled;
  payload.after_enabled  = EnabledForAdjustmentParams(after_params);

  // Apply to the live executor only if the render lock is free. Never block the
  // GUI on a long Apply/present. The next routed render re-applies the full
  // adjustment snapshot under prepare_with_render_lock on the worker.
  if (state->pipeline_guard && state->pipeline_guard->pipeline_) {
    alcedo::EditorAdjustmentOperatorState after{after_params, payload.after_enabled};
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock(),
                                             std::try_to_lock);
    if (render_lock.owns_lock()) {
      if (!alcedo::ApplyEditorAdjustmentOperatorState(*state->pipeline_guard->pipeline_, *spec,
                                                      after, error)) {
        return false;
      }
    }
  }

  const auto append = state->history->AppendEdit(std::move(payload));
  if (!append.committed) {
    if (error) *error = append.error;
    return false;
  }
  state->pipeline_guard->dirty_                    = true;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_   = state->history->transaction_chain_hash();
  UpsertCommittedSnapshot(&state->committed_snapshot, patch.field_key, after_params);
  state->pending_before.erase(before);
  state->recovered_head = false;
  return true;
}

auto EditorSessionHistoryPort::Undo(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string*                            error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  const auto       result = state->history->Undo();
  if (!result.error.empty()) {
    if (error) *error = result.error;
    return false;
  }
  if (!result.moved || !result.selected_commit) return true;
  const auto payload =
      alcedo::OrdinaryEditPayload::FromJSON(result.selected_commit->GetPayloadJSON());
  if (!ApplyCommittedPayload(*state->pipeline_guard, &state->committed_snapshot, payload, false,
                             error)) {
    return false;
  }
  state->pipeline_guard->dirty_                    = true;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_   = state->history->transaction_chain_hash();
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorSessionHistoryPort::Redo(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string*                            error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  const auto       result = state->history->Redo();
  if (!result.error.empty()) {
    if (error) *error = result.error;
    return false;
  }
  if (!result.moved || !result.selected_commit) return true;
  const auto payload =
      alcedo::OrdinaryEditPayload::FromJSON(result.selected_commit->GetPayloadJSON());
  if (!ApplyCommittedPayload(*state->pipeline_guard, &state->committed_snapshot, payload, true,
                             error)) {
    return false;
  }
  state->pipeline_guard->dirty_                    = true;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_   = state->history->transaction_chain_hash();
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorSessionHistoryPort::CheckoutVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                               const alcedo::Hash128&                  version_id,
                                               std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) {
    return false;
  }
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port;
  {
    std::scoped_lock lock(mutex_);
    pipeline_port = pipeline_port_.lock();
  }
  if (!pipeline_port) {
    if (error) *error = "Editor pipeline port is unavailable for Version checkout";
    return false;
  }
  auto pipeline_service = pipeline_port->PipelineService();
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for Version checkout";
    return false;
  }

  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto&      graph            = *state->pipeline_guard->commit_graph_;
  const auto graph_before     = graph;
  const auto prior_head       = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain      = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty      = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot   = state->committed_snapshot;
  const auto prior_pending    = state->pending_before;
  const bool prior_recovered  = state->recovered_head;

  // Rebuild the live pipeline under the render lock first. Fail closed keeps the
  // prior Version active and the prior executor contents published.
  if (!pipeline_port->CheckoutVersion(guard.element_id, version_id, error)) {
    return false;
  }

  // Clear the in-memory redo stack after a successful Version switch. Detached
  // HEAD editing is not supported; SelectVersion only adjusts redo state here
  // because CheckoutVersion already moved the active Version ref.
  if (!state->history->SelectVersion(version_id, error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    return false;
  }

  // Refresh the committed snapshot from the rebuilt executor so panels and the
  // first post-checkout frame observe the same values as the pipeline.
  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!ReadPipelineSnapshot(*state->pipeline_guard, &next_snapshot, error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    return false;
  }

  std::string persistence_error;
  if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                   graph_before.GetImageEditState(),
                                                   &persistence_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    if (error) *error = persistence_error;
    return false;
  }

  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_   = state->history->transaction_chain_hash();
  state->pending_before.clear();
  state->committed_snapshot = std::move(next_snapshot);
  state->recovered_head = false;
  return true;
}

auto EditorSessionHistoryPort::ReadHistorySnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                                                   alcedo::EditorHistorySnapshot*          snapshot,
                                                   std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (snapshot == nullptr || !state->pipeline_guard || !state->pipeline_guard->commit_graph_ ||
      !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }

  const auto&                   graph             = *state->pipeline_guard->commit_graph_;
  const auto                    active_version_id = graph.GetActiveVersionId();
  const auto                    active_head       = graph.GetActiveVersionRef().head_commit_hash;
  alcedo::EditorHistorySnapshot projection;
  projection.active_version_id = active_version_id;
  projection.active_head       = active_head;
  projection.recovered_head    = state->recovered_head;
  projection.can_undo          = active_head.has_value();
  projection.can_redo          = state->history->redo_count() > 0;

  projection.versions.reserve(graph.GetAllVersionRefs().size());
  for (const auto& [version_id, version] : graph.GetAllVersionRefs()) {
    projection.versions.push_back({version_id, version.display_name, version.head_commit_hash,
                                   version.created_at, version.updated_at,
                                   version_id == active_version_id});
  }
  std::sort(projection.versions.begin(), projection.versions.end(),
            [](const auto& left, const auto& right) {
              if (left.created_at != right.created_at) return left.created_at < right.created_at;
              return left.version_id.ToString() < right.version_id.ToString();
            });

  if (active_head.has_value()) {
    const auto chain = graph.FirstParentChain(active_head);
    projection.commits.reserve(chain.size());
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const auto&                 commit = graph.GetCommit(*it);
      alcedo::EditorHistoryCommit row;
      row.commit_hash        = commit.GetCommitHash();
      row.first_parent_hash  = commit.GetFirstParentHash();
      row.second_parent_hash = commit.GetSecondParentHash();
      row.kind               = commit.GetKind();
      row.created_at_ns      = commit.GetCreatedAtNs();
      row.field_key          = CommitFieldKey(commit);
      row.label              = CommitLabel(commit);
      row.current            = true;
      projection.commits.push_back(std::move(row));
    }
  }
  *snapshot = std::move(projection);
  return true;
}

auto EditorSessionHistoryPort::CreateRootVersionAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, std::string display_name,
    alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port;
  {
    std::scoped_lock lock(mutex_);
    pipeline_port = pipeline_port_.lock();
  }
  if (!pipeline_port) {
    if (error) *error = "Editor pipeline port is unavailable for root Version creation";
    return false;
  }
  auto pipeline_service = pipeline_port->PipelineService();
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for root Version creation";
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto&      graph            = *state->pipeline_guard->commit_graph_;
  const auto graph_before     = graph;
  const auto prior_head       = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain      = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty      = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot   = state->committed_snapshot;
  const auto prior_pending    = state->pending_before;
  const bool prior_recovered  = state->recovered_head;

  alcedo::version_ref_id_t new_id{};
  try {
    new_id = graph.CreateVersionRefAtRoot(UniqueVersionName(graph, std::move(display_name)));
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  // Rebuild the pipeline for the new root ref. Fail closed: if the rebuild
  // fails, the pipeline service restores the prior Version. Remove the
  // provisional ref so the graph matches.
  std::string rebuild_error;
  if (!pipeline_port->CheckoutVersion(guard.element_id, new_id, &rebuild_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    if (error) *error = rebuild_error;
    return false;
  }

  std::string select_error;
  if (!state->history->SelectVersion(new_id, &select_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    if (error) *error = select_error;
    return false;
  }
  // Refresh the committed snapshot from the rebuilt executor.
  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!ReadPipelineSnapshot(*state->pipeline_guard, &next_snapshot, error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    return false;
  }

  std::string persistence_error;
  if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                   graph_before.GetImageEditState(),
                                                   &persistence_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    if (error) *error = persistence_error;
    return false;
  }

  if (version_id) *version_id = new_id;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_   = state->history->transaction_chain_hash();
  state->pipeline_guard->dirty_                    = true;
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  state->pending_before.clear();
  state->recovered_head     = false;
  state->committed_snapshot = std::move(next_snapshot);
  return true;
}

auto EditorSessionHistoryPort::BranchFromCommitAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::commit_hash_t& commit_id,
    std::string display_name, alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port;
  {
    std::scoped_lock lock(mutex_);
    pipeline_port = pipeline_port_.lock();
  }
  if (!pipeline_port) {
    if (error) *error = "Editor pipeline port is unavailable for branch creation";
    return false;
  }
  auto pipeline_service = pipeline_port->PipelineService();
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for branch creation";
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto&      graph            = *state->pipeline_guard->commit_graph_;
  const auto graph_before     = graph;
  const auto prior_head       = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain      = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty      = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot   = state->committed_snapshot;
  const auto prior_pending    = state->pending_before;
  const bool prior_recovered  = state->recovered_head;

  // Validate the target commit exists in the graph.
  if (!graph.FindCommit(commit_id)) {
    if (error) *error = "Branch target commit does not exist in the editor history";
    return false;
  }

  alcedo::version_ref_id_t new_id{};
  try {
    new_id = graph.CreateVersionRefAtHead(UniqueVersionName(graph, std::move(display_name)),
                                          commit_id);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  std::string rebuild_error;
  if (!pipeline_port->CheckoutVersion(guard.element_id, new_id, &rebuild_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    if (error) *error = rebuild_error;
    return false;
  }

  std::string select_error;
  if (!state->history->SelectVersion(new_id, &select_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    if (error) *error = select_error;
    return false;
  }

  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!ReadPipelineSnapshot(*state->pipeline_guard, &next_snapshot, error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    return false;
  }

  std::string persistence_error;
  if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                   graph_before.GetImageEditState(),
                                                   &persistence_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before     = prior_pending;
    state->recovered_head     = prior_recovered;
    if (error) *error = persistence_error;
    return false;
  }

  if (version_id) *version_id = new_id;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_   = state->history->transaction_chain_hash();
  state->pipeline_guard->dirty_                    = true;
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  state->pending_before.clear();
  state->recovered_head     = false;
  state->committed_snapshot = std::move(next_snapshot);
  return true;
}

auto EditorSessionHistoryPort::RenameVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::Hash128&                  version_id,
                                             std::string display_name, std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  try {
    auto&      ref                = graph.GetVersionRef(version_id);
    const auto name               = UniqueVersionName(graph, std::move(display_name), &version_id);
    ref.display_name              = name;
    ref.updated_at                = std::time(nullptr);
    state->pipeline_guard->dirty_ = true;
    state->pipeline_guard->working_head_commit_hash_ = graph.GetActiveVersionRef().head_commit_hash;
    state->pipeline_guard->transaction_chain_hash_ =
        graph.ChainHashForHead(state->pipeline_guard->working_head_commit_hash_);
    state->pipeline_guard->serialized_state_needs_writeback_ = true;
    state->recovered_head                                    = false;
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto EditorSessionHistoryPort::RemoveVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::Hash128& version_id, std::string* error)
    -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  if (!state->pipeline_guard->commit_graph_->RemoveVersionRef(version_id)) {
    if (error) {
      *error = "The active Version or the final remaining Version cannot be removed";
    }
    return false;
  }
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->working_head_commit_hash_ =
      state->pipeline_guard->commit_graph_->GetActiveVersionRef().head_commit_hash;
  state->pipeline_guard->transaction_chain_hash_ =
      state->pipeline_guard->commit_graph_->ChainHashForHead(
          state->pipeline_guard->working_head_commit_hash_);
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  state->recovered_head                                    = false;
  return true;
}

auto EditorSessionHistoryPort::PasteAdjustments(const alcedo::EditorHistoryGuardHandle&  guard,
                                                const alcedo::AdjustmentTransferPackage& package,
                                                std::string                    version_display_name,
                                                alcedo::AdjustmentPasteResult* result,
                                                std::string*                   error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto pipeline_port    = pipeline_port_.lock();
  auto pipeline_service = pipeline_port ? pipeline_port->PipelineService() : nullptr;
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for editor Paste";
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto&      graph            = *state->pipeline_guard->commit_graph_;
  const auto graph_before     = graph;
  const auto prior_head       = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain      = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty      = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot   = state->committed_snapshot;
  alcedo::AdjustmentPasteResult paste_result;
  try {
    paste_result = alcedo::AdjustmentTransferService::PasteAsRootRelativeVersion(
        graph, *pipeline_service, guard.element_id, package, std::move(version_display_name));
  } catch (const std::exception& ex) {
    RestoreGraphAndPipeline(*state->pipeline_guard->commit_graph_, graph_before, *pipeline_service,
                            state->pipeline_guard, prior_head, prior_chain, prior_dirty,
                            prior_serialized);
    if (error) *error = ex.what();
    return false;
  }
  if (!paste_result.pasted) {
    if (error) *error = paste_result.error;
    return false;
  }
  std::string rebuild_error;
  if (!pipeline_service->RebuildActiveEditorPipeline(state->pipeline_guard, &rebuild_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    if (error)
      *error =
          rebuild_error.empty() ? "Failed to rebuild pasted pipeline" : std::move(rebuild_error);
    return false;
  }
  state->pipeline_guard->dirty_                    = true;
  state->pipeline_guard->working_head_commit_hash_ = paste_result.new_head;
  state->pipeline_guard->transaction_chain_hash_   = graph.ChainHashForHead(paste_result.new_head);
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  if (!ReadPipelineSnapshot(*state->pipeline_guard, &state->committed_snapshot, error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    return false;
  }
  if (result) *result = paste_result;
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorSessionHistoryPort::BeginMerge(const alcedo::EditorHistoryGuardHandle&  guard,
                                          const alcedo::AdjustmentTransferPackage& package,
                                          std::string incoming_version_display_name,
                                          alcedo::AdjustmentMergePreview* preview,
                                          std::string*                    error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto pipeline_port    = pipeline_port_.lock();
  auto pipeline_service = pipeline_port ? pipeline_port->PipelineService() : nullptr;
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for editor Merge";
    return false;
  }
  if (preview == nullptr) {
    if (error) *error = "Merge preview storage is required";
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto&      graph         = *state->pipeline_guard->commit_graph_;
  const auto graph_before  = graph;
  auto       merge_preview = alcedo::AdjustmentTransferService::InitiateMerge(
      graph, *pipeline_service, guard.element_id, package,
      std::move(incoming_version_display_name));
  if (!merge_preview.error.empty()) {
    graph = graph_before;
    if (error) *error = merge_preview.error;
    return false;
  }
  *preview                                         = std::move(merge_preview);
  state->pipeline_guard->dirty_                    = true;
  state->pipeline_guard->working_head_commit_hash_ = graph.GetActiveVersionRef().head_commit_hash;
  state->pipeline_guard->transaction_chain_hash_ =
      graph.ChainHashForHead(state->pipeline_guard->working_head_commit_hash_);
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  return true;
}

auto EditorSessionHistoryPort::CompleteMerge(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::AdjustmentMergePreview& preview,
    const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
    alcedo::AdjustmentMergeResult* result, std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto pipeline_port    = pipeline_port_.lock();
  auto pipeline_service = pipeline_port ? pipeline_port->PipelineService() : nullptr;
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for editor Merge";
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto&      graph            = *state->pipeline_guard->commit_graph_;
  const auto graph_before     = graph;
  const auto prior_head       = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain      = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty      = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot   = state->committed_snapshot;
  alcedo::AdjustmentMergeResult merge_result;
  try {
    merge_result = alcedo::AdjustmentTransferService::CompleteMerge(graph, *pipeline_service,
                                                                    preview, resolutions);
  } catch (const std::exception& ex) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    if (error) *error = ex.what();
    return false;
  }
  if (!merge_result.merged) {
    if (error) *error = merge_result.error;
    return false;
  }
  std::string rebuild_error;
  if (!pipeline_service->RebuildActiveEditorPipeline(state->pipeline_guard, &rebuild_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    if (error)
      *error =
          rebuild_error.empty() ? "Failed to rebuild merged pipeline" : std::move(rebuild_error);
    return false;
  }
  state->pipeline_guard->dirty_                    = true;
  state->pipeline_guard->working_head_commit_hash_ = merge_result.merge_commit_hash;
  state->pipeline_guard->transaction_chain_hash_ =
      graph.ChainHashForHead(merge_result.merge_commit_hash);
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  if (!ReadPipelineSnapshot(*state->pipeline_guard, &state->committed_snapshot, error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    return false;
  }
  if (result) *result = merge_result;
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorSessionHistoryPort::CancelMerge(const alcedo::EditorHistoryGuardHandle& guard,
                                           const alcedo::AdjustmentMergePreview&   preview,
                                           std::string*                            error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto mutable_preview = preview;
  alcedo::AdjustmentTransferService::CancelMerge(*state->pipeline_guard->commit_graph_,
                                                 mutable_preview);
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  return true;
}

auto EditorSessionHistoryPort::ReadAdjustmentSnapshot(
    const alcedo::EditorHistoryGuardHandle& guard, alcedo::EditorRenderAdjustmentSnapshot* snapshot,
    std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (snapshot) *snapshot = state->committed_snapshot;
  return true;
}

auto EditorSessionHistoryPort::CaptureSaveCheckpoint(const alcedo::EditorHistoryGuardHandle& guard,
                                                     std::string*                            error)
    -> std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> {
  std::function<std::filesystem::path(sl_element_id_t)> journal_path;
  {
    std::scoped_lock lock(mutex_);
    journal_path = services_.mini_git_journal_path;
  }
  // Fail closed: a configured editor session must resolve a Mini-Git journal
  // path. Missing configuration is not a successful empty capture.
  if (!journal_path) {
    if (error) *error = "Mini-Git journal path is unavailable";
    return nullptr;
  }

  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return nullptr;
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->pipeline_ ||
      !state->pipeline_guard->commit_graph_ || !state->history || !state->journal) {
    if (error) *error = "Mini-Git save capture requires a live pipeline snapshot";
    return nullptr;
  }

  nlohmann::json pipeline_params;
  try {
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    pipeline_params = state->pipeline_guard->pipeline_->ExportPipelineParams();
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return nullptr;
  }

  // Snapshot journal records under the journal mutex before releasing live
  // history access. Post-capture appends get later sequences and must not enter
  // this range or be deleted when the capture is truncated.
  const auto                       journal_snapshot = state->journal->Snapshot();

  alcedo::EditorMiniGitSaveCapture capture;
  capture.element_id             = guard.element_id;
  capture.version_id             = state->pipeline_guard->commit_graph_->GetActiveVersionId();
  capture.root_id                = state->pipeline_guard->root_id_;
  capture.working_head           = state->history->working_head();
  capture.transaction_chain_hash = state->history->transaction_chain_hash();
  capture.journal_records        = journal_snapshot.records;
  capture.journal_path           = state->journal->path();
  capture.first_journal_sequence = journal_snapshot.first_sequence;
  capture.last_journal_sequence  = journal_snapshot.last_sequence;
  const auto serialized          = alcedo::MakeEditorSerializedPipelineState(
      state->pipeline_guard->root_id_, capture.working_head, capture.transaction_chain_hash,
      pipeline_params);
  try {
    capture.materialization =
        state->pipeline_guard->commit_graph_->CaptureMaterializationWithSerializedPipelineState(
            serialized);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return nullptr;
  }
  return std::make_shared<const alcedo::EditorMiniGitSaveCapture>(std::move(capture));
}

auto EditorSessionHistoryPort::DiscardMaterializedJournalThrough(
    const alcedo::EditorHistoryGuardHandle& guard, std::uint64_t last_sequence, std::string* error)
    -> bool {
  if (last_sequence == 0) {
    if (error) *error = "DiscardMaterializedJournalThrough requires a non-zero sequence";
    return false;
  }
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (!state->journal) {
    if (error) *error = "Mini-Git journal is unavailable for prefix discard";
    return false;
  }
  return state->journal->TruncateThroughSequence(last_sequence, error);
}

}  // namespace alcedo::ui
