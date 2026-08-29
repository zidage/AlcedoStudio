//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <utility>

#include "edit/operators/models/i_operator_model.hpp"

namespace alcedo {

/**
 * @brief RAII guard for a taken dirty patch. Destructor restores dirty bits
 * unless @ref Commit was called after a successful transfer.
 *
 * Does not own the Model. The Model must outlive this object.
 */
class PendingParameterPatch {
 public:
  PendingParameterPatch(IOperatorModel& model, OperatorParamPatchDto patch)
      : model_(&model), patch_(std::move(patch)) {}

  PendingParameterPatch(const PendingParameterPatch&)            = delete;
  auto operator=(const PendingParameterPatch&) -> PendingParameterPatch& = delete;

  PendingParameterPatch(PendingParameterPatch&& other) noexcept
      : model_(other.model_), patch_(std::move(other.patch_)), committed_(other.committed_) {
    other.model_     = nullptr;
    other.committed_ = true;
  }

  auto operator=(PendingParameterPatch&& other) noexcept -> PendingParameterPatch& {
    if (this == &other) {
      return *this;
    }
    AbortIfNeeded();
    model_           = other.model_;
    patch_           = std::move(other.patch_);
    committed_       = other.committed_;
    other.model_     = nullptr;
    other.committed_ = true;
    return *this;
  }

  ~PendingParameterPatch() { AbortIfNeeded(); }

  /// Marks the transfer successful so dirty bits stay clear.
  void Commit() { committed_ = true; }

  [[nodiscard]] auto Patch() const -> const OperatorParamPatchDto& { return patch_; }

 private:
  void AbortIfNeeded() {
    if (model_ != nullptr && !committed_) {
      model_->RestoreDirty(patch_.dirty_fields);
    }
  }

  IOperatorModel*        model_     = nullptr;
  OperatorParamPatchDto  patch_{};
  bool                   committed_ = false;
};

/**
 * @brief Take a dirty patch and wrap it for commit-or-restore.
 * @return nullopt when the Model has no dirty fields.
 */
[[nodiscard]] inline auto TakePendingParameterPatch(IOperatorModel& model)
    -> std::optional<PendingParameterPatch> {
  auto patch = model.TakeDirtyPatch();
  if (!patch.has_value()) {
    return std::nullopt;
  }
  return PendingParameterPatch{model, std::move(*patch)};
}

}  // namespace alcedo
