//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/mask_id.hpp"
#include "json.hpp"

#ifdef ALCEDO_ENABLE_BRUSH_MASK
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_asset.hpp"
#endif

namespace alcedo {

/** @brief Discriminator for @ref MaskSource. */
enum class MaskSourceKind : std::uint8_t {
#ifdef ALCEDO_ENABLE_BRUSH_MASK
  Brush           = 0,
#endif
  Radial          = 1,
  LinearGradient  = 2,
};

#ifdef ALCEDO_ENABLE_BRUSH_MASK
/**
 * @brief Brush coverage source owned by a Color Grade Mask.
 *
 * Canonical persistent data is ordered immutable strokes, algorithm versions, and
 * @p placement_translation. Samples live in local reference pixels; translation does
 * not rewrite them. JSON loaders reject raster-only `asset_key` encoding.
 * @p asset_key remains only for in-memory native evaluation of already-constructed
 * Brush values. A missing asset key with empty strokes yields zero coverage.
 */
struct BrushMaskSource {
  std::uint32_t               source_format_version    = kBrushSourceFormatVersion;
  std::uint32_t               raster_algorithm_version = kBrushRasterAlgorithmVersion;
  Vector2                     placement_translation{};
  std::vector<BrushStroke>    strokes;
  float                       feather_radius = 0.0f;
  std::optional<MaskAssetKey> asset_key;
  MaskAssetDescriptor         descriptor{};

  friend auto operator==(const BrushMaskSource&, const BrushMaskSource&) -> bool = default;
};

/**
 * @brief True when @p brush stores stroke bodies or a non-zero placement.
 *
 * Persistent JSON always uses the parameterized encoding. This predicate is a
 * load-only query of in-memory contents.
 */
[[nodiscard]] auto BrushSourceHasParameterizedPayload(const BrushMaskSource& brush) -> bool;
#endif

/** @brief Radial ellipse in normalized reference space. */
struct RadialMaskSource {
  float center_x      = 0.5f;
  float center_y      = 0.5f;
  float major_radius  = 0.5f;
  float minor_radius  = 0.5f;
  float rotation      = 0.0f;
  float inner_feather = 0.0f;
  float outer_feather = 0.0f;

  friend auto operator==(const RadialMaskSource&, const RadialMaskSource&) -> bool = default;
};

/** @brief Linear Gradient coverage in normalized reference space. */
struct LinearGradientMaskSource {
  float origin_x            = 0.5f;
  float origin_y            = 0.5f;
  float normal_x            = 0.0f;
  float normal_y            = 1.0f;
  float transition_distance = 0.2f;
  float start_value         = 1.0f;
  float end_value           = 0.0f;

  friend auto operator==(const LinearGradientMaskSource&, const LinearGradientMaskSource&)
      -> bool = default;
};

using MaskSource = std::variant<
#ifdef ALCEDO_ENABLE_BRUSH_MASK
    BrushMaskSource,
#endif
    RadialMaskSource, LinearGradientMaskSource>;

/**
 * @brief Direct Color Range field. Only the disabled placeholder is supported.
 *
 * @p enabled must be false. Extra serialized fields are rejected.
 */
struct ColorRangeModel {
  bool enabled = false;

  friend auto operator==(const ColorRangeModel&, const ColorRangeModel&) -> bool = default;
};

/**
 * @brief Direct Luminance Range field. Only the disabled placeholder is supported.
 *
 * @p enabled must be false. Extra serialized fields are rejected.
 */
struct LuminanceRangeModel {
  bool enabled = false;

  friend auto operator==(const LuminanceRangeModel&, const LuminanceRangeModel&) -> bool = default;
};

/**
 * @brief One Color Grade Mask: identity, display metadata, source, and range fields.
 *
 * GPU-free. Invert and opacity belong to the Mask, not to the source variant.
 */
struct MaskModel {
  MaskId                             id;
  std::string                        display_name;
  bool                               enabled = true;
  /// Persistent deletion-only protection; does not affect coverage or parameter editing.
  bool                               deletion_protected = false;
  float                              opacity = 1.0f;
  bool                               invert  = false;
  MaskSource                         source{RadialMaskSource{}};
  std::optional<ColorRangeModel>     color_range;
  std::optional<LuminanceRangeModel> luminance_range;

  friend auto operator==(const MaskModel&, const MaskModel&) -> bool = default;
};

/**
 * @brief Source discriminator for @p source.
 */
[[nodiscard]] auto GetMaskSourceKind(const MaskSource& source) -> MaskSourceKind;

/**
 * @brief JSON kind text for @p kind (`brush`, `radial`, `linear_gradient`).
 */
[[nodiscard]] auto MaskSourceKindText(MaskSourceKind kind) -> std::string_view;

/**
 * @brief Validate one Mask value. Does not inspect other Masks in a Grade.
 *
 * @param mask Candidate Mask.
 * @throws std::runtime_error when identity, numeric, source, descriptor, or range
 *         rules fail. Does not mutate @p mask.
 */
void ValidateMaskModel(const MaskModel& mask);

/**
 * @brief Serialize one Mask, including null range fields and an explicit source kind.
 */
[[nodiscard]] auto MaskModelToJson(const MaskModel& mask) -> nlohmann::json;

/**
 * @brief Read one Mask object. Requires an explicit source kind and both range keys.
 *
 * @throws std::runtime_error on missing fields, unknown kinds, extra range keys,
 *         enabled ranges, or invalid values.
 */
[[nodiscard]] auto MaskModelFromJson(const nlohmann::json& json) -> MaskModel;

/**
 * @brief Read-only view of an ordered Mask sequence, addressed by index.
 *
 * Does not own the Masks. Functions that read a Mask sequence take this one type, whether the
 * Masks live in a Color Grade (@ref ColorGradeNodeModel::Masks, which stores each Mask behind its
 * own pointer) or in a contiguous container (implicit conversion from `std::vector`, `std::span`,
 * or an array of @ref MaskModel).
 *
 * Lifetime: a view of a Color Grade's Masks is valid until the next Mask write on that node; a
 * view of a container is valid while the container is unchanged. Iterators copy the view's
 * storage pointer, so they do not depend on the view object that produced them.
 */
class MaskListView {
 public:
  /// Return element @p index of @p storage. @p index is below the view size.
  using ElementAccessor = auto (*)(const void* storage, std::size_t index) -> const MaskModel&;

