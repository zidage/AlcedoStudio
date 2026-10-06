//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
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
 * @brief Device-uploadable form of a 3D LUT table plus its content identity.
 *
 * Either a parsed .cube file or an LMT composite table (lut_bake.hpp). Produced once per
 * content version by @ref TryPackGradeLut and shared immutably across callers and backends.
 * @ref key is the @ref ContentKey the backend LUT caches index on; it hashes @ref rgba and
 * @ref edge and is computed at pack time so unchanged content never pays the byte-wise hash
 * again. The domain is not in the key: the GPU passes sample only unit-domain tables, and a
 * table with another domain is always baked into a composite table first.
 */
struct PackedGradeLut {
  std::vector<std::byte> rgba;
  std::uint32_t          edge = 0;
  ContentKey             key{};
  /// DOMAIN_MIN / DOMAIN_MAX of the .cube; [0, 1] for composite tables.
  std::array<float, 3>   domain_min{0.0f, 0.0f, 0.0f};
  std::array<float, 3>   domain_max{1.0f, 1.0f, 1.0f};
};

/**
 * @brief Pack a parsed 3D cube as tightly packed RGBA32F voxels, X varying fastest.
 */
[[nodiscard]] auto PackCubeLutRgba(const CubeLut& lut) -> std::vector<std::byte>;

/**
 * @brief Load the table the ColorGrade LMT samples: the cube that @p resources resolves for its
 * reference, composed with the LMT's input and output encodings.
 *
 * The reference is resolved and parsed inside LutResourceResolver::ReadResource, so a
 * library owner cannot remove the file while it is read; GPU work starts after the call.
 * The packed result is memoized per normalized path, file stamp (size + last write time),
 * and the verified inventory digest when the library has one: repeated calls for
 * unchanged content return the same immutable instance instead of re-reading, re-parsing,
 * and re-hashing the .cube text on every pipeline execute. Changed content is parsed again
 * and replaces the entry; the old instance stays alive until its in-flight callers release it.
 * ACEScc to ACEScc with a unit domain returns that parsed table; any other encoding pair or
 * domain returns the 65^3 composite table of ResolveLmtSampledTable.
 *
 * @return nullptr when the node has no LMT model, references no LUT, its strength is 0
 *         (the operation samples nothing), or the referenced file is missing. A missing
 *         file skips only this LUT operation; the reference and strength are unchanged.
 * @throws std::runtime_error when a resolved file cannot be parsed as a 3D cube, or when it
 *         has a 1D shaper table ("1D shaper LUTs are not supported"). Invalid, unreadable or
 *         unsupported content is an error, never an identity substitute.
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
