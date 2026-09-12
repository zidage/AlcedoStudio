//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <cstdint>
#include <optional>
#include <vector>

#include "app/editor_mask_creation_controller.hpp"
#include "edit/mask/mask_model.hpp"
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
  Q_PROPERTY(qreal innerFeatherPercent READ inner_feather_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal outerFeatherPercent READ outer_feather_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal majorRadiusPercent READ major_radius_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal minorRadiusPercent READ minor_radius_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal rotationDegrees READ rotation_degrees NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal transitionPercent READ transition_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal brushDiameter READ brush_diameter NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal brushDiameterPercent READ brush_diameter_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal brushStrengthPercent READ brush_strength_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(QString brushTool READ brush_tool_name NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal brushFeatherPercent READ brush_feather_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(bool maskEnabled READ mask_enabled NOTIFY maskCreationChanged)
  Q_PROPERTY(bool maskInvert READ mask_invert NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal maskOpacityPercent READ mask_opacity_percent NOTIFY maskCreationChanged)
  Q_PROPERTY(QString maskName READ mask_name NOTIFY maskCreationChanged)
  Q_PROPERTY(bool maskNudgeAvailable READ mask_nudge_available NOTIFY maskCreationChanged)

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
  [[nodiscard]] auto edit_node_id() const -> const NodeId& { return edit_node_id_; }
  [[nodiscard]] auto inner_feather_percent() const -> qreal;
  [[nodiscard]] auto outer_feather_percent() const -> qreal;
  [[nodiscard]] auto major_radius_percent() const -> qreal;
  [[nodiscard]] auto minor_radius_percent() const -> qreal;
  [[nodiscard]] auto rotation_degrees() const -> qreal;
  [[nodiscard]] auto transition_percent() const -> qreal;
  [[nodiscard]] auto brush_radius() const -> qreal { return static_cast<qreal>(brush_radius_); }
  /// Brush dab diameter in reference pixels (2 * radius).
  [[nodiscard]] auto brush_diameter() const -> qreal { return 2.0 * static_cast<qreal>(brush_radius_); }
  /**
   * @brief Brush dab diameter as a percent of the shorter full-reference edge.
   *
   * The panel slider edits diameter, not radius. Reference extents are read
   * from the bound interaction mapping. Zero when no extent is published.
   */
  [[nodiscard]] auto brush_diameter_percent() const -> qreal;
  [[nodiscard]] auto brush_strength_percent() const -> qreal {
    return static_cast<qreal>(brush_strength_) * 100.0;
  }
  [[nodiscard]] auto brush_tool_name() const -> QString;
  [[nodiscard]] auto brush_feather_percent() const -> qreal;
  [[nodiscard]] auto mask_enabled() const -> bool;
  [[nodiscard]] auto mask_invert() const -> bool;
  [[nodiscard]] auto mask_opacity_percent() const -> qreal;
  [[nodiscard]] auto mask_name() const -> QString;
  /**
   * @brief True when the selected Mask has a keyboard-movable position.
   *
   * Brush placement nudges require Move mode; Radial center and Linear origin
   * are always movable while selected.
   */
  [[nodiscard]] auto mask_nudge_available() const -> bool;

  Q_INVOKABLE void   bindInteractionItem(QObject* interaction);
  Q_INVOKABLE void   bindOverlayItem(QObject* overlay);
  Q_INVOKABLE void   beginRadial();
  Q_INVOKABLE void   beginLinear();
  Q_INVOKABLE void   beginBrush();
  Q_INVOKABLE void   setBrushTool(const QString& tool);
  Q_INVOKABLE void   setBrushRadius(qreal radius);
  /// Set the dab diameter as a percent of the shorter full-reference edge.
  Q_INVOKABLE void   setBrushDiameterPercent(qreal percent);
  Q_INVOKABLE void   setBrushStrengthPercent(qreal percent);
  Q_INVOKABLE void   setMaskEnabled(bool enabled);
  Q_INVOKABLE void   setMaskInvert(bool invert);
  Q_INVOKABLE void   setMaskName(const QString& name);
  Q_INVOKABLE void   beginMaskOpacity();
  Q_INVOKABLE void   updateMaskOpacity(qreal percent);
  Q_INVOKABLE void   beginBrushFeather();
  Q_INVOKABLE void   updateBrushFeatherPercent(qreal percent);
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

 private:
  void               BeginTool(MaskSourceKind kind, const QString& tool_kind);
  void               BeginBrushTool();
  [[nodiscard]] auto ResolveBrushResumeMask(const NodeId& grade_id) const -> MaskId;
  /// Item-space dab radius at the current mapping. Affine, so position-free.
  [[nodiscard]] auto BrushRadiusLogicalPx() const -> float;
  void               EnqueueBrushSettings(EditorMaskCreationCommand& command) const;
  void               ResetLocal();
  void               PublishOverlay();
  void               HideOverlay();
  /// Cancel the open op when the press-time mapping no longer matches.
  auto               CancelIfMappingChanged() -> bool;
  void               ApplyOwnerSource(const MaskId& mask_id, const MaskSource& source);
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
  Vector2                                           press_reference_pixels_{};
  Vector2                                           brush_placement_before_{};
  std::optional<MaskSource>                         overlay_source_;
  MaskOverlayDisplay                                overlay_display_{};
  AnalyticMaskHandle                                active_handle_    = AnalyticMaskHandle::None;
  MaskOverlayHandleId                               hovered_handle_   = MaskOverlayHandleId::None;
  std::uint64_t                                     next_sequence_id_ = 1;
  EditorBrushTool                                   brush_tool_       = EditorBrushTool::Idle;
  float                                             brush_radius_     = 0.0f;
  float                                             brush_strength_   = 1.0f;
  float                                             brush_hardness_   = 1.0f;
  std::vector<QPointF>                              brush_item_path_;
  /// Mapping snapshot taken when the open pointer op started. A presented-frame
  /// or view change that alters it cancels the op before the next sample.
  std::optional<MaskEditMappingIdentity>            press_mapping_identity_;
  /// Last hover point in item space; drives the armed-Brush cursor ring.
  QPointF                                           hover_item_{};
  bool                                              hover_valid_ = false;
  Vector2                                           nudge_base_normalized_{};
  Vector2                                           nudge_base_reference_{};
  Vector2                                           nudge_offset_{};
};

}  // namespace alcedo::ui
