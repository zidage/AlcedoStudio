//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

inline constexpr std::uint32_t kMaskThumbnailSamplingVersion = 1;
inline constexpr std::uint32_t kMaskThumbnailSize            = 128;
inline constexpr std::size_t   kMaskThumbnailCacheCapacity   = 1000;

/**
 * @brief Photograph geometry that changes 128×128 Mask coverage pixels.
 *
 * Holds full-reference size and committed crop/rotation. Viewer pan, zoom, DPR,
 * decode size, NodeId, MaskId, and history revision are not part of this value.
 */
struct MaskThumbnailGeometry {
  Extent2D       full_reference{};
  NormalizedRect crop_rect{};
  float          rotation_degrees = 0.0f;
  bool           expand_to_fit    = true;

  [[nodiscard]] auto operator==(const MaskThumbnailGeometry& other) const -> bool;
};

/**
 * @brief One analytic Mask's pixel-affecting fields.
 *
 * Identity, display name, and deletion protection are omitted. A disabled Single
 * layer still belongs in the spec so the output is all black.
 */
struct MaskThumbnailLayer {
  MaskSourceKind kind    = MaskSourceKind::Radial;
  bool           enabled = true;
  bool           invert  = false;
  float          opacity = 1.0f;
  std::variant<RadialMaskParams, LinearGradientMaskParams> params{RadialMaskParams{}};

  auto operator==(const MaskThumbnailLayer& other) const -> bool;
};

enum class MaskThumbnailKind : std::uint8_t {
  Single = 0,
  Group  = 1,
};

/**
 * @brief Immutable pixel recipe for one Mask thumbnail.
 *
 * @c key is a hash of the canonical fields for LRU lookup. Cache hits still
 * compare the full spec so a hash collision cannot return the wrong image.
 * UI routing (MaskId, NodeId, row, request id) does not belong here.
 */
struct MaskThumbnailSpec {
  MaskThumbnailKind                kind              = MaskThumbnailKind::Single;
  std::uint32_t                    sampling_version  = kMaskThumbnailSamplingVersion;
  MaskThumbnailGeometry            geometry{};
  std::vector<MaskThumbnailLayer>  layers;
  std::uint64_t                    key               = 0;

  auto operator==(const MaskThumbnailSpec& other) const -> bool;
};

/**
 * @brief Canonicalize floats, compute @ref MaskThumbnailSpec::key, and sort Group layers.
 *
 * @throws std::invalid_argument when a field is non-finite or geometry is empty.
 */
[[nodiscard]] auto CanonicalizeMaskThumbnailSpec(MaskThumbnailSpec spec) -> MaskThumbnailSpec;

/// One Mask, including a disabled Mask which renders all black.
[[nodiscard]] auto MakeSingleMaskThumbnailSpec(const MaskThumbnailGeometry& geometry,
                                               const MaskModel&             mask)
    -> MaskThumbnailSpec;

/**
 * @brief Group coverage before Grade Mix.
 *
 * Enabled members are sorted so display order does not change the key. A nonempty
 * Grade whose members are all disabled keeps @c kind = Group and an empty layer
 * list, which renders all black. Callers must not build a spec for a Grade with
 * no Masks at all.
 */
[[nodiscard]] auto MakeGroupMaskThumbnailSpec(const MaskThumbnailGeometry& geometry,
                                              std::span<const MaskModel>   masks)
    -> MaskThumbnailSpec;

}  // namespace alcedo
