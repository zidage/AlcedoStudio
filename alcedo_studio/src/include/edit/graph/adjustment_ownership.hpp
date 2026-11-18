//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/operator_type_id.hpp"

namespace alcedo {

/**
 * @brief One adjustment of a Color Grade or DRT node.
 *
 * The Model is shared with frozen documents (@ref PipelineDocument::Freeze). Read it through
 * @p model; write it only through @ref MutableAdjustmentModel.
 */
struct AdjustmentModelEntry {
  AdjustmentInstanceId                  instance_id;
  std::shared_ptr<const IOperatorModel> model;
};

/**
 * @brief Write access to the Model of @p entry.
 *
 * Copies the Model first when a frozen document shares it (@ref UnshareForWrite), so the
 * frozen document keeps its values. Call only on the thread that writes the owning document.
 */
[[nodiscard]] auto MutableAdjustmentModel(AdjustmentModelEntry& entry) -> IOperatorModel&;

/// Document node that may own an adjustment instance.
enum class AdjustmentParameterOwner : std::uint8_t {
  ColorGrade,
  DrtPost,
  Unsupported,
};

/**
 * @brief Color Grade catalog types in documented order: CAT02 through LMT.
 *
 * Clarity, Sharpen, Halation, and Film Grain are not included.
 */
[[nodiscard]] auto ColorGradeAdjustmentTypes() -> std::array<OperatorTypeId, 13>;

/**
 * @brief DRT/Post catalog types in reference order: Clarity, Sharpen, Halation,
 *        Film Grain.
 */
[[nodiscard]] auto DrtPostAdjustmentTypes() -> std::array<OperatorTypeId, 4>;

/**
 * @brief Fixed Color Grade execution order: Basic Tone, Local Tone, then Color.
 *
 * Basic Tone keeps CAT02 White Balance, Exposure, Contrast, White, Black order.
 * Shadows/Highlights form the Local Tone group compiled into the LLF stage.
 * Color keeps Curve, HLS, Saturation, Vibrance, ColorWheel, LMT order.
 * The GraphCompiler orders every supported Grade this way; stored document
 * order selects parameter identity only, never execution order. Issue #900 moved
 * Local Tone before the Curve, so Shadows and Highlights act on the basic tone
 * result and the Curve shapes their output.
 */
[[nodiscard]] auto ColorGradeCompileOrder() -> std::array<OperatorTypeId, 13>;

/** @brief Stage group of a Color Grade adjustment in @ref ColorGradeCompileOrder. */
enum class ColorGradeCompileGroup : std::uint8_t {
  /// White Balance, Exposure, Contrast, Whites, Blacks: the pointwise stage before the LLF.
  BasicTone,
  /// Shadows and Highlights: the LLF stage.
  LocalTone,
  /// Curve through LMT, and any type outside the Color Grade catalog: after the LLF.
  Color,
};

/**
 * @brief Group of @p type in @ref ColorGradeCompileOrder.
 *
 * A type outside the Color Grade catalog ranks last and is in the Color group.
 */
[[nodiscard]] auto ColorGradeCompileGroupOf(const OperatorTypeId& type) -> ColorGradeCompileGroup;

/**
 * @brief 0-based rank of @p type in @ref ColorGradeCompileOrder.
 *
 * @return Position in the compile order, or the order size when @p type is not a
 *         Color Grade catalog type.
 */
[[nodiscard]] auto ColorGradeCompileRank(const OperatorTypeId& type) -> std::uint32_t;

/**
 * @brief Document indices of @p types in fixed compile order.
 *
 * Stable-sorts positions by @ref ColorGradeCompileRank, preserving document order
 * for repeated instances of one type. This is the single ordering rule shared by
 * the GraphCompiler and content-key hashing.
 */
[[nodiscard]] auto ColorGradeCompileIndexOrder(std::span<const OperatorTypeId> types)
    -> std::vector<std::size_t>;

[[nodiscard]] auto OwnerOfAdjustment(const OperatorTypeId& type) -> AdjustmentParameterOwner;

/**
 * @brief Stable instance-id suffix for a catalog type (for example `cat02_wb`).
 */
[[nodiscard]] auto AdjustmentInstanceSuffix(const OperatorTypeId& type) -> std::string;

/**
 * @brief `{node_id}.{suffix}` instance id used by Default and Clean factories.
 */
[[nodiscard]] auto MakeAdjustmentInstanceId(const NodeId& node_id, const OperatorTypeId& type)
    -> AdjustmentInstanceId;

/**
 * @brief Reject when @p type is not owned by @p expected.
 *
 * @param type Adjustment catalog type.
 * @param expected Required owner.
 * @param context Prefix for the exception, such as `ColorGrade InsertAdjustment`.
 * @throws std::runtime_error when the owner does not match, including unsupported types.
 */
void RequireAdjustmentOwner(const OperatorTypeId& type, AdjustmentParameterOwner expected,
                            std::string_view context);

/**
 * @brief Require the four DRT/Post types, each exactly once.
 *
 * @param present Types currently stored on the DRT node, in model order.
 * @param context Prefix for the exception.
 * @throws std::runtime_error when a type is missing, duplicated, extra, or wrong-owner.
 */
void RequireCompleteDrtPostTypes(std::span<const OperatorTypeId> present, std::string_view context);

}  // namespace alcedo
