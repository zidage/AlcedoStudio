//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "type/type.hpp"

namespace alcedo {
class PipelineMgmtService;
}

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
 */
class AdjustmentTransferApplyCoordinator final : public QObject {
  Q_OBJECT

 public:
  AdjustmentTransferApplyCoordinator(ProjectModule* project, LibraryModule* library,
                                     QObject* parent = nullptr);
  ~AdjustmentTransferApplyCoordinator() override = default;

  /**
   * @brief Paste @p package as one new root-relative Version on each target.
   *
   * The result map reports `appliedCount`, `unchangedCount`, `failureCount`,
   * and a `failures` list of `{elementId, message}` rows. A target that fails
   * package validation, rebuild, or persistence is restored to its prior graph
   * and receives no metadata or thumbnail refresh.
   */
  [[nodiscard]] auto ApplyToTargets(const alcedo::AdjustmentTransferPackage& package,
                                    const std::vector<sl_element_id_t>&      target_ids,
                                    const QString& version_display_name) -> QVariantMap;

 signals:
  /// Emitted once per target whose persisted paste completed its HDR and
  /// thumbnail refresh. Tests assert only successful targets appear.
  void TargetRefreshed(uint elementId);

 private:
  /// Per-target refresh: HDR flag persistence, thumbnail invalidation, and the
  /// shared thumbnail state update. Only runs for targets in
  /// `AdjustmentApplyResult::applied_ids_`.
  void           RefreshAppliedTargets(const alcedo::AdjustmentApplyResult& result);
  void           RefreshOneTarget(sl_element_id_t element_id, bool* hdr_metadata_dirty);

  ProjectModule* project_ = nullptr;
  LibraryModule* library_ = nullptr;
};

}  // namespace alcedo::ui
