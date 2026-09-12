//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_mask_creation_adapter.hpp"

#include <QLineF>
#include <QPointF>
#include <Qt>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <variant>

#include "edit/geometry/render_request.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/analytic_mask_edit.hpp"
#include "edit/mask/brush_placement.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_layout.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_overlay_item.hpp"

namespace alcedo::ui {
namespace {

constexpr float    kPi = 3.14159265358979323846f;

[[nodiscard]] auto AnalyticHandleFromOverlay(MaskOverlayHandleId id) -> AnalyticMaskHandle {
  switch (id) {
    case MaskOverlayHandleId::RadialCenter:
      return AnalyticMaskHandle::RadialCenter;
    case MaskOverlayHandleId::RadialMajor:
      return AnalyticMaskHandle::RadialMajor;
    case MaskOverlayHandleId::RadialMinor:
      return AnalyticMaskHandle::RadialMinor;
    case MaskOverlayHandleId::RadialRotate:
      return AnalyticMaskHandle::RadialRotate;
    case MaskOverlayHandleId::RadialInnerFeather:
      return AnalyticMaskHandle::RadialInnerFeather;
    case MaskOverlayHandleId::RadialOuterFeather:
      return AnalyticMaskHandle::RadialOuterFeather;
    case MaskOverlayHandleId::LinearOrigin:
      return AnalyticMaskHandle::LinearOrigin;
    case MaskOverlayHandleId::LinearDirection:
      return AnalyticMaskHandle::LinearDirection;
    case MaskOverlayHandleId::LinearStartBoundary:
      return AnalyticMaskHandle::LinearStartBoundary;
    case MaskOverlayHandleId::LinearEndBoundary:
      return AnalyticMaskHandle::LinearEndBoundary;
    case MaskOverlayHandleId::BrushMove:
      return AnalyticMaskHandle::BrushMove;
    default:
      return AnalyticMaskHandle::None;
  }
}

[[nodiscard]] auto OverlayHandleFromAnalytic(AnalyticMaskHandle handle) -> MaskOverlayHandleId {
  switch (handle) {
    case AnalyticMaskHandle::RadialCenter:
      return MaskOverlayHandleId::RadialCenter;
    case AnalyticMaskHandle::RadialMajor:
      return MaskOverlayHandleId::RadialMajor;
    case AnalyticMaskHandle::RadialMinor:
      return MaskOverlayHandleId::RadialMinor;
    case AnalyticMaskHandle::RadialRotate:
      return MaskOverlayHandleId::RadialRotate;
    case AnalyticMaskHandle::RadialInnerFeather:
      return MaskOverlayHandleId::RadialInnerFeather;
    case AnalyticMaskHandle::RadialOuterFeather:
      return MaskOverlayHandleId::RadialOuterFeather;
    case AnalyticMaskHandle::LinearOrigin:
      return MaskOverlayHandleId::LinearOrigin;
    case AnalyticMaskHandle::LinearDirection:
      return MaskOverlayHandleId::LinearDirection;
    case AnalyticMaskHandle::LinearStartBoundary:
      return MaskOverlayHandleId::LinearStartBoundary;
    case AnalyticMaskHandle::LinearEndBoundary:
      return MaskOverlayHandleId::LinearEndBoundary;
    case AnalyticMaskHandle::BrushMove:
      return MaskOverlayHandleId::BrushMove;
    default:
      return MaskOverlayHandleId::None;
  }
}

[[nodiscard]] auto ToolKindFromSource(MaskSourceKind kind) -> QString {
  switch (kind) {
    case MaskSourceKind::Brush:
      return QStringLiteral("brush");
    case MaskSourceKind::Radial:
      return QStringLiteral("radial");
    case MaskSourceKind::LinearGradient:
      return QStringLiteral("linear");
  }
  return QStringLiteral("radial");
}

[[nodiscard]] auto MaskIdFromQString(const QString& value) -> MaskId {
  return MaskId{value.toStdString()};
}

[[nodiscard]] auto MaskIdToQString(const MaskId& id) -> QString {
  const auto view = id.Value();
  return QString::fromUtf8(view.data(), static_cast<int>(view.size()));
}

[[nodiscard]] auto IsAnalyticKind(MaskSourceKind kind) -> bool {
  return kind == MaskSourceKind::Radial || kind == MaskSourceKind::LinearGradient;
}

}  // namespace

EditorMaskCreationAdapter::EditorMaskCreationAdapter(EditorSessionController* session,
                                                     QObject*                 parent)
    : QObject(parent), session_(session) {}

EditorMaskCreationAdapter::~EditorMaskCreationAdapter() {
  if (view_change_connection_) {
    QObject::disconnect(view_change_connection_);
  }
  if (overlay_geometry_connection_) {
    QObject::disconnect(overlay_geometry_connection_);
  }
}

auto EditorMaskCreationAdapter::owns_left_button() const -> bool {
  if (edit_mode_ == EditMode::Creating) {
    return true;
  }
  if (edit_mode_ == EditMode::Editing && IsAnalyticKind(source_kind_)) {
    return true;
  }
  return source_kind_ == MaskSourceKind::Brush &&
         (brush_tool_ == EditorBrushTool::Paint || brush_tool_ == EditorBrushTool::Erase ||
          brush_tool_ == EditorBrushTool::Move);
}

auto EditorMaskCreationAdapter::body_visible() const -> bool { return active(); }

auto EditorMaskCreationAdapter::mask_controls_active() const -> bool { return active(); }

auto EditorMaskCreationAdapter::inner_feather_percent() const -> qreal {
  const auto* radial = overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr;
  return radial == nullptr ? 0.0 : static_cast<qreal>(radial->inner_feather) * 100.0;
}

auto EditorMaskCreationAdapter::outer_feather_percent() const -> qreal {
  const auto* radial = overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr;
  return radial == nullptr ? 0.0
                           : static_cast<qreal>(std::max(0.0f, radial->outer_feather)) * 100.0;
}

auto EditorMaskCreationAdapter::major_radius_percent() const -> qreal {
  const auto* radial = overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr;
  return radial == nullptr ? 0.0 : static_cast<qreal>(radial->major_radius) * 100.0;
}

auto EditorMaskCreationAdapter::minor_radius_percent() const -> qreal {
  const auto* radial = overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr;
  return radial == nullptr ? 0.0 : static_cast<qreal>(radial->minor_radius) * 100.0;
}

auto EditorMaskCreationAdapter::rotation_degrees() const -> qreal {
  if (const auto* radial =
          overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr) {
    return static_cast<qreal>(radial->rotation) * 180.0 / kPi;
  }
  if (const auto* linear =
          overlay_source_ ? std::get_if<LinearGradientMaskSource>(&*overlay_source_) : nullptr) {
    return static_cast<qreal>(std::atan2(linear->normal_y, linear->normal_x)) * 180.0 / kPi;
  }
  return 0.0;
}

auto EditorMaskCreationAdapter::transition_percent() const -> qreal {
  const auto* linear =
      overlay_source_ ? std::get_if<LinearGradientMaskSource>(&*overlay_source_) : nullptr;
  return linear == nullptr ? 0.0 : static_cast<qreal>(linear->transition_distance) * 100.0;
}

void EditorMaskCreationAdapter::bindInteractionItem(QObject* interaction) {
  auto* typed = qobject_cast<editor_rhi::EditorInteractionController*>(interaction);
  if (interaction_ == typed) {
    return;
  }
  if (view_change_connection_) {
    QObject::disconnect(view_change_connection_);
    view_change_connection_ = {};
  }
  if (overlay_geometry_connection_) {
    QObject::disconnect(overlay_geometry_connection_);
    overlay_geometry_connection_ = {};
  }
  interaction_ = typed;
  ConnectInteraction(typed);
  PublishOverlay();
}

void EditorMaskCreationAdapter::bindOverlayItem(QObject* overlay) {
  overlay_ = qobject_cast<editor_rhi::EditorOverlayItem*>(overlay);
  PublishOverlay();
}

void EditorMaskCreationAdapter::ConnectInteraction(
    editor_rhi::EditorInteractionController* interaction) {
  if (interaction == nullptr) {
    return;
  }
  view_change_connection_ = connect(
      interaction, &editor_rhi::EditorInteractionController::viewChangeReported, this, [this](int) {
        // Pan/zoom must not disarm the Mask tool. Cancel only the open canvas
        // sequence when the composed item→reference mapping actually changed.
        if (open_ && !open_via_panel_) {
          (void)CancelIfMappingChanged();
        }
      });
  overlay_geometry_connection_ = connect(
      interaction, &editor_rhi::EditorInteractionController::overlayGeometryChanged, this, [this] {
        // Mask chrome is item-space: a viewport/view change moves it even
        // though the owner Mask did not. Republish the display so handles and
        // contours track the letterbox without a backend NotifyChange.
        if (interaction_ == nullptr ||
            (edit_mode_ == EditMode::Inactive && !overlay_source_.has_value())) {
          return;
        }
        const auto identity = interaction_->maskEditMappingIdentity();
        if (identity != published_mapping_identity_) {
          PublishOverlay();
        }
      });
}

void EditorMaskCreationAdapter::beginRadial() {
  BeginTool(MaskSourceKind::Radial, QStringLiteral("radial"));
}

void EditorMaskCreationAdapter::beginLinear() {
  BeginTool(MaskSourceKind::LinearGradient, QStringLiteral("linear"));
}

void EditorMaskCreationAdapter::beginBrush() { BeginBrushTool(); }

auto EditorMaskCreationAdapter::brush_tool_name() const -> QString {
  switch (brush_tool_) {
    case EditorBrushTool::Paint:
      return QStringLiteral("paint");
    case EditorBrushTool::Erase:
      return QStringLiteral("erase");
    case EditorBrushTool::Move:
      return QStringLiteral("move");
    case EditorBrushTool::Idle:
      return QString();
  }
  return QString();
}

void EditorMaskCreationAdapter::EnqueueBrushSettings(EditorMaskCreationCommand& command) const {
  command.brush_tool     = brush_tool_;
  command.brush_radius   = brush_radius_;
  command.brush_strength = brush_strength_;
  command.brush_hardness = brush_hardness_;
}

auto EditorMaskCreationAdapter::ResolveBrushResumeMask(const NodeId& grade_id) const -> MaskId {
  if (session_ == nullptr || grade_id.Empty()) {
    return {};
  }
  const auto* document = session_->pipeline_document();
  if (document == nullptr) {
    return {};
  }
  const auto* grade =
      dynamic_cast<const ColorGradeNodeModel*>(document->Graph().FindNode(grade_id));
  if (grade == nullptr) {
    return {};
  }
  std::vector<MaskId> brushes;
  for (std::size_t i = 0; i < grade->MaskCount(); ++i) {
    if (GetMaskSourceKind(grade->MaskAt(i).source) == MaskSourceKind::Brush) {
      brushes.push_back(grade->MaskAt(i).id);
    }
  }
  if (brushes.empty()) {
    return {};
  }
  if (brushes.size() == 1) {
    return brushes.front();
  }
  const auto selected = MaskIdFromQString(selected_mask_id_);
  for (const auto& id : brushes) {
    if (id == selected) {
      return selected;
    }
  }
  return {};
}

auto EditorMaskCreationAdapter::BrushRadiusLogicalPx() const -> float {
  if (interaction_ == nullptr || brush_radius_ <= 0.0f) {
    return kMaskOverlayHandleRadiusLogicalPx * 2.0f;
  }
  const auto mapping = interaction_->maskEditViewMapping();
  // Reference->item is affine, so the dab radius is position-independent.
  const auto center = MaskEditGeometry::MapReferenceToItem(mapping, Vector2{0.0f, 0.0f});
  const auto edge =
      MaskEditGeometry::MapReferenceToItem(mapping, Vector2{brush_radius_, 0.0f});
  if (!center || !edge) {
    return kMaskOverlayHandleRadiusLogicalPx * 2.0f;
  }
  return static_cast<float>(QLineF(*center, *edge).length());
}

void EditorMaskCreationAdapter::BeginBrushTool() {
  const NodeId grade = CurrentGradeId();
  if (!CanAuthorMasksFor(grade)) {
    return;
  }
  if (open_) {
    EditorMaskCreationCommand cancel;
    cancel.kind    = EditorMaskCreationCommandKind::Cancel;
    cancel.node_id = edit_node_id_;
    cancel.mask_id = MaskIdFromQString(selected_mask_id_);
    (void)Enqueue(cancel);
  }
  if (interaction_ != nullptr) {
    const auto extent = interaction_->maskEditViewMapping().geometry.full_reference_extent;
    if (brush_radius_ <= 0.0f && !extent.Empty()) {
      brush_radius_ = DefaultBrushRadiusReferencePixels(extent);
    }
  }
  const auto                resume = ResolveBrushResumeMask(grade);
  EditorMaskCreationCommand command;
  command.kind        = EditorMaskCreationCommandKind::BeginCreation;
  command.source_kind = MaskSourceKind::Brush;
  command.node_id     = grade;
  command.mask_id     = resume;
  command.brush_tool  = EditorBrushTool::Paint;
  EnqueueBrushSettings(command);
  if (command.node_id.Empty() || !Enqueue(command)) {
    return;
  }
  source_kind_      = MaskSourceKind::Brush;
  tool_kind_        = QStringLiteral("brush");
  edit_node_id_     = grade;
  edit_mode_        = EditMode::Creating;
  brush_tool_       = EditorBrushTool::Paint;
  selected_mask_id_ = MaskIdToQString(resume);
  open_             = false;
  open_via_panel_   = false;
  brush_item_path_.clear();
  overlay_source_.reset();
  overlay_display_ = {};
  hovered_handle_  = MaskOverlayHandleId::None;
  active_handle_   = AnalyticMaskHandle::None;
  if (!resume.Empty() && session_ != nullptr) {
    const auto* document = session_->pipeline_document();
    const auto* grade_model =
        document == nullptr
            ? nullptr
            : dynamic_cast<const ColorGradeNodeModel*>(document->Graph().FindNode(grade));
    const auto* mask = grade_model == nullptr ? nullptr : grade_model->FindMask(resume);
    if (mask != nullptr) {
      overlay_source_ = mask->source;
    }
  }
  HideOverlay();
  if (overlay_source_.has_value() || hover_valid_) {
    PublishOverlay();
  }
  emit maskCreationChanged();
}

void EditorMaskCreationAdapter::setBrushTool(const QString& tool) {
  EditorBrushTool next = EditorBrushTool::Idle;
  if (tool == QLatin1String("paint")) {
    next = EditorBrushTool::Paint;
  } else if (tool == QLatin1String("erase")) {
    next = EditorBrushTool::Erase;
  } else if (tool == QLatin1String("move")) {
    next = EditorBrushTool::Move;
  } else {
    return;
  }
  if (!CanAuthorMasks() || source_kind_ != MaskSourceKind::Brush) {
    return;
  }
  EditorMaskCreationCommand command;
  command.kind       = EditorMaskCreationCommandKind::SetBrushTool;
  command.node_id    = edit_node_id_;
  command.mask_id    = MaskIdFromQString(selected_mask_id_);
  command.brush_tool = next;
  if (!Enqueue(command)) {
    return;
  }
  brush_tool_ = next;
  if (next == EditorBrushTool::Move) {
    edit_mode_ = EditMode::Editing;
  } else {
    edit_mode_ = EditMode::Creating;
  }
  PublishOverlay();
  emit maskCreationChanged();
}

void EditorMaskCreationAdapter::setBrushRadius(qreal radius) {
  if (!(radius > 0.0)) {
    return;
  }
  brush_radius_ = static_cast<float>(radius);
  EditorMaskCreationCommand command;
  command.kind    = EditorMaskCreationCommandKind::SetBrushStrokeParameters;
  command.node_id = edit_node_id_;
  command.mask_id = MaskIdFromQString(selected_mask_id_);
  EnqueueBrushSettings(command);
  (void)Enqueue(command);
  PublishOverlay();
  emit maskCreationChanged();
}

void EditorMaskCreationAdapter::setBrushStrengthPercent(qreal percent) {
  brush_strength_ = std::clamp(static_cast<float>(percent) / 100.0f, 0.0f, 1.0f);
  EditorMaskCreationCommand command;
  command.kind    = EditorMaskCreationCommandKind::SetBrushStrokeParameters;
  command.node_id = edit_node_id_;
  command.mask_id = MaskIdFromQString(selected_mask_id_);
  EnqueueBrushSettings(command);
  (void)Enqueue(command);
  emit maskCreationChanged();
}

auto EditorMaskCreationAdapter::SelectedMask() const -> const MaskModel* {
  if (session_ == nullptr || selected_mask_id_.isEmpty()) {
    return nullptr;
  }
  const auto* document = session_->pipeline_document();
  if (document == nullptr) {
    return nullptr;
  }
  const auto* grade =
      dynamic_cast<const ColorGradeNodeModel*>(document->Graph().FindNode(edit_node_id_));
  if (grade == nullptr) {
    return nullptr;
  }
  return grade->FindMask(MaskIdFromQString(selected_mask_id_));
}

auto EditorMaskCreationAdapter::ReferenceShorterEdgePx() const -> float {
  if (interaction_ == nullptr) {
    return 0.0f;
  }
  const auto& extent = interaction_->maskEditViewMapping().geometry.full_reference_extent;
  if (extent.Empty()) {
    return 0.0f;
  }
  return static_cast<float>(std::min(extent.width, extent.height));
}

auto EditorMaskCreationAdapter::brush_diameter_percent() const -> qreal {
  const float edge = ReferenceShorterEdgePx();
  if (edge <= 0.0f) {
    return 0.0;
  }
  return 2.0 * static_cast<qreal>(brush_radius_) * 100.0 / static_cast<qreal>(edge);
}

auto EditorMaskCreationAdapter::brush_feather_percent() const -> qreal {
  const auto* mask  = SelectedMask();
  const auto* brush = mask == nullptr ? nullptr : std::get_if<BrushMaskSource>(&mask->source);
  const float edge  = ReferenceShorterEdgePx();
  if (brush == nullptr || edge <= 0.0f) {
    return 0.0;
  }
  return static_cast<qreal>(brush->feather_radius) * 100.0 / static_cast<qreal>(edge);
}

auto EditorMaskCreationAdapter::mask_enabled() const -> bool {
  const auto* mask = SelectedMask();
  return mask == nullptr || mask->enabled;
}

auto EditorMaskCreationAdapter::mask_invert() const -> bool {
  const auto* mask = SelectedMask();
  return mask != nullptr && mask->invert;
}

auto EditorMaskCreationAdapter::mask_opacity_percent() const -> qreal {
  const auto* mask = SelectedMask();
  return mask == nullptr ? 100.0 : static_cast<qreal>(mask->opacity) * 100.0;
}

auto EditorMaskCreationAdapter::mask_name() const -> QString {
  const auto* mask = SelectedMask();
  return mask == nullptr ? QString{} : QString::fromStdString(mask->display_name);
}

auto EditorMaskCreationAdapter::mask_nudge_available() const -> bool {
  if (!overlay_source_.has_value() || selected_mask_id_.isEmpty()) {
    return false;
  }
  if (source_kind_ == MaskSourceKind::Brush) {
    return brush_tool_ == EditorBrushTool::Move;
  }
  return IsAnalyticKind(source_kind_);
}

void EditorMaskCreationAdapter::setBrushDiameterPercent(qreal percent) {
  const float edge = ReferenceShorterEdgePx();
  if (edge <= 0.0f || !(percent > 0.0)) {
    return;
  }
  setBrushRadius(static_cast<qreal>(percent) * 0.005 * static_cast<qreal>(edge));
}

void EditorMaskCreationAdapter::EnqueueMaskField(std::string_view field_key, nlohmann::json value) {
  if (selected_mask_id_.isEmpty() || !CanAuthorMasks()) {
    return;
  }
  EditorMaskCreationCommand command;
  command.kind        = EditorMaskCreationCommandKind::SetMaskField;
  command.node_id     = edit_node_id_;
  command.mask_id     = MaskIdFromQString(selected_mask_id_);
  command.field_key   = std::string{field_key};
  command.field_value = std::move(value);
  (void)Enqueue(command);
}

void EditorMaskCreationAdapter::BeginMaskField(std::string_view field_key) {
  if (selected_mask_id_.isEmpty() || !CanAuthorMasks() || open_) {
    return;
  }
  EditorMaskCreationCommand command;
  command.kind      = EditorMaskCreationCommandKind::BeginMaskField;
  command.node_id   = edit_node_id_;
  command.mask_id   = MaskIdFromQString(selected_mask_id_);
  command.field_key = std::string{field_key};
  if (!Enqueue(command)) {
    return;
  }
  active_handle_  = AnalyticMaskHandle::None;
  open_           = true;
  open_via_panel_ = true;
  edit_mode_      = EditMode::Editing;
  emit maskCreationChanged();
}

void EditorMaskCreationAdapter::setMaskEnabled(bool enabled) {
  EnqueueMaskField(kMaskFieldEnabled, enabled);
}

void EditorMaskCreationAdapter::setMaskInvert(bool invert) {
  EnqueueMaskField(kMaskFieldInvert, invert);
}

void EditorMaskCreationAdapter::setMaskName(const QString& name) {
  const QString trimmed = name.trimmed();
  if (trimmed.isEmpty() || trimmed == mask_name()) {
    return;
  }
  EnqueueMaskField(kMaskFieldDisplayName, trimmed.toStdString());
}

void EditorMaskCreationAdapter::beginMaskOpacity() { BeginMaskField(kMaskFieldOpacity); }

void EditorMaskCreationAdapter::updateMaskOpacity(qreal percent) {
  EnqueueMaskField(kMaskFieldOpacity, std::clamp(static_cast<float>(percent) / 100.0f, 0.0f, 1.0f));
}

void EditorMaskCreationAdapter::beginBrushFeather() { BeginMaskField(kMaskFieldBrushFeather); }

void EditorMaskCreationAdapter::updateBrushFeatherPercent(qreal percent) {
  const float edge = ReferenceShorterEdgePx();
  if (edge <= 0.0f || !(percent >= 0.0)) {
    return;
  }
  EnqueueMaskField(kMaskFieldBrushFeather, static_cast<float>(percent) * 0.01f * edge);
}

auto EditorMaskCreationAdapter::beginMaskNudge() -> bool {
  if (open_ || !CanAuthorMasks() || selected_mask_id_.isEmpty() || interaction_ == nullptr ||
      !overlay_source_.has_value()) {
    return false;
  }
  AnalyticMaskHandle handle     = AnalyticMaskHandle::None;
  Vector2            normalized = {0.5f, 0.5f};
  if (const auto* radial = std::get_if<RadialMaskSource>(&*overlay_source_)) {
    handle     = AnalyticMaskHandle::RadialCenter;
    normalized = {radial->center_x, radial->center_y};
  } else if (const auto* linear = std::get_if<LinearGradientMaskSource>(&*overlay_source_)) {
    handle     = AnalyticMaskHandle::LinearOrigin;
    normalized = {linear->origin_x, linear->origin_y};
  } else if (const auto* brush = std::get_if<BrushMaskSource>(&*overlay_source_)) {
    if (brush_tool_ != EditorBrushTool::Move) {
      return false;
    }
    handle                  = AnalyticMaskHandle::BrushMove;
    brush_placement_before_ = brush->placement_translation;
  } else {
    return false;
  }
  const auto& extent = interaction_->maskEditViewMapping().geometry.full_reference_extent;
  if (extent.Empty()) {
    return false;
  }
  const auto reference    = MaskEditGeometry::ReferencePixelsFromNormalized(normalized, extent);
  pointer_.device_id      = 2;
  pointer_.point_id       = 1;
  pointer_.sequence_id    = next_sequence_id_++;
  nudge_base_normalized_  = normalized;
  nudge_base_reference_   = reference;
  nudge_offset_           = {};
  press_normalized_       = normalized;
  press_reference_pixels_ = reference;

  MaskCreationSample sample;
  sample.normalized        = normalized;
  sample.reference_pixels  = reference;
  sample.inside_photograph = true;

  EditorMaskCreationCommand select;
  select.kind    = EditorMaskCreationCommandKind::SelectMask;
  select.node_id = edit_node_id_;
  select.mask_id = MaskIdFromQString(selected_mask_id_);
  (void)Enqueue(select);

  EditorMaskCreationCommand move;
  move.kind     = EditorMaskCreationCommandKind::BeginMove;
  move.handle   = handle;
  move.sample   = sample;
  move.identity = pointer_;
  move.node_id  = edit_node_id_;
  move.mask_id  = MaskIdFromQString(selected_mask_id_);
  if (handle == AnalyticMaskHandle::BrushMove) {
    move.brush_tool = EditorBrushTool::Move;
  }
  if (!Enqueue(move)) {
    return false;
  }
  active_handle_  = handle;
  open_           = true;
  open_via_panel_ = true;
  edit_mode_      = EditMode::Editing;
  emit maskCreationChanged();
  return true;
}

void EditorMaskCreationAdapter::nudgeMaskBy(qreal dx_px, qreal dy_px) {
  if (!open_ || !open_via_panel_ || active_handle_ == AnalyticMaskHandle::None ||
      interaction_ == nullptr) {
    return;
  }
  const auto& extent = interaction_->maskEditViewMapping().geometry.full_reference_extent;
  if (extent.Empty()) {
    return;
  }
  nudge_offset_.x += static_cast<float>(dx_px);
  nudge_offset_.y += static_cast<float>(dy_px);
  MaskCreationSample sample;
  sample.normalized =
      Vector2{nudge_base_normalized_.x + nudge_offset_.x / static_cast<float>(extent.width),
              nudge_base_normalized_.y + nudge_offset_.y / static_cast<float>(extent.height)};
  sample.reference_pixels =
      Vector2{nudge_base_reference_.x + nudge_offset_.x, nudge_base_reference_.y + nudge_offset_.y};
  sample.inside_photograph = true;
  EnqueueAppendSample(sample);
}

void EditorMaskCreationAdapter::BeginTool(MaskSourceKind kind, const QString& tool_kind) {
  const NodeId grade = CurrentGradeId();
  if (!CanAuthorMasksFor(grade)) {
    return;
  }
  if (open_) {
    EditorMaskCreationCommand cancel;
    cancel.kind    = EditorMaskCreationCommandKind::Cancel;
    cancel.node_id = edit_node_id_;
    cancel.mask_id = MaskIdFromQString(selected_mask_id_);
    (void)Enqueue(cancel);
  }
  EditorMaskCreationCommand command;
  command.kind        = EditorMaskCreationCommandKind::BeginCreation;
  command.source_kind = kind;
  command.node_id     = grade;
  if (command.node_id.Empty() || !Enqueue(command)) {
    return;
  }
  source_kind_  = kind;
  tool_kind_    = tool_kind;
  edit_node_id_ = grade;
  edit_mode_    = EditMode::Creating;
  selected_mask_id_.clear();
  open_           = false;
  open_via_panel_ = false;
  brush_tool_     = EditorBrushTool::Idle;
  brush_item_path_.clear();
  overlay_source_.reset();
  overlay_display_ = {};
  hovered_handle_  = MaskOverlayHandleId::None;
  active_handle_   = AnalyticMaskHandle::None;
  published_mapping_identity_ = {};
  HideOverlay();
  emit maskCreationChanged();
}

void EditorMaskCreationAdapter::cancel() {
  if (!active()) {
    return;
  }
  EditorMaskCreationCommand command;
  command.kind    = EditorMaskCreationCommandKind::CancelMode;
  command.node_id = edit_node_id_;
  command.mask_id = MaskIdFromQString(selected_mask_id_);
  (void)Enqueue(command);
  ResetLocal();
}

void EditorMaskCreationAdapter::cancelOpenPointerInput() {
  if (!open_ || open_via_panel_) {
    return;
  }
  EditorMaskCreationCommand cancel;
  cancel.kind    = EditorMaskCreationCommandKind::Cancel;
  cancel.node_id = edit_node_id_;
  cancel.mask_id = MaskIdFromQString(selected_mask_id_);
  (void)Enqueue(cancel);
  open_                   = false;
  open_via_panel_         = false;
  active_handle_          = AnalyticMaskHandle::None;
  press_mapping_identity_ = std::nullopt;
  brush_item_path_.clear();
  PublishOverlay();
  emit maskCreationChanged();
}

void EditorMaskCreationAdapter::hideBody() { finishBody(); }

void EditorMaskCreationAdapter::finishBody() {
  if (!active()) {
    return;
  }
  EditorMaskCreationCommand finish;
  finish.kind     = EditorMaskCreationCommandKind::FinishMode;
  finish.node_id  = edit_node_id_;
  finish.mask_id  = MaskIdFromQString(selected_mask_id_);
  finish.identity = pointer_;
  (void)Enqueue(finish);
  ResetLocal();
}

void EditorMaskCreationAdapter::selectMask(const QString& node_id, const QString& mask_id) {
  if (mask_id.isEmpty()) {
    return;
  }
  NodeId grade{node_id.toStdString()};
  if (grade.Empty()) {
    grade = active() ? edit_node_id_ : CurrentGradeId();
  }
  if (!CanAuthorMasksFor(grade)) {
    return;
  }
  if (open_) {
    EditorMaskCreationCommand cancel;
    cancel.kind    = EditorMaskCreationCommandKind::Cancel;
    cancel.node_id = edit_node_id_;
    cancel.mask_id = MaskIdFromQString(selected_mask_id_);
    (void)Enqueue(cancel);
    open_ = false;
  }
  EditorMaskCreationCommand command;
  command.kind    = EditorMaskCreationCommandKind::SelectMask;
  command.node_id = grade;
  command.mask_id = MaskIdFromQString(mask_id);
  if (!Enqueue(command)) {
    return;
  }
  selected_mask_id_ = mask_id;
  edit_node_id_     = grade;
  edit_mode_        = EditMode::Editing;
  open_             = false;
  open_via_panel_   = false;
  emit maskCreationChanged();
}

void EditorMaskCreationAdapter::removeMask(const QString& node_id, const QString& mask_id) {
  if (mask_id.isEmpty()) {
    return;
  }
  NodeId grade{node_id.toStdString()};
  if (grade.Empty()) {
    grade = active() ? edit_node_id_ : CurrentGradeId();
  }
  if (!CanAuthorMasksFor(grade)) {
    return;
  }
  EditorMaskCreationCommand command;
  command.kind    = EditorMaskCreationCommandKind::RemoveMask;
  command.node_id = grade;
  command.mask_id = MaskIdFromQString(mask_id);
  if (!Enqueue(command)) {
    return;
  }
  if (selected_mask_id_ == mask_id) {
    selected_mask_id_.clear();
    overlay_source_.reset();
    overlay_display_ = {};
    HideOverlay();
    EditorMaskCreationCommand finish;
    finish.kind    = EditorMaskCreationCommandKind::FinishMode;
    finish.node_id = grade;
    (void)Enqueue(finish);
    ResetLocal();
    return;
  }
  emit maskCreationChanged();
}

void EditorMaskCreationAdapter::removeSelectedMask() {
  if (selected_mask_id_.isEmpty()) {
    return;
  }
  removeMask(QString{}, selected_mask_id_);
}

void EditorMaskCreationAdapter::deleteActiveMask() {
  if (!active()) {
    return;
  }
  if (selected_mask_id_.isEmpty() && session_ != nullptr) {
    selected_mask_id_ = MaskIdToQString(session_->mask_creation_mask_id());
  }
  if (selected_mask_id_.isEmpty()) {
    cancel();
    return;
  }
  removeMask(QString{}, selected_mask_id_);
}

void EditorMaskCreationAdapter::OnImageClosed() { ResetLocal(); }

void EditorMaskCreationAdapter::SyncFromSession() {
  if (session_ == nullptr) {
    return;
  }
  if (open_ || session_->mask_creation_commands_pending()) {
    return;
  }
  const auto owner_node  = session_->mask_creation_node_id();
  const auto owner_id    = session_->mask_creation_mask_id();
  const auto owner_state = session_->mask_creation_state();
  const auto source      = session_->mask_creation_source();
  if (!owner_id.Empty() && source.has_value()) {
    // Backend change notifications also arrive for events that cannot move
    // the owner mask. Re-applying an unchanged owner rebuilds the whole
    // overlay display and emits maskCreationChanged every time, so only
    // re-apply on a real owner/source change; pure view remapping is driven
    // by the overlayGeometryChanged connection in ConnectInteraction.
    const bool owner_changed = owner_node != edit_node_id_ ||
                               MaskIdToQString(owner_id) != selected_mask_id_ ||
                               !overlay_source_.has_value() || *overlay_source_ != *source;
    if (owner_changed) {
      edit_node_id_ = owner_node;
      ApplyOwnerSource(owner_id, *source);
    }
    return;
  }
  if (owner_state == EditorMaskCreationState::Inactive) {
    // The owner rejected or dropped the armed tool after this adapter queued
    // its arm; local state must not advertise a tool the owner will reject.
    if (active() || open_) {
      ResetLocal();
    }
    return;
  }
  if (selected_mask_id_.isEmpty()) {
    const auto restored = session_->mask_creation_last_removed_mask_id();
    if (!restored.Empty() && DocumentContainsMask(restored)) {
      selectMask(QString{}, MaskIdToQString(restored));
      return;
    }
  }
}

void EditorMaskCreationAdapter::ApplyOwnerSource(const MaskId& mask_id, const MaskSource& source) {
  overlay_source_   = source;
  source_kind_      = GetMaskSourceKind(source);
  tool_kind_        = ToolKindFromSource(source_kind_);
  selected_mask_id_ = MaskIdToQString(mask_id);
  edit_mode_        = EditMode::Editing;
  PublishOverlay();
  emit maskCreationChanged();
}

void EditorMaskCreationAdapter::ResetLocal() {
  const bool changed = active() || open_;
  tool_kind_.clear();
  selected_mask_id_.clear();
  edit_node_id_            = {};
  edit_mode_               = EditMode::Inactive;
  open_                    = false;
  open_via_panel_          = false;
  overlay_source_.reset();
  overlay_display_ = {};
  hovered_handle_          = MaskOverlayHandleId::None;
  active_handle_           = AnalyticMaskHandle::None;
  brush_tool_              = EditorBrushTool::Idle;
  brush_item_path_.clear();
  press_mapping_identity_  = std::nullopt;
  published_mapping_identity_ = {};
  hover_valid_             = false;
  HideOverlay();
  if (changed) {
    emit maskCreationChanged();
  }
}

auto EditorMaskCreationAdapter::CurrentGradeId() const -> NodeId {
  if (session_ == nullptr) {
    return {};
  }
  auto* nodes = session_->node_selection_source();
  if (nodes == nullptr || nodes->selected_node_kind() != QLatin1String("colorGrade")) {
    return {};
  }
  return nodes->selected_node_id();
}

auto EditorMaskCreationAdapter::CanAuthorMasks() const -> bool {
  return CanAuthorMasksFor(active() ? edit_node_id_ : CurrentGradeId());
}

auto EditorMaskCreationAdapter::CanAuthorMasksFor(const NodeId& grade_id) const -> bool {
  if (session_ == nullptr || !session_->can_edit() || grade_id.Empty()) {
    return false;
  }
  if (auto* nodes = session_->node_selection_source()) {
    for (const auto& node : nodes->ActiveNodes()) {
      if (node.node_id == grade_id && node.node_kind == EditorNodeKind::ColorGrade) {
        return true;
      }
    }
  }
  const auto* document = session_->pipeline_document();
  return document != nullptr &&
         dynamic_cast<const ColorGradeNodeModel*>(document->Graph().FindNode(grade_id)) != nullptr;
}

auto EditorMaskCreationAdapter::DocumentContainsMask(const MaskId& mask_id) const -> bool {
  if (session_ == nullptr || mask_id.Empty()) {
    return false;
  }
  const auto* document = session_->pipeline_document();
  if (document == nullptr) {
    return false;
  }
  const auto* node  = document->Graph().FindNode(edit_node_id_);
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(node);
  return grade != nullptr && grade->FindMask(mask_id) != nullptr;
}

auto EditorMaskCreationAdapter::MakeSample(qreal x, qreal y, bool allow_outside) const
    -> std::optional<MaskCreationSample> {
  if (interaction_ == nullptr) {
    return std::nullopt;
  }
  const auto mapped = interaction_->MapItemToMaskReference(x, y, allow_outside);
  if (!mapped) {
    return std::nullopt;
  }
  MaskCreationSample sample;
  sample.normalized        = mapped->normalized;
  sample.reference_pixels  = mapped->reference_pixels;
  sample.inside_photograph = mapped->inside_photograph;
  return sample;
}

auto EditorMaskCreationAdapter::MakeNormalizedSample(Vector2 normalized) const
    -> MaskCreationSample {
  MaskCreationSample sample;
  sample.normalized        = normalized;
  sample.inside_photograph = true;
  if (interaction_ != nullptr) {
    const auto mapping = interaction_->maskEditViewMapping();
    if (!mapping.geometry.full_reference_extent.Empty()) {
      sample.reference_pixels = MaskEditGeometry::ReferencePixelsFromNormalized(
          normalized, mapping.geometry.full_reference_extent);
    }
  }
  return sample;
}

auto EditorMaskCreationAdapter::OverlayClip() const -> QRectF {
  if (interaction_ == nullptr) {
    return {};
  }
  return QRectF(0.0, 0.0, interaction_->viewportWidth(), interaction_->viewportHeight());
}

auto EditorMaskCreationAdapter::OverlayStyle() const -> MaskOverlayStyle {
  if (overlay_ != nullptr) {
    return overlay_->maskOverlayStyle();
  }
  return DefaultMaskOverlayStyle();
}

auto EditorMaskCreationAdapter::Enqueue(EditorMaskCreationCommand command) -> bool {
  if (session_ == nullptr) {
    return false;
  }
  // Stamp the armed source kind on every dispatched command so the owner can
  // reject a stale queued dispatch whose kind no longer matches the armed or
  // selected Mask. BeginCreation carries its own target kind already.
  if (command.kind != EditorMaskCreationCommandKind::BeginCreation) {
    command.source_kind = source_kind_;
  }
  return session_->EnqueueMaskCreation(std::move(command));
}

void EditorMaskCreationAdapter::HideOverlay() {
  if (overlay_ == nullptr) {
    return;
  }
  overlay_->setMaskOverlayDisplay(MaskOverlayDisplay{});
}

void EditorMaskCreationAdapter::PublishOverlay() {
  if (overlay_ == nullptr || interaction_ == nullptr) {
    HideOverlay();
    return;
  }
  const auto         mapping = interaction_->maskEditViewMapping();
  published_mapping_identity_ = MaskEditGeometry::Identity(mapping);
  const auto         style   = OverlayStyle();
  const auto         clip    = OverlayClip();
  MaskOverlayDisplay display;
  const bool         brush_paint_tool =
      brush_tool_ == EditorBrushTool::Paint || brush_tool_ == EditorBrushTool::Erase;
  if (const auto* radial =
          overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr) {
    display = (creating() && open_)
                  ? MakeRadialCreatingOverlayDisplay(mapping, *radial, style, clip)
                  : MakeRadialExistingOverlayDisplay(mapping, *radial, style, clip);
  } else if (const auto* linear =
                 overlay_source_ ? std::get_if<LinearGradientMaskSource>(&*overlay_source_)
                                 : nullptr) {
    display = (creating() && open_)
                  ? MakeLinearCreatingOverlayDisplay(mapping, *linear, style, clip)
                  : MakeLinearExistingOverlayDisplay(mapping, *linear, style, clip);
  } else if (source_kind_ == MaskSourceKind::Brush) {
    const auto* brush =
        overlay_source_ ? std::get_if<BrushMaskSource>(&*overlay_source_) : nullptr;
    if (open_ && !open_via_panel_ && brush_paint_tool) {
      // Open stroke: temporary path through canonical samples plus the live
      // cursor. The cursor follows the last sampled item point so a fast drag
      // keeps the ring under the pen even between hover callbacks.
      const QPointF cursor =
          !brush_item_path_.empty() ? brush_item_path_.back() : hover_item_;
      display = MakeBrushCreatingOverlayDisplay(brush_item_path_, cursor, BrushRadiusLogicalPx(),
                                                clip, brush_tool_ == EditorBrushTool::Erase);
    } else if (brush_paint_tool) {
      // Armed Paint/Erase with no open stroke: a hollow (dashed for Erase)
      // cursor ring follows the pointer; it is not a handle.
      display = MakeBrushCreatingOverlayDisplay(
          {}, hover_item_, BrushRadiusLogicalPx(), clip,
          brush_tool_ == EditorBrushTool::Erase);
      if (!hover_valid_) {
        display.mode           = MaskOverlayMode::Hidden;
        display.cursor_visible = false;
      }
    } else if (brush != nullptr) {
      display = MakeBrushMoveOverlayDisplay(mapping, *brush, clip);
    }
  }
  if (display.mode == MaskOverlayMode::Hidden || display.handles.empty()) {
    display.hovered_handle = MaskOverlayHandleId::None;
  } else {
    display.hovered_handle = hovered_handle_;
  }
  display.active_handle = OverlayHandleFromAnalytic(active_handle_);
  overlay_display_      = display;
  overlay_->setMaskOverlayDisplay(std::move(display));
}

void EditorMaskCreationAdapter::handleHover(qreal x, qreal y) {
  if (!owns_left_button()) {
    hover_valid_ = false;
    return;
  }
  hover_item_  = QPointF(x, y);
  hover_valid_ = true;
  // An armed Brush paint/erase cursor follows the pointer even when no handles
  // exist (e.g. before the first stroke creates the Mask).
  const bool brush_cursor = source_kind_ == MaskSourceKind::Brush &&
                            (brush_tool_ == EditorBrushTool::Paint ||
                             brush_tool_ == EditorBrushTool::Erase);
  if (overlay_display_.mode == MaskOverlayMode::Hidden && !brush_cursor) {
    return;
  }
  const auto hit = HitTestMaskOverlayHandle(overlay_display_, QPointF(x, y),
                                            OverlayStyle().hit_radius_logical_px);
  if (hit == hovered_handle_ && !brush_cursor) {
    return;
  }
  hovered_handle_ = hit;
  PublishOverlay();
}

void EditorMaskCreationAdapter::BeginAnalyticMove(AnalyticMaskHandle handle) {
  if (!overlay_source_.has_value() || selected_mask_id_.isEmpty() || !CanAuthorMasks()) {
    return;
  }
  Vector2 normalized;
  if (const auto* radial = std::get_if<RadialMaskSource>(&*overlay_source_)) {
    float rho   = 1.0f;
    float theta = 0.0f;
    if (handle == AnalyticMaskHandle::RadialMinor) {
      theta = kPi * 0.5f;
    } else if (handle == AnalyticMaskHandle::RadialInnerFeather) {
      rho = std::max(0.0f, 1.0f - radial->inner_feather);
    } else if (handle == AnalyticMaskHandle::RadialOuterFeather) {
      rho = 1.0f + std::max(0.0f, radial->outer_feather);
    }
    normalized = RadialNormalizedPoint(*radial, rho, theta);
  } else if (const auto* linear = std::get_if<LinearGradientMaskSource>(&*overlay_source_)) {
    if (handle != AnalyticMaskHandle::LinearDirection &&
        handle != AnalyticMaskHandle::LinearStartBoundary &&
        handle != AnalyticMaskHandle::LinearEndBoundary) {
      return;
    }
    const float length = std::hypot(linear->normal_x, linear->normal_y);
    if (length <= kAnalyticMaskEpsilon) {
      return;
    }
    const float distance = handle == AnalyticMaskHandle::LinearDirection
                               ? std::max(0.1f, linear->transition_distance)
                               : 0.5f * linear->transition_distance;
    normalized           = Vector2{linear->origin_x + distance * linear->normal_x / length,
                         linear->origin_y + distance * linear->normal_y / length};
  } else {
    return;
  }
  const auto sample    = MakeNormalizedSample(normalized);
  pointer_.device_id   = 2;
  pointer_.point_id    = 1;
  pointer_.sequence_id = next_sequence_id_++;
  EditorMaskCreationCommand select;
  select.kind    = EditorMaskCreationCommandKind::SelectMask;
  select.node_id = edit_node_id_;
  select.mask_id = MaskIdFromQString(selected_mask_id_);
  (void)Enqueue(select);
  EditorMaskCreationCommand move;
  move.kind     = EditorMaskCreationCommandKind::BeginMove;
  move.handle   = handle;
  move.sample   = sample;
  move.identity = pointer_;
  move.node_id  = edit_node_id_;
  move.mask_id  = MaskIdFromQString(selected_mask_id_);
  if (!Enqueue(move)) {
    return;
  }
  active_handle_  = handle;
  open_           = true;
  open_via_panel_ = true;
  edit_mode_      = EditMode::Editing;
  emit maskCreationChanged();
}

void EditorMaskCreationAdapter::beginInnerFeather() {
  BeginAnalyticMove(AnalyticMaskHandle::RadialInnerFeather);
}

void EditorMaskCreationAdapter::beginOuterFeather() {
  BeginAnalyticMove(AnalyticMaskHandle::RadialOuterFeather);
}

void EditorMaskCreationAdapter::UpdateFeatherPercent(AnalyticMaskHandle handle, qreal percent) {
  const auto* radial = overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr;
  if (radial == nullptr || !open_) {
    return;
  }
  const float clamped = handle == AnalyticMaskHandle::RadialInnerFeather
                            ? std::clamp(static_cast<float>(percent) / 100.0f, 0.0f, 1.0f)
                            : std::max(0.0f, static_cast<float>(percent) / 100.0f);
  const float rho     = handle == AnalyticMaskHandle::RadialInnerFeather
                            ? std::max(0.0f, 1.0f - clamped)
                            : 1.0f + clamped;
  EnqueueAppendSample(MakeNormalizedSample(RadialNormalizedPoint(*radial, rho, 0.0f)));
}

void EditorMaskCreationAdapter::updateInnerFeather(qreal percent) {
  UpdateFeatherPercent(AnalyticMaskHandle::RadialInnerFeather, percent);
}

void EditorMaskCreationAdapter::updateOuterFeather(qreal percent) {
  UpdateFeatherPercent(AnalyticMaskHandle::RadialOuterFeather, percent);
}

void EditorMaskCreationAdapter::beginMajorRadius() {
  BeginAnalyticMove(AnalyticMaskHandle::RadialMajor);
}

void EditorMaskCreationAdapter::beginMinorRadius() {
  BeginAnalyticMove(AnalyticMaskHandle::RadialMinor);
}

void EditorMaskCreationAdapter::UpdateRadialRadiusPercent(AnalyticMaskHandle handle,
                                                          qreal              percent) {
  const auto* radial = overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr;
  if (radial == nullptr || !open_) {
    return;
  }
  RadialMaskSource next = *radial;
  const float radius = std::max(kAnalyticCreationMinRadius, static_cast<float>(percent) / 100.0f);
  if (handle == AnalyticMaskHandle::RadialMajor) {
    next.major_radius = radius;
    EnqueueAppendSample(MakeNormalizedSample(RadialNormalizedPoint(next, 1.0f, 0.0f)));
  } else if (handle == AnalyticMaskHandle::RadialMinor) {
    next.minor_radius = radius;
    EnqueueAppendSample(MakeNormalizedSample(RadialNormalizedPoint(next, 1.0f, kPi * 0.5f)));
  }
}

void EditorMaskCreationAdapter::updateMajorRadius(qreal percent) {
  UpdateRadialRadiusPercent(AnalyticMaskHandle::RadialMajor, percent);
}

void EditorMaskCreationAdapter::updateMinorRadius(qreal percent) {
  UpdateRadialRadiusPercent(AnalyticMaskHandle::RadialMinor, percent);
}

void EditorMaskCreationAdapter::beginRotation() {
  BeginAnalyticMove(source_kind_ == MaskSourceKind::Radial ? AnalyticMaskHandle::RadialRotate
                                                           : AnalyticMaskHandle::LinearDirection);
}

void EditorMaskCreationAdapter::updateRotation(qreal degrees) {
  if (!overlay_source_.has_value() || !open_) {
    return;
  }
  const float radians = static_cast<float>(degrees) * kPi / 180.0f;
  if (const auto* radial = std::get_if<RadialMaskSource>(&*overlay_source_)) {
    const float radius = std::max(radial->major_radius, kAnalyticCreationMinRadius);
    EnqueueAppendSample(
        MakeNormalizedSample(Vector2{radial->center_x + radius * std::cos(radians),
                                     radial->center_y + radius * std::sin(radians)}));
  } else if (const auto* linear = std::get_if<LinearGradientMaskSource>(&*overlay_source_)) {
    EnqueueAppendSample(MakeNormalizedSample(
        Vector2{linear->origin_x + std::cos(radians), linear->origin_y + std::sin(radians)}));
  }
}

void EditorMaskCreationAdapter::beginTransition() {
  BeginAnalyticMove(AnalyticMaskHandle::LinearEndBoundary);
}

void EditorMaskCreationAdapter::updateTransition(qreal percent) {
  const auto* linear =
      overlay_source_ ? std::get_if<LinearGradientMaskSource>(&*overlay_source_) : nullptr;
  if (linear == nullptr || !open_) {
    return;
  }
  const float length = std::hypot(linear->normal_x, linear->normal_y);
  if (length <= kAnalyticMaskEpsilon) {
    return;
  }
  const float half = std::max(0.0f, static_cast<float>(percent) / 200.0f);
  EnqueueAppendSample(
      MakeNormalizedSample(Vector2{linear->origin_x + half * linear->normal_x / length,
                                   linear->origin_y + half * linear->normal_y / length}));
}

void EditorMaskCreationAdapter::finishAnalyticControl() {
  if (!open_) {
    return;
  }
  EditorMaskCreationCommand finish;
  finish.kind     = EditorMaskCreationCommandKind::Finish;
  finish.node_id  = edit_node_id_;
  finish.mask_id  = MaskIdFromQString(selected_mask_id_);
  finish.identity = pointer_;
  (void)Enqueue(finish);
  open_           = false;
  open_via_panel_ = false;
  active_handle_  = AnalyticMaskHandle::None;
  PublishOverlay();
  emit maskCreationChanged();
}

auto EditorMaskCreationAdapter::CancelIfMappingChanged() -> bool {
  if (!open_ || !press_mapping_identity_.has_value() || interaction_ == nullptr) {
    return false;
  }
  if (!MaskEditGeometry::MappingChanged(*press_mapping_identity_,
                                        interaction_->maskEditMappingIdentity())) {
    return false;
  }
  // The presented-frame or view mapping changed mid-drag; queued samples would
  // land in the new reference space against a press taken in the old one.
  EditorMaskCreationCommand cancel;
  cancel.kind    = EditorMaskCreationCommandKind::Cancel;
  cancel.node_id = edit_node_id_;
  cancel.mask_id = MaskIdFromQString(selected_mask_id_);
  (void)Enqueue(cancel);
  open_                    = false;
  open_via_panel_          = false;
  active_handle_           = AnalyticMaskHandle::None;
  press_mapping_identity_  = std::nullopt;
  brush_item_path_.clear();
  PublishOverlay();
  emit maskCreationChanged();
  return true;
}

void EditorMaskCreationAdapter::EnqueueAppendSample(const MaskCreationSample& sample) {
  EditorMaskCreationCommand command;
  command.kind     = EditorMaskCreationCommandKind::Append;
  command.sample   = sample;
  command.identity = pointer_;
  command.node_id  = edit_node_id_;
  command.mask_id  = MaskIdFromQString(selected_mask_id_);
  if (source_kind_ == MaskSourceKind::Brush &&
      (brush_tool_ == EditorBrushTool::Paint || brush_tool_ == EditorBrushTool::Erase)) {
    command.ordered_append = true;
    EnqueueBrushSettings(command);
  }
  (void)Enqueue(command);
  if (source_kind_ == MaskSourceKind::Brush &&
      (brush_tool_ == EditorBrushTool::Paint || brush_tool_ == EditorBrushTool::Erase)) {
    if (interaction_ != nullptr) {
      if (const auto item = MaskEditGeometry::MapReferenceToItem(
              interaction_->maskEditViewMapping(), sample.reference_pixels)) {
        brush_item_path_.push_back(*item);
      }
    }
  } else if (creating()) {
    overlay_source_ = source_kind_ == MaskSourceKind::Radial
                          ? MaskSource{RadialFromCenterOut(press_normalized_, sample.normalized)}
                          : MaskSource{LinearFromEndpoints(press_normalized_, sample.normalized)};
  } else if (source_kind_ == MaskSourceKind::Brush &&
             active_handle_ == AnalyticMaskHandle::BrushMove) {
    if (auto* brush = overlay_source_ ? std::get_if<BrushMaskSource>(&*overlay_source_) : nullptr) {
      brush->placement_translation = BrushPlacementForReferenceDrag(
          brush_placement_before_, press_reference_pixels_, sample.reference_pixels);
    }
  } else if (overlay_source_.has_value() && active_handle_ != AnalyticMaskHandle::None) {
    float unwrapped = 0.0f;
    if (const auto* radial = std::get_if<RadialMaskSource>(&*overlay_source_)) {
      unwrapped = radial->rotation;
    }
    if (auto next = ApplyAnalyticMaskHandle(active_handle_, *overlay_source_, sample.normalized,
                                            unwrapped)) {
      overlay_source_ = std::move(*next);
    }
  }
  PublishOverlay();
}

auto EditorMaskCreationAdapter::handlePress(qreal x, qreal y, int button) -> bool {
  if (!owns_left_button() || button != static_cast<int>(Qt::LeftButton)) {
    return false;
  }
  if (open_) {
    return true;
  }
  if (!CanAuthorMasks() || interaction_ == nullptr) {
    return true;
  }
  const auto sample = MakeSample(x, y, false);
  if (!sample || !sample->inside_photograph) {
    return true;
  }
  pointer_.device_id      = 1;
  pointer_.point_id       = 1;
  pointer_.sequence_id    = next_sequence_id_++;
  press_normalized_       = sample->normalized;
  press_reference_pixels_ = sample->reference_pixels;

  const auto hit          = HitTestMaskOverlayHandle(overlay_display_, QPointF(x, y),
                                                     OverlayStyle().hit_radius_logical_px);
  const auto handle       = AnalyticHandleFromOverlay(hit);
  if (source_kind_ == MaskSourceKind::Brush && brush_tool_ == EditorBrushTool::Move &&
      handle == AnalyticMaskHandle::BrushMove) {
    if (const auto* brush =
            overlay_source_ ? std::get_if<BrushMaskSource>(&*overlay_source_) : nullptr) {
      brush_placement_before_ = brush->placement_translation;
    }
    EditorMaskCreationCommand move;
    move.kind       = EditorMaskCreationCommandKind::BeginMove;
    move.handle     = AnalyticMaskHandle::BrushMove;
    move.sample     = *sample;
    move.identity   = pointer_;
    move.node_id    = edit_node_id_;
    move.mask_id    = MaskIdFromQString(selected_mask_id_);
    move.brush_tool = EditorBrushTool::Move;
    if (Enqueue(move)) {
      press_mapping_identity_ = interaction_->maskEditMappingIdentity();
      active_handle_          = AnalyticMaskHandle::BrushMove;
      open_                   = true;
      open_via_panel_         = false;
      edit_mode_              = EditMode::Editing;
      emit maskCreationChanged();
      return true;
    }
    return true;
  }
  if (source_kind_ == MaskSourceKind::Brush &&
      (brush_tool_ == EditorBrushTool::Paint || brush_tool_ == EditorBrushTool::Erase)) {
    EditorMaskCreationCommand input;
    input.kind     = EditorMaskCreationCommandKind::BeginInput;
    input.sample   = *sample;
    input.identity = pointer_;
    input.node_id  = edit_node_id_;
    input.mask_id  = MaskIdFromQString(selected_mask_id_);
    if (const auto& extent = interaction_->maskEditViewMapping().geometry.full_reference_extent;
        !extent.Empty()) {
      input.default_feather_reference_px = DefaultBrushFeatherReferencePixels(extent);
    }
    EnqueueBrushSettings(input);
    if (!Enqueue(input)) {
      return true;
    }
    press_mapping_identity_ = interaction_->maskEditMappingIdentity();
    active_handle_          = AnalyticMaskHandle::None;
    open_                   = true;
    open_via_panel_         = false;
    brush_item_path_.clear();
    if (interaction_ != nullptr) {
      if (const auto item = MaskEditGeometry::MapReferenceToItem(
              interaction_->maskEditViewMapping(), sample->reference_pixels)) {
        brush_item_path_.push_back(*item);
      }
    }
    if (!overlay_source_.has_value()) {
      overlay_source_ = BrushMaskSource{};
    }
    PublishOverlay();
    emit maskCreationChanged();
    return true;
  }
  if (handle != AnalyticMaskHandle::None && edit_mode_ == EditMode::Editing &&
      IsAnalyticKind(source_kind_)) {
    EditorMaskCreationCommand select;
    select.kind    = EditorMaskCreationCommandKind::SelectMask;
    select.node_id = edit_node_id_;
    select.mask_id = MaskIdFromQString(selected_mask_id_);
    if (!select.mask_id.Empty()) {
      (void)Enqueue(select);
      EditorMaskCreationCommand move;
      move.kind     = EditorMaskCreationCommandKind::BeginMove;
      move.handle   = handle;
      move.sample   = *sample;
      move.identity = pointer_;
      move.node_id  = edit_node_id_;
      move.mask_id  = MaskIdFromQString(selected_mask_id_);
      if (Enqueue(move)) {
        press_normalized_       = sample->normalized;
        press_mapping_identity_ = interaction_->maskEditMappingIdentity();
        active_handle_          = handle;
        open_                   = true;
        open_via_panel_         = false;
        edit_mode_              = EditMode::Editing;
        emit maskCreationChanged();
        return true;
      }
    }
  }

  if (!creating()) {
    return true;
  }
  EditorMaskCreationCommand input;
  input.kind     = EditorMaskCreationCommandKind::BeginInput;
  input.sample   = *sample;
  input.identity = pointer_;
  input.node_id  = edit_node_id_;
  if (!Enqueue(input)) {
    return true;
  }
  press_normalized_       = sample->normalized;
  press_mapping_identity_ = interaction_->maskEditMappingIdentity();
  active_handle_          = AnalyticMaskHandle::None;
  open_                   = true;
  open_via_panel_         = false;
  overlay_source_   = source_kind_ == MaskSourceKind::Radial
                          ? MaskSource{RadialFromCenterOut(sample->normalized, sample->normalized)}
                          : MaskSource{LinearFromEndpoints(sample->normalized, sample->normalized)};
  PublishOverlay();
  emit maskCreationChanged();
  return true;
}

auto EditorMaskCreationAdapter::handleMove(qreal x, qreal y, int /*buttons*/) -> bool {
  if (!open_ || open_via_panel_) {
    return owns_left_button();
  }
  if (CancelIfMappingChanged()) {
    return true;
  }
  auto sample = MakeSample(x, y, true);
  if (!sample) {
    return true;
  }
  if (active_handle_ == AnalyticMaskHandle::LinearDirection) {
    const auto* linear =
        overlay_source_ ? std::get_if<LinearGradientMaskSource>(&*overlay_source_) : nullptr;
    if (linear != nullptr && interaction_ != nullptr) {
      const auto normalized = MapItemPointToLinearDirectionSample(
          interaction_->maskEditViewMapping(), *linear, QPointF(x, y));
      if (!normalized) {
        return true;
      }
      sample = MakeNormalizedSample(*normalized);
    }
  }
  EnqueueAppendSample(*sample);
  return true;
}

auto EditorMaskCreationAdapter::handleRelease(qreal x, qreal y, int button) -> bool {
  if (!open_ || open_via_panel_ || button != static_cast<int>(Qt::LeftButton)) {
    return owns_left_button() && button == static_cast<int>(Qt::LeftButton);
  }
  if (CancelIfMappingChanged()) {
    return true;
  }
  auto sample = MakeSample(x, y, true);
  if (sample && active_handle_ == AnalyticMaskHandle::LinearDirection) {
    const auto* linear =
        overlay_source_ ? std::get_if<LinearGradientMaskSource>(&*overlay_source_) : nullptr;
    if (linear != nullptr && interaction_ != nullptr) {
      const auto normalized = MapItemPointToLinearDirectionSample(
          interaction_->maskEditViewMapping(), *linear, QPointF(x, y));
      if (normalized) {
        sample = MakeNormalizedSample(*normalized);
      }
    }
  }
  if (sample) {
    EnqueueAppendSample(*sample);
  }
  EditorMaskCreationCommand finish;
  finish.kind     = EditorMaskCreationCommandKind::Finish;
  finish.node_id  = edit_node_id_;
  finish.mask_id  = MaskIdFromQString(selected_mask_id_);
  finish.identity = pointer_;
  (void)Enqueue(finish);
  open_                   = false;
  open_via_panel_         = false;
  active_handle_          = AnalyticMaskHandle::None;
  press_mapping_identity_ = std::nullopt;
  brush_item_path_.clear();
  if (source_kind_ == MaskSourceKind::Brush &&
      (brush_tool_ == EditorBrushTool::Paint || brush_tool_ == EditorBrushTool::Erase)) {
    edit_mode_ = EditMode::Creating;
  } else {
    edit_mode_ = overlay_source_.has_value() ? EditMode::Editing : EditMode::Creating;
  }
  if (selected_mask_id_.isEmpty() && session_ != nullptr) {
    selected_mask_id_ = MaskIdToQString(session_->mask_creation_mask_id());
  }
  PublishOverlay();
  emit maskCreationChanged();
  return true;
}

}  // namespace alcedo::ui
