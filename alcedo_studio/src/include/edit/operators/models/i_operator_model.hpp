//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>

#include "edit/operators/models/dirty_field_mask.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "edit/operators/models/parameter_revision.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Counts @ref IOperatorModel::MakeFullDto copies on this thread.
 *
 * Grade GPU packing, CameraColor/DRT packed-slot writes, and panel projection
 * must leave this at zero. Persistence, history, and device-recovery paths that
 * still need a full payload snapshot increment it. Tests reset the counter around
 * the call they are proving.
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
 * Every setter that changes a field stamps that field with @ref NextParameterRevision.
 * Readers never clear anything: a renderer remembers the stamps it last applied in its own
 * workspace and compares them with @ref Revision or @ref FieldsRevision using `!=`. So any
 * number of renderers can read one Model, and a render never writes the document.
 *
 * Thread-safe: setters, reads, and JSON load serialize on an internal mutex in
 * OperatorModelBase.
 */
class IOperatorModel {
 public:
  virtual ~IOperatorModel() = default;

  [[nodiscard]] virtual auto Type() const -> OperatorTypeId = 0;
  [[nodiscard]] virtual auto IsDefault() const -> bool             = 0;

  /**
   * @brief Stamp of the last change to any field. Never @ref kNoParameterRevision.
   *
   * A new Model stamps all fields once, so the first reader always sees a change.
   */
  [[nodiscard]] virtual auto Revision() const -> ParameterRevision = 0;

  /**
   * @brief Stamp of the last change to any field in @p fields.
   *
   * Use this when different fields invalidate different results (for example Develop
   * sensor fields and white balance). Bits outside the Model's field set are ignored.
   * @return @ref kNoParameterRevision when @p fields selects no field of this Model.
   */
  [[nodiscard]] virtual auto FieldsRevision(DirtyFieldMask fields) const -> ParameterRevision = 0;

  /**
   * @brief Copy the field stamps of @p source, a Model with equal field values.
   *
   * @pre @p source has the same type and equal field values, as after a JSON round trip of
   *      @p source into this Model. Only document cloning calls this.
   * @throws std::invalid_argument when @p source is another Model type.
   */
  virtual void               CopyRevisionsFrom(const IOperatorModel& source)                  = 0;

  /**
   * @brief Independent Model with the same field values and the same field stamps.
   *
   * Copy-on-write documents call this before they write a Model that a frozen document
   * shares. Equal stamps are correct because the values are equal, so a renderer that
   * applied this Model does not repack the copy.
   */
  [[nodiscard]] virtual auto Clone() const -> std::unique_ptr<IOperatorModel>              = 0;

  /// Full payload snapshot.
  [[nodiscard]] virtual auto MakeFullDto() const -> OperatorParamDto                          = 0;

  [[nodiscard]] virtual auto ToJson() const -> nlohmann::json     = 0;
  virtual void               LoadJson(const nlohmann::json& json) = 0;
};

}  // namespace alcedo
