//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_production.hpp"

#include <QMetaObject>
#include <QThread>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_history_materializer.hpp"
#include "app/editor_mini_git_materializer.hpp"
#include "edit/frame_presentation_types.hpp"
#include "edit/history/editor_journal_recovery.hpp"
#include "edit/history/editor_transaction_journal.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "io/image/image_loader.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/image_controller.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/pipeline_controller.hpp"
#include "ui/editor_rhi/direct_frame_sink.hpp"

namespace alcedo::ui {
namespace {

auto RenderTypeForIntent(const alcedo::EditorRenderIntent& intent) -> alcedo::RenderType {
  switch (intent.quality) {
    case alcedo::EditorRenderQuality::Detail:
      return alcedo::RenderType::DETAIL_ROI_PREVIEW;
    case alcedo::EditorRenderQuality::Quality:
      return alcedo::RenderType::QUALITY_BASE_PREVIEW;
    case alcedo::EditorRenderQuality::Interactive:
      return alcedo::RenderType::FAST_PREVIEW;
  }
  return alcedo::RenderType::FAST_PREVIEW;
}

auto FrameRoleToPreviewMetadata(const alcedo::EditorRenderIntent& intent)
    -> alcedo::FramePreviewMetadata {
  alcedo::FramePreviewMetadata meta;
  meta.frame_role         = intent.frame_role;
  meta.preview_generation = intent.render_generation;
  meta.detail_serial      = 0;
  return meta;
}

}  // namespace

// ── Pipeline port ───────────────────────────────────────────────────────────

void EditorSessionProductionPipelinePort::SetServices(EditorSessionProductionServices services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

auto EditorSessionProductionPipelinePort::Acquire(sl_element_id_t element_id,
                                                  std::string* /*error*/)
    -> alcedo::EditorPipelineGuardHandle {
  // Intentionally does not call LoadPipeline here. Open must stay non-blocking
  // for shell/synthetic ids; first-frame production loads the real guard on demand.
  return alcedo::EditorPipelineGuardHandle{element_id, true};
}

void EditorSessionProductionPipelinePort::Release(const alcedo::EditorPipelineGuardHandle& guard) {
  if (!guard.valid) {
    return;
  }
  std::shared_ptr<alcedo::PipelineGuard>       loaded_guard;
  std::shared_ptr<alcedo::PipelineMgmtService> service;
  {
    std::scoped_lock lock(mutex_);
    auto             it = guards_.find(guard.element_id);
    if (it != guards_.end()) {
      loaded_guard = it->second;
      guards_.erase(it);
    }
    if (services_.pipeline_service) {
      service = services_.pipeline_service();
    }
  }
  if (service && loaded_guard) {
    service->SavePipeline(std::move(loaded_guard));
  }
}

auto EditorSessionProductionPipelinePort::CurrentGuard(sl_element_id_t element_id) const
    -> std::shared_ptr<alcedo::PipelineGuard> {
  std::scoped_lock lock(mutex_);
  auto             it = guards_.find(element_id);
  return it == guards_.end() ? nullptr : it->second;
}

auto EditorSessionProductionPipelinePort::EnsureLoaded(sl_element_id_t element_id,
                                                       std::string*    error)
    -> std::shared_ptr<alcedo::PipelineGuard> {
  {
    std::scoped_lock lock(mutex_);
    auto             it = guards_.find(element_id);
    if (it != guards_.end()) {
      return it->second;
    }
  }
  std::function<std::shared_ptr<alcedo::PipelineGuard>(sl_element_id_t)> guard_loader;
  std::shared_ptr<alcedo::PipelineMgmtService>                           service;
  {
    std::scoped_lock lock(mutex_);
    guard_loader = services_.load_editor_pipeline_guard;
    if (services_.pipeline_service) {
      service = services_.pipeline_service();
    }
  }
  if (!guard_loader && !service) {
    if (error) {
      *error = "Pipeline service is unavailable";
    }
    return nullptr;
  }
  try {
    auto guard = guard_loader ? guard_loader(element_id) : service->LoadEditorPipeline(element_id);
    if (!guard || !guard->pipeline_) {
      if (error) {
        *error = "Failed to load pipeline for editor session";
      }
      return nullptr;
    }
    std::scoped_lock lock(mutex_);
    guards_[element_id] = guard;
    return guard;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return nullptr;
  } catch (...) {
    if (error) {
      *error = "Unknown pipeline load failure";
    }
    return nullptr;
  }
}

// ── Production background-task port ─────────────────────────────────────────

EditorSessionProductionTaskPort::EditorSessionProductionTaskPort(
    BackgroundTaskController* background_tasks)
    : background_tasks_(background_tasks) {}

void EditorSessionProductionTaskPort::SetBackgroundTasks(
    BackgroundTaskController* background_tasks) {
  std::scoped_lock lock(mutex_);
  background_tasks_ = background_tasks;
}

auto EditorSessionProductionTaskPort::BeginTask(const std::string& name, sl_element_id_t element_id)
    -> std::uint64_t {
  std::uint64_t             task_id = 0;
  BackgroundTaskController* tasks   = nullptr;
  {
    std::scoped_lock lock(mutex_);
    task_id = ++next_id_;
    tasks   = background_tasks_;
  }
  if (!tasks) {
    // Shell hosts without a task bar still get a stable non-zero id so the
    // session service can pair Begin/End without failing seal.
    return task_id;
  }

  BackgroundTaskSnapshot snapshot;
  snapshot.kind_             = BackgroundTaskKind::EditorSave;
  snapshot.state_            = BackgroundTaskState::Running;
  snapshot.title_            = QString::fromUtf8(name.empty() ? "editor_save" : name.c_str());
  snapshot.detail_           = QObject::tr("Saving editor changes");
  snapshot.progress_percent_ = -1;
  snapshot.cancelable_       = false;
  snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
  if (element_id != 0) {
    snapshot.affected_targets_ = QVariantList{static_cast<qulonglong>(element_id)};
  }
  // Phase 6C-5: explicit editor navigation locks while the global save
  // checkpoint is held. InteractionPolicyController publishes these as disabled
  // capabilities with a localized reason — do not infer from a generic busy flag.
  const QString save_reason = QObject::tr("Saving editor changes");
  snapshot.locks_           = {
      {InteractionCapability::SelectEditorImage, 0, save_reason},
      {InteractionCapability::SwitchWorkspace, 0, save_reason},
      {InteractionCapability::CheckoutVersion, 0, save_reason},
      {InteractionCapability::PasteAdjustments, 0, save_reason},
      {InteractionCapability::MergeAdjustments, 0, save_reason},
  };
  const QString ui_id = tasks->RegisterTask(snapshot, {});
  {
    std::scoped_lock lock(mutex_);
    active_task_ids_[task_id] = ui_id;
  }
  return task_id;
}

void EditorSessionProductionTaskPort::EndTask(std::uint64_t task_id, bool success,
                                              const std::string& message) {
  QString                   ui_id;
  BackgroundTaskController* tasks = nullptr;
  {
    std::scoped_lock lock(mutex_);
    tasks   = background_tasks_;
    auto it = active_task_ids_.find(task_id);
    if (it != active_task_ids_.end()) {
      ui_id = it->second;
      active_task_ids_.erase(it);
    }
  }
  if (!tasks || ui_id.isEmpty()) {
    return;
  }
  const auto final_state = success ? BackgroundTaskState::Succeeded : BackgroundTaskState::Failed;
  const auto detail      = QString::fromUtf8(message.c_str());
  auto       finish      = [tasks, ui_id, final_state, detail] {
    tasks->FinishTask(ui_id, final_state, detail);
  };
  if (QThread::currentThread() == tasks->thread()) {
    finish();
  } else {
    QMetaObject::invokeMethod(tasks, std::move(finish), Qt::QueuedConnection);
  }
}

// ── Production journal port ────────────────────────────────────────────────

EditorSessionProductionJournalPort::EditorSessionProductionJournalPort(
    EditorSessionProductionServices services)
    : services_(std::move(services)) {}

EditorSessionProductionJournalPort::~EditorSessionProductionJournalPort() {
  std::vector<std::jthread> workers;
  {
    std::scoped_lock lock(mutex_);
    shutting_down_ = true;
    workers.swap(workers_);
  }
  // Join outside mutex_ so a completion cannot deadlock while it reports its
  // terminal state through the session service.
  workers.clear();
}

void EditorSessionProductionJournalPort::SetServices(EditorSessionProductionServices services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
  materializer_.reset();
  materializer_storage_.reset();
  mini_git_materializer_.reset();
  mini_git_materializer_storage_.reset();
}

void EditorSessionProductionJournalPort::SetHistoryPort(
    std::weak_ptr<EditorSessionProductionHistoryPort> history_port) {
  std::scoped_lock lock(mutex_);
  history_port_ = std::move(history_port);
}

auto EditorSessionProductionJournalPort::HasJournalPathResolver() const -> bool {
  std::scoped_lock lock(mutex_);
  return static_cast<bool>(services_.journal_path);
}

auto EditorSessionProductionJournalPort::ImageLockFor(sl_element_id_t element_id)
    -> std::shared_ptr<std::mutex> {
  std::scoped_lock lock(mutex_);
  auto             it = image_locks_.find(element_id);
  if (it != image_locks_.end()) {
    return it->second;
  }
  auto image_lock = std::make_shared<std::mutex>();
  image_locks_.emplace(element_id, image_lock);
  return image_lock;
}

auto EditorSessionProductionJournalPort::FinalizeEdit(sl_element_id_t /*element_id*/,
                                                      std::uint64_t /*session_generation*/,
                                                      std::string* /*error*/) -> bool {
  // The current session service owns the provisional edit coalescer. This
  // boundary intentionally performs no file or database work.
  return true;
}

auto EditorSessionProductionJournalPort::WriterFor(sl_element_id_t element_id,
                                                   std::uint64_t   session_generation,
                                                   std::string*    error)
    -> std::shared_ptr<alcedo::EditorJournalWriter> {
  std::scoped_lock lock(mutex_);
  if (!services_.journal_path) {
    return nullptr;
  }

  std::filesystem::path path;
  try {
    path = services_.journal_path(element_id);
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return nullptr;
  } catch (...) {
    if (error) {
      *error = "Failed to resolve editor journal path";
    }
    return nullptr;
  }
  if (path.empty()) {
    return nullptr;
  }

  auto existing = writers_.find(element_id);
  if (existing != writers_.end() && existing->second->path() == path) {
    auto identity               = existing->second->identity();
    identity.session_generation = session_generation;
    if (!existing->second->SetIdentity(identity)) {
      if (error) {
        *error = existing->second->last_error();
      }
      return nullptr;
    }
    return existing->second;
  }
  if (existing != writers_.end()) {
    writers_.erase(existing);
  }

  try {
    const alcedo::EditorJournalIdentity identity{element_id, {}, session_generation, 1};
    auto writer = std::make_shared<alcedo::EditorJournalWriter>(identity, std::move(path));
    writers_.emplace(element_id, writer);
    return writer;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
  } catch (...) {
    if (error) {
      *error = "Failed to open editor journal";
    }
  }
  return nullptr;
}

auto EditorSessionProductionJournalPort::CommitJournal(sl_element_id_t element_id,
                                                       std::uint64_t   session_generation,
                                                       std::string*    error)
    -> alcedo::EditorJournalCommitOutcome {
  // No project/workspace path means this is the same non-persistent shell mode
  // used by the bootstrap runtime. Once a path exists, all file I/O is owned by
  // EditorJournalWriter.
  if (!HasJournalPathResolver()) {
    return {true, true, false, 0, 0, {}};
  }
  const auto       image_lock = ImageLockFor(element_id);
  std::scoped_lock image_guard(*image_lock);
  auto             writer = WriterFor(element_id, session_generation, error);
  if (!writer) {
    return {false, false,
            false, 0,
            0,     error != nullptr && !error->empty() ? *error : "Editor journal unavailable"};
  }
  const auto result = writer->CommitQueued();
  return {result.accepted,
          result.durable,
          result.pending,
          result.batch_commit_sequence,
          result.durable_operation_sequence,
          result.error};
}

auto EditorSessionProductionJournalPort::CommitJournalAsync(
    sl_element_id_t element_id, std::uint64_t session_generation,
    alcedo::EditorJournalCommitCallback callback) -> bool {
  std::scoped_lock lock(mutex_);
  if (shutting_down_) {
    return false;
  }
  std::jthread worker(
      [this, element_id, session_generation, callback = std::move(callback)]() mutable {
        std::string error;
        auto        outcome = CommitJournal(element_id, session_generation, &error);
        if (outcome.error.empty()) {
          outcome.error = std::move(error);
        }
        if (callback) {
          callback(std::move(outcome));
        }
      });
  workers_.push_back(std::move(worker));
  return true;
}

auto EditorSessionProductionJournalPort::Materialize(sl_element_id_t element_id,
                                                     std::uint64_t   session_generation,
                                                     std::string*    error)
    -> alcedo::EditorMaterializeOutcome {
  // Phase 6C-5 mini-Git path: capture was taken at save-checkpoint start from the
  // live pipeline; materialization validates the journal fold and writes DuckDB.
  bool use_mini_git = false;
  {
    std::scoped_lock lock(mutex_);
    use_mini_git = static_cast<bool>(services_.mini_git_journal_path);
  }
  if (use_mini_git) {
    return MaterializeMiniGit(element_id, session_generation, error);
  }
  // Legacy transaction-journal path (bootstrap / pre-cutover tests).
  if (!HasJournalPathResolver()) {
    return {true, true, 0, {}};
  }
  const auto       image_lock = ImageLockFor(element_id);
  std::scoped_lock image_guard(*image_lock);
  auto             materializer = EnsureMaterializer();
  if (!materializer) {
    return {true, true, 0, {}};
  }
  std::string writer_error;
  auto        writer = WriterFor(element_id, session_generation, &writer_error);
  if (!writer) {
    return {false, false, 0, writer_error.empty() ? "Editor journal unavailable" : writer_error};
  }
  auto history = services_.load_history ? services_.load_history(element_id) : nullptr;
  if (!history) {
    return {false, false, 0, "Editor history unavailable for materialize"};
  }
  const auto                       pipeline_params = LoadHeadPipelineParams(element_id, history);

  alcedo::EditorMaterializeRequest request;
  request.identity                  = writer->identity();
  request.target_operation_sequence = 0;  // materialize the durable journal head
  const auto result =
      materializer->Materialize(request, &writer->mutable_journal(), history,
                                pipeline_params.value_or(nlohmann::json::object()), error);
  alcedo::EditorMaterializeOutcome outcome{result.accepted, result.materialized,
                                           result.materialized_operation_sequence, result.error};
  if (!outcome.accepted || !outcome.materialized) {
    return outcome;
  }
  if (outcome.materialized_operation_sequence != 0) {
    CompactMaterializedHead(element_id, *writer, history, pipeline_params);
  }
  InvalidateThumbnail(element_id);
  return outcome;
}

auto EditorSessionProductionJournalPort::RecoverAndMaterialize(sl_element_id_t element_id,
                                                               std::uint64_t   session_generation,
                                                               std::string*    error)
    -> alcedo::EditorMaterializeOutcome {
  bool use_mini_git = false;
  {
    std::scoped_lock lock(mutex_);
    use_mini_git = static_cast<bool>(services_.mini_git_journal_path);
  }
  if (use_mini_git) {
    return RecoverMiniGit(element_id, error);
  }
  if (!HasJournalPathResolver()) {
    return {true, true, 0, {}};
  }
  const auto       image_lock = ImageLockFor(element_id);
  std::scoped_lock image_guard(*image_lock);
  auto             materializer = EnsureMaterializer();
  if (!materializer) {
    return {true, true, 0, {}};
  }
  std::string writer_error;
  auto        writer = WriterFor(element_id, session_generation, &writer_error);
  if (!writer) {
    return {false, false, 0, writer_error.empty() ? "Editor journal unavailable" : writer_error};
  }
  auto history = services_.load_history ? services_.load_history(element_id) : nullptr;
  if (!history) {
    return {false, false, 0, "Editor history unavailable for recovery"};
  }
  const auto pipeline_params = LoadHeadPipelineParams(element_id, history);
  const auto identity        = writer->identity();
  const auto result = materializer->RecoverAndMaterialize(identity, &writer->mutable_journal(),
                                                          history, pipeline_params, error);
  if (!result.accepted) {
    EmitRecoveryDiagnostic(element_id, *writer,
                           result.error.empty() ? "editor journal recovery failed" : result.error);
  }
  alcedo::EditorMaterializeOutcome outcome{result.accepted, result.materialized,
                                           result.materialized_operation_sequence, result.error};
  if (outcome.accepted && outcome.materialized) {
    InvalidateThumbnail(element_id);
  }
  return outcome;
}

auto EditorSessionProductionJournalPort::MaterializeAsync(
    sl_element_id_t element_id, std::uint64_t session_generation,
    alcedo::EditorMaterializeCallback callback) -> bool {
  std::scoped_lock lock(mutex_);
  if (shutting_down_) {
    return false;
  }
  std::jthread worker(
      [this, element_id, session_generation, callback = std::move(callback)]() mutable {
        std::string error;
        auto        outcome = Materialize(element_id, session_generation, &error);
        if (outcome.error.empty()) {
          outcome.error = std::move(error);
        }
        if (callback) {
          callback(std::move(outcome));
        }
      });
  workers_.push_back(std::move(worker));
  return true;
}

auto EditorSessionProductionJournalPort::DiscardUnflushed(sl_element_id_t element_id,
                                                          std::string*    error) -> bool {
  const auto                                   image_lock = ImageLockFor(element_id);
  std::scoped_lock                             image_guard(*image_lock);
  std::shared_ptr<alcedo::EditorJournalWriter> writer;
  {
    std::scoped_lock lock(mutex_);
    auto             it = writers_.find(element_id);
    if (it == writers_.end()) {
      return true;
    }
    writer = it->second;
  }
  return writer->DiscardQueued(error);
}

auto EditorSessionProductionJournalPort::RecordEdit(sl_element_id_t element_id,
                                                    std::uint64_t   session_generation,
                                                    const alcedo::EditTransaction& transaction,
                                                    std::string*                   error) -> bool {
  const auto       image_lock = ImageLockFor(element_id);
  std::scoped_lock image_guard(*image_lock);
  std::string      writer_error;
  auto             writer = WriterFor(element_id, session_generation, &writer_error);
  if (!writer) {
    if (error) {
      *error = writer_error.empty() ? "Editor journal unavailable" : writer_error;
    }
    return false;
  }
  const auto identity = writer->identity();
  if (writer->AppendEdit(identity, transaction) != 0) {
    return true;
  }
  if (error) {
    *error = writer->last_error();
  }
  return false;
}

auto EditorSessionProductionJournalPort::RecordCursorMove(sl_element_id_t element_id,
                                                          std::uint64_t   session_generation,
                                                          std::uint64_t   from_cursor,
                                                          std::uint64_t   to_cursor,
                                                          std::string*    error) -> bool {
  const auto       image_lock = ImageLockFor(element_id);
  std::scoped_lock image_guard(*image_lock);
  std::string      writer_error;
  auto             writer = WriterFor(element_id, session_generation, &writer_error);
  if (!writer) {
    if (error) {
      *error = writer_error.empty() ? "Editor journal unavailable" : writer_error;
    }
    return false;
  }
  const auto identity = writer->identity();
  if (writer->AppendCursorMove(identity, from_cursor, to_cursor) != 0) {
    return true;
  }
  if (error) {
    *error = writer->last_error();
  }
  return false;
}

auto EditorSessionProductionJournalPort::RecordRewriteTimeline(
    sl_element_id_t element_id, std::uint64_t session_generation,
    const alcedo::Hash128& expected_timeline_hash, const alcedo::Hash128& discarded_tail_hash,
    std::uint64_t retained_cursor, const alcedo::EditTransaction& replacement, std::string* error)
    -> bool {
  const auto       image_lock = ImageLockFor(element_id);
  std::scoped_lock image_guard(*image_lock);
  std::string      writer_error;
  auto             writer = WriterFor(element_id, session_generation, &writer_error);
  if (!writer) {
    if (error) {
      *error = writer_error.empty() ? "Editor journal unavailable" : writer_error;
    }
    return false;
  }
  const auto identity = writer->identity();
  if (writer->AppendRewriteTimeline(identity, expected_timeline_hash, discarded_tail_hash,
                                    retained_cursor, replacement) != 0) {
    return true;
  }
  if (error) {
    *error = writer->last_error();
  }
  return false;
}

auto EditorSessionProductionJournalPort::EnsureMaterializer()
    -> std::shared_ptr<alcedo::EditorHistoryMaterializer> {
  std::scoped_lock                                         lock(mutex_);
  std::function<std::shared_ptr<alcedo::StorageService>()> storage_resolver;
  storage_resolver = services_.storage_service;
  if (!storage_resolver) {
    return nullptr;
  }
  auto storage = storage_resolver();
  if (!storage) {
    return nullptr;
  }
  if (materializer_ && materializer_storage_ == storage) {
    return materializer_;
  }
  try {
    materializer_storage_ = storage;
    materializer_         = std::make_shared<alcedo::EditorHistoryMaterializer>(std::move(storage));
  } catch (...) {
    materializer_ = nullptr;
    materializer_storage_.reset();
  }
  return materializer_;
}

auto EditorSessionProductionJournalPort::EnsureMiniGitMaterializer()
    -> std::shared_ptr<alcedo::EditorMiniGitMaterializer> {
  std::scoped_lock                                         lock(mutex_);
  std::function<std::shared_ptr<alcedo::StorageService>()> storage_resolver;
  storage_resolver = services_.storage_service;
  if (!storage_resolver) {
    return nullptr;
  }
  auto storage = storage_resolver();
  if (!storage) {
    return nullptr;
  }
  if (mini_git_materializer_ && mini_git_materializer_storage_ == storage) {
    return mini_git_materializer_;
  }
  try {
    mini_git_materializer_storage_ = storage;
    mini_git_materializer_ =
        std::make_shared<alcedo::EditorMiniGitMaterializer>(std::move(storage));
  } catch (...) {
    mini_git_materializer_ = nullptr;
    mini_git_materializer_storage_.reset();
  }
  return mini_git_materializer_;
}

auto EditorSessionProductionJournalPort::MaterializeMiniGit(sl_element_id_t element_id,
                                                            std::uint64_t /*session_generation*/,
                                                            std::string* error)
    -> alcedo::EditorMaterializeOutcome {
  std::shared_ptr<EditorSessionProductionHistoryPort> history;
  {
    std::scoped_lock lock(mutex_);
    history = history_port_.lock();
  }
  std::optional<alcedo::EditorMiniGitSaveCapture> capture;
  if (history) {
    capture = history->ConsumeSaveCapture(element_id);
  }
  if (!capture.has_value()) {
    // Seal without CaptureSaveCheckpoint (e.g. no edits) — treat as empty journal save.
    alcedo::EditorMiniGitSaveCapture empty;
    empty.element_id                   = element_id;
    empty.journal_already_materialized = true;
    if (services_.mini_git_journal_path) {
      try {
        empty.journal_path = services_.mini_git_journal_path(element_id);
      } catch (...) {
      }
    }
    // Without a live capture we can only recover/truncate; success with no head move.
    auto materializer = EnsureMiniGitMaterializer();
    if (!materializer) {
      return {true, true, 0, {}};
    }
    // Recover path handles empty or leftover journals without requiring live capture.
    const auto recovered =
        materializer->RecoverAndMaterialize(element_id, empty.journal_path, error);
    alcedo::EditorMaterializeOutcome outcome{recovered.accepted, recovered.materialized, 0,
                                             recovered.error};
    if (outcome.accepted && outcome.materialized) {
      InvalidateThumbnail(element_id);
    }
    return outcome;
  }

  auto materializer = EnsureMiniGitMaterializer();
  if (!materializer) {
    return {true, true, 0, {}};
  }
  const auto                       result = materializer->Materialize(*capture, error);
  alcedo::EditorMaterializeOutcome outcome{result.accepted, result.materialized, 0, result.error};
  if (outcome.accepted && outcome.materialized) {
    // Clear in-memory journal records when the working state is still alive.
    if (history) {
      // Truncate already happened on disk; in-memory journal is dropped on Release.
    }
    InvalidateThumbnail(element_id);
  }
  return outcome;
}

auto EditorSessionProductionJournalPort::RecoverMiniGit(sl_element_id_t element_id,
                                                        std::string*    error)
    -> alcedo::EditorMaterializeOutcome {
  std::filesystem::path journal_path;
  {
    std::scoped_lock lock(mutex_);
    if (!services_.mini_git_journal_path) {
      return {true, true, 0, {}};
    }
    try {
      journal_path = services_.mini_git_journal_path(element_id);
    } catch (const std::exception& ex) {
      return {false, false, 0, ex.what()};
    }
  }
  auto materializer = EnsureMiniGitMaterializer();
  if (!materializer) {
    return {true, true, 0, {}};
  }
  const auto result = materializer->RecoverAndMaterialize(element_id, journal_path, error);
  alcedo::EditorMaterializeOutcome outcome{result.accepted, result.materialized, 0, result.error};
  if (outcome.accepted && outcome.materialized) {
    InvalidateThumbnail(element_id);
  }
  return outcome;
}

auto EditorSessionProductionJournalPort::LoadHeadPipelineParams(
    sl_element_id_t element_id, const std::shared_ptr<alcedo::EditHistory>& history)
    -> std::optional<nlohmann::json> {
  if (services_.load_pipeline) {
    if (auto pipeline = services_.load_pipeline(element_id)) {
      try {
        return pipeline->ExportPipelineParams();
      } catch (...) {
      }
    }
  }
  if (history) {
    if (auto materialized = history->GetActiveVersion().GetMaterializedParams()) {
      return materialized;
    }
    return history->GetImportPipelineParams();
  }
  return std::nullopt;
}

void EditorSessionProductionJournalPort::CompactMaterializedHead(
    sl_element_id_t element_id, alcedo::EditorJournalWriter& writer,
    const std::shared_ptr<alcedo::EditHistory>& history,
    const std::optional<nlohmann::json>&        pipeline_params) {
  if (!history) {
    return;
  }
  std::function<std::filesystem::path(sl_element_id_t)> journal_path_resolver;
  {
    std::scoped_lock lock(mutex_);
    journal_path_resolver = services_.journal_path;
  }
  if (!journal_path_resolver) {
    return;
  }
  std::filesystem::path active_path;
  try {
    active_path = journal_path_resolver(element_id);
  } catch (...) {
    return;
  }
  if (active_path.empty()) {
    return;
  }
  auto&      active = history->GetActiveVersion();
  const auto timeline_hash =
      alcedo::ComputeEditorTimelineHash(active.GetAllEditTransactions(), active.GetCursor());
  const auto head_params =
      active.GetMaterializedParams().value_or(pipeline_params.value_or(nlohmann::json::object()));
  const auto compact_path = std::filesystem::path(active_path.string() + ".compact");
  auto       identity     = writer.identity();
  ++identity.journal_generation;
  std::string compact_error;
  try {
    (void)writer.CompactToMaterializedHead(identity, timeline_hash, active.GetCursor(), head_params,
                                           active_path, compact_path, &compact_error);
  } catch (...) {
    // Compaction is a maintenance step; a failure leaves the previous journal
    // recoverable and does not undo the materialized DuckDB state.
  }
}

void EditorSessionProductionJournalPort::InvalidateThumbnail(sl_element_id_t element_id) {
  std::function<void(sl_element_id_t)> invalidate_thumbnail;
  {
    std::scoped_lock lock(mutex_);
    invalidate_thumbnail = services_.invalidate_thumbnail;
  }
  if (invalidate_thumbnail) {
    try {
      invalidate_thumbnail(element_id);
    } catch (...) {
      // Thumbnail invalidation is an acceleration step. A committed
      // history and serialized pipeline state remain durable if it cannot run.
    }
  }
}

void EditorSessionProductionJournalPort::EmitRecoveryDiagnostic(
    sl_element_id_t element_id, const alcedo::EditorJournalWriter& writer,
    const std::string& reason) {
  std::function<std::filesystem::path(sl_element_id_t)> journal_path_resolver;
  {
    std::scoped_lock lock(mutex_);
    journal_path_resolver = services_.journal_path;
  }
  if (!journal_path_resolver) {
    return;
  }
  std::filesystem::path journal_path;
  try {
    journal_path = journal_path_resolver(element_id);
  } catch (...) {
    return;
  }
  if (journal_path.empty()) {
    return;
  }
  std::string diag_error;
  (void)alcedo::WriteEditorJournalDiagnosticBundle(journal_path, writer.journal().bytes(), reason,
                                                   &diag_error);
}

// ── History port ────────────────────────────────────────────────────────────

struct EditorSessionProductionHistoryPort::WorkingState {
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
  if (snapshot == nullptr) {
    return;
  }
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
    if (!snapshot->fingerprint.empty()) {
      snapshot->fingerprint += "|";
    }
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
  if (record.kind == alcedo::MiniGitJournalRecordKind::kEditCommit &&
      record.edit_commit.has_value()) {
    const auto payload =
        alcedo::OrdinaryEditPayload::FromJSON(record.edit_commit->GetPayloadJSON());
    if (!ApplyCommittedPayload(guard, snapshot, payload, true, error)) {
      return false;
    }
  } else if (record.kind == alcedo::MiniGitJournalRecordKind::kHeadMove) {
    const auto source_head = replay_graph->GetActiveVersionRef().head_commit_hash;
    if (source_head.has_value()) {
      const auto& source = replay_graph->GetCommit(*source_head);
      if (record.target_head == source.GetFirstParentHash()) {
        const auto payload = alcedo::OrdinaryEditPayload::FromJSON(source.GetPayloadJSON());
        if (!ApplyCommittedPayload(guard, snapshot, payload, false, error)) {
          return false;
        }
      } else if (record.target_head.has_value()) {
        const auto& target  = replay_graph->GetCommit(*record.target_head);
        const auto  payload = alcedo::OrdinaryEditPayload::FromJSON(target.GetPayloadJSON());
        if (!ApplyCommittedPayload(guard, snapshot, payload, true, error)) {
          return false;
        }
      }
    } else if (record.target_head.has_value()) {
      const auto& target  = replay_graph->GetCommit(*record.target_head);
      const auto  payload = alcedo::OrdinaryEditPayload::FromJSON(target.GetPayloadJSON());
      if (!ApplyCommittedPayload(guard, snapshot, payload, true, error)) {
        return false;
      }
    }
  }
  return alcedo::MiniGitWorkingHistory::Replay(*replay_graph, {record}, error);
}

}  // namespace

void EditorSessionProductionHistoryPort::SetServices(EditorSessionProductionServices services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

void EditorSessionProductionHistoryPort::SetPipelinePort(
    std::shared_ptr<EditorSessionProductionPipelinePort> pipeline_port) {
  std::scoped_lock lock(mutex_);
  pipeline_port_ = std::move(pipeline_port);
}

auto EditorSessionProductionHistoryPort::EnsureWorkingState(sl_element_id_t element_id,
                                                            std::string*    error)
    -> std::shared_ptr<WorkingState> {
  std::shared_ptr<EditorSessionProductionPipelinePort>  pipeline_port;
  std::function<std::filesystem::path(sl_element_id_t)> journal_path;
  {
    std::scoped_lock lock(mutex_);
    const auto       existing = working_states_.find(element_id);
    if (existing != working_states_.end()) {
      return existing->second;
    }
    pipeline_port = pipeline_port_.lock();
    journal_path  = services_.mini_git_journal_path;
  }
  if (!pipeline_port) {
    if (error) *error = "Editor production pipeline port is unavailable";
    return nullptr;
  }
  auto guard = pipeline_port->EnsureLoaded(element_id, error);
  if (!guard || !guard->pipeline_ || !guard->commit_graph_) {
    if (error && error->empty()) *error = "Editor mini-Git pipeline state is unavailable";
    return nullptr;
  }

  std::filesystem::path path;
  if (journal_path) {
    path = journal_path(element_id);
  }
  auto journal = std::make_shared<alcedo::MiniGitJournal>(std::move(path));
  if (!journal->Load(error)) {
    return nullptr;
  }

  auto state            = std::make_shared<WorkingState>();
  state->pipeline_guard = guard;
  state->journal        = journal;
  auto replay_graph     = *guard->commit_graph_;
  auto validated_graph  = replay_graph;
  if (!alcedo::MiniGitWorkingHistory::Replay(validated_graph, journal->records(), error)) {
    return nullptr;
  }
  for (const auto& record : journal->records()) {
    if (!ApplyRecoveredRecord(*guard, &state->committed_snapshot, &replay_graph, record, error)) {
      return nullptr;
    }
  }
  *guard->commit_graph_            = std::move(replay_graph);
  guard->working_head_commit_hash_ = guard->commit_graph_->GetActiveVersionRef().head_commit_hash;
  guard->transaction_chain_hash_ =
      guard->commit_graph_->ChainHashForHead(guard->working_head_commit_hash_);
  guard->dirty_ = !journal->records().empty();
  if (!journal->records().empty()) {
    guard->pipeline_->SetExecutionStages();
  }
  state->history = std::make_unique<alcedo::MiniGitWorkingHistory>(guard->commit_graph_, journal);

  std::scoped_lock lock(mutex_);
  const auto [it, inserted] = working_states_.emplace(element_id, state);
  return inserted ? state : it->second;
}

auto EditorSessionProductionHistoryPort::Acquire(sl_element_id_t element_id, std::string* error)
    -> alcedo::EditorHistoryGuardHandle {
  std::function<std::shared_ptr<alcedo::PipelineGuard>(sl_element_id_t)> direct_loader;
  std::function<std::shared_ptr<alcedo::PipelineMgmtService>()>          service_loader;
  {
    std::scoped_lock lock(mutex_);
    direct_loader  = services_.load_editor_pipeline_guard;
    service_loader = services_.pipeline_service;
  }
  const bool has_real_pipeline =
      static_cast<bool>(direct_loader) || (service_loader && static_cast<bool>(service_loader()));
  if (has_real_pipeline) {
    std::string prepare_error;
    if (!EnsureWorkingState(element_id, &prepare_error)) {
      if (error) {
        *error = prepare_error.empty() ? "Editor mini-Git history initialization failed"
                                       : std::move(prepare_error);
      }
      return {};
    }
  }
  return alcedo::EditorHistoryGuardHandle{element_id, true};
}

void EditorSessionProductionHistoryPort::Release(const alcedo::EditorHistoryGuardHandle& guard) {
  if (!guard.valid) {
    return;
  }
  {
    std::scoped_lock lock(mutex_);
    working_states_.erase(guard.element_id);
    auto it = guards_.find(guard.element_id);
    if (it != guards_.end()) {
      guards_.erase(it);
    }
  }
}

auto EditorSessionProductionHistoryPort::CaptureAdjustmentBeforePreview(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::EditorAdjustmentPatch& patch,
    std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) {
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (state->pending_before.contains(patch.field_key)) {
    return true;
  }
  alcedo::EditorAdjustmentOperatorState before;
  std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
  if (!alcedo::ReadEditorAdjustmentOperatorState(*state->pipeline_guard->pipeline_, patch.field_key,
                                                 &before, error)) {
    return false;
  }
  state->pending_before.emplace(patch.field_key, std::move(before));
  return true;
}

auto EditorSessionProductionHistoryPort::CommitAdjustment(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::EditorAdjustmentPatch& patch,
    std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) {
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  const auto       spec = alcedo::ResolveEditorAdjustmentField(patch.field_key);
  if (!spec.has_value()) {
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

auto EditorSessionProductionHistoryPort::Undo(const alcedo::EditorHistoryGuardHandle& guard,
                                              std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  const auto       result = state->history->Undo();
  if (!result.error.empty()) {
    if (error) *error = result.error;
    return false;
  }
  if (!result.moved || !result.selected_commit.has_value()) return true;
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

auto EditorSessionProductionHistoryPort::Redo(const alcedo::EditorHistoryGuardHandle& guard,
                                              std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  const auto       result = state->history->Redo();
  if (!result.error.empty()) {
    if (error) *error = result.error;
    return false;
  }
  if (!result.moved || !result.selected_commit.has_value()) return true;
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

auto EditorSessionProductionHistoryPort::ReadAdjustmentSnapshot(
    const alcedo::EditorHistoryGuardHandle& guard, alcedo::EditorRenderAdjustmentSnapshot* snapshot,
    std::string* error) -> bool {
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (snapshot) *snapshot = state->committed_snapshot;
  return true;
}

auto EditorSessionProductionHistoryPort::CaptureSaveCheckpoint(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  // Shell hosts without a real pipeline still seal/switch; there is nothing to
  // materialize for mini-Git until a project-backed working state exists.
  bool has_real_pipeline = false;
  {
    std::scoped_lock lock(mutex_);
    has_real_pipeline =
        static_cast<bool>(services_.load_editor_pipeline_guard) ||
        (services_.pipeline_service && static_cast<bool>(services_.pipeline_service()));
  }
  if (!has_real_pipeline) {
    return true;
  }
  auto state = EnsureWorkingState(guard.element_id, error);
  if (!state) {
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->pipeline_ ||
      !state->pipeline_guard->commit_graph_ || !state->history || !state->journal) {
    if (error) *error = "Editor mini-Git save capture requires a live pipeline snapshot";
    return false;
  }

  nlohmann::json pipeline_params;
  try {
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    pipeline_params = state->pipeline_guard->pipeline_->ExportPipelineParams();
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  alcedo::EditorMiniGitSaveCapture capture;
  capture.element_id                   = guard.element_id;
  capture.working_head                 = state->history->working_head();
  capture.transaction_chain_hash       = state->history->transaction_chain_hash();
  capture.journal_records              = state->journal->records();
  capture.journal_path                 = state->journal->path();
  capture.journal_already_materialized = capture.journal_records.empty();

  const auto serialized                = alcedo::MakeEditorSerializedPipelineState(
      state->pipeline_guard->root_id_, capture.working_head, capture.transaction_chain_hash,
      pipeline_params);
  try {
    capture.materialization =
        state->pipeline_guard->commit_graph_->CaptureMaterializationWithSerializedPipelineState(
            serialized);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  // Freeze further journal appends for this prefix while materialization runs:
  // clear the working journal records after capture so a concurrent append cannot
  // land in the file being truncated. New edits after save use a fresh prefix.
  // The on-disk journal still holds the captured records until truncate succeeds.
  {
    std::scoped_lock lock(mutex_);
    save_captures_[guard.element_id] = std::move(capture);
  }
  return true;
}

auto EditorSessionProductionHistoryPort::ConsumeSaveCapture(sl_element_id_t element_id)
    -> std::optional<alcedo::EditorMiniGitSaveCapture> {
  std::scoped_lock lock(mutex_);
  auto             it = save_captures_.find(element_id);
  if (it == save_captures_.end()) {
    return std::nullopt;
  }
  auto capture = std::move(it->second);
  save_captures_.erase(it);
  return capture;
}

// ── Production scheduler ────────────────────────────────────────────────────

EditorSessionProductionSchedulerPort::EditorSessionProductionSchedulerPort(
    std::shared_ptr<alcedo::PipelineScheduler> pipeline_scheduler)
    : pipeline_scheduler_(std::move(pipeline_scheduler)) {
  // Lazy-create the pipeline scheduler on first real produce so shell host
  // construction never starts a worker thread pool.
}

EditorSessionProductionSchedulerPort::~EditorSessionProductionSchedulerPort() {
  std::vector<std::jthread>                                           workers;
  std::vector<std::shared_ptr<alcedo::EditorRenderCancellationToken>> cancellations;
  {
    std::scoped_lock lock(mutex_);
    shutting_down_ = true;
    for (auto& entry : jobs_) {
      auto& job     = entry.second;
      job.cancelled = true;
      if (job.request.intent.cancellation) {
        cancellations.push_back(job.request.intent.cancellation);
      }
    }
    sink_resolver_ = {};
    workers.swap(workers_);
  }
  for (const auto& cancellation : cancellations) {
    cancellation->Cancel();
  }
  // jthread destruction joins outside mutex_, allowing workers to finish their
  // cancellation/error bookkeeping without a teardown deadlock.
  workers.clear();
}

void EditorSessionProductionSchedulerPort::SetCoordinator(
    std::weak_ptr<alcedo::EditorRenderCoordinator> coordinator) {
  std::scoped_lock lock(mutex_);
  coordinator_ = std::move(coordinator);
}

void EditorSessionProductionSchedulerPort::SetSinkResolver(EditorFrameSinkResolver resolver) {
  std::scoped_lock lock(mutex_);
  sink_resolver_ = std::move(resolver);
}

void EditorSessionProductionSchedulerPort::SetPipelinePort(
    std::shared_ptr<EditorSessionProductionPipelinePort> pipeline_port) {
  std::scoped_lock lock(mutex_);
  pipeline_port_ = std::move(pipeline_port);
}

void EditorSessionProductionSchedulerPort::SetServices(EditorSessionProductionServices services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

void EditorSessionProductionSchedulerPort::SetTestFrameProducer(EditorTestFrameProducer producer) {
  std::scoped_lock lock(mutex_);
  test_producer_ = std::move(producer);
}

auto EditorSessionProductionSchedulerPort::Schedule(const alcedo::EditorRenderRequest& request)
    -> std::uint64_t {
  Job  job;
  bool has_producer = false;
  {
    std::scoped_lock lock(mutex_);
    if (shutting_down_) {
      return 0;
    }
    job.job_id        = ++next_job_id_;
    job.request       = request;
    jobs_[job.job_id] = job;
    scheduled_.push_back(request);

    // Prefer an injected test producer (shell / integration tests). Real pipeline
    // only when the image pool can resolve a descriptor with a path.
    if (test_producer_) {
      has_producer = true;
    } else if (services_.image_pool) {
      try {
        if (auto pool = services_.image_pool()) {
          const auto img = pool->Read<std::shared_ptr<alcedo::Image>>(
              request.intent.image_id,
              [](const std::shared_ptr<alcedo::Image>& image) { return image; });
          has_producer = static_cast<bool>(img) && !img->image_path_.empty();
        }
      } catch (...) {
        has_producer = false;
      }
    }
  }

  // Shell / synthetic ids: accept the job like the bootstrap scheduler and leave
  // it in-flight so the session stays Loading without a fake Failed transition.
  if (!has_producer) {
    return job.job_id;
  }

  {
    std::scoped_lock lock(mutex_);
    auto             it = jobs_.find(job.job_id);
    if (it != jobs_.end()) {
      it->second.running = true;
    }
  }

  // Complete asynchronously so the coordinator can mark the request in-flight
  // before NotifySchedulerCompleted runs (Schedule is called under ScheduleNext).
  std::jthread worker([this, job]() mutable {
    try {
      ExecuteJob(job);
    } catch (const std::exception& ex) {
      RemoveJob(job.job_id);
      CompleteJob(job.request, false, false, ex.what());
    } catch (...) {
      RemoveJob(job.job_id);
      CompleteJob(job.request, false, false, "First-frame producer exception");
    }
  });
  bool         cancel_started_worker = false;
  {
    std::scoped_lock lock(mutex_);
    if (shutting_down_) {
      cancel_started_worker = true;
    } else {
      workers_.push_back(std::move(worker));
    }
  }
  if (cancel_started_worker) {
    // Cancellation can re-enter scheduler/coordinator code, and local jthread
    // destruction joins. Both must happen after releasing mutex_.
    if (job.request.intent.cancellation) {
      job.request.intent.cancellation->Cancel();
    }
    return 0;
  }
  return job.job_id;
}

void EditorSessionProductionSchedulerPort::Cancel(std::uint64_t scheduler_job_id) {
  std::shared_ptr<alcedo::EditorRenderCancellationToken> cancellation;
  {
    std::scoped_lock lock(mutex_);
    auto             it = jobs_.find(scheduler_job_id);
    if (it == jobs_.end()) {
      return;
    }
    it->second.cancelled = true;
    cancellation         = it->second.request.intent.cancellation;
    pending_presentations_.erase(it->second.request.request_id);
    if (!it->second.running) {
      jobs_.erase(it);
      jobs_changed_.notify_all();
    }
  }
  // Cancellation callbacks can re-enter the coordinator and this scheduler.
  // Never invoke them while holding mutex_.
  if (cancellation) {
    cancellation->Cancel();
  }
}

void EditorSessionProductionSchedulerPort::WaitForSessionIdle(std::uint64_t session_generation) {
  std::unique_lock lock(mutex_);
  jobs_changed_.wait(lock, [&] {
    return std::none_of(jobs_.begin(), jobs_.end(), [&](const auto& entry) {
      return entry.second.request.intent.session_generation == session_generation;
    });
  });
}

void EditorSessionProductionSchedulerPort::RemoveJob(std::uint64_t job_id) {
  std::scoped_lock lock(mutex_);
  jobs_.erase(job_id);
  jobs_changed_.notify_all();
}

void EditorSessionProductionSchedulerPort::NotifyPresentationAcknowledged(
    std::uint64_t request_id, std::uint64_t image_generation, std::uint64_t image_identity) {
  bool                                             notify = false;
  std::shared_ptr<alcedo::EditorRenderCoordinator> coordinator;
  {
    std::scoped_lock lock(mutex_);
    auto             it = pending_presentations_.find(request_id);
    if (it == pending_presentations_.end()) {
      return;
    }
    if (it->second.image_generation != image_generation ||
        it->second.image_identity != image_identity) {
      return;
    }
    it->second.acknowledged = true;
    if (it->second.frame_submitted) {
      notify = true;
      pending_presentations_.erase(it);
      coordinator = coordinator_.lock();
    }
  }
  if (notify && coordinator) {
    coordinator->NotifyFramePresented(request_id);
  }
}

auto EditorSessionProductionSchedulerPort::last_scheduled() const
    -> std::vector<alcedo::EditorRenderRequest> {
  std::scoped_lock lock(mutex_);
  return scheduled_;
}

auto EditorSessionProductionSchedulerPort::pending_present_request_id() const -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  return pending_presentations_.empty() ? 0 : pending_presentations_.begin()->first;
}

void EditorSessionProductionSchedulerPort::ExecuteJob(Job job) {
  bool cancelled_before_execution = false;
  {
    std::scoped_lock lock(mutex_);
    auto             it = jobs_.find(job.job_id);
    if (it != jobs_.end() && it->second.cancelled) {
      jobs_.erase(it);
      jobs_changed_.notify_all();
      cancelled_before_execution = true;
    }
  }
  if (cancelled_before_execution) {
    CompleteJob(job.request, false, false, "Cancelled before execution");
    return;
  }

  alcedo::IFrameSink*     sink = nullptr;
  EditorFrameSinkResolver resolver;
  EditorTestFrameProducer test_producer;
  {
    std::scoped_lock lock(mutex_);
    resolver      = sink_resolver_;
    test_producer = test_producer_;
  }
  if (resolver) {
    sink = resolver();
  }

  // No presentation sink yet: accept schedule for shell routing but do not
  // claim success. Session stays Loading until a later retry with a bound sink.
  if (!sink) {
    // When presentation target is not bound, treat as deferred failure so the
    // coordinator can start the next request. Open path typically has sink id
    // stamped before Schedule; missing sink is a real error.
    RemoveJob(job.job_id);
    CompleteJob(job.request, false, false, "No presentation frame sink bound");
    return;
  }

  // One-shot first-frame composition only. QualityBase / DetailPatch / superseded
  // interactive frames must not accumulate in an application-level pending map.
  if (auto* direct_sink = dynamic_cast<alcedo::editor_rhi::DirectFrameSink*>(sink)) {
    direct_sink->SetFirstFrameCompositionCallback(
        [weak = weak_from_this()](std::uint64_t request_id, std::uint64_t image_generation,
                                  std::uint64_t image_identity) {
          if (const auto self = weak.lock()) {
            self->NotifyPresentationAcknowledged(request_id, image_generation, image_identity);
          }
        });
  }

  // Stamp role/mode before EnsureSize so DirectFrameSink can suppress
  // targetSizeRequested for non-reference frames (DetailPatch / RoiFrame).
  // Otherwise a zoomed ROI output size rewrites render-reference geometry and
  // the detail patch no longer covers the viewport.
  alcedo::FramePreviewMetadata meta = FrameRoleToPreviewMetadata(job.request.intent);
  meta.presentation_request_id      = job.request.request_id;
  sink->SetNextFramePreviewMetadata(meta);
  if (job.request.intent.frame_role == alcedo::FrameRole::DetailPatch) {
    sink->SetNextFramePresentationMode(alcedo::FramePresentationMode::ViewportTransformed);
  } else {
    sink->SetNextFramePresentationMode(alcedo::FramePresentationMode::FullFrame);
  }

  const int width  = std::max(1, job.request.intent.requested_width);
  const int height = std::max(1, job.request.intent.requested_height);
  sink->EnsureSize(width, height);

  auto*       direct_sink      = dynamic_cast<alcedo::editor_rhi::DirectFrameSink*>(sink);
  const auto  submitted_before = direct_sink ? direct_sink->submitted_frame_count() : 0;

  std::string error;
  bool        submitted = false;
  bool        ok        = false;

  // Only the actual session-opening InteractivePrimary is tracked for the
  // one-shot composition acknowledgement. Adjustment FAST frames also use the
  // InteractivePrimary role, but AcknowledgeFirstComposition deliberately
  // fires once per image generation; tracking those requests would leave every
  // drag frame permanently resident in pending_presentations_.
  const auto  reason    = job.request.intent.reason;
  const bool  track_first_composition =
      job.request.intent.frame_role == alcedo::FrameRole::InteractivePrimary &&
      (reason == alcedo::EditorRenderReason::InitialFrame ||
       reason == alcedo::EditorRenderReason::ImageSwitch ||
       reason == alcedo::EditorRenderReason::Retry);
  if (track_first_composition) {
    std::scoped_lock lock(mutex_);
    pending_presentations_[job.request.request_id] = PendingPresentation{
        job.request.intent.session_generation, job.request.intent.image_id, false, false};
  }

  if (test_producer) {
    ok        = test_producer(sink, job.request);
    submitted = ok && (!direct_sink || direct_sink->submitted_frame_count() > submitted_before);
    if (!ok) {
      error = "Test frame producer failed";
    }
  } else {
    ok        = TryProducePipelineFrame(job.request, sink, &error);
    // Pipeline writes through Map/Unmap/NotifyFrameReady when successful.
    submitted = ok && (!direct_sink || direct_sink->submitted_frame_count() > submitted_before);
    if (ok && !submitted) {
      ok    = false;
      error = "Pipeline completed without submitting a native presentation frame";
    }
  }

  bool cancelled_during_execution = false;
  {
    std::scoped_lock lock(mutex_);
    auto             it = jobs_.find(job.job_id);
    if (it != jobs_.end() && it->second.cancelled) {
      jobs_.erase(it);
      jobs_changed_.notify_all();
      pending_presentations_.erase(job.request.request_id);
      cancelled_during_execution = true;
    } else {
      if (!ok || !submitted) {
        pending_presentations_.erase(job.request.request_id);
      }
      jobs_.erase(job.job_id);
      jobs_changed_.notify_all();
    }
  }
  if (cancelled_during_execution) {
    CompleteJob(job.request, false, false, "Cancelled during execution");
    return;
  }

  CompleteJob(job.request, ok, submitted,
              error.empty() ? (ok ? "Render completed" : "Render failed") : error);
}

auto EditorSessionProductionSchedulerPort::TryProducePipelineFrame(
    const alcedo::EditorRenderRequest& request, alcedo::IFrameSink* sink, std::string* error)
    -> bool {
  if (!sink) {
    if (error) {
      *error = "Presentation sink is null";
    }
    return false;
  }

  std::shared_ptr<EditorSessionProductionPipelinePort> pipeline_port;
  EditorSessionProductionServices                      services;
  std::shared_ptr<alcedo::PipelineScheduler>           scheduler;
  {
    std::scoped_lock lock(mutex_);
    pipeline_port = pipeline_port_;
    services      = services_;
    if (!pipeline_scheduler_) {
      pipeline_scheduler_ = std::make_shared<alcedo::PipelineScheduler>(1);
    }
    scheduler = pipeline_scheduler_;
  }

  if (!pipeline_port || !scheduler) {
    if (error) {
      *error = "Production pipeline scheduler is not fully configured";
    }
    return false;
  }

  auto guard = pipeline_port->EnsureLoaded(request.intent.element_id, error);
  if (!guard || !guard->pipeline_) {
    if (error && error->empty()) {
      *error = "No pipeline guard for image; open may lack a project";
    }
    return false;
  }

  std::shared_ptr<alcedo::ImagePoolService> image_pool;
  if (services.image_pool) {
    image_pool = services.image_pool();
  }
  if (!image_pool) {
    if (error) {
      *error = "Image pool is unavailable";
    }
    return false;
  }

  try {
    auto                           exec = guard->pipeline_;
    std::shared_ptr<alcedo::Image> image_desc;
    try {
      image_desc = image_pool->Read<std::shared_ptr<alcedo::Image>>(
          request.intent.image_id,
          [](const std::shared_ptr<alcedo::Image>& image) { return image; });
    } catch (...) {
    }

    std::shared_ptr<alcedo::ImageBuffer> input;
    {
      std::scoped_lock lock(mutex_);
      if (cached_input_image_id_ == request.intent.image_id) {
        input = cached_input_;
      }
    }
    if (!input) {
      auto loaded = controllers::LoadImageInputBuffer(image_pool, request.intent.image_id);
      std::scoped_lock lock(mutex_);
      if (cached_input_image_id_ != request.intent.image_id || !cached_input_) {
        cached_input_image_id_ = request.intent.image_id;
        cached_input_          = std::move(loaded);
      }
      input = cached_input_;
    }

    alcedo::PipelineTask task;
    task.input_                             = std::move(input);
    task.input_desc_                        = std::move(image_desc);
    task.pipeline_executor_                 = exec;
    task.options_.render_desc_.render_type_ = RenderTypeForIntent(request.intent);
    // Phase 5D A5: a DetailPatch (Detail quality) must load the visible viewport
    // ROI from the executor/sink so the produced frame carries the correct
    // source_roi_norm. Full-frame renders (Interactive/Quality) keep the whole
    // frame; AttachExecutionStages wired the executor's GetViewportRenderRegion
    // to the DirectFrameSink, whose region the interaction controller keeps
    // current via applyViewStateToViewport before the render intent is submitted.
    task.options_.render_desc_.use_viewport_region_ =
        request.intent.quality == alcedo::EditorRenderQuality::Detail;
    task.options_.render_desc_.frame_metadata_ = FrameRoleToPreviewMetadata(request.intent);
    // PipelineTask::SetExecutorRenderParams forwards this metadata to the sink
    // again, after ExecuteJob's initial stamp. Preserve the coordinator request
    // id here or the forwarded copy resets it to zero, making the already
    // visible first frame impossible to acknowledge and leaving the session in
    // Loading forever (which then rejects zoom DetailRefresh requests).
    task.options_.render_desc_.frame_metadata_.presentation_request_id = request.request_id;
    task.options_.is_callback_                                         = false;
    task.options_.is_seq_callback_                                     = false;
    task.options_.is_blocking_                                         = true;
    task.prepare_with_render_lock_ = [snapshot = request.intent.adjustment,
                                      sink](alcedo::PipelineTask& locked_task) {
      auto locked_exec = locked_task.pipeline_executor_;
      if (!locked_exec) {
        return false;
      }
      std::string apply_error;
      if (!alcedo::ApplyEditorAdjustmentSnapshot(*locked_exec, snapshot, &apply_error)) {
        throw std::runtime_error(apply_error.empty() ? "Failed to apply editor adjustment"
                                                     : apply_error);
      }
      controllers::EnsureLoadingOperatorDefaults(locked_exec);
      controllers::AttachExecutionStages(locked_exec, sink);
      return true;
    };

    auto promise = std::make_shared<std::promise<std::shared_ptr<alcedo::ImageBuffer>>>();
    auto future  = promise->get_future();
    task.result_ = promise;

    if (request.intent.cancellation) {
      task.cancel_requested_ = [token = request.intent.cancellation]() {
        return token && token->IsCancelled();
      };
    }

    scheduler->ScheduleTask(std::move(task));
    const auto result = future.get();
    if (!result) {
      if (error) {
        *error = "Pipeline returned an empty result";
      }
      return false;
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return false;
  } catch (...) {
    if (error) {
      *error = "Pipeline render failed";
    }
    return false;
  }
}

void EditorSessionProductionSchedulerPort::CompleteJob(const alcedo::EditorRenderRequest& request,
                                                       bool success, bool frame_submitted,
                                                       std::string message) {
  std::shared_ptr<alcedo::EditorRenderCoordinator> coordinator;
  {
    std::scoped_lock lock(mutex_);
    coordinator = coordinator_.lock();
    if (!success || !frame_submitted) {
      pending_presentations_.erase(request.request_id);
    }
  }
  if (!coordinator) {
    return;
  }
  coordinator->NotifySchedulerCompleted(request.request_id, success, std::move(message));
  if (success && frame_submitted) {
    coordinator->NotifyFrameSubmitted(request.request_id);
    bool acknowledged = false;
    {
      std::scoped_lock lock(mutex_);
      auto             it = pending_presentations_.find(request.request_id);
      if (it != pending_presentations_.end()) {
        it->second.frame_submitted = true;
        acknowledged               = it->second.acknowledged;
        if (acknowledged) {
          pending_presentations_.erase(it);
        }
      }
    }
    if (acknowledged) {
      coordinator->NotifyFramePresented(request.request_id);
    }
  }
}

}  // namespace alcedo::ui
