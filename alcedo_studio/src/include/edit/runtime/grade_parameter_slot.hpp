//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>

#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"

namespace alcedo {

/**
 * @brief Bind or refresh one Grade GPU slot from typed Model fields.
 *
 * Packs @ref MakeGradeRuntimeParams only when the slot is missing or the Model is
 * dirty. Writes the packed GPU layout into the existing ParameterArena. The returned
 * guard restores dirty bits unless @ref PendingParameterPatch::Commit is called after
 * a successful @ref ParameterArena::UploadDirty.
 *
 * @return nullopt when the slot already holds the current packed parameters.
 */
template <class Backend>
[[nodiscard]] auto BindOrRefreshGradeRuntimeSlot(ParameterArena<Backend>& arena,
                                                 const ParameterSlotKey& key, IOperatorModel& model,
                                                 AdjustmentBehavior behavior)
    -> std::optional<PendingParameterPatch> {
  const bool missing = !arena.Contains(key);
  if (!missing && !model.IsDirty()) {
    return std::nullopt;
  }

  const auto packed = MakeGradeRuntimeParams(model, behavior);
  if (missing) {
    const ParameterFieldBinding field{DirtyFieldMask{kGradeRuntimeParamDirtyBit}, 0, 0,
                                      kGradeRuntimeParamBytes};
    arena.BindSlot(key, kGradeRuntimeParamBytes, std::span{&field, 1});
  }
  arena.WritePackedSlot(key, packed);
  return TakePendingDirtyFields(model);
}

/**
 * @brief Read values[0] from a packed Grade slot in the arena host mirror.
 *
 * Used for LLF slider gating and Metal neighborhood enable checks after the slot
 * has been bound or refreshed. Does not read the Model or allocate a DTO.
 */
template <class Backend>
[[nodiscard]] auto PackedGradeControlValue(const ParameterArena<Backend>& arena,
                                           const ParameterSlotKey&        key) -> float {
  const auto& binding = arena.Binding(key);
  if (binding.size < kGradeRuntimeParamBytes) {
    throw std::runtime_error("ParameterArena: Grade slot is smaller than the GPU layout");
  }
  float value = 0.0f;
  std::memcpy(&value,
              arena.HostSpan().data() + binding.offset + offsetof(GradeAdjustmentParams, values),
              sizeof(value));
  return value;
}

}  // namespace alcedo
