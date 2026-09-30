//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"
#include "utils/lut/cube_lut.hpp"

namespace alcedo {

/**
 * @brief Device-uploadable form of a parsed .cube file plus its content identity.
 *
 * Produced once per cube file version by @ref TryPackGradeLut and shared
 * immutably across callers and backends. @ref key is the @ref ContentKey the
 * backend LUT caches index on; it is computed at pack time so an unchanged
 * file never pays the byte-wise hash again.
 */
struct PackedGradeLut {
  std::vector<std::byte> rgba;
  std::uint32_t          edge = 0;
  ContentKey             key{};
};

/**
 * @brief Pack a parsed 3D cube as tightly packed RGBA32F voxels, X varying fastest.
 */
[[nodiscard]] auto PackCubeLutRgba(const CubeLut& lut) -> std::vector<std::byte>;

/**
 * @brief Load the ColorGrade LMT cube that @p resources resolves for its reference.
 *
 * The reference is resolved and parsed inside LutResourceResolver::ReadResource, so a
 * library owner cannot remove the file while it is read; GPU work starts after the call.
 * The packed result is memoized per normalized path, file stamp (size + last write time),
 * and the verified inventory digest when the library has one: repeated calls for
 * unchanged content return the same immutable instance instead of re-reading, re-parsing,
 * and re-hashing the .cube text on every pipeline execute. Changed content is parsed again
 * and replaces the entry; the old instance stays alive until its in-flight callers release it.
 *
 * @return nullptr when the node has no LMT model, references no LUT, its strength is 0
 *         (the operation samples nothing), or the referenced file is missing. A missing
 *         file skips only this LUT operation; the reference and strength are unchanged.
 * @throws std::runtime_error when a resolved file cannot be parsed as a 3D cube. Invalid or
 *         unreadable content is an error, never an identity substitute.
 */
[[nodiscard]] auto TryPackGradeLut(const ColorGradeNodeModel& grade,
                                   const LutResourceResolver& resources)
    -> std::shared_ptr<const PackedGradeLut>;

/**
 * @brief Content identity of the LUT that @p grade's LMT currently resolves to.
 *
 * 0 when the node has no LMT model or references no LUT. Changes when the resolved file,
 * its stamp or digest, or its availability changes, even though the Model is unchanged.
 */
[[nodiscard]] auto GradeLutResourceIdentity(const ColorGradeNodeModel& grade,
                                            const LutResourceResolver& resources) -> std::uint64_t;

}  // namespace alcedo
