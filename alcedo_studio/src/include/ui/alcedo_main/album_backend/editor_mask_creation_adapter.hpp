//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/editor_mask_creation_controller.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "json.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_geometry.hpp"

namespace alcedo::editor_rhi {
class EditorInteractionController;
class EditorOverlayItem;
}  // namespace alcedo::editor_rhi

namespace alcedo::ui {

class EditorNodeController;
class EditorSessionController;

/**
 * @brief QML adapter for Radial/Linear Mask creation and existing-mask editing.
 *
 * Maps item pointers, publishes control-only overlay geometry, and queues
 * owner-thread Mask commands. Does not take the pipeline lock. Selection is
 * load-only: it does not create a Mask, history, or photo render.
 */
class EditorMaskCreationAdapter : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY maskCreationChanged)
  Q_PROPERTY(bool creating READ creating NOTIFY maskCreationChanged)
  Q_PROPERTY(bool ownsLeftButton READ owns_left_button NOTIFY maskCreationChanged)
  Q_PROPERTY(bool bodyVisible READ body_visible NOTIFY maskCreationChanged)
  Q_PROPERTY(bool maskControlsActive READ mask_controls_active NOTIFY maskCreationChanged)
  Q_PROPERTY(QString toolKind READ tool_kind NOTIFY maskCreationChanged)
  Q_PROPERTY(QString selectedMaskId READ selected_mask_id NOTIFY maskCreationChanged)
  // Mask ids are unique only within their owning Color Grade node; pair with
  // selectedMaskId to identify the selected Mask across groups.
  Q_PROPERTY(QString selectedMaskNodeId READ selected_mask_node_id NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal innerFeatherPercent READ inner_feather_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal outerFeatherPercent READ outer_feather_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal majorRadiusPercent READ major_radius_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal minorRadiusPercent READ minor_radius_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal rotationDegrees READ rotation_degrees NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal transitionPercent READ transition_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(bool maskEnabled READ mask_enabled NOTIFY maskCreationChanged)
  Q_PROPERTY(bool maskInvert READ mask_invert NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal maskOpacityPercent READ mask_opacity_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(QString maskName READ mask_name NOTIFY maskCreationChanged)
  Q_PROPERTY(bool maskNudgeAvailable READ mask_nudge_available NOTIFY maskCreationChanged)
  // Luminance Range and Hue Range of the range target Mask (Mask ranges MR4): the Mask of the
  // open AI Mask edit when one is open (none for a new candidate), else the selected Mask.
  Q_PROPERTY(bool maskRangeAvailable READ mask_range_available NOTIFY maskRangeChanged)
  Q_PROPERTY(qreal maskLuminanceLow READ mask_luminance_low NOTIFY maskRangeChanged)
  Q_PROPERTY(qreal maskLuminanceHigh READ mask_luminance_high NOTIFY maskRangeChanged)
  Q_PROPERTY(qreal maskHueStart READ mask_hue_start NOTIFY maskRangeChanged)
  Q_PROPERTY(qreal maskHueEnd READ mask_hue_end NOTIFY maskRangeChanged)
  /// Soft transition outside the handles (stored value, or the default for a full range): OkLab
  /// L units and degrees. The bars draw the dimming over this width, as the kernels weight it.
  Q_PROPERTY(qreal maskLuminanceSoftness READ mask_luminance_softness NOTIFY maskRangeChanged)
  Q_PROPERTY(qreal maskHueSoftness READ mask_hue_softness NOTIFY maskRangeChanged)
  /// Saturation threshold of the Hue Range (OkLCh chroma); the default for a full range.
  Q_PROPERTY(qreal maskHueMinChroma READ mask_hue_min_chroma NOTIFY maskRangeChanged)
  /// Bounds and defaults of the Softness and saturation threshold controls, in model units.
  Q_PROPERTY(QVariantMap rangeParameterLimits READ range_parameter_limits CONSTANT)
  /// The "Show selection" toggle: the range coverage of the target Mask stays shown.
  Q_PROPERTY(bool maskRangeCoverageVisible READ mask_range_coverage_visible NOTIFY maskRangeChanged)
  /// Smallest handle distance of the bars, a little above the stored-range minimum widths.
  Q_PROPERTY(qreal luminanceRangeMinimumWidth READ luminance_range_minimum_width CONSTANT)
  Q_PROPERTY(qreal hueRangeMinimumWidth READ hue_range_minimum_width CONSTANT)
  /// Fill of the Hue bar: {position, color} maps, hue = position * 360 (MaskHueBarColor).
  Q_PROPERTY(QVariantList hueGradientStops READ hue_gradient_stops CONSTANT)
  /// Fill of the Luminance bar: {position, color} maps, OkLab L = position (MaskLuminanceBarGray).
  Q_PROPERTY(QVariantList luminanceGradientStops READ luminance_gradient_stops CONSTANT)

 public:
  enum class EditMode : std::uint8_t {
    Inactive = 0,
    Creating,
    Editing,
  };

  explicit EditorMaskCreationAdapter(EditorSessionController* session, QObject* parent = nullptr);
  ~EditorMaskCreationAdapter() override;

  [[nodiscard]] auto active() const -> bool { return edit_mode_ != EditMode::Inactive; }
  [[nodiscard]] auto creating() const -> bool { return edit_mode_ == EditMode::Creating; }
  [[nodiscard]] auto owns_left_button() const -> bool;
  [[nodiscard]] auto body_visible() const -> bool;
  [[nodiscard]] auto mask_controls_active() const -> bool;
  [[nodiscard]] auto tool_kind() const -> QString { return tool_kind_; }
  [[nodiscard]] auto selected_mask_id() const -> QString { return selected_mask_id_; }
  [[nodiscard]] auto selected_mask_node_id() const -> QString {
    return selected_mask_id_.isEmpty() ? QString{}
                                       : QString::fromStdString(std::string{edit_node_id_.Value()});
  }
  [[nodiscard]] auto edit_node_id() const -> const NodeId& { return edit_node_id_; }
  [[nodiscard]] auto inner_feather_percent() const -> qreal;
  [[nodiscard]] auto outer_feather_percent() const -> qreal;
  [[nodiscard]] auto major_radius_percent() const -> qreal;
  [[nodiscard]] auto minor_radius_percent() const -> qreal;
  [[nodiscard]] auto rotation_degrees() const -> qreal;
  [[nodiscard]] auto transition_percent() const -> qreal;
  [[nodiscard]] auto mask_enabled() const -> bool;
  [[nodiscard]] auto mask_invert() const -> bool;
  [[nodiscard]] auto mask_opacity_percent() const -> qreal;
  [[nodiscard]] auto mask_name() const -> QString;
  /**
   * @brief True when the selected Mask has a keyboard-movable position.
   *
   * Radial center and Linear origin are movable while selected.
   */
  [[nodiscard]] auto mask_nudge_available() const -> bool;
  [[nodiscard]] auto mask_range_available() const -> bool;
  [[nodiscard]] auto mask_luminance_low() const -> qreal;
  [[nodiscard]] auto mask_luminance_high() const -> qreal;
  [[nodiscard]] auto mask_hue_start() const -> qreal;
  [[nodiscard]] auto mask_hue_end() const -> qreal;
  [[nodiscard]] auto mask_range_coverage_visible() const -> bool {
    return range_coverage_requested_;
  }
  [[nodiscard]] auto        mask_luminance_softness() const -> qreal;
  [[nodiscard]] auto        mask_hue_softness() const -> qreal;
  [[nodiscard]] auto        mask_hue_min_chroma() const -> qreal;
  [[nodiscard]] static auto range_parameter_limits() -> QVariantMap;
  [[nodiscard]] static auto luminance_range_minimum_width() -> qreal;
  [[nodiscard]] static auto hue_range_minimum_width() -> qreal;
  [[nodiscard]] static auto hue_gradient_stops() -> QVariantList;
  [[nodiscard]] static auto luminance_gradient_stops() -> QVariantList;

  /**
   * @brief Open a range drag of the target Mask: @p kind is "luminance" or "hue".
   *
   * Queues one BeginMaskField of the range key for the target Mask (any kind). Without the
   * "Show selection" toggle the range coverage of the Mask is shown until @ref finishRangeEdit
   * (D12). @return False when no target Mask can be edited or another edit is open.
   */
  Q_INVOKABLE bool          beginRangeEdit(const QString& kind);
  /**
   * @brief Write the handles of the open range drag: Luminance @p first = low, @p second = high
   *        (OkLab L, [0, 1]); Hue @p first = start, @p second = end (degrees; start > end wraps
   *        through 0). The values are clamped and widened to the minimum width; both handles at
   *        the ends write the full range (`null`). Interactive render, no commit.
   */
  Q_INVOKABLE void          updateRangeEdit(const QString& kind, qreal first, qreal second);
  /// Settle the open range drag: one history commit when the value changed, Quality render.
  Q_INVOKABLE void          finishRangeEdit(const QString& kind);
  /**
   * @brief Move one handle of the target Mask by @p steps key or wheel steps.
   *
   * @p handle is "first" (Luminance low, Hue start) or "second" (Luminance high, Hue end). One
   * step is 0.005 in OkLab L or 0.5 degrees, ten times that with @p coarse. A step that breaks
   * the minimum handle distance does not move the handle. The first step opens a range edit
   * (and shows the coverage like a drag); the steps of one burst settle as one history commit
   * when no step follows for the settle interval (180 ms, as AdjustmentSlider key steps), or
   * when another range input starts. Returns false when no step was applied.
   */
  Q_INVOKABLE bool          stepRangeHandle(const QString& kind, const QString& handle, int steps,
                                            bool coarse);
  /**
   * @brief Set one value of a range of the target Mask (value fields, Softness and threshold
   *        sliders), in model units.
   *
   * @p field is "first" / "second" (Luminance low / high in OkLab L, Hue start / end in degrees;
   * the value is clamped so the handles keep the minimum distance, and a Hue value wraps into
   * the bar), "softness" (clamped to the slider bounds), or "minChroma" (Hue only, clamped to
   * [0.015, 0.2]). Softness and threshold are refused while the range is full. The value is
   * written into the open drag of the same range (a slider drag opened with @ref
   * beginRangeEdit), else into a key-step burst that settles as one commit like @ref
   * stepRangeHandle; @ref commitRangeEdits settles it at once. Returns false when nothing was
   * written.
   */
  Q_INVOKABLE bool          editRangeValue(const QString& kind, const QString& field, qreal value);
  /// Settle an open value or step burst now (a typed value: Enter or focus-out).
  Q_INVOKABLE void          commitRangeEdits();
  /// Write the full range (`null`) of @p kind of the target Mask with one commit.
  Q_INVOKABLE void          resetRange(const QString& kind);
  /// The "Show selection" toggle. Shows or hides the range coverage of the target Mask.
  Q_INVOKABLE void          setRangeCoverageVisible(bool visible);

  /**
   * @brief Set the range target of an AI Mask edit (EditorAiMaskTool, GUI thread).
   *
   * @p open true with @p mask: the stored Mask of the edit is the range target. @p open true
   * without @p mask: a new candidate is edited, and no range target exists (D11). @p open false:
   * the selected Mask is the target again.
   */
  void                      SetAiMaskEditRangeTarget(bool open, const NodeId& grade_id,
                                                     const std::optional<MaskId>& mask_id);
  /// Settle interval of a key or wheel step burst (tests shorten or lengthen it).
  void                      SetRangeStepSettleIntervalMs(int interval_ms);
  /// An adjustment input started: hide the range coverage and turn the toggle off (D12).
  void                      OnAdjustmentInputStarted();
  /// The AI Mask candidate replaced the range coverage in the session: turn the toggle off.
  void                      OnRangeCoverageReplaced();

  Q_INVOKABLE void   bindInteractionItem(QObject* interaction);
  Q_INVOKABLE void   bindOverlayItem(QObject* overlay);
  Q_INVOKABLE void   beginRadial();
  Q_INVOKABLE void   beginLinear();
  Q_INVOKABLE void   setMaskEnabled(bool enabled);
  Q_INVOKABLE void   setMaskInvert(bool invert);
  /**
   * @brief Queue one Invert toggle of (@p node_id, @p mask_id).
   *
   * Empty ids target the selected Mask. The owner reads the stored `invert` value when it runs
   * the command, so two clicks always return the Mask to its first coverage. Any Mask source
   * kind is accepted, AI Masks included.
   */
  Q_INVOKABLE void   toggleMaskInvert(const QString& node_id, const QString& mask_id);
  Q_INVOKABLE void   setMaskName(const QString& name);
  Q_INVOKABLE void   beginMaskOpacity();
  Q_INVOKABLE void   updateMaskOpacity(qreal percent);
  Q_INVOKABLE bool   beginMaskNudge();
  Q_INVOKABLE void   nudgeMaskBy(qreal dx_px, qreal dy_px);
  Q_INVOKABLE void   cancel();
  /**
   * @brief Cancel the open canvas pointer sequence and keep the armed tool.
   *
   * Use for grab loss / handler cancellation. Does not enqueue CancelMode.
   * No-op when no pointer sequence is open or the open op is a panel control.
   */
  Q_INVOKABLE void   cancelOpenPointerInput();
  Q_INVOKABLE void   hideBody();
  Q_INVOKABLE void   finishBody();
  Q_INVOKABLE void   selectMask(const QString& node_id, const QString& mask_id);
  Q_INVOKABLE void   removeMask(const QString& node_id, const QString& mask_id);
  Q_INVOKABLE void   removeSelectedMask();
  Q_INVOKABLE void   deleteActiveMask();
  Q_INVOKABLE void   handleHover(qreal x, qreal y);
  Q_INVOKABLE void   beginInnerFeather();
  Q_INVOKABLE void   beginOuterFeather();
  Q_INVOKABLE void   updateInnerFeather(qreal percent);
  Q_INVOKABLE void   updateOuterFeather(qreal percent);
  Q_INVOKABLE void   beginMajorRadius();
  Q_INVOKABLE void   updateMajorRadius(qreal percent);
  Q_INVOKABLE void   beginMinorRadius();
  Q_INVOKABLE void   updateMinorRadius(qreal percent);
  Q_INVOKABLE void   beginRotation();
  Q_INVOKABLE void   updateRotation(qreal degrees);
  Q_INVOKABLE void   beginTransition();
  Q_INVOKABLE void   updateTransition(qreal percent);
  Q_INVOKABLE void   finishAnalyticControl();
  Q_INVOKABLE bool   handlePress(qreal x, qreal y, int button);
  Q_INVOKABLE bool   handleMove(qreal x, qreal y, int buttons);
  Q_INVOKABLE bool   handleRelease(qreal x, qreal y, int button);

  void               OnImageClosed();
  /**
   * @brief Copy owner selection/source after consume, Undo, or session rebind.
   *
   * Open pointer/numeric edits keep GUI-predicted overlay fields. Empty
   * selection after Undo selects the restored Mask only when it is present.
   */
  void               SyncFromSession();

 signals:
  void maskCreationChanged();
  void maskRangeChanged();

 private:
  enum class RangeKind : std::uint8_t { None, Luminance, Hue };

  /// One Mask of one Color Grade; empty when no Mask is the range target.
  struct RangeTarget {
    NodeId             grade_id;
    MaskId             mask_id;

    [[nodiscard]] auto Empty() const -> bool { return grade_id.Empty() || mask_id.Empty(); }
    auto               operator==(const RangeTarget&) const -> bool = default;
  };

  /// Range values of the range target as last published to QML; full ranges are 0..1 and
  /// 0..360. The softness and saturation threshold values are kept for the next restricted write
  /// (D6).
  struct RangeView {
    RangeTarget target;
    bool        present                                    = false;
    float       luminance_low                              = 0.0f;
    float       luminance_high                             = 1.0f;
    float       luminance_softness                         = LuminanceRangeModel{}.softness;
    float       hue_start                                  = 0.0f;
    float       hue_end                                    = 360.0f;
    float       hue_softness                               = ColorRangeModel{}.softness;
    float       hue_min_chroma                             = ColorRangeModel{}.min_chroma;

    auto        operator==(const RangeView&) const -> bool = default;
  };

  [[nodiscard]] static auto RangeKindFromString(const QString& kind) -> RangeKind;
  [[nodiscard]] static auto RangeFieldKey(RangeKind kind) -> std::string_view;
  [[nodiscard]] auto        RangeTargetNow() const -> RangeTarget;
  /// Read the range values of the target from the document. Retargets the toggle when the target
  /// changed. True when the published values changed.
  auto                      SyncRangeView() -> bool;
  /// The range JSON of @p kind for the handle values @p first and @p second (`null` when full).
  [[nodiscard]] auto        RangeJson(RangeKind kind, float first, float second) -> nlohmann::json;
  void                      ShowRangeCoverage(const RangeTarget& target);
  void                      HideRangeCoverage();
  void                      ResetRangeEdit();
  /// Finish an open key or wheel step burst with its one commit; no-op otherwise.
  void                      SettleRangeSteps();
  /**
   * Open the edit that a value input of @p kind writes into: a burst of steps (settled by the
   * timer), or, with @p join_pointer_edit, also the open drag of the same kind. Settles a burst
   * of another range or target first. False when a drag of another range is open or no edit
   * can start.
   */
  auto                      OpenRangeInput(RangeKind kind, bool join_pointer_edit) -> bool;
  /// Write the open range edit with handles @p first and @p second and the edit softness.
  void                      WriteRangeEdit(RangeKind kind, float first, float second);
  void               BeginTool(MaskSourceKind kind, const QString& tool_kind);
  void               ResetLocal();
  void               PublishOverlay();
  void               HideOverlay();
  /// Cancel the open op when the press-time mapping no longer matches.
  auto               CancelIfMappingChanged() -> bool;
  void               ApplyOwnerSource(const MaskId& mask_id, const MaskSource& source);
  void               SyncOwnerSelection();
  /// Store the selected Mask's Enabled/Invert/Opacity/Name; true when they changed.
  [[nodiscard]] auto UpdateMaskFieldView() -> bool;
  void               BeginAnalyticMove(AnalyticMaskHandle handle);
  void               UpdateFeatherPercent(AnalyticMaskHandle handle, qreal percent);
  void               UpdateRadialRadiusPercent(AnalyticMaskHandle handle, qreal percent);
  void               EnqueueAppendSample(const MaskCreationSample& sample);
  void               EnqueueMaskField(std::string_view field_key, nlohmann::json value);
  void               BeginMaskField(std::string_view field_key);
  [[nodiscard]] auto SelectedMask() const -> const MaskModel*;
  [[nodiscard]] auto ReferenceShorterEdgePx() const -> float;
  [[nodiscard]] auto CurrentGradeId() const -> NodeId;
  [[nodiscard]] auto CanAuthorMasks() const -> bool;
  [[nodiscard]] auto CanAuthorMasksFor(const NodeId& grade_id) const -> bool;
  [[nodiscard]] auto DocumentContainsMask(const MaskId& mask_id) const -> bool;
  [[nodiscard]] auto MakeSample(qreal x, qreal y, bool allow_outside) const
      -> std::optional<MaskCreationSample>;
  [[nodiscard]] auto MakeNormalizedSample(Vector2 normalized) const -> MaskCreationSample;
  [[nodiscard]] auto OverlayClip() const -> QRectF;
  [[nodiscard]] auto OverlayStyle() const -> MaskOverlayStyle;
  [[nodiscard]] auto Enqueue(EditorMaskCreationCommand command) -> bool;
  void               ConnectInteraction(editor_rhi::EditorInteractionController* interaction);

  QPointer<EditorSessionController>                 session_;
  QPointer<editor_rhi::EditorInteractionController> interaction_;
  QPointer<editor_rhi::EditorOverlayItem>           overlay_;
  QMetaObject::Connection                           view_change_connection_;
  /// overlayGeometryChanged → PublishOverlay. Mask chrome is item-space, so
  /// view churn (panel fold, window resize, zoom/pan) remaps it through this
  /// signal instead of relying on backend NotifyChange storms.
  QMetaObject::Connection                           overlay_geometry_connection_;
  /// Mapping inputs of the last PublishOverlay; compared against the live
  /// maskEditMappingIdentity so identical geometry never republishes.
  MaskEditMappingIdentity                           published_mapping_identity_{};
  /// Pins the published document snapshot backing the last SelectedMask()
  /// pointer so the MaskModel address stays valid between adapter calls.
  mutable std::shared_ptr<const alcedo::PipelineDocument> selected_mask_document_;
  QString                                           tool_kind_;
  QString                                           selected_mask_id_;
  NodeId                                            edit_node_id_;
  MaskSourceKind                                    source_kind_    = MaskSourceKind::Radial;
  EditMode                                          edit_mode_      = EditMode::Inactive;
  bool                                              open_           = false;
  /// Open op belongs to a panel control (slider/keyboard), not a canvas drag.
  bool                                              open_via_panel_ = false;
  MaskPointerIdentity                               pointer_{};
  Vector2                                           press_normalized_{};
  std::optional<MaskSource>                         overlay_source_;
  MaskOverlayDisplay                                overlay_display_{};
  AnalyticMaskHandle                                active_handle_    = AnalyticMaskHandle::None;
  MaskOverlayHandleId                               hovered_handle_   = MaskOverlayHandleId::None;
  std::uint64_t                                     next_sequence_id_ = 1;
  /// Mapping snapshot taken when the open pointer op started. A presented-frame
  /// or view change that alters it cancels the op before the next sample.
  std::optional<MaskEditMappingIdentity>            press_mapping_identity_;
  Vector2                                           nudge_base_normalized_{};
  Vector2                                           nudge_base_reference_{};
  Vector2                                           nudge_offset_{};

  /// Mask-level fields of the selected Mask as last published to QML. Field edits, Undo, and
  /// Redo do not change the owner source, so SyncFromSession compares these values instead.
  struct MaskFieldView {
    QString mask_id;
    bool    present = false;
    bool    enabled = true;
    bool    invert  = false;
    float   opacity = 1.0f;
    QString display_name;

    auto    operator==(const MaskFieldView&) const -> bool = default;
  };
  MaskFieldView published_mask_fields_{};

  RangeView     range_view_{};
  /// Range target of the open AI Mask edit (empty for a new candidate); used while
  /// ai_mask_edit_open_.
  bool          ai_mask_edit_open_ = false;
  RangeTarget   ai_mask_edit_target_{};
  /// The open range drag: its kind, target, and the handle, softness, and saturation threshold
  /// values written last.
  RangeKind     range_open_ = RangeKind::None;
  RangeTarget   range_edit_target_{};
  float         range_first_              = 0.0f;
  float         range_second_             = 0.0f;
  float         range_softness_           = 0.0f;
  float         range_min_chroma_         = kColorRangeDefaultMinChroma;
  /// The open range edit is a key or wheel step burst; range_step_timer_ settles it.
  bool          range_step_burst_         = false;
  QTimer*       range_step_timer_         = nullptr;
  /// The "Show selection" toggle.
  bool          range_coverage_requested_ = false;
  /// The Mask whose range coverage the adapter asked the session to show; empty when hidden.
  RangeTarget   shown_range_coverage_{};
};

}  // namespace alcedo::ui
