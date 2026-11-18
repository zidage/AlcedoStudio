//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/image_geometry_model.hpp"
#include "edit/graph/pipeline_graph.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "edit/operators/models/parameter_revision.hpp"
#include "image/raster_color_description.hpp"
#include "json.hpp"

namespace alcedo {

/// First value reserved for the next automatically named Color Grade.
inline constexpr std::uint64_t kInitialNextColorGradeNameNumber = 2;

/**
 * @brief Serializable pipeline: geometry plus DAG. Does not own a GPU workspace.
 *
 * Graph edits stamp @ref TopologyRevision. Parameter Model setters stamp their Models.
 *
 * Copy-on-write: nodes, adjustment Models, and Color Grade Mask lists are shared with the
 * immutable documents that @ref Freeze returns. Every non-const accessor (@ref Develop,
 * @ref PrimaryGrade, @ref Drt, `Graph().FindNode`, and the node and Model lookups under them)
 * first copies the one part it returns when a frozen document shares it. So a frozen document
 * keeps its values while this document changes, and a freeze costs O(nodes), not O(document).
 *
 * The document is movable. It is not copyable: use @ref Freeze for an immutable copy and
 * @ref ClonePipelineDocument for an independent editable copy.
 */
class PipelineDocument {
 public:
  PipelineDocument()                                       = default;
  PipelineDocument(PipelineDocument&&) noexcept            = default;
  PipelineDocument& operator=(PipelineDocument&&) noexcept = default;
  PipelineDocument& operator=(const PipelineDocument&)     = delete;
  ~PipelineDocument()                                      = default;

  /**
   * @brief Immutable copy of the current values that shares every node with this document.
   *
   * Cost is one document object plus the node and edge vectors; no node, Model, or Mask is
   * copied. Later writes to this document copy only the parts they change, so the returned
   * document never changes. Readers on any thread may read it for as long as they hold it.
   *
   * @pre Called on the thread that writes this document (or under the lock that serializes its
   *      writers). This document is the only writable holder of its parts.
   */
  [[nodiscard]] auto Freeze() const -> std::shared_ptr<const PipelineDocument>;

  [[nodiscard]] auto FormatVersion() const -> std::uint32_t { return format_version_; }
  [[nodiscard]] auto Geometry() -> ImageGeometryModel& { return geometry_; }
  [[nodiscard]] auto Geometry() const -> const ImageGeometryModel& { return geometry_; }
  [[nodiscard]] auto Graph() -> PipelineGraph& { return graph_; }
  [[nodiscard]] auto Graph() const -> const PipelineGraph& { return graph_; }

  /// Persistent default identity, independent of current-panel routing; empty after deletion.
  [[nodiscard]] auto DefaultGradeId() const -> const NodeId& { return default_grade_id_; }
  /// Restore identity on the owner thread; rejects nonempty IDs that are not Color Grades.
  void SetDefaultGradeId(NodeId id);
  /// Preflight a user deletion without mutation. Node removal checks every owned Mask.
  /// Call under owner access before any batch mutation; trusted history replay does not call this.
  [[nodiscard]] auto ValidateUserDeletion(const NodeId& node_id,
                                         std::optional<MaskId> mask_id = std::nullopt) const
      -> std::vector<GraphValidationError>;

  /**
   * @brief Return the next automatically assigned Color Grade display-name number.
   */
  [[nodiscard]] auto NextColorGradeNameNumber() const -> std::uint64_t {
    return next_color_grade_name_number_;
  }

  /**
   * @brief Restore an exact automatically assigned Color Grade display-name number.
   * @throws std::invalid_argument when @p number is zero.
   */
  void               SetNextColorGradeNameNumber(std::uint64_t number);

  /**
   * @brief Advance the automatically assigned Color Grade display-name number.
   * @throws std::overflow_error when no larger value can be represented.
   */
  void               ConsumeNextColorGradeNameNumber();

  /**
   * @brief Stamp of the last node, edge, or adjustment-list change (@ref NextParameterRevision).
   *
   * Readers compare it with the stamp they saw last. Nothing clears it.
   */
  [[nodiscard]] auto TopologyRevision() const -> ParameterRevision { return topology_revision_; }
  /// Record a node, edge, or adjustment-list change with a new stamp.
  void               MarkTopologyChanged() { topology_revision_ = NextParameterRevision(); }
  /// Take the topology stamp of @p source, a document this one was cloned from.
  void               CopyTopologyRevisionFrom(const PipelineDocument& source) {
    topology_revision_ = source.topology_revision_;
  }

  [[nodiscard]] auto Develop() -> DevelopNodeModel*;
  [[nodiscard]] auto Develop() const -> const DevelopNodeModel*;
  /**
   * @brief First Color Grade used for current-panel routing.
   *
   * Prefers the default `grade.primary` node. When that ID is absent, returns the
   * first Color Grade on the image backbone. Null when the backbone has no Grade.
   */
  [[nodiscard]] auto PrimaryGrade() -> ColorGradeNodeModel*;
  [[nodiscard]] auto PrimaryGrade() const -> const ColorGradeNodeModel*;
  [[nodiscard]] auto Drt() -> DrtNodeModel*;
  [[nodiscard]] auto Drt() const -> const DrtNodeModel*;