  class Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = MaskModel;
    using difference_type   = std::ptrdiff_t;
    using pointer           = const MaskModel*;
    using reference         = const MaskModel&;

    Iterator() = default;

    auto operator*() const -> reference { return at_(storage_, index_); }
    auto operator->() const -> pointer { return &at_(storage_, index_); }
    auto operator++() -> Iterator& {
      ++index_;
      return *this;
    }
    auto operator++(int) -> Iterator {
      auto previous = *this;
      ++index_;
      return previous;
    }
    friend auto operator==(const Iterator& left, const Iterator& right) -> bool {
      return left.storage_ == right.storage_ && left.index_ == right.index_;
    }

   private:
    friend class MaskListView;
    Iterator(const void* storage, ElementAccessor at, std::size_t index)
        : storage_(storage), at_(at), index_(index) {}

    const void*     storage_ = nullptr;
    ElementAccessor at_      = nullptr;
    std::size_t     index_   = 0;
  };

  /// Empty view.
  MaskListView() = default;

  /**
   * @brief View of a contiguous Mask container. Implicit so containers pass where a view is taken.
   * @param masks Container that outlives the view and is not changed while the view is used.
   */
  template <class Range>
    requires std::ranges::contiguous_range<const Range&> &&
             std::same_as<std::ranges::range_value_t<Range>, MaskModel>
  MaskListView(const Range& masks)  // NOLINT(google-explicit-constructor)
      : storage_(std::ranges::data(masks)),
        size_(std::ranges::size(masks)),
        at_(&ContiguousElementAt) {}

  /**
   * @brief View of @p size Masks read through @p at from @p storage.
   * @param storage Owner-defined storage; passed back to @p at unchanged.
   * @param at Accessor for one element; must not be null when @p size is non-zero.
   */
  MaskListView(const void* storage, std::size_t size, ElementAccessor at)
      : storage_(storage), size_(size), at_(at) {}

  [[nodiscard]] auto size() const -> std::size_t { return size_; }
  [[nodiscard]] auto empty() const -> bool { return size_ == 0; }
  /// Mask @p index in display order. @pre @p index < size().
  [[nodiscard]] auto operator[](std::size_t index) const -> const MaskModel& {
    return at_(storage_, index);
  }
  [[nodiscard]] auto begin() const -> Iterator { return {storage_, at_, 0}; }
  [[nodiscard]] auto end() const -> Iterator { return {storage_, at_, size_}; }

 private:
  static auto ContiguousElementAt(const void* storage, std::size_t index) -> const MaskModel& {
    return static_cast<const MaskModel*>(storage)[index];
  }

  const void*     storage_ = nullptr;
  std::size_t     size_    = 0;
  ElementAccessor at_      = nullptr;
};

/**
 * @brief True when @p masks contains a duplicate or empty @ref MaskId.
 */
[[nodiscard]] auto HasDuplicateOrEmptyMaskId(const std::vector<MaskModel>& masks) -> bool;

/**
 * @brief First enabled Mask in display order, or null when none are enabled.
 */
[[nodiscard]] auto FirstEnabledMask(MaskListView masks) -> const MaskModel*;

/**
 * @brief Packed Radial parameters for native analytic evaluators.
 *
 * Invert and opacity are copied from the Mask. Evaluators apply invert, then opacity,
 * then clamp to `[0, 1]` before R8 quantization.
 */
struct RadialMaskParams {
  float center_x      = 0.5f;
  float center_y      = 0.5f;
  float major_radius  = 0.5f;
  float minor_radius  = 0.5f;
  float rotation      = 0.0f;
  float inner_feather = 0.0f;
  float outer_feather = 0.0f;
  bool  invert        = false;
  float opacity       = 1.0f;
};

/**
 * @brief Packed Linear Gradient parameters for native analytic evaluators.
 *
 * Invert and opacity follow the same Mask-level order as Radial.
 */
struct LinearGradientMaskParams {
  float origin_x            = 0.5f;
  float origin_y            = 0.5f;
  float normal_x            = 0.0f;
  float normal_y            = 1.0f;
  float transition_distance = 0.2f;
  float start_value         = 1.0f;
  float end_value           = 0.0f;
  bool  invert              = false;
  float opacity             = 1.0f;
};

enum class AnalyticMaskKind : std::uint8_t {
  Radial         = 0,
  LinearGradient = 1,
};

[[nodiscard]] auto AnalyticKindFromMask(const MaskModel& mask) -> AnalyticMaskKind;
[[nodiscard]] auto RadialParamsFromMask(const MaskModel& mask) -> RadialMaskParams;
[[nodiscard]] auto LinearGradientParamsFromMask(const MaskModel& mask) -> LinearGradientMaskParams;

}  // namespace alcedo
