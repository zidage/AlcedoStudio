//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_mask_creation_adapter.hpp"

#include <QPointF>
#include <Qt>
#include <algorithm>
#include <cmath>
#include <string>
#include <variant>

#include "edit/geometry/render_request.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/analytic_mask_edit.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_layout.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_overlay_item.hpp"

namespace alcedo::ui {
namespace {

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
                                                     QObject* parent)
    : QObject(parent), session_(session) {}

EditorMaskCreationAdapter::~EditorMaskCreationAdapter() {
  if (view_change_connection_) {
    QObject::disconnect(view_change_connection_);
  }
}

auto EditorMaskCreationAdapter::owns_left_button() const -> bool {
  return creating_ || (selected_ && IsAnalyticKind(source_kind_));
}

auto EditorMaskCreationAdapter::body_visible() const -> bool {
  return body_open_ && (creating_ || selected_ || !selected_mask_id_.isEmpty());
}

auto EditorMaskCreationAdapter::mask_controls_active() const -> bool {
  return creating_ || selected_ || !selected_mask_id_.isEmpty() || body_open_;
}

auto EditorMaskCreationAdapter::inner_feather_percent() const -> qreal {
  const auto* radial = overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr;
  return radial == nullptr ? 0.0 : static_cast<qreal>(radial->inner_feather) * 100.0;
}

auto EditorMaskCreationAdapter::outer_feather_percent() const -> qreal {
  const auto* radial = overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr;
  return radial == nullptr ? 0.0 : static_cast<qreal>(std::max(0.0f, radial->outer_feather)) * 100.0;
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
  interaction_ = typed;
  ConnectInteraction(typed);
  PublishDisplayedGeometry();
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
      interaction, &editor_rhi::EditorInteractionController::viewChangeReported, this,
      [this](int) {
        if (open_) {
          cancel();
        }
      });
}

void EditorMaskCreationAdapter::beginRadial() {
  BeginTool(MaskSourceKind::Radial, QStringLiteral("radial"));
}

void EditorMaskCreationAdapter::beginLinear() {
  BeginTool(MaskSourceKind::LinearGradient, QStringLiteral("linear"));
}

void EditorMaskCreationAdapter::BeginTool(MaskSourceKind kind, const QString& tool_kind) {
  if (!CanAuthorMasks()) {
    return;
  }
  if (open_) {
    EditorMaskCreationCommand cancel;
    cancel.kind     = EditorMaskCreationCommandKind::Cancel;
    cancel.node_id  = CurrentGradeId();
    cancel.mask_id  = MaskIdFromQString(selected_mask_id_);
    (void)Enqueue(cancel);
  }
  EditorMaskCreationCommand command;
  command.kind        = EditorMaskCreationCommandKind::BeginCreation;
  command.source_kind = kind;
  command.node_id     = CurrentGradeId();
  if (command.node_id.Empty() || !Enqueue(command)) {
    return;
  }
  source_kind_       = kind;
  tool_kind_         = tool_kind;
  creating_          = true;
  selected_          = false;
  selected_mask_id_.clear();
  open_              = false;
  body_open_         = true;
  overlay_source_.reset();
  overlay_display_   = {};
  hovered_handle_    = MaskOverlayHandleId::None;
  active_handle_     = AnalyticMaskHandle::None;
  HideOverlay();
  PublishDisplayedGeometry();
  emit MaskCreationChanged();
}

void EditorMaskCreationAdapter::cancel() {
  if (!active() && !body_open_) {
    return;
  }
  EditorMaskCreationCommand command;
  command.kind    = EditorMaskCreationCommandKind::CancelMode;
  command.node_id = CurrentGradeId();
  command.mask_id = MaskIdFromQString(selected_mask_id_);
  (void)Enqueue(command);
  ResetLocal();
}

void EditorMaskCreationAdapter::hideBody() {
  if (!body_open_) {
    return;
  }
  body_open_ = false;
  emit MaskCreationChanged();
}

void EditorMaskCreationAdapter::finishBody() {
  if (open_) {
    EditorMaskCreationCommand finish;
    finish.kind     = EditorMaskCreationCommandKind::Finish;
    finish.node_id  = CurrentGradeId();
    finish.mask_id  = MaskIdFromQString(selected_mask_id_);
    finish.identity = pointer_;
    (void)Enqueue(finish);
    open_ = false;
  }
  creating_  = false;
  body_open_ = false;
  emit MaskCreationChanged();
}

