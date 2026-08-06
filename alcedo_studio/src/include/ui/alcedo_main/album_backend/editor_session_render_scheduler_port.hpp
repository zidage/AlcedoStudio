//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

/// Thin adapter: builds a PipelineTask and hands it to PipelineScheduler.
/// No private worker thread and no second request queue — the coordinator owns
/// single-flight; PipelineScheduler owns execution.
class EditorSessionRenderSchedulerPort final : public alcedo::IEditorPipelineSchedulerPort {
 public:
  explicit EditorSessionRenderSchedulerPort(
      std::shared_ptr<alcedo::PipelineScheduler> pipeline_scheduler = nullptr);
  ~EditorSessionRenderSchedulerPort() override;

  void SetCoordinator(std::weak_ptr<alcedo::EditorRenderCoordinator> coordinator);
  void SetSinkResolver(EditorSessionFrameSinkResolver resolver);
  void SetPipelinePort(std::shared_ptr<EditorSessionPipelinePort> pipeline_port);
  void SetServices(EditorSessionSchedulerServices services);
  /// Deterministic frame producer for focused tests (runs on the pipeline pool).
  void SetTestFrameProducer(EditorSessionTestFrameProducer producer);

  auto Schedule(const alcedo::EditorRenderRequest& request) -> std::uint64_t override;
  void Cancel(std::uint64_t scheduler_job_id) override;
  void WaitForSessionIdle(std::uint64_t session_epoch) override;
  [[nodiscard]] auto last_scheduled() const -> std::vector<alcedo::EditorRenderRequest>;

 private:
  struct Job {
    std::uint64_t               job_id = 0;
    alcedo::EditorRenderRequest request{};
    bool                        cancelled = false;
  };

  [[nodiscard]] auto CanProduceFrame(const alcedo::EditorRenderRequest& request) const -> bool;
  [[nodiscard]] auto EnsurePipelineScheduler() -> std::shared_ptr<alcedo::PipelineScheduler>;
  void               DispatchJob(Job job);
  void               DispatchTestProducer(Job job, alcedo::IFrameSink* sink,
                                          EditorSessionTestFrameProducer producer);
  void               DispatchPipelineFrame(Job job, alcedo::IFrameSink* sink);
  void               FinishJob(const Job& job, bool success, std::string message);
  void CompleteJob(const alcedo::EditorRenderRequest& request, bool success, std::string message);
  [[nodiscard]] auto JobIsCancelled(const Job& job) const -> bool;

  std::shared_ptr<alcedo::PipelineScheduler>     pipeline_scheduler_;
  std::weak_ptr<alcedo::EditorRenderCoordinator> coordinator_;
  EditorSessionFrameSinkResolver                 sink_resolver_;
  std::shared_ptr<EditorSessionPipelinePort>     pipeline_port_;
  EditorSessionSchedulerServices                 services_{};
  EditorSessionTestFrameProducer                 test_producer_;
  mutable std::mutex                             mutex_;
  std::condition_variable                        jobs_changed_;
  std::uint64_t                                  next_job_id_ = 0;
  std::optional<Job>                             running_job_;
  std::vector<alcedo::EditorRenderRequest>       scheduled_;
  image_id_t                                     cached_input_image_id_ = 0;
  std::shared_ptr<alcedo::ImageBuffer>           cached_input_;
  bool                                           shutting_down_ = false;
};

}  // namespace alcedo::ui
