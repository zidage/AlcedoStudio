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
#include "edit/mask/ai_mask_recipe.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_id.hpp"
#include "json.hpp"

#ifdef ALCEDO_ENABLE_BRUSH_MASK
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_stroke.hpp"
#endif

namespace alcedo {

/** @brief Discriminator for @ref MaskSource. */
enum class MaskSourceKind : std::uint8_t {
#ifdef ALCEDO_ENABLE_BRUSH_MASK
  Brush = 0,
#endif
  Radial         = 1,
  LinearGradient = 2,
  AiSegmentation = 3,
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

/// Format version of the `ai_segmentation` source JSON.
inline constexpr std::uint32_t kAiSegmentationSourceFormatVersion = 1;

/**
 * @brief AI Segmentation Mask: a stored R8 coverage asset and the recipe that made it.
 *
 * Rendering reads @p asset_key, @p descriptor, and @p edge. The edit session reads @p recipe.
 * @p analysis_extent is the size of the analysis image that the session used; the asset raster
 * has its resolution and covers @p descriptor.reference_bounds of the full ReferenceSpace.
 * @p model_set_id and @p geometry_fingerprint identify the models and the Lens and RAW decode
 * settings of the analysis image. The asset bytes live in the project database (`MaskAsset`).
 */
struct AiSegmentationMaskSource {
  MaskAssetKey         asset_key;
  MaskAssetDescriptor  descriptor{};
  Extent2D             analysis_extent{};
  AiMaskRecipe         recipe;
  AiMaskEdgeAdjustment edge;
  std::string          model_set_id;
  std::string          geometry_fingerprint;

  friend auto          operator==(const AiSegmentationMaskSource&, const AiSegmentationMaskSource&)
      -> bool = default;
};

using MaskSource = std::variant<
#ifdef ALCEDO_ENABLE_BRUSH_MASK
    BrushMaskSource,
#endif
    RadialMaskSource, LinearGradientMaskSource, AiSegmentationMaskSource>;

/// Smallest selected width of a Luminance Range, in OkLab L units.
inline constexpr float kLuminanceRangeMinimumWidth       = 0.01f;
/// Largest soft transition of a Luminance Range, in OkLab L units.
inline constexpr float kLuminanceRangeMaximumSoftness    = 0.5f;
/// Smallest selected width of a Hue Range, in degrees.
inline constexpr float kColorRangeMinimumWidthDegrees    = 1.0f;
/// Largest soft transition of a Hue Range, in degrees.
inline constexpr float kColorRangeMaximumSoftnessDegrees = 90.0f;

/**
 * @brief Hue Range of a Mask (UI: "Hue Range"), in OkLCh hue degrees.
 *
 * Selects hues from @p hue_start to @p hue_end, counterclockwise. `hue_start > hue_end` wraps
 * through 0. @p hue_start is in [0, 360), @p hue_end in (0, 360], and the circular width is at
 * least @ref kColorRangeMinimumWidthDegrees. @p softness is the transition width outside the
 * range, in (0, @ref kColorRangeMaximumSoftnessDegrees]. A Mask stores only a restricted range:
 * the full range [0, 360] is an empty `std::optional`.
 */
struct ColorRangeModel {
  float hue_start = 0.0f;
  float hue_end   = 360.0f;
  float softness  = 12.0f;

  friend auto operator==(const ColorRangeModel&, const ColorRangeModel&) -> bool = default;
};

/**
 * @brief Luminance Range of a Mask, in OkLab L.
 *
 * Selects L from @p low to @p high: `0 <= low`, `high <= 1`, and
 * `high - low >= kLuminanceRangeMinimumWidth`. @p softness is the transition width outside the
 * range, in (0, @ref kLuminanceRangeMaximumSoftness]. A Mask stores only a restricted range: the
 * full range [0, 1] is an empty `std::optional`.
 */
struct LuminanceRangeModel {
  float low      = 0.0f;
  float high     = 1.0f;
  float softness = 0.05f;

