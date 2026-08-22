//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <mutex>
#include <utility>

#include "edit/operators/models/i_operator_model.hpp"

namespace alcedo {

/**
 * @brief CRTP helper that owns payload, dirty mask, and the take/restore lock.
 *
 * Derived must provide `static auto TypeId() -> const OperatorTypeId&` and a
 * Dirty enum with `All`. New instances start with All dirty so the first patch
 * can upload every field.
 *
 * @tparam Derived CRTP type.
 * @tparam Payload Copyable parameter struct stored in DTOs.
 * @tparam DirtyEnum Bit flags convertible to DirtyFieldMask.
 */
template <class Derived, class Payload, class DirtyEnum>
class OperatorModelBase : public IOperatorModel {
 public:
  [[nodiscard]] auto Type() const -> OperatorTypeId override { return Derived::TypeId(); }

  [[nodiscard]] auto IsDirty() const -> bool override {
    std::lock_guard lock(mutex_);
    return dirty_.Any();
  }

  [[nodiscard]] auto MakeFullDto() const -> OperatorParamDto override {
    std::lock_guard lock(mutex_);
    return MakeDtoLocked();
  }

  auto TakeDirtyPatch() -> std::optional<OperatorParamPatchDto> override {
    std::lock_guard lock(mutex_);
    if (!dirty_.Any()) {
      return std::nullopt;
    }
    OperatorParamPatchDto patch;
    patch.type         = Derived::TypeId();
    patch.dirty_fields = dirty_;
    patch.payload      = std::make_shared<TypedOperatorParamPayload<Payload>>(
        Derived::TypeId(), kDataVersion, payload_);
    dirty_ = DirtyFieldMask{};
    return patch;
  }

  void RestoreDirty(DirtyFieldMask fields) override {
    std::lock_guard lock(mutex_);
    dirty_ |= fields;
  }

  void MarkAllDirty() override {
    std::lock_guard lock(mutex_);
    dirty_ = DirtyFieldMask{DirtyEnum::All};
  }

 protected:
  static constexpr std::uint32_t kDataVersion = 1;

  template <class Fn>
  void Mutate(DirtyEnum bit, Fn&& fn) {
    std::lock_guard lock(mutex_);
    fn(payload_);
    dirty_ |= DirtyFieldMask{bit};
  }

  template <class Fn>
  auto Read(Fn&& fn) const {
    std::lock_guard lock(mutex_);
    return fn(payload_);
  }

  [[nodiscard]] auto PayloadCopy() const -> Payload {
    std::lock_guard lock(mutex_);
    return payload_;
  }

  Payload                  payload_{};
  DirtyFieldMask           dirty_{DirtyEnum::All};
  mutable std::mutex       mutex_;

 private:
  [[nodiscard]] auto MakeDtoLocked() const -> OperatorParamDto {
    OperatorParamDto dto;
    dto.type          = Derived::TypeId();
    dto.data_version  = kDataVersion;
    dto.payload       = std::make_shared<TypedOperatorParamPayload<Payload>>(
        Derived::TypeId(), kDataVersion, payload_);
    return dto;
  }
};

}  // namespace alcedo
