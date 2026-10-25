//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

#include "ui/edit_viewer/ai_mask_input_overlay_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_geometry.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"

namespace alcedo::editor_rhi {

/**
 * @brief QSG item that draws the AI Mask tool input over the photograph (SM9): the prompt
 *        markers of the open prompt group, the stroke or box that the pointer draws, the outline
 *        of the selected people, and the tool cursor.
 *
 * The AI Mask tool supplies normalized analysis image prompts (@ref promptSteps, @ref
 * personBoxes); the item maps them through the current view of @ref interaction, so a zoom or
 * pan moves them with the photograph. @ref liveInput is in item coordinates. The item takes no
 * pointer input and sets no cursor; the viewport hides the platform cursor while the tool cursor
 * shows.
 *
 * A step or live input map has `kind` ("point", "path", or "box"), `add` (its effect on the
 * Mask), `points` (a list of QPointF), and `box` (a QRectF).
 *
 * Thread: GUI. @c updatePolish builds the triangles; @c updatePaintNode copies them.
 */
class AiMaskInputOverlayItem : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(EditorInteractionController* interaction READ interaction WRITE setInteraction NOTIFY
                 interactionChanged)
  Q_PROPERTY(QVariantList promptSteps READ promptSteps WRITE setPromptSteps NOTIFY displayChanged)
  Q_PROPERTY(QVariantMap liveInput READ liveInput WRITE setLiveInput NOTIFY displayChanged)
  Q_PROPERTY(QVariantList personBoxes READ personBoxes WRITE setPersonBoxes NOTIFY displayChanged)
  Q_PROPERTY(QRectF highlightedPersonBox READ highlightedPersonBox WRITE setHighlightedPersonBox
                 NOTIFY displayChanged)
  Q_PROPERTY(QString cursorKind READ cursorKind WRITE setCursorKind NOTIFY displayChanged)
  Q_PROPERTY(
      QPointF cursorPosition READ cursorPosition WRITE setCursorPosition NOTIFY displayChanged)
  Q_PROPERTY(bool cursorVisible READ cursorVisible WRITE setCursorVisible NOTIFY displayChanged)
  Q_PROPERTY(bool cursorAdd READ cursorAdd WRITE setCursorAdd NOTIFY displayChanged)
  Q_PROPERTY(bool cursorBusy READ cursorBusy WRITE setCursorBusy NOTIFY displayChanged)
  Q_PROPERTY(QColor addColor READ addColor WRITE setAddColor NOTIFY styleChanged)
  Q_PROPERTY(QColor subtractColor READ subtractColor WRITE setSubtractColor NOTIFY styleChanged)
  Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor NOTIFY styleChanged)
  Q_PROPERTY(QColor outlineColor READ outlineColor WRITE setOutlineColor NOTIFY styleChanged)
  Q_PROPERTY(QColor inactiveColor READ inactiveColor WRITE setInactiveColor NOTIFY styleChanged)
  /// Number of triangle vertices of the last build (tests).
  Q_PROPERTY(int vertexCount READ vertexCount NOTIFY trianglesBuilt)

 public:
  explicit AiMaskInputOverlayItem(QQuickItem* parent = nullptr);

  [[nodiscard]] auto interaction() const -> EditorInteractionController* { return interaction_; }
  void               setInteraction(EditorInteractionController* interaction);

  [[nodiscard]] auto promptSteps() const -> QVariantList { return prompt_steps_; }
  void               setPromptSteps(const QVariantList& steps);
  [[nodiscard]] auto liveInput() const -> QVariantMap { return live_input_; }
  void               setLiveInput(const QVariantMap& input);
  [[nodiscard]] auto personBoxes() const -> QVariantList { return person_boxes_; }
  void               setPersonBoxes(const QVariantList& boxes);
  [[nodiscard]] auto highlightedPersonBox() const -> QRectF { return highlighted_person_box_; }
  void               setHighlightedPersonBox(const QRectF& box);
  [[nodiscard]] auto cursorKind() const -> QString { return cursor_kind_; }
  void               setCursorKind(const QString& kind);
  [[nodiscard]] auto cursorPosition() const -> QPointF { return cursor_position_; }
  void               setCursorPosition(const QPointF& position);
  [[nodiscard]] auto cursorVisible() const -> bool { return cursor_visible_; }
  void               setCursorVisible(bool visible);
  [[nodiscard]] auto cursorAdd() const -> bool { return cursor_add_; }
  void               setCursorAdd(bool add);
  [[nodiscard]] auto cursorBusy() const -> bool { return cursor_busy_; }
  void               setCursorBusy(bool busy);

  [[nodiscard]] auto addColor() const -> QColor { return style_.add_color; }
  void               setAddColor(const QColor& color);
  [[nodiscard]] auto subtractColor() const -> QColor { return style_.subtract_color; }
  void               setSubtractColor(const QColor& color);
  [[nodiscard]] auto lineColor() const -> QColor { return style_.line_color; }
  void               setLineColor(const QColor& color);
  [[nodiscard]] auto outlineColor() const -> QColor { return style_.outline_color; }
  void               setOutlineColor(const QColor& color);
  [[nodiscard]] auto inactiveColor() const -> QColor { return style_.inactive_color; }
  void               setInactiveColor(const QColor& color);

  [[nodiscard]] auto vertexCount() const -> int { return static_cast<int>(triangles_.size()); }
  /// The display of the last build, in item coordinates (tests).
  [[nodiscard]] auto lastDisplay() const -> const AiMaskInputOverlayDisplay& {
    return last_display_;
  }

 signals:
  void interactionChanged();
  void displayChanged();
  void styleChanged();
  void trianglesBuilt();

 protected:
  void updatePolish() override;
  auto updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) -> QSGNode* override;
  void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

 private:
  void                                  OnDisplayChanged();
  void                                  OnStyleChanged();
  [[nodiscard]] auto                    BuildDisplay() const -> AiMaskInputOverlayDisplay;

  QPointer<EditorInteractionController> interaction_;
  QVariantList                          prompt_steps_;
  QVariantMap                           live_input_;
  QVariantList                          person_boxes_;
  QRectF                                highlighted_person_box_;
  QString                               cursor_kind_;
  QPointF                               cursor_position_;
  bool                                  cursor_visible_ = false;
  bool                                  cursor_add_     = true;
  bool                                  cursor_busy_    = false;
  AiMaskInputOverlayStyle               style_{};
  AiMaskInputOverlayDisplay             last_display_{};
  std::vector<MaskOverlayVertex>        triangles_;
  bool                                  triangles_dirty_ = true;
};

}  // namespace alcedo::editor_rhi
