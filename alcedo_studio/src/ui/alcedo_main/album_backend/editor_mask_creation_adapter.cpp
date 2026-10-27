//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_mask_creation_adapter.hpp"

#include <QColor>
#include <QPointF>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <variant>

#include "app/editor_session_service.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/analytic_mask_edit.hpp"
#include "edit/mask/mask_range_display.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_node_layout_store.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/mask_thumbnail_coordinator.hpp"
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
    default:
      return MaskOverlayHandleId::None;
  }
}

[[nodiscard]] auto ToolKindFromSource(MaskSourceKind kind) -> QString {
  switch (kind) {
    case MaskSourceKind::Radial:
      return QStringLiteral("radial");
    case MaskSourceKind::LinearGradient:
      return QStringLiteral("linear");
    case MaskSourceKind::AiSegmentation:
      return QStringLiteral("aiSegmentation");
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

/// Handle distances of the range bars: a little above the stored minimum widths, so float
/// rounding of a handle at the minimum distance never writes an invalid range (MR1 record).
constexpr float    kLuminanceHandleMinimumWidth = kLuminanceRangeMinimumWidth * 1.2f;
constexpr float    kHueHandleMinimumWidth       = kColorRangeMinimumWidthDegrees * 1.2f;
constexpr int      kHueGradientSegments         = 36;
constexpr int      kLuminanceGradientSegments   = 16;

[[nodiscard]] auto GradientStop(double position, const std::array<float, 3>& rgb) -> QVariant {
  QVariantMap stop;
  stop.insert(QStringLiteral("position"), position);
  stop.insert(QStringLiteral("color"), QColor::fromRgbF(rgb[0], rgb[1], rgb[2]));
  return stop;
}

/// @p degrees in [0, 360).
[[nodiscard]] auto WrapHueStart(float degrees) -> float {
  float wrapped = std::fmod(degrees, 360.0f);
  if (wrapped < 0.0f) {
    wrapped += 360.0f;
  }
  return wrapped >= 360.0f ? 0.0f : wrapped;
}

/// @p degrees in (0, 360].
[[nodiscard]] auto WrapHueEnd(float degrees) -> float {
  float wrapped = std::fmod(degrees, 360.0f);
  if (wrapped <= 0.0f) {
    wrapped += 360.0f;
  }
  return wrapped;
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
  return edit_mode_ == EditMode::Editing && IsAnalyticKind(source_kind_);
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

auto EditorMaskCreationAdapter::SelectedMask() const -> const MaskModel* {
  if (session_ == nullptr || selected_mask_id_.isEmpty()) {
    selected_mask_document_.reset();
    return nullptr;
  }
  const auto document = session_->pipeline_document();
  if (!document) {
    selected_mask_document_.reset();
    return nullptr;
  }
  const auto* grade =
      dynamic_cast<const ColorGradeNodeModel*>(document->Graph().FindNode(edit_node_id_));
  if (grade == nullptr) {
    selected_mask_document_.reset();
    return nullptr;
  }
  const auto* mask = grade->FindMask(MaskIdFromQString(selected_mask_id_));
  // The returned pointer addresses the published snapshot; pin the snapshot
  // so callers can dereference the mask until the next SelectedMask() call.
  selected_mask_document_ = mask != nullptr ? std::move(document) : nullptr;
  return mask;
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
  return IsAnalyticKind(source_kind_);
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

void EditorMaskCreationAdapter::toggleMaskInvert(const QString& node_id, const QString& mask_id) {
  const QString target_mask = mask_id.isEmpty() ? selected_mask_id_ : mask_id;
  if (target_mask.isEmpty()) {
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
  command.kind    = EditorMaskCreationCommandKind::ToggleMaskInvert;
  command.node_id = grade;
  command.mask_id = MaskIdFromQString(target_mask);
  (void)Enqueue(command);
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
  NodeId grade = CurrentGradeId();
  if (grade.Empty()) {
    // With no selected Color Grade the tool targets the top editable group.
    if (auto* nodes = session_ != nullptr ? session_->node_selection_source() : nullptr;
        nodes != nullptr && !nodes->has_draft()) {
      for (const auto& node : nodes->ActiveNodes()) {
        if (node.node_kind == EditorNodeKind::ColorGrade) {
          grade = node.node_id;
          break;
        }
      }
      if (!grade.Empty()) {
        nodes->selectNode(NodeIdToQString(grade));
      }
    }
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
  if (SyncRangeView()) {
    emit maskRangeChanged();
  }
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
  MaskThumbnailCoordinator* thumbs = nullptr;
  const QString             node_q = QString::fromStdString(std::string(grade.Value()));
  if (session_ != nullptr) {
    if (auto* nodes = session_->node_selection_source()) {
      thumbs = qobject_cast<MaskThumbnailCoordinator*>(nodes->mask_thumbnails_object());
    }
  }
  if (thumbs != nullptr) {
    thumbs->invalidateTarget(node_q, mask_id);
    thumbs->invalidateTarget(node_q, QString());
  }
  EditorMaskCreationCommand command;
  command.kind    = EditorMaskCreationCommandKind::RemoveMask;
  command.node_id = grade;
  command.mask_id = MaskIdFromQString(mask_id);
  // Enqueue is not deletion admission. The owner publishes the next selection
  // on success; rejected protection must retain the local selection and edit.
  if (!Enqueue(command) && thumbs != nullptr) {
    thumbs->requestCurrent(node_q, mask_id);
    thumbs->requestCurrent(node_q, QString());
  }
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

void EditorMaskCreationAdapter::OnImageClosed() {
  // The session ends the range coverage with the image.
  const bool range_state =
      range_coverage_requested_ || !shown_range_coverage_.Empty() || range_open_ != RangeKind::None;
  range_coverage_requested_ = false;
  shown_range_coverage_     = {};
  range_open_               = RangeKind::None;
  ResetLocal();
  if (range_state) {
    emit maskRangeChanged();
  }
}

void EditorMaskCreationAdapter::SyncFromSession() {
  if (session_ == nullptr) {
    return;
  }
  if (session_->mask_creation_commands_pending()) {
    return;
  }
  SyncOwnerSelection();
  // An open edit keeps its GUI-predicted values until it settles.
  if (!open_ && UpdateMaskFieldView()) {
    emit maskCreationChanged();
  }
  if (SyncRangeView()) {
    emit maskRangeChanged();
  }
}

auto EditorMaskCreationAdapter::UpdateMaskFieldView() -> bool {
  MaskFieldView view;
  view.mask_id = selected_mask_id_;
  if (const auto* mask = SelectedMask()) {
    view.present      = true;
    view.enabled      = mask->enabled;
    view.invert       = mask->invert;
    view.opacity      = mask->opacity;
    view.display_name = QString::fromStdString(mask->display_name);
  }
  if (view == published_mask_fields_) {
    return false;
  }
  published_mask_fields_ = std::move(view);
  return true;
}

void EditorMaskCreationAdapter::SyncOwnerSelection() {
  const auto owner_node  = session_->mask_creation_node_id();
  const auto owner_id    = session_->mask_creation_mask_id();
  const auto owner_state = session_->mask_creation_state();
  const auto source      = session_->mask_creation_source();
  if (open_) {
    if (owner_state == EditorMaskCreationState::Editing) {
      return;
    }
    // A successful deletion may close the owner's current edit. A rejected
    // deletion leaves it Editing, so the local mask drag remains intact above.
    open_ = false;
    open_via_panel_ = false;
    active_handle_ = AnalyticMaskHandle::None;
  }
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
  press_mapping_identity_  = std::nullopt;
  published_mapping_identity_ = {};
  HideOverlay();
  if (changed) {
    emit maskCreationChanged();
  }
  // The owner closed the Mask mode: an open range drag of the selection ended with it.
  if (range_open_ != RangeKind::None && !ai_mask_edit_open_) {
    ResetRangeEdit();
  }
  if (SyncRangeView()) {
    emit maskRangeChanged();
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
    // A live node-graph draft is an uncommitted graph: Mask creation targets
    // only the committed document and stays disabled until the draft completes.
    if (nodes->has_draft()) {
      return false;
    }
    for (const auto& node : nodes->ActiveNodes()) {
      if (node.node_id == grade_id && node.node_kind == EditorNodeKind::ColorGrade) {
        return true;
      }
    }
  }
  const auto document = session_->pipeline_document();
  return document &&
         dynamic_cast<const ColorGradeNodeModel*>(document->Graph().FindNode(grade_id)) != nullptr;
}

auto EditorMaskCreationAdapter::DocumentContainsMask(const MaskId& mask_id) const -> bool {
  if (session_ == nullptr || mask_id.Empty()) {
    return false;
  }
  const auto document = session_->pipeline_document();
  if (!document) {
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
    return;
  }
  if (overlay_display_.mode == MaskOverlayMode::Hidden) {
    return;
  }
  const auto hit = HitTestMaskOverlayHandle(overlay_display_, QPointF(x, y),
                                            OverlayStyle().hit_radius_logical_px);
  if (hit == hovered_handle_) {
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
  (void)Enqueue(command);
  if (creating()) {
    overlay_source_ = source_kind_ == MaskSourceKind::Radial
                          ? MaskSource{RadialFromCenterOut(press_normalized_, sample.normalized)}
                          : MaskSource{LinearFromEndpoints(press_normalized_, sample.normalized)};
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
  // Handles of an existing Mask may sit outside the photograph (a center
  // dragged past the edge); only new-Mask creation requires an inside press.
  const auto sample = MakeSample(x, y, true);
  if (!sample) {
    return true;
  }
  pointer_.device_id   = 1;
  pointer_.point_id    = 1;
  pointer_.sequence_id = next_sequence_id_++;
  press_normalized_    = sample->normalized;

  const auto hit    = HitTestMaskOverlayHandle(overlay_display_, QPointF(x, y),
                                               OverlayStyle().hit_radius_logical_px);
  const auto handle = AnalyticHandleFromOverlay(hit);
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

  if (!creating() || !sample->inside_photograph) {
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
  edit_mode_              = overlay_source_.has_value() ? EditMode::Editing : EditMode::Creating;
  if (selected_mask_id_.isEmpty() && session_ != nullptr) {
    selected_mask_id_ = MaskIdToQString(session_->mask_creation_mask_id());
  }
  PublishOverlay();
  emit maskCreationChanged();
  return true;
}

// ── Luminance Range and Hue Range (Mask ranges MR4) ────────────────────────

auto EditorMaskCreationAdapter::luminance_range_minimum_width() -> qreal {
  return static_cast<qreal>(kLuminanceHandleMinimumWidth);
}

auto EditorMaskCreationAdapter::hue_range_minimum_width() -> qreal {
  return static_cast<qreal>(kHueHandleMinimumWidth);
}

auto EditorMaskCreationAdapter::hue_gradient_stops() -> QVariantList {
  QVariantList stops;
  for (int i = 0; i <= kHueGradientSegments; ++i) {
    const double position = static_cast<double>(i) / kHueGradientSegments;
    stops.push_back(GradientStop(position, MaskHueBarColor(static_cast<float>(position * 360.0))));
  }
  return stops;
}

auto EditorMaskCreationAdapter::luminance_gradient_stops() -> QVariantList {
  QVariantList stops;
  for (int i = 0; i <= kLuminanceGradientSegments; ++i) {
    const double position = static_cast<double>(i) / kLuminanceGradientSegments;
    const float  gray     = MaskLuminanceBarGray(static_cast<float>(position));
    stops.push_back(GradientStop(position, {gray, gray, gray}));
  }
  return stops;
}

auto EditorMaskCreationAdapter::mask_range_available() const -> bool {
  if (!range_view_.present || range_view_.target.Empty()) {
    return false;
  }
  if (ai_mask_edit_open_) {
    // The AI Mask edit holds the document writes; only the range edit of its stored Mask writes.
    auto* backend = session_ != nullptr ? session_->session_backend() : nullptr;
    return backend != nullptr && session_->has_image() && backend->ai_mask_edit_open();
  }
  return CanAuthorMasksFor(range_view_.target.grade_id);
}

auto EditorMaskCreationAdapter::mask_luminance_low() const -> qreal {
  return static_cast<qreal>(range_open_ == RangeKind::Luminance ? range_first_
                                                                : range_view_.luminance_low);
}

auto EditorMaskCreationAdapter::mask_luminance_high() const -> qreal {
  return static_cast<qreal>(range_open_ == RangeKind::Luminance ? range_second_
                                                                : range_view_.luminance_high);
}

auto EditorMaskCreationAdapter::mask_hue_start() const -> qreal {
  return static_cast<qreal>(range_open_ == RangeKind::Hue ? range_first_ : range_view_.hue_start);
}

auto EditorMaskCreationAdapter::mask_hue_end() const -> qreal {
  return static_cast<qreal>(range_open_ == RangeKind::Hue ? range_second_ : range_view_.hue_end);
}

auto EditorMaskCreationAdapter::RangeKindFromString(const QString& kind) -> RangeKind {
  if (kind == QLatin1String("luminance")) {
    return RangeKind::Luminance;
  }
  if (kind == QLatin1String("hue")) {
    return RangeKind::Hue;
  }
  return RangeKind::None;
}

auto EditorMaskCreationAdapter::RangeFieldKey(RangeKind kind) -> std::string_view {
  return kind == RangeKind::Luminance ? kMaskFieldLuminanceRange : kMaskFieldColorRange;
}

auto EditorMaskCreationAdapter::RangeTargetNow() const -> RangeTarget {
  if (ai_mask_edit_open_) {
    return ai_mask_edit_target_;
  }
  if (selected_mask_id_.isEmpty()) {
    return {};
  }
  return RangeTarget{edit_node_id_, MaskIdFromQString(selected_mask_id_)};
}

auto EditorMaskCreationAdapter::SyncRangeView() -> bool {
  // An open drag keeps its handle values until it settles.
  if (range_open_ != RangeKind::None) {
    return false;
  }
  RangeView view;
  view.target = RangeTargetNow();
  if (!view.target.Empty() && session_ != nullptr) {
    if (const auto document = session_->pipeline_document()) {
      const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
          document->Graph().FindNode(view.target.grade_id));
      if (const auto* mask = grade != nullptr ? grade->FindMask(view.target.mask_id) : nullptr) {
        view.present = true;
        if (mask->luminance_range.has_value()) {
          view.luminance_low      = mask->luminance_range->low;
          view.luminance_high     = mask->luminance_range->high;
          view.luminance_softness = mask->luminance_range->softness;
        }
        if (mask->color_range.has_value()) {
          view.hue_start    = mask->color_range->hue_start;
          view.hue_end      = mask->color_range->hue_end;
          view.hue_softness = mask->color_range->softness;
        }
      }
    }
  }
  if (view == range_view_) {
    return false;
  }
  const bool retarget = view.target != range_view_.target || view.present != range_view_.present;
  range_view_         = view;
  if (retarget && range_coverage_requested_) {
    // The toggle follows the target Mask; without one it turns off.
    if (range_view_.present && !range_view_.target.Empty()) {
      ShowRangeCoverage(range_view_.target);
    } else {
      range_coverage_requested_ = false;
      HideRangeCoverage();
    }
  }
  return true;
}

auto EditorMaskCreationAdapter::RangeJson(RangeKind kind, float first, float second)
    -> nlohmann::json {
  if (kind == RangeKind::Luminance) {
    float low  = std::clamp(std::min(first, second), 0.0f, 1.0f);
    float high = std::clamp(std::max(first, second), 0.0f, 1.0f);
    if (high - low < kLuminanceHandleMinimumWidth) {
      if (low + kLuminanceHandleMinimumWidth <= 1.0f) {
        high = low + kLuminanceHandleMinimumWidth;
      } else {
        high = 1.0f;
        low  = 1.0f - kLuminanceHandleMinimumWidth;
      }
    }
    range_first_  = low;
    range_second_ = high;
    if (low <= 0.0f && high >= 1.0f) {
      return nullptr;
    }
    return LuminanceRangeToJson(LuminanceRangeModel{low, high, range_view_.luminance_softness});
  }
  if (first <= 0.0f && second >= 360.0f) {
    range_first_  = 0.0f;
    range_second_ = 360.0f;
    return nullptr;
  }
  const float start = WrapHueStart(first);
  float       end   = WrapHueEnd(second);
  const float width = end > start ? end - start : end + 360.0f - start;
  if (width < kHueHandleMinimumWidth) {
    end = WrapHueEnd(start + kHueHandleMinimumWidth);
  }
  range_first_  = start;
  range_second_ = end;
  if (start <= 0.0f && end >= 360.0f) {
    return nullptr;
  }
  return ColorRangeToJson(ColorRangeModel{start, end, range_view_.hue_softness});
}

void EditorMaskCreationAdapter::ShowRangeCoverage(const RangeTarget& target) {
  auto* backend = session_ != nullptr ? session_->session_backend() : nullptr;
  if (backend == nullptr || target.Empty()) {
    return;
  }
  (void)backend->ShowMaskRangeCoverage(target.grade_id, target.mask_id);
  shown_range_coverage_ = target;
}

void EditorMaskCreationAdapter::HideRangeCoverage() {
  if (shown_range_coverage_.Empty()) {
    return;
  }
  shown_range_coverage_ = {};
  if (auto* backend = session_ != nullptr ? session_->session_backend() : nullptr) {
    (void)backend->HideMaskRangeCoverage();
  }
}

void EditorMaskCreationAdapter::ResetRangeEdit() {
  range_open_ = RangeKind::None;
  if (!range_coverage_requested_) {
    HideRangeCoverage();
  }
}

auto EditorMaskCreationAdapter::beginRangeEdit(const QString& kind) -> bool {
  const auto range_kind = RangeKindFromString(kind);
  if (range_kind == RangeKind::None || range_open_ != RangeKind::None || open_) {
    return false;
  }
  (void)SyncRangeView();
  const auto target = range_view_.target;
  if (!mask_range_available()) {
    return false;
  }
  EditorMaskCreationCommand command;
  command.kind      = EditorMaskCreationCommandKind::BeginMaskField;
  command.node_id   = target.grade_id;
  command.mask_id   = target.mask_id;
  command.field_key = std::string{RangeFieldKey(range_kind)};
  if (!Enqueue(command)) {
    return false;
  }
  range_open_        = range_kind;
  range_edit_target_ = target;
  range_first_ =
      range_kind == RangeKind::Luminance ? range_view_.luminance_low : range_view_.hue_start;
  range_second_ =
      range_kind == RangeKind::Luminance ? range_view_.luminance_high : range_view_.hue_end;
  // D12: the range coverage is shown while a handle is dragged.
  if (!range_coverage_requested_) {
    ShowRangeCoverage(target);
  }
  emit maskRangeChanged();
  return true;
}

void EditorMaskCreationAdapter::updateRangeEdit(const QString& kind, qreal first, qreal second) {
  const auto range_kind = RangeKindFromString(kind);
  if (range_kind == RangeKind::None || range_kind != range_open_ || !std::isfinite(first) ||
      !std::isfinite(second)) {
    return;
  }
  EditorMaskCreationCommand command;
  command.kind      = EditorMaskCreationCommandKind::SetMaskField;
  command.node_id   = range_edit_target_.grade_id;
  command.mask_id   = range_edit_target_.mask_id;
  command.field_key = std::string{RangeFieldKey(range_kind)};
  command.field_value =
      RangeJson(range_kind, static_cast<float>(first), static_cast<float>(second));
  (void)Enqueue(command);
  emit maskRangeChanged();
}

void EditorMaskCreationAdapter::finishRangeEdit(const QString& kind) {
  const auto range_kind = RangeKindFromString(kind);
  if (range_kind == RangeKind::None || range_kind != range_open_) {
    return;
  }
  EditorMaskCreationCommand command;
  command.kind    = EditorMaskCreationCommandKind::Finish;
  command.node_id = range_edit_target_.grade_id;
  command.mask_id = range_edit_target_.mask_id;
  (void)Enqueue(command);
  // The settled value stays shown until the owner publishes it.
  if (range_kind == RangeKind::Luminance) {
    range_view_.luminance_low  = range_first_;
    range_view_.luminance_high = range_second_;
  } else {
    range_view_.hue_start = range_first_;
    range_view_.hue_end   = range_second_;
  }
  ResetRangeEdit();
  emit maskRangeChanged();
}

void EditorMaskCreationAdapter::resetRange(const QString& kind) {
  const auto range_kind = RangeKindFromString(kind);
  if (range_kind == RangeKind::None || range_open_ != RangeKind::None || open_) {
    return;
  }
  (void)SyncRangeView();
  if (!mask_range_available()) {
    return;
  }
  // One SetMaskField without an open edit commits once; an unchanged value commits nothing.
  EditorMaskCreationCommand command;
  command.kind        = EditorMaskCreationCommandKind::SetMaskField;
  command.node_id     = range_view_.target.grade_id;
  command.mask_id     = range_view_.target.mask_id;
  command.field_key   = std::string{RangeFieldKey(range_kind)};
  command.field_value = nullptr;
  if (!Enqueue(command)) {
    return;
  }
  if (range_kind == RangeKind::Luminance) {
    range_view_.luminance_low  = 0.0f;
    range_view_.luminance_high = 1.0f;
  } else {
    range_view_.hue_start = 0.0f;
    range_view_.hue_end   = 360.0f;
  }
  emit maskRangeChanged();
}

void EditorMaskCreationAdapter::setRangeCoverageVisible(bool visible) {
  if (visible == range_coverage_requested_) {
    return;
  }
  (void)SyncRangeView();
  if (visible) {
    if (!mask_range_available()) {
      return;
    }
    range_coverage_requested_ = true;
    ShowRangeCoverage(range_view_.target);
  } else {
    range_coverage_requested_ = false;
    // An open drag keeps its coverage until it settles.
    if (range_open_ == RangeKind::None) {
      HideRangeCoverage();
    }
  }
  emit maskRangeChanged();
}

void EditorMaskCreationAdapter::SetAiMaskEditRangeTarget(bool open, const NodeId& grade_id,
                                                         const std::optional<MaskId>& mask_id) {
  const RangeTarget target =
      open && mask_id.has_value() ? RangeTarget{grade_id, *mask_id} : RangeTarget{};
  if (open == ai_mask_edit_open_ && target == ai_mask_edit_target_) {
    return;
  }
  const bool open_changed = open != ai_mask_edit_open_;
  ai_mask_edit_open_      = open;
  ai_mask_edit_target_    = target;
  if (open_changed) {
    // The AI Mask edit starts and ends its own coverage (D11); the toggle does not carry over.
    if (range_open_ != RangeKind::None) {
      range_open_ = RangeKind::None;
    }
    range_coverage_requested_ = false;
    HideRangeCoverage();
  }
  (void)SyncRangeView();
  emit maskRangeChanged();
}

void EditorMaskCreationAdapter::OnAdjustmentInputStarted() {
  if (!range_coverage_requested_ && shown_range_coverage_.Empty()) {
    return;
  }
  // Master plan 10.4: an adjustment input hides the coverage, and it is not shown again by itself.
  range_coverage_requested_ = false;
  if (range_open_ == RangeKind::None) {
    HideRangeCoverage();
  }
  emit maskRangeChanged();
}

void EditorMaskCreationAdapter::OnRangeCoverageReplaced() {
  if (!range_coverage_requested_ && shown_range_coverage_.Empty()) {
    return;
  }
  // The session already replaced the probe with the candidate; no Hide is sent.
  range_coverage_requested_ = false;
  shown_range_coverage_     = {};
  emit maskRangeChanged();
}

}  // namespace alcedo::ui
