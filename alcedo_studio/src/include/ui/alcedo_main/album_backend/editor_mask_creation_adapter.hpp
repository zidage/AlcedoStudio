//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <cstdint>
#include <optional>

#include "app/editor_mask_creation_controller.hpp"
#include "edit/mask/mask_model.hpp"
#include "ui/edit_viewer/mask_overlay_geometry.hpp"

namespace alcedo::editor_rhi {
class EditorInteractionController;
class EditorOverlayItem;
}  // namespace alcedo::editor_rhi

namespace alcedo::ui {

class EditorNodeController;
class EditorSessionController;

/**
 * @brief QML adapter for Radial/Linear Mask creation and existing-mask movement.
 *
 * Maps item pointers, publishes control-only overlay geometry, and queues
 * owner-thread Mask commands. Does not take the pipeline lock.
 */
class EditorMaskCreationAdapter : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY MaskCreationChanged)
  Q_PROPERTY(bool creating READ creating NOTIFY MaskCreationChanged)
  Q_PROPERTY(bool ownsLeftButton READ owns_left_button NOTIFY MaskCreationChanged)
  Q_PROPERTY(QString toolKind READ tool_kind NOTIFY MaskCreationChanged)

 public:
  explicit EditorMaskCreationAdapter(EditorSessionController* session, QObject* parent = nullptr);
  ~EditorMaskCreationAdapter() override;

  [[nodiscard]] auto active() const -> bool { return !tool_kind_.isEmpty(); }
  [[nodiscard]] auto creating() const -> bool { return creating_; }
  [[nodiscard]] auto owns_left_button() const -> bool { return active(); }
  [[nodiscard]] auto tool_kind() const -> QString { return tool_kind_; }

  Q_INVOKABLE void bindInteractionItem(QObject* interaction);
  Q_INVOKABLE void bindOverlayItem(QObject* overlay);
  Q_INVOKABLE void beginRadial();
  Q_INVOKABLE void beginLinear();
  Q_INVOKABLE void cancel();
  Q_INVOKABLE bool handlePress(qreal x, qreal y, int button);
  Q_INVOKABLE bool handleMove(qreal x, qreal y, int buttons);
  Q_INVOKABLE bool handleRelease(qreal x, qreal y, int button);

  void OnImageClosed();

 signals:
  void MaskCreationChanged();

 private:
  void BeginTool(MaskSourceKind kind, const QString& tool_kind);
  void ResetLocal();
  void PublishOverlay();
  void HideOverlay();
  [[nodiscard]] auto CurrentGradeId() const -> NodeId;
  [[nodiscard]] auto CanAuthorMasks() const -> bool;
  [[nodiscard]] auto MakeSample(qreal x, qreal y, bool allow_outside) const
      -> std::optional<MaskCreationSample>;
  [[nodiscard]] auto OverlayClip() const -> QRectF;
  [[nodiscard]] auto OverlayStyle() const -> MaskOverlayStyle;
  [[nodiscard]] auto Enqueue(EditorMaskCreationCommand command) -> bool;
  void ConnectInteraction(editor_rhi::EditorInteractionController* interaction);

  QPointer<EditorSessionController>                 session_;
  QPointer<editor_rhi::EditorInteractionController> interaction_;
  QPointer<editor_rhi::EditorOverlayItem>           overlay_;
  QMetaObject::Connection                           view_change_connection_;
  QString                                           tool_kind_;
  MaskSourceKind                                    source_kind_ = MaskSourceKind::Radial;
  bool                                              creating_    = false;
  bool                                              open_        = false;
  bool                                              selected_    = false;
  MaskPointerIdentity                               pointer_{};
  Vector2                                           press_normalized_{};
  std::optional<MaskSource>                         overlay_source_;
  MaskOverlayDisplay                                overlay_display_{};
  AnalyticMaskHandle                                active_handle_ = AnalyticMaskHandle::None;
  std::uint64_t                                     next_sequence_id_ = 1;
};

}  // namespace alcedo::ui
