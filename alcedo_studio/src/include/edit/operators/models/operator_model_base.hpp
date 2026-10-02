//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "edit/operators/models/i_operator_model.hpp"

namespace alcedo {

/**
 * @brief CRTP helper that owns payload, per-field revision stamps, and the Model lock.
 *
 * Derived must provide `static auto TypeId() -> const OperatorTypeId&` and a field enum with
 * `All`. Each bit of `All` is one field with its own stamp. A new instance stamps every field
 * once, so the first renderer that reads it packs every field.
 *
 * @tparam Derived CRTP type.
 * @tparam Payload Copyable parameter struct stored in DTOs.
 * @tparam DirtyEnum Field bit flags convertible to DirtyFieldMask.
 */
template <class Derived, class Payload, class DirtyEnum>
class OperatorModelBase : public IOperatorModel {
 public:
  OperatorModelBase() {
    const auto revision = NextParameterRevision();
    field_revisions_.fill(revision);
    revision_ = revision;
  }

  OperatorModelBase& operator=(const OperatorModelBase&) = delete;

  [[nodiscard]] auto Type() const -> OperatorTypeId override { return Derived::TypeId(); }

  [[nodiscard]] auto Clone() const -> std::unique_ptr<IOperatorModel> override {
    return std::make_unique<Derived>(static_cast<const Derived&>(*this));
  }

  [[nodiscard]] auto Revision() const -> ParameterRevision override {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
  }

  [[nodiscard]] auto FieldsRevision(DirtyFieldMask fields) const -> ParameterRevision override {
    std::lock_guard<std::mutex> lock(mutex_);
    ParameterRevision           latest = kNoParameterRevision;
    for (std::size_t index = 0; index < kFieldCount; ++index) {
      if ((fields.Bits() & (std::uint64_t{1} << index)) != 0) {
        latest = std::max(latest, field_revisions_[index]);
      }
    }
    return latest;
  }

  void CopyRevisionsFrom(const IOperatorModel& source) override {
    const auto* typed = dynamic_cast<const Derived*>(&source);
    if (typed == nullptr) {
      throw std::invalid_argument("OperatorModelBase: revision source is another Model type");
    }
    if (typed == this) {
      return;
    }
    std::scoped_lock lock(mutex_, typed->mutex_);
    field_revisions_ = typed->field_revisions_;
    revision_        = typed->revision_;
  }

  [[nodiscard]] auto MakeFullDto() const -> OperatorParamDto override {
    OperatorModelFullDtoCopyCount::Note();
    std::lock_guard<std::mutex> lock(mutex_);
    return MakeDtoLocked();
  }

  /**
   * @brief Read live owner fields under the Model lock.
   *
   * The callback receives the payload stored by this Model. It must not retain
   * references or pointers to that payload after returning. This does not allocate
   * a DTO or copy the whole payload unless the callback does.
   */
  template <class Fn>
  auto Read(Fn&& fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fn(payload_);
  }

 protected:
  static constexpr std::uint32_t kDataVersion = 1;

  /// Copy payload and field stamps of @p other under its lock. Used only by @ref Clone.
  OperatorModelBase(const OperatorModelBase& other) : IOperatorModel(other) {
    std::lock_guard<std::mutex> lock(other.mutex_);
    payload_         = other.payload_;
    field_revisions_ = other.field_revisions_;
    revision_        = other.revision_;
  }

  template <class Fn>
  void Mutate(DirtyEnum bit, Fn&& fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    fn(payload_);
    StampLocked(DirtyFieldMask{bit});
  }

  /**
   * @brief Apply one focused update and stamp the fields it reports as changed, under one lock.
   *
   * The callback must update only the supplied owner fields and return the bits of the fields
   * that changed. Returning an empty mask makes an equivalent normalized update a no-op, so the
   * Model revision stays the same.
   */
  template <class Fn>
  void MutateWithDirtyFields(Fn&& fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    StampLocked(fn(payload_));
  }

  /**
   * @brief Take the payload and every field stamp of @p source, then apply one focused update,
   *        under both Model locks.
   *
   * After the copy, this Model reports the same value and stamp as @p source for every field.
   * @p fn then receives the copied payload and this Model's payload from before the copy. It must
   * write back only the fields it keeps from the previous payload and return the bits of the
   * fields whose value now differs from @p source; those fields get one new stamp. Returning an
   * empty mask keeps every stamp of @p source, which is valid because every value then equals
   * @p source.
   *
   * @throws std::invalid_argument when @p source is this Model; this Model is unchanged.
   */
  template <class Fn>
  void TakeFieldsFromThenMutate(const Derived& source, Fn&& fn) {
    const auto& typed_source = static_cast<const OperatorModelBase&>(source);
    if (&typed_source == this) {
      throw std::invalid_argument("OperatorModelBase: a Model cannot take fields from itself");
    }
    std::scoped_lock lock(mutex_, typed_source.mutex_);
    Payload          previous = std::move(payload_);
    payload_                  = typed_source.payload_;
    field_revisions_          = typed_source.field_revisions_;
    revision_                 = typed_source.revision_;
    StampLocked(fn(payload_, std::as_const(previous)));
  }

  [[nodiscard]] auto PayloadCopy() const -> Payload {
    std::lock_guard<std::mutex> lock(mutex_);
    return payload_;
  }

  Payload            payload_{};
  mutable std::mutex mutex_;

 private:
  static constexpr std::size_t kFieldCount =
      static_cast<std::size_t>(std::bit_width(static_cast<std::uint64_t>(DirtyEnum::All)));
  static_assert(kFieldCount > 0 && kFieldCount <= 64, "Model field enum must have 1 to 64 bits");

  void StampLocked(DirtyFieldMask changed) {
    if (!changed.Any()) {
      return;
    }
    const auto revision = NextParameterRevision();
    for (std::size_t index = 0; index < kFieldCount; ++index) {
      if ((changed.Bits() & (std::uint64_t{1} << index)) != 0) {
        field_revisions_[index] = revision;
      }
    }
    revision_ = revision;
  }

  [[nodiscard]] auto MakeDtoLocked() const -> OperatorParamDto {
    OperatorParamDto dto;
    dto.type         = Derived::TypeId();
    dto.data_version = kDataVersion;
    dto.payload      = std::make_shared<TypedOperatorParamPayload<Payload>>(Derived::TypeId(),
                                                                            kDataVersion, payload_);
    return dto;
  }

  std::array<ParameterRevision, kFieldCount> field_revisions_{};
  ParameterRevision                          revision_ = kNoParameterRevision;
};

}  // namespace alcedo
