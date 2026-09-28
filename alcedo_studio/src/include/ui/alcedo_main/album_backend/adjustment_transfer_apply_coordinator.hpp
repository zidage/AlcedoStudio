//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <unordered_map>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/pipeline_service.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui {

class LibraryModule;
class ProjectModule;

/**
 * @brief Owns multi-target Paste apply work for Adjustment Transfer.
 *
 * Applies one copied package as a root-relative Version per target through the
 * existing history owner, persists each successful target, then refreshes HDR
 * metadata and thumbnails for applied targets only. One target's failure never
 * hides another target's result; every failure is reported.
 *
 * Targets must not be open in the editor: the editor session owns that image's
 * history and pastes through its own queue. The controller routes such a target
 * there before calling this coordinator.
 */
class AdjustmentTransferApplyCoordinator final : public QObject {
  Q_OBJECT

 public:
  AdjustmentTransferApplyCoordinator(ProjectModule* project, LibraryModule* library,
                                     BackgroundTaskController* background_tasks,
                                     QObject*                  parent = nullptr);
  ~AdjustmentTransferApplyCoordinator() override = default;

  /**
   * @brief Paste @p package as one new root-relative Version on each target and
   *        wait for the result on the calling thread.
   *
   * The result map reports `appliedCount`, `unchangedCount`, `failureCount`,
   * and a `failures` list of `{elementId, message}` rows. A target that fails
   * package validation, rebuild, or persistence is restored to its prior graph
   * and receives no metadata or thumbnail refresh.
   */
  [[nodiscard]] auto ApplyToTargets(const alcedo::AdjustmentTransferPackage& package,
                                    const std::vector<sl_element_id_t>&      target_ids,
                                    const QString& version_display_name) -> QVariantMap;

  /**
   * @brief Start the same Paste on a worker thread and return immediately.
   *
   * Registers one AdjustmentPaste background task that locks Paste, project
   * close, image deletion, editor image selection, and workspace switching
   * until the worker finishes, so no other owner loads a target pipeline while
   * this Paste writes it. The result map of @ref ApplyToTargets arrives through
   * @ref ApplyFinished on the owner thread after the thumbnail refresh.
   *
   * @return false with @p error when another Paste is running or the pipeline
   *         service is unavailable; nothing starts in that case.
   */
  auto StartApplyToTargets(const alcedo::AdjustmentTransferPackage& package,
                           std::vector<sl_element_id_t> target_ids,
                           const QString& version_display_name, QString* error) -> bool;

  [[nodiscard]] bool running() const { return running_; }

 signals:
  /// Emitted once per target whose persisted paste completed its HDR and
  /// thumbnail refresh. Tests assert only successful targets appear.
  void TargetRefreshed(uint elementId);
  /// Result of a Paste started by @ref StartApplyToTargets.
  void ApplyFinished(const QVariantMap& result);
  void RunningChanged();

 private:
  /// Result of the worker-side loop: the apply result plus the HDR display
  /// encoding of every applied target, read from its pasted document.
  struct TargetApplyOutcome {
    alcedo::AdjustmentApplyResult             result_;
    std::unordered_map<sl_element_id_t, bool> hdr_by_target_;
  };

  /// Per-target Paste, rebuild, and persistence. Uses only the pipeline
  /// service, so it runs on a worker thread.
  [[nodiscard]] static auto ApplyPackageToTargets(
      alcedo::PipelineMgmtService& pipeline_service,
      const alcedo::AdjustmentTransferPackage& package,
      const std::vector<sl_element_id_t>& target_ids, const std::string& version_display_name)
      -> TargetApplyOutcome;

  /// Owner-thread completion: HDR flag, thumbnail refresh, and the result map.
  auto           FinishApply(const TargetApplyOutcome& outcome) -> QVariantMap;
  void           RefreshOneTarget(sl_element_id_t element_id, bool is_hdr,
                                  bool* hdr_metadata_dirty);

  ProjectModule*            project_          = nullptr;
  LibraryModule*            library_          = nullptr;
  BackgroundTaskController* background_tasks_ = nullptr;
  bool                      running_          = false;
  QString                   task_id_;
};

}  // namespace alcedo::ui
