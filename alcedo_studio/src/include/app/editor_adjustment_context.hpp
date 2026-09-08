//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/editor_adjustment_types.hpp"
#include "app/editor_node_graph_projection.hpp"
#include "app/editor_panel_projection.hpp"
#include "edit/graph/graph_ids.hpp"
#include "image/metadata.hpp"

namespace alcedo {

class Image;
class PipelineDocument;

inline constexpr std::string_view kAdjustmentPanelTone     = "tone";
inline constexpr std::string_view kAdjustmentPanelLook     = "look";
inline constexpr std::string_view kAdjustmentPanelLut      = "lut";
inline constexpr std::string_view kAdjustmentPanelMasks    = "masks";
inline constexpr std::string_view kAdjustmentPanelRaw      = "raw";
inline constexpr std::string_view kAdjustmentPanelGeometry = "geometry";
inline constexpr std::string_view kAdjustmentPanelDisplay  = "display";
inline constexpr std::string_view kAdjustmentPanelDetail   = "detail";

/**
 * @brief Image-owned EXIF fields shown in the adjustment header.
 *
 * Source owner: Image::exif_display_. This is not PipelineDocument state and is
 * not reread when the selected NodeId changes. Nullopt means missing or invalid;
 * the header omits those tokens from the EXIF line.
 */
struct EditorImageExifDisplay {
  std::optional<std::pair<int, int>> shutter_speed;
  std::optional<std::uint64_t>       iso;
  std::optional<float>               aperture;
  std::optional<float>               focal_mm;
};

/// UTF-8 em dash used when a header EXIF field is missing or invalid.
inline constexpr std::string_view kMissingExifDisplay = "\xE2\x80\x94";

/**
 * @brief Compact header EXIF tokens, including units.
 *
 * Missing or invalid fields are @ref kMissingExifDisplay. Tokens match the
 * one-line readout: `100mm`, `f2.8`, `1/500s`, `ISO 100`.
 */
struct EditorExifRowText {
  std::string shutter;
  std::string iso;
  std::string aperture;
  std::string focal;
};

/**
 * @brief Read-only selected-node capabilities for panel navigation.
 *
 * @p selected_node_id is copied from EditorNodeController at read time. This
 * struct does not own graph selection or mutable Models. Supported parameters
 * are loaded separately through ProjectSelectedNodePanelFields.
 *
 * GUI lifetime: value copy. Drop it when the session or selection changes.
 */
struct EditorAdjustmentContext {
  NodeId                           selected_node_id;
  EditorNodeKind                   node_kind = EditorNodeKind::ColorGrade;
  std::string                      display_name;
  std::span<const std::string_view> supported_panels{};
  std::string_view                 default_panel = kAdjustmentPanelTone;
  EditorImageExifDisplay           exif{};
};

/**
 * @brief Copy the four display EXIF fields from the Image metadata owner.
 *
 * Does not parse EXIF JSON. Invalid shutter, non-positive ISO/aperture/focal,
 * and missing metadata yield nullopt fields.
 */
[[nodiscard]] auto ReadEditorImageExifDisplay(const ExifDisplayMetaData& metadata)
    -> EditorImageExifDisplay;

/**
 * @brief Copy the four display EXIF fields from @p image when display metadata is present.
 */
[[nodiscard]] auto ReadEditorImageExifDisplay(const Image& image) -> EditorImageExifDisplay;

/**
 * @brief Format compact header EXIF tokens for QML.
 *
 * Valid shutter rationals become `1/250s` or `2s`. Positive ISO is `ISO 100`.
 * Positive aperture is `f2.8`. Focal length uses the actual mm value with a
 * `mm` unit and no space, never the 35 mm equivalent. Does not parse EXIF JSON.
 */
[[nodiscard]] auto FormatEditorImageExifDisplay(const EditorImageExifDisplay& display)
    -> EditorExifRowText;

/**
 * @brief Join compact EXIF tokens into one readout line.
 *
 * Order is focal, aperture, shutter, ISO. Missing tokens are omitted. If every
 * field is missing, the result is @ref kMissingExifDisplay. Example:
 * `100mm f2.8 1/500s ISO 100`.
 */
[[nodiscard]] auto FormatEditorImageExifLine(const EditorExifRowText& text) -> std::string;

[[nodiscard]] auto FormatEditorImageExifLine(const EditorImageExifDisplay& display) -> std::string;

/// Panels the selected node kind may show. Geometry is Develop-only.
[[nodiscard]] auto SupportedAdjustmentPanels(EditorNodeKind kind)
    -> std::span<const std::string_view>;

/// First supported panel when the current page is illegal for @p kind.
[[nodiscard]] auto DefaultAdjustmentPanel(EditorNodeKind kind) -> std::string_view;

[[nodiscard]] auto AdjustmentPanelIsSupported(EditorNodeKind kind, std::string_view panel) -> bool;

/// True when @p field_key may be submitted or projected for @p kind.
[[nodiscard]] auto AdjustmentFieldIsSupported(EditorNodeKind kind, std::string_view field_key)
    -> bool;

/**
 * @brief Fill a production target from the selected node, not PrimaryGrade.
 *
 * Geometry stays document-owned and is accepted only when @p selected_node_id
 * is Develop. Color Grade fields require that node to be the owning Grade.
 * Missing nodes or instances fail; the first operator of a type on another
 * node is never substituted.
 *
 * @pre Caller holds the executor render lock when @p document is live.
 */
[[nodiscard]] auto CompleteSelectedNodeParameterTarget(const PipelineDocument& document,
                                                       const NodeId& selected_node_id,
                                                       std::string field_key, std::string* error)
    -> std::optional<EditorParameterTarget>;

/**
 * @brief Supported production adapter targets for @p selected_node_id.
 *
 * @return nullopt when the node is missing or a supported instance cannot be
 *         resolved. Output is unchanged by the caller on failure.
 */
[[nodiscard]] auto SelectedNodeProjectionTargets(const PipelineDocument& document,
                                                 const NodeId& selected_node_id, std::string* error)
    -> std::optional<std::vector<EditorParameterTarget>>;

/**
 * @brief Project every supported panel field for @p selected_node_id in one read.
 *
 * Failure leaves @p out unchanged. Does not call Model ToJson, LoadJson, or
 * MakeFullDto. Load-only: does not mutate parameters. Must not acquire the live
 * render lock — selection cannot stall present.
 */
auto ProjectSelectedNodePanelFields(const PipelineDocument& document, const NodeId& selected_node_id,
                                    std::uint64_t session_generation, EditorPanelProjection* out,
                                    std::string* error) -> bool;

/**
 * @brief Copy selected-node identity, capabilities, and caller-supplied EXIF.
 *
 * @p exif must already be read from the current Image. This function does not
 * look up Image or parse metadata. Returns nullopt when the node is missing.
 */
[[nodiscard]] auto MakeEditorAdjustmentContext(const PipelineDocument& document,
                                               const NodeId& selected_node_id,
                                               EditorImageExifDisplay exif)
    -> std::optional<EditorAdjustmentContext>;

}  // namespace alcedo