  /**
   * @brief Give this document the sensor settings of @p current and keep its own white balance
   *        and every downstream node (DevelopParamsModel::UseSensorSettingsFrom).
   *
   * For a private document of the editor comparison: a replay of one state of the image that
   * @p current shows. Geometry, Color Grades, Masks, DRT, and topology stay unchanged. Checks
   * every precondition before it writes.
   *
   * @throws std::invalid_argument when either document has no Develop node, the Develop node IDs
   *         differ, or the camera profiles differ (the documents are not of the same image). This
   *         document is unchanged.
   */
  void               UseSensorSettingsFrom(const PipelineDocument& current);

  /**
   * @brief Give this document every Develop setting of @p current, including the white balance,
   *        and keep every downstream node (DevelopParamsModel::UseAllSettingsFrom).
   *
   * For the private AI Mask analysis document: the import root look of the image that @p current
   * shows, with its current Develop node. Geometry, Color Grades, Masks, DRT, and topology stay
   * unchanged. Checks the preconditions of @ref UseSensorSettingsFrom before it writes.
   *
   * @throws std::invalid_argument under the conditions of @ref UseSensorSettingsFrom. This
   *         document is unchanged.
   */
  void               UseDevelopSettingsFrom(const PipelineDocument& current);

  /**
   * @brief Insert an adjustment on a ColorGrade node and record a topology change.
   */
  void InsertAdjustment(const NodeId& grade_id, std::size_t index, AdjustmentInstanceId instance_id,
                        std::unique_ptr<IOperatorModel> model);

  [[nodiscard]] auto ToJson() const -> nlohmann::json;
  static auto        FromJson(const nlohmann::json& json) -> PipelineDocument;

 private:
  /// Develop node of @p current after the checks of @ref UseSensorSettingsFrom; writes nothing.
  [[nodiscard]] auto RequireDevelopOfSameImage(const PipelineDocument& current) const
      -> const DevelopNodeModel&;

  /// Shares every node with @p other. Only @ref Freeze calls this.
  PipelineDocument(const PipelineDocument& other) = default;

  std::uint32_t       format_version_ = kPipelineDocumentFormatVersion;
  ImageGeometryModel  geometry_{};
  PipelineGraph       graph_{};
  std::uint64_t       next_color_grade_name_number_ = kInitialNextColorGradeNameNumber;
  NodeId              default_grade_id_;
  ParameterRevision   topology_revision_ = NextParameterRevision();
};

/**
 * @brief Format the product display name for an automatically assigned Color Grade.
 * @throws std::invalid_argument when @p number is zero.
 */
[[nodiscard]] auto DefaultColorGradeDisplayName(std::uint64_t number) -> std::string;

/// Product Default Color Grade exposure, in EV. Baked by @ref CreateDefaultPipelineDocument.
inline constexpr float kDefaultPipelineExposureEv = 1.5f;
/// Product Default saturation multiplier (legacy UI +30 → 1 + 30/100).
inline constexpr float kDefaultPipelineSaturation = 1.3f;
/// Product Default RGB curve: the Tone Curve preset `cl_digital` (Cl. Digit,
/// tone_curve_presets.hpp), one S curve placed by hand on top of the ACES 2.0 display transform
/// (#914; Film-like before).
inline constexpr std::string_view kDefaultPipelineToneCurvePresetId = "cl_digital";
/**
 * Contrast slider value of the RAW import look before the Film-like curve (OkLab lightness S
 * curve, slope 2^(15/100) at mid grey). Documents imported before #891 keep it; the entry resets of
 * the profile tone stage still read it. The look of a new document has contrast 0.
 */
inline constexpr float            kImportLookContrastBeforeFilmLike = 15.0f;
/// Product Default DRT method of a RAW document.
inline constexpr DrtMethod        kDefaultPipelineDrtMethod         = DrtMethod::OpenDrt;
/// Product Default OpenDRT look preset of a RAW document.
inline constexpr std::string_view kDefaultPipelineOpenDrtLookPreset = "standard";
/// Product Default lens correction switch (Develop `lens_enabled`) of a RAW document.
inline constexpr bool             kDefaultPipelineLensCorrection    = false;

/// Exposure range of the RAW import look, in EV: the range of the editor Exposure slider.
inline constexpr float            kRawImportLookMinExposureEv       = -10.0f;
inline constexpr float            kRawImportLookMaxExposureEv       = 10.0f;
/// Saturation multiplier range of the RAW import look: the editor Saturation slider -100 to +100.
inline constexpr float            kRawImportLookMinSaturation       = 0.0f;
inline constexpr float            kRawImportLookMaxSaturation       = 2.0f;

/// OpenDRT look presets that the RAW import look can select: the named presets of the editor
/// Display Transform menu (`custom` reads the detailed parameters, which the import look keeps at
/// their defaults).
[[nodiscard]] auto RawImportOpenDrtLookPresets() -> std::span<const std::string_view>;

/**
 * @brief The values of a RAW document that the user can choose for new imports (#918).
 *
 * The default member values are the product Default of @ref CreateDefaultPipelineDocument. A
 * document built from a value-initialized RawImportLook has the same JSON as the product Default.
 * The values are plain data; the application reads them from its settings through
 * `RawImportLookSource` (app layer) and passes them to @ref CreateDefaultPipelineDocument.
 */
struct RawImportLook {
  /// Exposure of Color Grade 1, in EV.
  float       exposure_ev          = kDefaultPipelineExposureEv;
  /// Saturation multiplier of Color Grade 1 (editor value +30 is 1.3).
  float       saturation           = kDefaultPipelineSaturation;
  /// Id of the Tone Curve preset (@ref ToneCurvePresets) whose points become the RGB curve.
  std::string tone_curve_preset_id = std::string{kDefaultPipelineToneCurvePresetId};
  /// DRT method: @ref DrtMethod::Aces20 or @ref DrtMethod::OpenDrt.
  DrtMethod   drt_method           = kDefaultPipelineDrtMethod;
  /// OpenDRT look preset, one of @ref RawImportOpenDrtLookPresets. The DRT stores it for both
  /// methods, so a later switch to OpenDRT in the editor starts from it.
  std::string open_drt_look_preset = std::string{kDefaultPipelineOpenDrtLookPreset};
  /// Develop `lens_enabled`.
  bool        lens_correction      = kDefaultPipelineLensCorrection;

