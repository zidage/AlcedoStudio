//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_mask_creation_adapter.hpp"

#include <QPointF>
#include <Qt>

#include "edit/mask/analytic_mask_edit.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
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

}  // namespace

EditorMaskCreationAdapter::EditorMaskCreationAdapter(EditorSessionController* session,
                                                     QObject* parent)
    : QObject(parent), session_(session) {}

EditorMaskCreationAdapter::~EditorMaskCreationAdapter() {
  if (view_change_connection_) {
    QObject::disconnect(view_change_connection_);
  }
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
    (void)Enqueue(EditorMaskCreationCommand{EditorMaskCreationCommandKind::Cancel});
  }
  EditorMaskCreationCommand command;
  command.kind        = EditorMaskCreationCommandKind::BeginCreation;
  command.source_kind = kind;
  command.node_id     = CurrentGradeId();
  if (command.node_id.Empty() || !Enqueue(command)) {
    return;
  }
  source_kind_     = kind;
  tool_kind_       = tool_kind;
  creating_        = true;
  selected_        = false;
  open_            = false;
  overlay_source_.reset();
  overlay_display_ = {};
  HideOverlay();
  emit MaskCreationChanged();
}

void EditorMaskCreationAdapter::cancel() {
  if (!active()) {
    return;
  }
  (void)Enqueue(EditorMaskCreationCommand{EditorMaskCreationCommandKind::CancelMode});
  ResetLocal();
}

void EditorMaskCreationAdapter::OnImageClosed() {
  ResetLocal();
}

void EditorMaskCreationAdapter::ResetLocal() {
  const bool changed = active() || open_ || creating_ || selected_;
  tool_kind_.clear();
  creating_ = false;
  open_     = false;
  selected_ = false;
  overlay_source_.reset();
  overlay_display_ = {};
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

void EditorMaskCreationAdapter::PublishOverlay() {
  if (overlay_ == nullptr || interaction_ == nullptr || !overlay_source_.has_value()) {
    HideOverlay();
    return;
  }
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
  }
  overlay_display_ = display;
  overlay_->setMaskOverlayDisplay(std::move(display));
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

  const auto hit = HitTestMaskOverlayHandle(overlay_display_, QPointF(x, y),
                                            OverlayStyle().hit_radius_logical_px);
  const auto handle = AnalyticHandleFromOverlay(hit);
  if (handle != AnalyticMaskHandle::None && selected_ && session_ != nullptr) {
    EditorMaskCreationCommand select;
    select.kind    = EditorMaskCreationCommandKind::SelectMask;
    select.node_id = CurrentGradeId();
    select.mask_id = session_->mask_creation_mask_id();
    if (!select.mask_id.Empty()) {
      (void)Enqueue(select);
      EditorMaskCreationCommand move;
      move.kind     = EditorMaskCreationCommandKind::BeginMove;
      move.handle   = handle;
      move.sample   = *sample;
      move.identity = pointer_;
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
    EditorMaskCreationCommand begin;
    begin.kind        = EditorMaskCreationCommandKind::BeginCreation;
    begin.source_kind = source_kind_;
    begin.node_id     = CurrentGradeId();
    if (!Enqueue(begin)) {
      return true;
    }
    creating_ = true;
  }
  EditorMaskCreationCommand input;
  input.kind     = EditorMaskCreationCommandKind::BeginInput;
  input.sample   = *sample;
  input.identity = pointer_;
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
  EditorMaskCreationCommand command;
  command.kind     = EditorMaskCreationCommandKind::Append;
  command.sample   = *sample;
  command.identity = pointer_;
  (void)Enqueue(command);
  if (creating_) {
    overlay_source_ = source_kind_ == MaskSourceKind::Radial
                          ? MaskSource{RadialFromCenterOut(press_normalized_, sample->normalized)}
                          : MaskSource{LinearFromEndpoints(press_normalized_, sample->normalized)};
  } else if (overlay_source_.has_value() && active_handle_ != AnalyticMaskHandle::None) {
    float unwrapped = 0.0f;
    if (const auto* radial = std::get_if<RadialMaskSource>(&*overlay_source_)) {
      unwrapped = radial->rotation;
    }
    if (auto next = ApplyAnalyticMaskHandle(active_handle_, *overlay_source_, sample->normalized,
                                            unwrapped)) {
      overlay_source_ = std::move(*next);
    }
  }
  PublishOverlay();
  return true;
}

auto EditorMaskCreationAdapter::handleRelease(qreal x, qreal y, int button) -> bool {
  if (!open_ || button != static_cast<int>(Qt::LeftButton)) {
    return owns_left_button() && button == static_cast<int>(Qt::LeftButton);
  }
  const auto sample = MakeSample(x, y, true);
  if (sample) {
    EditorMaskCreationCommand append;
    append.kind     = EditorMaskCreationCommandKind::Append;
    append.sample   = *sample;
    append.identity = pointer_;
    (void)Enqueue(append);
    if (creating_) {
      overlay_source_ =
          source_kind_ == MaskSourceKind::Radial
              ? MaskSource{RadialFromCenterOut(press_normalized_, sample->normalized)}
              : MaskSource{LinearFromEndpoints(press_normalized_, sample->normalized)};
    }
  }
  (void)Enqueue(EditorMaskCreationCommand{EditorMaskCreationCommandKind::Finish});
  open_     = false;
  creating_ = false;
  selected_ = overlay_source_.has_value();
  PublishOverlay();
  emit MaskCreationChanged();
  return true;
}

}  // namespace alcedo::ui