void EditorMaskCreationAdapter::selectMask(const QString& node_id, const QString& mask_id) {
  if (!CanAuthorMasks() || mask_id.isEmpty()) {
    return;
  }
  NodeId grade{node_id.toStdString()};
  if (grade.Empty()) {
    grade = CurrentGradeId();
  }
  if (grade.Empty()) {
    return;
  }
  if (open_) {
    EditorMaskCreationCommand cancel;
    cancel.kind    = EditorMaskCreationCommandKind::Cancel;
    cancel.node_id = grade;
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
  selected_          = true;
  creating_          = false;
  open_              = false;
  body_open_         = true;
  emit MaskCreationChanged();
}

void EditorMaskCreationAdapter::removeMask(const QString& node_id, const QString& mask_id) {
  if (!CanAuthorMasks() || mask_id.isEmpty()) {
    return;
  }
  NodeId grade{node_id.toStdString()};
  if (grade.Empty()) {
    grade = CurrentGradeId();
  }
  if (grade.Empty()) {
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
    selected_        = false;
    selected_mask_id_.clear();
    overlay_source_.reset();
    overlay_display_ = {};
    HideOverlay();
  }
  emit MaskCreationChanged();
}

void EditorMaskCreationAdapter::removeSelectedMask() {
  if (selected_mask_id_.isEmpty()) {
    return;
  }
  removeMask(QString{}, selected_mask_id_);
}

void EditorMaskCreationAdapter::OnImageClosed() {
  ResetLocal();
}

void EditorMaskCreationAdapter::SyncFromSession() {
  PublishDisplayedGeometry();
  if (open_ || session_ == nullptr) {
    return;
  }
  const auto owner_id = session_->mask_creation_mask_id();
  const auto source    = session_->mask_creation_source();
  if (!owner_id.Empty() && source.has_value()) {
    ApplyOwnerSource(owner_id, *source);
    return;
  }
  if (selected_mask_id_.isEmpty()) {
    const auto restored = session_->mask_creation_last_removed_mask_id();
    if (!restored.Empty() && DocumentContainsMask(restored)) {
      selectMask(QString{}, MaskIdToQString(restored));
      return;
    }
  }
  if (!selected_mask_id_.isEmpty() || creating_) {
    return;
  }
  if (active() || body_open_) {
    ResetLocal();
  }
}

void EditorMaskCreationAdapter::ApplyOwnerSource(const MaskId& mask_id,
                                                   const MaskSource& source) {
  overlay_source_   = source;
  source_kind_       = GetMaskSourceKind(source);
  tool_kind_         = ToolKindFromSource(source_kind_);
  selected_mask_id_ = MaskIdToQString(mask_id);
  selected_          = true;
  creating_          = false;
  body_open_         = true;
  PublishOverlay();
  emit MaskCreationChanged();
}

void EditorMaskCreationAdapter::ResetLocal() {
  const bool changed = active() || open_ || creating_ || selected_ || body_open_;
  tool_kind_.clear();
  selected_mask_id_.clear();
  creating_        = false;
  open_            = false;
  selected_        = false;
  body_open_       = false;
  overlay_source_.reset();
  overlay_display_ = {};
  hovered_handle_  = MaskOverlayHandleId::None;
  active_handle_   = AnalyticMaskHandle::None;
  HideOverlay();
  if (changed) {
    emit MaskCreationChanged();
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
  if (session_ == nullptr || !session_->can_edit()) {
    return false;
  }
  if (CurrentGradeId().Empty()) {
    return false;
  }
  if (interaction_ != nullptr && interaction_->cropOverlayVisible()) {
    return false;
  }
  return true;
}

auto EditorMaskCreationAdapter::DocumentContainsMask(const MaskId& mask_id) const -> bool {
  if (session_ == nullptr || mask_id.Empty()) {
    return false;
  }
  const auto* document = session_->pipeline_document();
  if (document == nullptr) {
    return false;
  }
  const auto* node = document->Graph().FindNode(CurrentGradeId());
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
  return session_ != nullptr && session_->EnqueueMaskCreation(std::move(command));
}

void EditorMaskCreationAdapter::HideOverlay() {
  if (overlay_ == nullptr) {
    return;
  }
  overlay_->setMaskOverlayDisplay(MaskOverlayDisplay{});
}

void EditorMaskCreationAdapter::PublishDisplayedGeometry() {
  if (interaction_ == nullptr || session_ == nullptr) {
    return;
  }
  const auto* document = session_->pipeline_document();
  if (document == nullptr) {
    return;
  }
  const int width  = interaction_->imageWidth();
  const int height = interaction_->imageHeight();
  if (width <= 0 || height <= 0) {
    return;
  }
  ImageGeometryParams image;
  image.crop_rect         = document->Geometry().CropRect();
  image.rotation_degrees  = document->Geometry().RotationDegrees();
  image.expand_to_fit     = document->Geometry().ExpandToFit();
  interaction_->setDisplayedMaskGeometry(MaskEditGeometry::MakeDocumentPhotographGeometry(
      Extent2D{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, image));
}

void EditorMaskCreationAdapter::PublishOverlay() {
  if (overlay_ == nullptr || interaction_ == nullptr || !overlay_source_.has_value()) {
    HideOverlay();
    return;
  }
  PublishDisplayedGeometry();
  const auto mapping = interaction_->maskEditViewMapping();
  const auto style   = OverlayStyle();
  const auto clip    = OverlayClip();
  MaskOverlayDisplay display;
  if (const auto* radial = std::get_if<RadialMaskSource>(&*overlay_source_)) {
    display = (creating_ && open_) ? MakeRadialCreatingOverlayDisplay(mapping, *radial, style, clip)
                                   : MakeRadialExistingOverlayDisplay(mapping, *radial, style, clip);
  } else if (const auto* linear = std::get_if<LinearGradientMaskSource>(&*overlay_source_)) {
    display = (creating_ && open_)
                  ? MakeLinearCreatingOverlayDisplay(mapping, *linear, style, clip)
                  : MakeLinearExistingOverlayDisplay(mapping, *linear, style, clip);
  } else if (const auto* brush = std::get_if<BrushMaskSource>(&*overlay_source_)) {
    display = MakeBrushExistingOverlayDisplay(mapping, brush->placement_translation, clip);
  }
  display.hovered_handle = hovered_handle_;
  display.active_handle  = OverlayHandleFromAnalytic(active_handle_);
  overlay_display_       = display;
  overlay_->setMaskOverlayDisplay(std::move(display));
}

void EditorMaskCreationAdapter::handleHover(qreal x, qreal y) {
  if (!owns_left_button() || overlay_display_.mode == MaskOverlayMode::Hidden) {
    return;
  }
  const auto hit =
      HitTestMaskOverlayHandle(overlay_display_, QPointF(x, y), OverlayStyle().hit_radius_logical_px);
  if (hit == hovered_handle_) {
    return;
  }
  hovered_handle_ = hit;
  PublishOverlay();
}

void EditorMaskCreationAdapter::BeginFeatherMove(AnalyticMaskHandle handle) {
  const auto* radial = overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr;
  if (radial == nullptr || selected_mask_id_.isEmpty() || !CanAuthorMasks()) {
    return;
  }
  const float rho = handle == AnalyticMaskHandle::RadialInnerFeather
                        ? std::max(0.0f, 1.0f - radial->inner_feather)
                        : 1.0f + std::max(0.0f, radial->outer_feather);
  const auto sample = MakeNormalizedSample(RadialNormalizedPoint(*radial, rho, 0.0f));
  pointer_.device_id   = 2;
  pointer_.point_id    = 1;
  pointer_.sequence_id = next_sequence_id_++;
  EditorMaskCreationCommand select;
  select.kind    = EditorMaskCreationCommandKind::SelectMask;
  select.node_id = CurrentGradeId();
  select.mask_id = MaskIdFromQString(selected_mask_id_);
  (void)Enqueue(select);
  EditorMaskCreationCommand move;
  move.kind     = EditorMaskCreationCommandKind::BeginMove;
  move.handle   = handle;
  move.sample   = sample;
  move.identity = pointer_;
  move.node_id  = CurrentGradeId();
  move.mask_id  = MaskIdFromQString(selected_mask_id_);
  if (!Enqueue(move)) {
    return;
  }
  active_handle_ = handle;
  open_          = true;
  creating_      = false;
  emit MaskCreationChanged();
}

void EditorMaskCreationAdapter::beginInnerFeather() {
  BeginFeatherMove(AnalyticMaskHandle::RadialInnerFeather);
}

void EditorMaskCreationAdapter::beginOuterFeather() {
  BeginFeatherMove(AnalyticMaskHandle::RadialOuterFeather);
}

void EditorMaskCreationAdapter::UpdateFeatherPercent(AnalyticMaskHandle handle, qreal percent) {
  const auto* radial = overlay_source_ ? std::get_if<RadialMaskSource>(&*overlay_source_) : nullptr;
  if (radial == nullptr || !open_) {
    return;
  }
  const float clamped = handle == AnalyticMaskHandle::RadialInnerFeather
                             ? std::clamp(static_cast<float>(percent) / 100.0f, 0.0f, 1.0f)
                             : std::max(0.0f, static_cast<float>(percent) / 100.0f);
  const float rho =
      handle == AnalyticMaskHandle::RadialInnerFeather ? std::max(0.0f, 1.0f - clamped)
                                                        : 1.0f + clamped;
  EnqueueAppendSample(MakeNormalizedSample(RadialNormalizedPoint(*radial, rho, 0.0f)));
}

void EditorMaskCreationAdapter::updateInnerFeather(qreal percent) {
  UpdateFeatherPercent(AnalyticMaskHandle::RadialInnerFeather, percent);
}

void EditorMaskCreationAdapter::updateOuterFeather(qreal percent) {
  UpdateFeatherPercent(AnalyticMaskHandle::RadialOuterFeather, percent);
}

void EditorMaskCreationAdapter::finishFeather() {
  if (!open_) {
    return;
  }
  EditorMaskCreationCommand finish;
  finish.kind     = EditorMaskCreationCommandKind::Finish;
  finish.node_id  = CurrentGradeId();
  finish.mask_id  = MaskIdFromQString(selected_mask_id_);
  finish.identity = pointer_;
  (void)Enqueue(finish);
  open_          = false;
  active_handle_ = AnalyticMaskHandle::None;
  PublishOverlay();
  emit MaskCreationChanged();
}

void EditorMaskCreationAdapter::EnqueueAppendSample(const MaskCreationSample& sample) {
  EditorMaskCreationCommand command;
  command.kind     = EditorMaskCreationCommandKind::Append;
  command.sample   = sample;
  command.identity = pointer_;
  command.node_id  = CurrentGradeId();
  command.mask_id  = MaskIdFromQString(selected_mask_id_);
  (void)Enqueue(command);
  if (creating_) {
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
  if (!CanAuthorMasks() || interaction_ == nullptr) {
    return true;
  }
  const auto sample = MakeSample(x, y, false);
  if (!sample || !sample->inside_photograph) {
    return true;
  }
  pointer_.device_id   = 1;
  pointer_.point_id    = 1;
  pointer_.sequence_id = next_sequence_id_++;

  const auto hit    = HitTestMaskOverlayHandle(overlay_display_, QPointF(x, y),
                                           OverlayStyle().hit_radius_logical_px);
  const auto handle = AnalyticHandleFromOverlay(hit);
  if (handle != AnalyticMaskHandle::None && selected_ && IsAnalyticKind(source_kind_)) {
    EditorMaskCreationCommand select;
    select.kind    = EditorMaskCreationCommandKind::SelectMask;
    select.node_id = CurrentGradeId();
    select.mask_id = MaskIdFromQString(selected_mask_id_);
    if (!select.mask_id.Empty()) {
      (void)Enqueue(select);
      EditorMaskCreationCommand move;
      move.kind     = EditorMaskCreationCommandKind::BeginMove;
      move.handle   = handle;
      move.sample   = *sample;
      move.identity = pointer_;
      move.node_id  = CurrentGradeId();
      move.mask_id  = MaskIdFromQString(selected_mask_id_);
      if (Enqueue(move)) {
        press_normalized_ = sample->normalized;
        active_handle_    = handle;
        open_             = true;
        creating_         = false;
        emit MaskCreationChanged();
        return true;
      }
    }
  }

  if (!creating_) {
    return true;
  }
  EditorMaskCreationCommand input;
  input.kind     = EditorMaskCreationCommandKind::BeginInput;
  input.sample   = *sample;
  input.identity = pointer_;
  input.node_id  = CurrentGradeId();
  if (!Enqueue(input)) {
    return true;
  }
  press_normalized_ = sample->normalized;
  active_handle_    = AnalyticMaskHandle::None;
  open_             = true;
  selected_         = false;
  overlay_source_   = source_kind_ == MaskSourceKind::Radial
                          ? MaskSource{RadialFromCenterOut(sample->normalized, sample->normalized)}
                          : MaskSource{LinearFromEndpoints(sample->normalized, sample->normalized)};
  PublishOverlay();
  emit MaskCreationChanged();
  return true;
}

auto EditorMaskCreationAdapter::handleMove(qreal x, qreal y, int /*buttons*/) -> bool {
  if (!open_) {
    return owns_left_button();
  }
  const auto sample = MakeSample(x, y, true);
  if (!sample) {
    return true;
  }
  EnqueueAppendSample(*sample);
  return true;
}

auto EditorMaskCreationAdapter::handleRelease(qreal x, qreal y, int button) -> bool {
  if (!open_ || button != static_cast<int>(Qt::LeftButton)) {
    return owns_left_button() && button == static_cast<int>(Qt::LeftButton);
  }
  const auto sample = MakeSample(x, y, true);
  if (sample) {
    EnqueueAppendSample(*sample);
  }
  EditorMaskCreationCommand finish;
  finish.kind     = EditorMaskCreationCommandKind::Finish;
  finish.node_id  = CurrentGradeId();
  finish.mask_id  = MaskIdFromQString(selected_mask_id_);
  finish.identity = pointer_;
  (void)Enqueue(finish);
  open_          = false;
  creating_      = false;
  selected_      = overlay_source_.has_value();
  active_handle_ = AnalyticMaskHandle::None;
  if (selected_ && selected_mask_id_.isEmpty() && session_ != nullptr) {
    selected_mask_id_ = MaskIdToQString(session_->mask_creation_mask_id());
  }
  PublishOverlay();
  emit MaskCreationChanged();
  return true;
}

}  // namespace alcedo::ui