  friend auto operator==(const RawImportLook&, const RawImportLook&) -> bool = default;
};

/**
 * @brief Check every value of @p look against the ranges and lists above.
 * @return An empty string when @p look is valid, else a message that names the first bad value.
 */
[[nodiscard]] auto DescribeRawImportLookError(const RawImportLook& look) -> std::string;

/**
 * @brief Three-node product Default: Develop -> Primary Color Grade -> DRT, with the RAW import
 *        look of @p look.
 *
 * The primary Color Grade is @ref ColorGradeNodeModel::MakeDefault with the exposure, the
 * saturation, and the RGB curve of the Tone Curve preset of @p look; contrast stays 0, because the
 * curve carries the contrast of the look. The DRT is @ref DrtNodeModel::MakeDefault with the method
 * and OpenDRT look preset of @p look, and Develop has `lens_enabled` of @p look. The editor
 * sliders and the curve Reset go to the catalog identity values (0 EV, the identity curve), not to
 * this look.
 *
 * @throws std::invalid_argument when @ref DescribeRawImportLookError rejects @p look.
 * @return A document that satisfies graph Validate and ValidateImageBackbone.
 */
[[nodiscard]] auto CreateDefaultPipelineDocument(const RawImportLook& look) -> PipelineDocument;

/// @ref CreateDefaultPipelineDocument with the product Default look (a value-initialized
/// @ref RawImportLook).
[[nodiscard]] auto CreateDefaultPipelineDocument() -> PipelineDocument;

/**
 * @brief Default document of a raster (JPEG, PNG, TIFF, OpenEXR) image (decision D2).
 *
 * Develop carries the raster `input` object with @p source_color and override `auto`; lens
 * correction is off. The primary Color Grade is neutral: the RAW default look is not applied.
 * The DRT is ACES 2.0 with the limiting and encoding space nearest the source primaries (Rec.709,
 * P3-D65, else Rec.2020), the source peak luminance, and the EOTF nearest the source transfer. A
 * scene-linear source has no display transfer and uses Rec.709, sRGB piecewise, 100 nits.
 */
[[nodiscard]] auto CreateDefaultRasterPipelineDocument(const RasterColorDescription& source_color)
    -> PipelineDocument;

/**
 * @brief Hash of every change stamp in @p document.
 *
 * Covers the topology stamp, every Model's field stamps (Develop, DRT, and every adjustment),
 * each Color Grade's mix stamp, and each Mask content revision. Two equal values mean that no
 * stamped write happened in between. Display names and deletion protection have no stamp and
 * are not covered.
 */
[[nodiscard]] auto DocumentRevisionFingerprint(const PipelineDocument& document) -> std::uint64_t;

/**
 * @brief Deep copy via JSON round-trip. The clone does not share Model pointers.
 *
 * The clone keeps the immutable DNG profile that the source Develop node has bound (shared,
 * not copied), because the JSON form holds only the profile fingerprint.
 */
[[nodiscard]] auto ClonePipelineDocument(const PipelineDocument& src) -> PipelineDocument;

/**
 * @brief Color Grade nodes on the unique Develop-to-DRT scene-image path, in path order.
 *
 * Empty when the backbone cannot be walked. Off-path Color Grades are omitted.
 */
[[nodiscard]] auto ColorGradesOnImageBackbone(const PipelineDocument& document)
    -> std::vector<const ColorGradeNodeModel*>;

}  // namespace alcedo
