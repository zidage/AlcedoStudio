//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_render_intent.hpp"
#include "app/pipeline_service.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
class ImagePoolService;
}

namespace alcedo::ui {

using EditorSessionFrameSinkResolver = std::function<alcedo::IFrameSink*()>;
using EditorSessionTestFrameProducer =
    std::function<bool(alcedo::IFrameSink*, const alcedo::EditorRenderRequest&)>;

struct EditorSessionSchedulerServices {
  /// Resolve the image pool used for render input acquisition.
  std::function<std::shared_ptr<alcedo::ImagePoolService>()> image_pool;
};

/// Owns render jobs, presentation acknowledgements, and worker lifetime for
/// the editor session. It is the only session port that invokes PipelineTask.
class EditorSessionRenderSchedulerPort final
    : public alcedo::IEditorPipelineSchedulerPort,
      public std::enable_shared_from_this<EditorSessionRenderSchedulerPort> {
 public:
  /// Construct the scheduler port around the application pipeline scheduler.
  explicit EditorSessionRenderSchedulerPort(
      std::shared_ptr<alcedo::PipelineScheduler> pipeline_scheduler = nullptr);
  /// Cancel jobs and join all scheduler workers.
  ~EditorSessionRenderSchedulerPort() override;

  /// Set the coordinator that receives render lifecycle outcomes.
  void SetCoordinator(std::weak_ptr<alcedo::EditorRenderCoordinator> coordinator);
  /// Set the sink used for native frame presentation.
  void SetSinkResolver(EditorSessionFrameSinkResolver resolver);
  /// Set the pipeline guard port used for frame production.
  void SetPipelinePort(std::shared_ptr<EditorSessionPipelinePort> pipeline_port);
  /// Set image-pool and related render dependencies.
  void SetServices(EditorSessionSchedulerServices services);
  /// Set an optional deterministic frame producer for focused tests.
  void SetTestFrameProducer(EditorSessionTestFrameProducer producer);

  /// Schedule one render request and return its scheduler job ID.
  auto Schedule(const alcedo::EditorRenderRequest& request) -> std::uint64_t override;
  /// Cancel one scheduled render job.
  void Cancel(std::uint64_t scheduler_job_id) override;
  /// Wait until all jobs for one session generation have finished.
  void WaitForSessionIdle(std::uint64_t session_generation) override;
  /// Record that the presentation sink accepted one frame.
  void NotifyPresentationAcknowledged(std::uint64_t request_id, std::uint64_t image_generation,
                                      std::uint64_t image_identity);

  /// Return a test-visible copy of scheduled requests.
  [[nodiscard]] auto last_scheduled() const -> std::vector<alcedo::EditorRenderRequest>;
  /// Return the request currently awaiting presentation acknowledgement.
  [[nodiscard]] auto pending_present_request_id() const -> std::uint64_t;

 private:
  struct Job {
    std::uint64_t               job_id = 0;
    alcedo::EditorRenderRequest request{};
    bool                        cancelled = false;
    bool                        running   = false;
  };
  struct PendingPresentation {
    std::uint64_t image_generation = 0;
    std::uint64_t image_identity   = 0;
    bool          frame_submitted  = false;
    bool          acknowledged     = false;
  };

  void ExecuteJob(Job job);
  auto TryProducePipelineFrame(const alcedo::EditorRenderRequest& request, alcedo::IFrameSink* sink,
                               std::string* error) -> bool;
  void CompleteJob(const alcedo::EditorRenderRequest& request, bool success, bool frame_submitted,
                   std::string message);
  void RemoveJob(std::uint64_t job_id);

  std::shared_ptr<alcedo::PipelineScheduler>             pipeline_scheduler_;
  std::weak_ptr<alcedo::EditorRenderCoordinator>         coordinator_;
  EditorSessionFrameSinkResolver                         sink_resolver_;
  std::shared_ptr<EditorSessionPipelinePort>             pipeline_port_;
  EditorSessionSchedulerServices                         services_{};
  EditorSessionTestFrameProducer                         test_producer_;
  mutable std::mutex                                     mutex_;
  std::condition_variable                                jobs_changed_;
  std::uint64_t                                          next_job_id_ = 0;
  std::unordered_map<std::uint64_t, Job>                 jobs_;
  std::unordered_map<std::uint64_t, PendingPresentation> pending_presentations_;
  std::vector<alcedo::EditorRenderRequest>               scheduled_;
  // std::thread (not jthread): Apple libc++ on the CI deployment target still
  // omits std::jthread even under -std=c++20. Destructor joins workers_ explicitly.
  std::vector<std::thread>                               workers_;
  image_id_t                                             cached_input_image_id_ = 0;
  std::shared_ptr<alcedo::ImageBuffer>                   cached_input_;
  bool                                                   shutting_down_ = false;
};

}  // namespace alcedo::ui
