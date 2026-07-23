//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_service.hpp"

namespace alcedo {

/// Phase 5A production bootstrap ports. They succeed without touching DuckDB or
/// the GPU so the session state machine and render-intent route can run before
/// Phase 5B wires real PipelineMgmtService / journal / presentation.

class EditorSessionBootstrapPipelinePort final : public IEditorPipelinePort {
 public:
  auto Acquire(sl_element_id_t element_id, std::string* /*error*/)
      -> EditorPipelineGuardHandle override {
    return EditorPipelineGuardHandle{element_id, true};
  }
  void Release(const EditorPipelineGuardHandle& /*guard*/) override {}
};

class EditorSessionBootstrapHistoryPort final : public IEditorHistoryPort {
 public:
  auto Acquire(sl_element_id_t element_id, std::string* /*error*/)
      -> EditorHistoryGuardHandle override {
    return EditorHistoryGuardHandle{element_id, true};
  }
  void Release(const EditorHistoryGuardHandle& /*guard*/) override {}
  auto Undo(const EditorHistoryGuardHandle& /*guard*/, std::string* /*error*/) -> bool override {
    return true;
  }
  auto Redo(const EditorHistoryGuardHandle& /*guard*/, std::string* /*error*/) -> bool override {
    return true;
  }
  auto ReadAdjustmentSnapshot(const EditorHistoryGuardHandle& /*guard*/,
                              EditorRenderAdjustmentSnapshot* snapshot, std::string* /*error*/)
      -> bool override {
    if (snapshot) {
      *snapshot = current_snapshot_;
    }
    return true;
  }

  void SetCurrentSnapshot(EditorRenderAdjustmentSnapshot snapshot) {
    current_snapshot_ = std::move(snapshot);
  }

 private:
  EditorRenderAdjustmentSnapshot current_snapshot_{};
};

class EditorSessionBootstrapTaskPort final : public IEditorTaskPort {
 public:
  auto BeginTask(const std::string& /*name*/, sl_element_id_t /*element_id*/)
      -> std::uint64_t override {
    return ++next_id_;
  }
  void EndTask(std::uint64_t /*task_id*/, bool /*success*/,
               const std::string& /*message*/) override {}

 private:
  std::uint64_t next_id_ = 0;
};

class EditorSessionBootstrapCheckpointStore final : public IEditorCheckpointStore {};

class EditorSessionBootstrapJournalPort     final : public IEditorJournalPort {
 public:
  auto CommitJournal(sl_element_id_t /*element_id*/, std::uint64_t /*session_generation*/,
                         std::string* /*error*/) -> EditorJournalCommitOutcome override {
    return {true, true, false, 0, 0, {}};
  }
  auto DiscardUnflushed(sl_element_id_t /*element_id*/, std::string* /*error*/) -> bool override {
    return true;
  }
};

/// Accepts schedule calls and records them. Does not run pipeline work.
class EditorSessionBootstrapSchedulerPort final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request) -> std::uint64_t override {
    scheduled_.push_back(request);
    return ++next_job_id_;
  }
  void               Cancel(std::uint64_t job_id) override { cancelled_.push_back(job_id); }

  [[nodiscard]] auto scheduled() const -> const std::vector<EditorRenderRequest>& {
    return scheduled_;
  }
  [[nodiscard]] auto cancelled() const -> const std::vector<std::uint64_t>& { return cancelled_; }

 private:
  std::uint64_t                    next_job_id_ = 0;
  std::vector<EditorRenderRequest> scheduled_;
  std::vector<std::uint64_t>       cancelled_;
};

struct EditorSessionRuntime {
  std::shared_ptr<IEditorPipelinePort>          pipeline;
  std::shared_ptr<IEditorHistoryPort>           history;
  std::shared_ptr<IEditorTaskPort>              tasks;
  std::shared_ptr<IEditorJournalPort>           journal;
  std::shared_ptr<IEditorCheckpointStore>       checkpoint_store;
  std::shared_ptr<IEditorThumbnailPort>         thumbnails;
  std::shared_ptr<IEditorPipelineSchedulerPort> scheduler;
  std::shared_ptr<EditorRenderCoordinator>      coordinator;
  std::unique_ptr<EditorSessionService>         service;

  /// Create runtime with bootstrap ports (no DuckDB/GPU). Coordinator results
  /// are forwarded into the session service.
  static auto                                   Create() -> std::unique_ptr<EditorSessionRuntime> {
    return CreateWithPorts(std::make_shared<EditorSessionBootstrapPipelinePort>(),
                                                             std::make_shared<EditorSessionBootstrapHistoryPort>(),
                                                             std::make_shared<EditorSessionBootstrapTaskPort>(),
                                                             std::make_shared<EditorSessionBootstrapJournalPort>(),
                                                             std::make_shared<EditorSessionBootstrapSchedulerPort>(),
                                                             std::make_shared<EditorSessionBootstrapCheckpointStore>());
  }

  /// Phase 5B: production (or test) ports. Same wiring as Create().
  static auto CreateWithPorts(std::shared_ptr<IEditorPipelinePort>          pipeline,
                              std::shared_ptr<IEditorHistoryPort>           history,
                              std::shared_ptr<IEditorTaskPort>              tasks,
                              std::shared_ptr<IEditorJournalPort>           journal,
                              std::shared_ptr<IEditorPipelineSchedulerPort> scheduler,
                              std::shared_ptr<IEditorCheckpointStore>       checkpoint_store =
                                  std::make_shared<EditorSessionBootstrapCheckpointStore>(),
                              std::shared_ptr<IEditorThumbnailPort> thumbnails = nullptr)
      -> std::unique_ptr<EditorSessionRuntime> {
    auto runtime              = std::make_unique<EditorSessionRuntime>();
    runtime->pipeline         = std::move(pipeline);
    runtime->history          = std::move(history);
    runtime->tasks            = std::move(tasks);
    runtime->journal          = std::move(journal);
    runtime->checkpoint_store = std::move(checkpoint_store);
    runtime->thumbnails       = std::move(thumbnails);
    runtime->scheduler        = std::move(scheduler);
    runtime->coordinator      = std::make_shared<EditorRenderCoordinator>(runtime->scheduler);
    EditorSessionService::Dependencies deps;
    deps.pipeline                     = runtime->pipeline;
    deps.history                      = runtime->history;
    deps.tasks                        = runtime->tasks;
    deps.journal                      = runtime->journal;
    deps.checkpoint_store             = runtime->checkpoint_store;
    deps.thumbnails                   = runtime->thumbnails;
    deps.render                       = runtime->coordinator;
    runtime->service                  = std::make_unique<EditorSessionService>(std::move(deps));

    EditorSessionService* service_ptr = runtime->service.get();
    runtime->coordinator->SetResultObserver([service_ptr](const EditorRenderResult& result) {
      if (service_ptr) {
        service_ptr->NotifyRenderResult(result);
      }
    });
    return runtime;
  }
};

}  // namespace alcedo