  friend auto operator==(const LuminanceRangeModel&, const LuminanceRangeModel&) -> bool = default;
};

/// Circular width of @p range in degrees, in [0, 360].
[[nodiscard]] auto ColorRangeWidthDegrees(const ColorRangeModel& range) -> float;
/// Whether @p range selects every hue (`hue_start == 0` and `hue_end == 360`).
[[nodiscard]] auto IsFullColorRange(const ColorRangeModel& range) -> bool;
/// Whether @p range selects every luminance (`low == 0` and `high == 1`).
[[nodiscard]] auto IsFullLuminanceRange(const LuminanceRangeModel& range) -> bool;

/**
 * @brief Validate a stored Hue Range: finite values, bounds, minimum width, and not full.
 * @throws std::runtime_error naming the first rule that fails.
 */
void ValidateColorRange(const ColorRangeModel& range);
/**
 * @brief Validate a stored Luminance Range: finite values, bounds, minimum width, and not full.
 * @throws std::runtime_error naming the first rule that fails.
 */
void ValidateLuminanceRange(const LuminanceRangeModel& range);

/**
 * @brief Serialize a Mask `color_range` field: `null` when empty, otherwise
 *        `{"hue_end", "hue_start", "softness"}`. Does not validate.
 */
[[nodiscard]] auto ColorRangeToJson(const std::optional<ColorRangeModel>& range) -> nlohmann::json;
/**
 * @brief Serialize a Mask `luminance_range` field: `null` when empty, otherwise
 *        `{"high", "low", "softness"}`. Does not validate.
 */
[[nodiscard]] auto LuminanceRangeToJson(const std::optional<LuminanceRangeModel>& range)
    -> nlohmann::json;

/**
 * @brief Read a Mask `color_range` field.
 *
 * `null` and the legacy placeholder `{"enabled": false}` read as empty. A range object needs
 * exactly `hue_start`, `hue_end`, and `softness`; a valid object that covers the full range
 * reads as empty.
 * @throws std::runtime_error on other keys, `"enabled": true`, or invalid values.
 */
[[nodiscard]] auto ColorRangeFromJson(const nlohmann::json& json) -> std::optional<ColorRangeModel>;
/**
 * @brief Read a Mask `luminance_range` field.
 *
 * `null` and the legacy placeholder `{"enabled": false}` read as empty. A range object needs
 * exactly `low`, `high`, and `softness`; a valid object that covers the full range reads as
 * empty.
 * @throws std::runtime_error on other keys, `"enabled": true`, or invalid values.
 */
[[nodiscard]] auto LuminanceRangeFromJson(const nlohmann::json& json)
    -> std::optional<LuminanceRangeModel>;

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
 * @brief JSON kind text for @p kind (`brush`, `radial`, `linear_gradient`, `ai_segmentation`).
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
 * @brief Validate one source value with the source rules of @ref ValidateMaskModel.
 *
 * @throws std::runtime_error naming the first rule that fails. Does not mutate @p source.
 */
void ValidateMaskSource(const MaskSource& source);

/**
 * @brief Validate one AI Segmentation source with the source rules of @ref ValidateMaskModel.
 *
 * @throws std::runtime_error naming the first rule that fails. Does not mutate @p source.
 */
void               ValidateAiSegmentationMaskSource(const AiSegmentationMaskSource& source);

/**
 * @brief Serialize one Mask, including null range fields and an explicit source kind.
 */
[[nodiscard]] auto MaskModelToJson(const MaskModel& mask) -> nlohmann::json;

/**
 * @brief Serialize one source value with its explicit `kind`, as in @ref MaskModelToJson.
 * @throws std::runtime_error when @p source fails @ref ValidateMaskSource.
 */
[[nodiscard]] auto MaskSourceToJson(const MaskSource& source) -> nlohmann::json;

/**
 * @brief Read one source object by its `kind`, as in @ref MaskModelFromJson.
 * @throws std::runtime_error on a missing or unknown kind, missing fields, or invalid values.
 */
[[nodiscard]] auto MaskSourceFromJson(const nlohmann::json& json) -> MaskSource;

/**
 * @brief Read one Mask object. Requires an explicit source kind and both range keys.
 *
 * @throws std::runtime_error on missing fields, unknown kinds, extra range keys,
 *         enabled legacy ranges, or invalid values (@ref ColorRangeFromJson,
 *         @ref LuminanceRangeFromJson).
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

/**
 * @brief Analytic kind of a Radial or Linear Gradient Mask.
 * @throws std::invalid_argument when the source is not analytic.
 */
[[nodiscard]] auto AnalyticKindFromMask(const MaskModel& mask) -> AnalyticMaskKind;
[[nodiscard]] auto RadialParamsFromMask(const MaskModel& mask) -> RadialMaskParams;
[[nodiscard]] auto LinearGradientParamsFromMask(const MaskModel& mask) -> LinearGradientMaskParams;

}  // namespace alcedo
