//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

#include <algorithm>
#include <utility>

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
  guard->dirty_ = !journal_records.empty();
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
  alcedo::EditorAdjustmentOperatorState before;
  std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
  if (!alcedo::ReadEditorAdjustmentOperatorState(*state->pipeline_guard->pipeline_, patch.field_key,
                                                 &before, error)) {
    return false;
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
  {
    alcedo::EditorAdjustmentOperatorState after{after_params, payload.after_enabled};
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    if (!alcedo::ApplyEditorAdjustmentOperatorState(*state->pipeline_guard->pipeline_, *spec, after,
                                                    error)) {
      return false;
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
  // Bootstrap history has no durable capture. The save service treats this as
  // a successful no-op and still completes the task ordering.
  if (!journal_path) return nullptr;

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
  const auto journal_snapshot = state->journal->Snapshot();

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
