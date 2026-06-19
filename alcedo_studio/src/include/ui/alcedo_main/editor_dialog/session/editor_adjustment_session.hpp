//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>
#include <functional>
#include <json.hpp>
#include <memory>
#include <optional>
#include <utility>

#include "app/pipeline_service.hpp"
#include "edit/history/version.hpp"
#include "ui/alcedo_main/editor_dialog/session/adjustment_patch.hpp"
#include "ui/alcedo_main/editor_dialog/state.hpp"

namespace alcedo::ui {

class EditorAdjustmentSession {
 public:
  struct Dependencies {
    std::shared_ptr<PipelineGuard> pipeline_guard;
    WorkingVersion*                working_version = nullptr;
    AdjustmentState*               state           = nullptr;
    AdjustmentState*               committed_state = nullptr;
  };

  struct Callbacks {
    std::function<void()> schedule_quality_preview;
    std::function<void()> schedule_detail_preview_from_viewport;
    std::function<void()> advance_preview_generation;
    std::function<void()> update_version_ui;
    std::function<void()> mark_full_frame_preview_after_geometry_commit;
  };

  enum class CommitStatus {
    UnchangedOrUnavailable,
    Applied,
    Failed,
  };

  struct CommitResult {
    CommitStatus status = CommitStatus::UnchangedOrUnavailable;
    QString      error;
  };

  EditorAdjustmentSession(Dependencies dependencies, Callbacks callbacks);

  void Preview(const AdjustmentPreview& preview);
  auto Commit(AdjustmentField field) -> CommitResult;
  auto Commit(const AdjustmentCommit& commit) -> CommitResult;

  auto LoadFromPipeline() -> bool;
  auto ReloadFromImportedPipelineParams() -> bool;

  auto ReadCurrentOperatorParams(PipelineStageName stage_name, OperatorType op_type) const
      -> std::optional<nlohmann::json>;
  auto FieldSpec(AdjustmentField field) const -> std::pair<PipelineStageName, OperatorType>;
  auto ParamsForField(AdjustmentField field, const AdjustmentState& state) const -> nlohmann::json;
  auto FieldChanged(AdjustmentField field) const -> bool;

  // Accessor used by panels that need to build raw decode params preserving the
  // executor's resolved accelerator backend (e.g. CUDA) instead of the generic
  // "gpu" default, which would otherwise re-resolve to a different backend.
  auto Pipeline() const -> CPUPipelineExecutor*;

 private:
  auto                             HasPipeline() const -> bool;
  void                             ScheduleQualityPreview() const;

  Dependencies                     dependencies_;
  Callbacks                        callbacks_;
  std::optional<AdjustmentPreview> last_preview_;
};

}  // namespace alcedo::ui
