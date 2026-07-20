//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"
#include "app/history_mgmt_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/editor_journal_writer.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
class ImagePoolService;
class ProjectService;
}  // namespace alcedo

namespace alcedo::ui {

class BackgroundTaskController;

/// Resolves the active production presentation sink for a render intent.
using EditorFrameSinkResolver = std::function<alcedo::IFrameSink*()>;

/// Optional project services for real pipeline open (null when no project).
struct EditorSessionProductionServices {
  std::function<std::shared_ptr<alcedo::PipelineMgmtService>()>    pipeline_service;
  std::function<std::shared_ptr<alcedo::EditHistoryMgmtService>()> history_service;
  std::function<std::shared_ptr<alcedo::ImagePoolService>()>       image_pool;
  std::function<std::filesystem::path(sl_element_id_t)>             journal_path;
  std::function<alcedo::EditorMaterializeOutcome(sl_element_id_t, std::uint64_t, std::string*)>
      materialize_editor_session;
  std::function<void(sl_element_id_t)>                               invalidate_thumbnail;
};

/// Production pipeline port: acquires real PipelineGuards when services exist;
/// falls back to a valid no-op handle so shell tests without a project keep working.
class EditorSessionProductionPipelinePort final : public alcedo::IEditorPipelinePort {
 public:
  void SetServices(EditorSessionProductionServices services);

  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> alcedo::EditorPipelineGuardHandle override;
  void Release(const alcedo::EditorPipelineGuardHandle& guard) override;

  [[nodiscard]] auto CurrentGuard(sl_element_id_t element_id) const
      -> std::shared_ptr<alcedo::PipelineGuard>;

  /// Lazy-load a real pipeline guard for first-frame production. Returns null
  /// when services are unavailable or the element has no stored pipeline.
  auto EnsureLoaded(sl_element_id_t element_id, std::string* error)
      -> std::shared_ptr<alcedo::PipelineGuard>;

 private:
  EditorSessionProductionServices                                         services_{};
  mutable std::mutex                                                      mutex_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<alcedo::PipelineGuard>> guards_;
};

/// Phase 5E production task port: registers editor_save work with the shared
/// BackgroundTaskController so image-switch/close seal is visible in the task bar.
class EditorSessionProductionTaskPort final : public alcedo::IEditorTaskPort {
 public:
  explicit EditorSessionProductionTaskPort(BackgroundTaskController* background_tasks = nullptr);

  void SetBackgroundTasks(BackgroundTaskController* background_tasks);

  auto BeginTask(const std::string& name, sl_element_id_t element_id) -> std::uint64_t override;
  void EndTask(std::uint64_t task_id, bool success, const std::string& message) override;

 private:
  BackgroundTaskController*                         background_tasks_ = nullptr;
  mutable std::mutex                                mutex_;
  std::uint64_t                                     next_id_ = 0;
  std::unordered_map<std::uint64_t, QString>        active_task_ids_;
};

/// Production journal port. One EditorJournalWriter is retained per image so
/// an A save can flush independently while the session service loads B.
/// Materialization is supplied as a narrow application callback; when no
/// project is open the port preserves the bootstrap no-op behavior.
class EditorSessionProductionJournalPort final : public alcedo::IEditorJournalPort {
 public:
  explicit EditorSessionProductionJournalPort(EditorSessionProductionServices services = {});
  ~EditorSessionProductionJournalPort() override;

  void SetServices(EditorSessionProductionServices services);

  auto FinalizeEdit(sl_element_id_t element_id, std::uint64_t session_generation,
                    std::string* error) -> bool override;
  auto CommitJournal(sl_element_id_t element_id, std::uint64_t session_generation,
                     std::string* error) -> alcedo::EditorJournalCommitOutcome override;
  auto CommitJournalAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                          alcedo::EditorJournalCommitCallback callback) -> bool override;
  auto Materialize(sl_element_id_t element_id, std::uint64_t session_generation,
                   std::string* error) -> alcedo::EditorMaterializeOutcome override;
  auto MaterializeAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                        alcedo::EditorMaterializeCallback callback) -> bool override;
  auto DiscardUnflushed(sl_element_id_t element_id, std::string* error) -> bool override;

 private:
  auto WriterFor(sl_element_id_t element_id, std::uint64_t session_generation,
                 std::string* error) -> std::shared_ptr<alcedo::EditorJournalWriter>;
  auto ImageLockFor(sl_element_id_t element_id) -> std::shared_ptr<std::mutex>;
  [[nodiscard]] auto HasJournalPathResolver() const -> bool;

  EditorSessionProductionServices                                  services_{};
  mutable std::mutex                                               mutex_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<alcedo::EditorJournalWriter>> writers_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<std::mutex>> image_locks_;
  std::vector<std::jthread>                                        workers_;
  bool                                                             shutting_down_ = false;
};

