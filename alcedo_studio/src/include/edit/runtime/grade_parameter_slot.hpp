//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>

#include "edit/operators/models/i_operator_model.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"

namespace alcedo {

/**
 * @brief Bind or refresh one Grade GPU slot from typed Model fields.
 *
 * Packs @ref MakeGradeRuntimeParams only when the slot is missing or holds values from
 * another Model revision than @p model's. Writes the packed GPU layout into the existing
 * ParameterArena and records the revision there. Reads the Model only; the caller uploads with
 * @ref ParameterArena::UploadDirty.
 *
 * @return true when the slot was packed in this call.
 */
template <class Backend>
auto BindOrRefreshGradeRuntimeSlot(ParameterArena<Backend>& arena, const ParameterSlotKey& key,
                                   const IOperatorModel& model, AdjustmentBehavior behavior)
    -> bool {
  const auto revision = model.Revision();
  const bool missing  = !arena.Contains(key);
  if (!missing && arena.AppliedRevision(key) == revision) {
    return false;
  }

  const auto packed = MakeGradeRuntimeParams(model, behavior);
  if (missing) {
    const ParameterFieldBinding field{DirtyFieldMask{kGradeRuntimeParamDirtyBit}, 0, 0,
                                      kGradeRuntimeParamBytes};
    arena.BindSlot(key, kGradeRuntimeParamBytes, std::span{&field, 1});
  }
  arena.WritePackedSlot(key, packed, revision);
  return true;
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
