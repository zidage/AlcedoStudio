//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QImage>
#include <QObject>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "app/editor_node_graph_projection.hpp"
#include "app/mask_thumbnail_service.hpp"
#include "app/mask_thumbnail_spec.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_id.hpp"
#include "ui/alcedo_main/album_backend/mask_thumbnail_image_provider.hpp"

namespace alcedo::ui {

/**
 * @brief GUI binding owner for Mask Group thumbnails.
 *
 * Builds @ref MaskThumbnailSpec from committed document state and holds the
 * monotonic request id for each visible target. The service never sees MaskId
 * or NodeId. A result is applied only when the request id and spec still match.
 */
class MaskThumbnailCoordinator final : public QObject {
  Q_OBJECT

 public:
  explicit MaskThumbnailCoordinator(QObject* parent = nullptr);

  void SetService(std::shared_ptr<alcedo::MaskThumbnailService> service);
  void SetImageStore(std::shared_ptr<MaskThumbnailImageStore> store);

  /**
   * @brief Store photograph full-reference size used in specs.
   * @return true when the extent changed.
   */
  auto SetFullReference(alcedo::Extent2D extent) -> bool;

  /// Rebuild bindings from the committed Mask Groups snapshot and document.
  void Sync(const alcedo::PipelineDocument&            document,
            const alcedo::EditorMaskGroupSnapshot&     groups);

  /// Drop every binding. Used on image switch and snapshot clear.
  void Clear();

  /**
   * @brief Bump the target's request id so in-flight pixels cannot paint it.
   *
   * Keeps the displayed URL. Does not cancel the worker. Call @ref requestCurrent
   * or @ref Sync after a failed domain delete so an empty URL can load again.
   */
  Q_INVOKABLE void invalidateTarget(const QString& node_id, const QString& mask_id);
  /// Bump every binding for @p node_id, including the group composite.
  Q_INVOKABLE void invalidateNode(const QString& node_id);
  /**
   * @brief Issue a new request for the target's current spec.
   *
   * No-ops when the spec is unchanged and a URL is already shown. Used after a
   * failed delete so a Loading row can bind again.
   */
  Q_INVOKABLE void requestCurrent(const QString& node_id, const QString& mask_id);

  Q_INVOKABLE [[nodiscard]] QString thumbnailUrl(const QString& node_id,
                                                 const QString& mask_id) const;

  [[nodiscard]] auto request_id(const QString& node_id, const QString& mask_id) const
      -> std::uint64_t;
  [[nodiscard]] auto current_spec(const QString& node_id, const QString& mask_id) const
      -> std::optional<alcedo::MaskThumbnailSpec>;

 signals:
  void thumbnailUrlChanged(const QString& nodeId, const QString& maskId);

 private:
  struct Target {
    alcedo::NodeId node_id;
    alcedo::MaskId mask_id;

    auto operator==(const Target&) const -> bool = default;
  };

  struct TargetHash {
    auto operator()(const Target& target) const noexcept -> std::size_t;
  };

  struct Binding {
    std::uint64_t                              request_id = 0;
    std::optional<alcedo::MaskThumbnailSpec>   spec;
    QString                                    url;
  };

  void Request(const Target& target, alcedo::MaskThumbnailSpec spec);
  void Invalidate(const Target& target);
  void OnResult(const Target& target, std::uint64_t request_id, alcedo::MaskThumbnailSpec spec,
                QImage image, QString error);
  [[nodiscard]] auto MakeGeometry(const alcedo::PipelineDocument& document) const
      -> std::optional<alcedo::MaskThumbnailGeometry>;
  [[nodiscard]] static auto MakeTarget(const QString& node_id, const QString& mask_id) -> Target;

  std::shared_ptr<alcedo::MaskThumbnailService>           service_;
  std::shared_ptr<MaskThumbnailImageStore>                store_;
  alcedo::Extent2D                                        full_reference_{};
  std::uint64_t                                           next_request_id_ = 1;
  std::unordered_map<Target, Binding, TargetHash>         bindings_;
};

}  // namespace alcedo::ui