/// Production history port: real EditHistoryMgmtService when available.
class EditorSessionProductionHistoryPort final : public alcedo::IEditorHistoryPort {
 public:
  void SetServices(EditorSessionProductionServices services);

  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> alcedo::EditorHistoryGuardHandle override;
  void Release(const alcedo::EditorHistoryGuardHandle& guard) override;
  auto Undo(const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool override;
  auto Redo(const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool override;
  auto ReadAdjustmentSnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                              alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
      -> bool override;

 private:
  EditorSessionProductionServices services_{};
  mutable std::mutex              mutex_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<alcedo::EditHistoryGuard>> guards_;
};

/// Optional test/harness producer: write real pixel data into the production sink.
/// Returns true when the frame was written and submitted through the sink.
using EditorTestFrameProducer =
    std::function<bool(alcedo::IFrameSink* sink, const alcedo::EditorRenderRequest& request)>;

/// Phase 5B production scheduler: sole path from EditorRenderCoordinator into
/// pipeline work that attaches the production presentation sink, runs the
/// InteractivePrimary / QualityBase open path, and reports complete/submit.
///
/// Without project services or a test producer, Schedule still accepts the job
/// so bootstrap-style shell tests keep routing intents; the job then fails with
/// a concrete message so the session does not silently hang forever when a real
/// sink and image are present.
class EditorSessionProductionSchedulerPort final
    : public alcedo::IEditorPipelineSchedulerPort,
      public std::enable_shared_from_this<EditorSessionProductionSchedulerPort> {
 public:
  explicit EditorSessionProductionSchedulerPort(
      std::shared_ptr<alcedo::PipelineScheduler> pipeline_scheduler = nullptr);
  ~EditorSessionProductionSchedulerPort() override;

  void SetCoordinator(std::weak_ptr<alcedo::EditorRenderCoordinator> coordinator);
  void SetSinkResolver(EditorFrameSinkResolver resolver);
  void SetPipelinePort(std::shared_ptr<EditorSessionProductionPipelinePort> pipeline_port);
  void SetServices(EditorSessionProductionServices services);
  void SetTestFrameProducer(EditorTestFrameProducer producer);

  auto Schedule(const alcedo::EditorRenderRequest& request) -> std::uint64_t override;
  void Cancel(std::uint64_t scheduler_job_id) override;
  void WaitForSessionIdle(std::uint64_t session_generation) override;

  /// Exact render-thread acknowledgement after a compatible frame was sampled.
  void NotifyPresentationAcknowledged(std::uint64_t request_id,
                                      std::uint64_t image_generation,
                                      std::uint64_t image_identity);

  [[nodiscard]] auto last_scheduled() const -> std::vector<alcedo::EditorRenderRequest>;
  [[nodiscard]] auto pending_present_request_id() const -> std::uint64_t;

 private:
  struct Job {
    std::uint64_t              job_id    = 0;
    alcedo::EditorRenderRequest request{};
    bool                       cancelled = false;
    bool                       running   = false;
  };

  struct PendingPresentation {
    std::uint64_t image_generation = 0;
    std::uint64_t image_identity   = 0;
    bool          frame_submitted  = false;
    bool          acknowledged     = false;
  };

  void ExecuteJob(Job job);
  auto TryProduceFrame(const alcedo::EditorRenderRequest& request, alcedo::IFrameSink* sink,
                       std::string* error) -> bool;
  auto TryProducePipelineFrame(const alcedo::EditorRenderRequest& request, alcedo::IFrameSink* sink,
                               std::string* error) -> bool;
  void CompleteJob(const alcedo::EditorRenderRequest& request, bool success, bool frame_submitted,
                   std::string message);
  void RemoveJob(std::uint64_t job_id);

  std::shared_ptr<alcedo::PipelineScheduler>              pipeline_scheduler_;
  std::weak_ptr<alcedo::EditorRenderCoordinator>          coordinator_;
  EditorFrameSinkResolver                                 sink_resolver_;
  std::shared_ptr<EditorSessionProductionPipelinePort>    pipeline_port_;
  EditorSessionProductionServices                         services_{};
  EditorTestFrameProducer                                 test_producer_;
  mutable std::mutex                                      mutex_;
  std::condition_variable                                 jobs_changed_;
  std::uint64_t                                           next_job_id_ = 0;
  std::unordered_map<std::uint64_t, Job>                  jobs_;
  std::unordered_map<std::uint64_t, PendingPresentation>  pending_presentations_;
  std::vector<alcedo::EditorRenderRequest>                scheduled_;
  std::vector<std::jthread>                               workers_;
  bool                                                    shutting_down_ = false;
};

}  // namespace alcedo::ui
