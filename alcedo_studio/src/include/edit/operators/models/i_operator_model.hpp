//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>

#include "edit/operators/models/dirty_field_mask.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Counts @ref IOperatorModel::MakeFullDto copies on this thread.
 *
 * Grade GPU packing and panel projection must leave this at zero. Persistence,
 * history, and device-recovery paths that still need a full payload snapshot
 * increment it. Tests reset the counter around the call they are proving.
 */
struct OperatorModelFullDtoCopyCount {
  static void Reset() noexcept { count_ = 0; }
  [[nodiscard]] static auto Peek() noexcept -> int { return count_; }
  static void               Note() noexcept { ++count_; }

 private:
  static inline thread_local int count_ = 0;
};

/**
 * @brief Pure parameter Model. Does not receive images, allocate GPU memory, or
 * apply pixels.
 *
 * Setters mark dirty field bits. @ref TakeDirtyPatch copies the current payload
 * and clears taken bits under a short lock. Failed uploads call @ref RestoreDirty.
 * GPU Grade packing uses @ref TakeDirtyFields so it can commit dirty bits without
 * copying the Model payload.
 *
 * Thread-safe: setters, take, restore, and JSON load serialize on an internal mutex
 * in OperatorModelBase.
 */
class IOperatorModel {
 public:
  virtual ~IOperatorModel() = default;

  [[nodiscard]] virtual auto Type() const -> OperatorTypeId = 0;
  [[nodiscard]] virtual auto IsDefault() const -> bool      = 0;
  [[nodiscard]] virtual auto IsDirty() const -> bool        = 0;

  /// Full payload snapshot. Ignores dirty bits.
  [[nodiscard]] virtual auto MakeFullDto() const -> OperatorParamDto = 0;

  /**
   * @brief Copy current payload and clear taken dirty bits.
   * @return nullopt when no field is dirty.
   */
  virtual auto TakeDirtyPatch() -> std::optional<OperatorParamPatchDto> = 0;

  /**
   * @brief Clear taken dirty bits without copying the Model payload.
   * @return nullopt when no field is dirty.
   *
   * The default implementation takes a full patch and discards the payload. Owner
   * Models override this to copy only the mask.
   */
  virtual auto TakeDirtyFields() -> std::optional<DirtyFieldMask> {
    auto patch = TakeDirtyPatch();
    if (!patch.has_value()) {
      return std::nullopt;
    }
    return patch->dirty_fields;
  }

  /// Re-set dirty bits after a cancelled or failed parameter transfer.
  virtual void RestoreDirty(DirtyFieldMask fields) = 0;
  virtual void MarkAllDirty()                      = 0;

  [[nodiscard]] virtual auto ToJson() const -> nlohmann::json     = 0;
  virtual void               LoadJson(const nlohmann::json& json) = 0;
};

}  // namespace alcedo
