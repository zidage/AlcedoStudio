//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/ai_mask_input_overlay_item.hpp"

#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QTransform>
#include <array>
#include <optional>
#include <utility>

#include "ui/edit_viewer/ai_mask_prompt_geometry.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_layout.hpp"

namespace alcedo::editor_rhi {
namespace {

[[nodiscard]] auto PointsOf(const QVariant& value) -> std::vector<QPointF> {
  std::vector<QPointF> points;
  for (const auto& point : value.toList()) {
    points.push_back(point.toPointF());
  }
  return points;
}

[[nodiscard]] auto CornersOf(const QRectF& rect) -> std::array<QPointF, 4> {
  return {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
}

/// Adds one step or live input map to @p display, mapped by @p to_item.
void AppendInput(const QVariantMap& input, const QTransform& to_item,
                 AiMaskInputOverlayDisplay& display) {
  const auto kind = input.value(QStringLiteral("kind")).toString();
  const bool add  = input.value(QStringLiteral("add"), true).toBool();
  if (kind == QLatin1String("box")) {
    const auto box = input.value(QStringLiteral("box")).toRectF().normalized();
    if (box.isEmpty()) {
      return;
    }
    AiMaskInputBox mapped;
    const auto     corners = CornersOf(box);
    for (std::size_t index = 0; index < corners.size(); ++index) {
      mapped.corners[index] = to_item.map(corners[index]);
    }
    mapped.add = add;
    display.boxes.push_back(mapped);
    return;
  }
  auto points = PointsOf(input.value(QStringLiteral("points")));
  if (points.empty()) {
    return;
  }
  for (auto& point : points) {
    point = to_item.map(point);
  }
  if (kind == QLatin1String("path")) {
    display.strokes.push_back(AiMaskInputStroke{std::move(points), add});
    return;
  }
  for (const auto& point : points) {
    display.points.push_back(AiMaskInputPointMarker{point, add});
  }
}

/// Assigns @p value to @p member; returns whether the value changed.
template <typename Member, typename Value>
[[nodiscard]] auto AssignChanged(Member& member, const Value& value) -> bool {
  if (member == value) {
    return false;
  }
  member = value;
  return true;
}

[[nodiscard]] auto CursorKindOf(const QString& kind) -> AiMaskInputCursorKind {
  if (kind == QLatin1String("point")) {
    return AiMaskInputCursorKind::Point;
  }
  if (kind == QLatin1String("box")) {
    return AiMaskInputCursorKind::Box;
  }
  if (kind == QLatin1String("path")) {
    return AiMaskInputCursorKind::Path;
  }
  return AiMaskInputCursorKind::None;
}

}  // namespace

AiMaskInputOverlayItem::AiMaskInputOverlayItem(QQuickItem* parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  setAcceptedMouseButtons(Qt::NoButton);
  setAcceptHoverEvents(false);
  connect(this, &AiMaskInputOverlayItem::displayChanged, this,
          &AiMaskInputOverlayItem::OnDisplayChanged);
  connect(this, &AiMaskInputOverlayItem::styleChanged, this,
          &AiMaskInputOverlayItem::OnStyleChanged);
}

void AiMaskInputOverlayItem::setInteraction(EditorInteractionController* interaction) {
  if (interaction_ == interaction) {
    return;
  }
  if (interaction_ != nullptr) {
    disconnect(interaction_, nullptr, this, nullptr);
  }
  interaction_ = interaction;
  if (interaction_ != nullptr) {
    // Every change of the view mapping moves the mapped prompts.
    for (const auto signal :
         {&EditorInteractionController::viewChanged, &EditorInteractionController::cropChanged,
          &EditorInteractionController::overlayGeometryChanged,
          &EditorInteractionController::imageGeometryChanged,
          &EditorInteractionController::viewportMetricsChanged}) {
      connect(interaction_, signal, this, &AiMaskInputOverlayItem::OnDisplayChanged);
    }
  }
  emit interactionChanged();
  OnDisplayChanged();
}

void AiMaskInputOverlayItem::setPromptSteps(const QVariantList& value) {
  if (AssignChanged(prompt_steps_, value)) {
    emit displayChanged();
  }
}

void AiMaskInputOverlayItem::setLiveInput(const QVariantMap& value) {
  if (AssignChanged(live_input_, value)) {
    emit displayChanged();
  }
}

void AiMaskInputOverlayItem::setPersonBoxes(const QVariantList& value) {
  if (AssignChanged(person_boxes_, value)) {
    emit displayChanged();
  }
}

void AiMaskInputOverlayItem::setHighlightedPersonBox(const QRectF& value) {
  if (AssignChanged(highlighted_person_box_, value)) {
    emit displayChanged();
  }
}

void AiMaskInputOverlayItem::setCursorKind(const QString& value) {
  if (AssignChanged(cursor_kind_, value)) {
    emit displayChanged();
  }
}

void AiMaskInputOverlayItem::setCursorPosition(const QPointF& value) {
  if (AssignChanged(cursor_position_, value)) {
    emit displayChanged();
  }
}

void AiMaskInputOverlayItem::setCursorVisible(bool value) {
  if (AssignChanged(cursor_visible_, value)) {
    emit displayChanged();
  }
}

void AiMaskInputOverlayItem::setCursorAdd(bool value) {
  if (AssignChanged(cursor_add_, value)) {
    emit displayChanged();
  }
}

void AiMaskInputOverlayItem::setCursorBusy(bool value) {
  if (AssignChanged(cursor_busy_, value)) {
    emit displayChanged();
  }
}

void AiMaskInputOverlayItem::setAddColor(const QColor& value) {
  if (AssignChanged(style_.add_color, value)) {
    emit styleChanged();
  }
}

void AiMaskInputOverlayItem::setSubtractColor(const QColor& value) {
  if (AssignChanged(style_.subtract_color, value)) {
    emit styleChanged();
  }
}

void AiMaskInputOverlayItem::setLineColor(const QColor& value) {
  if (AssignChanged(style_.line_color, value)) {
    emit styleChanged();
  }
}

void AiMaskInputOverlayItem::setOutlineColor(const QColor& value) {
  if (AssignChanged(style_.outline_color, value)) {
    emit styleChanged();
  }
}

void AiMaskInputOverlayItem::setInactiveColor(const QColor& value) {
  if (AssignChanged(style_.inactive_color, value)) {
    emit styleChanged();
  }
}

void AiMaskInputOverlayItem::OnDisplayChanged() { polish(); }

void AiMaskInputOverlayItem::OnStyleChanged() { polish(); }

void AiMaskInputOverlayItem::geometryChange(const QRectF& new_geometry,
                                            const QRectF& old_geometry) {
  QQuickItem::geometryChange(new_geometry, old_geometry);
  if (new_geometry.size() != old_geometry.size()) {
    polish();
  }
}

auto AiMaskInputOverlayItem::BuildDisplay() const -> AiMaskInputOverlayDisplay {
  AiMaskInputOverlayDisplay display;
  std::optional<QTransform> analysis_to_item;
  if (interaction_ != nullptr) {
    const auto mapping = interaction_->maskEditViewMapping();
    if (MaskEditGeometry::IsValid(mapping)) {
      analysis_to_item   = AiMaskAnalysisToItemTransform(mapping);
      display.photo_rect = PhotographItemRect(mapping);
    }
  }
  if (analysis_to_item.has_value()) {
    for (const auto& step : prompt_steps_) {
      AppendInput(step.toMap(), *analysis_to_item, display);
    }
    std::vector<QRectF> boxes;
    for (const auto& box : person_boxes_) {
      boxes.push_back(box.toRectF());
    }
    display.person_outlines = AiMaskPersonUnionOutlines(boxes, *analysis_to_item);
    if (!highlighted_person_box_.isEmpty()) {
      for (const auto& corner : CornersOf(highlighted_person_box_.normalized())) {
        display.highlighted_person.push_back(analysis_to_item->map(corner));
      }
    }
  }
  if (!live_input_.isEmpty()) {
    AppendInput(live_input_, QTransform{}, display);
  }
  if (cursor_visible_) {
    display.cursor      = CursorKindOf(cursor_kind_);
    display.cursor_item = cursor_position_;
    display.cursor_add  = cursor_add_;
    display.cursor_busy = cursor_busy_;
  }
  return display;
}

void AiMaskInputOverlayItem::updatePolish() {
  last_display_    = BuildDisplay();
  triangles_       = BuildAiMaskInputOverlayTriangles(last_display_, style_);
  triangles_dirty_ = true;
  emit trianglesBuilt();
  update();
}

auto AiMaskInputOverlayItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
    -> QSGNode* {
  auto* node = static_cast<QSGGeometryNode*>(old_node);
  if (triangles_.empty()) {
    delete node;
    triangles_dirty_ = false;
    return nullptr;
  }
  if (node == nullptr) {
    node           = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                     static_cast<int>(triangles_.size()));
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlag(QSGNode::OwnsMaterial);
    triangles_dirty_ = true;
  }
  if (triangles_dirty_) {
    auto* geometry = node->geometry();
    geometry->allocate(static_cast<int>(triangles_.size()));
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    for (std::size_t index = 0; index < triangles_.size(); ++index) {
      const auto& vertex = triangles_[index];
      vertices[index].set(vertex.x, vertex.y, vertex.r, vertex.g, vertex.b, vertex.a);
    }
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);
    triangles_dirty_ = false;
  }
  return node;
}

}  // namespace alcedo::editor_rhi
