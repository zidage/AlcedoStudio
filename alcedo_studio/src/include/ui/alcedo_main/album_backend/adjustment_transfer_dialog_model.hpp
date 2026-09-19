//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/adjustment_transfer_catalog.hpp"
#include "app/adjustment_transfer_types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_list_models.hpp"

namespace alcedo::ui {

/**
 * @brief Qt-facing owner of the Adjustment Transfer Copy dialog state.
 *
 * Owns the current source Version id, the immutable replayed source document,
 * node focus, and every item's checked state. Node and bulk checkbox states
 * are derived from child item states; QML sends commands with stable identity
 * and never edits checked roles directly. The model does not paste or persist
 * a target image.
 */
class AdjustmentTransferDialogModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(alcedo::ui::AdjustmentTransferVersionListModel* versions READ versions CONSTANT)
  Q_PROPERTY(alcedo::ui::AdjustmentTransferNodeListModel* nodes READ nodes CONSTANT)
  Q_PROPERTY(alcedo::ui::AdjustmentTransferItemListModel* items READ items CONSTANT)
  Q_PROPERTY(QString selectedVersionId READ selected_version_id NOTIFY StateChanged)
  Q_PROPERTY(QString selectedVersionName READ selected_version_name NOTIFY StateChanged)
  Q_PROPERTY(QString focusedNodeId READ focused_node_id NOTIFY StateChanged)
  Q_PROPERTY(bool canCopy READ can_copy NOTIFY StateChanged)
  Q_PROPERTY(int allNodesCheckState READ all_nodes_check_state NOTIFY StateChanged)
  Q_PROPERTY(int focusedItemsCheckState READ focused_items_check_state NOTIFY StateChanged)
  Q_PROPERTY(QString errorText READ error_text NOTIFY StateChanged)

 public:
  explicit AdjustmentTransferDialogModel(QObject* parent = nullptr);
  ~AdjustmentTransferDialogModel() override = default;

  /**
   * @brief Open one source image catalog.
   *
   * Lists Versions from @p graph, replays the active Version through the
   * read-only catalog service, and publishes Version, node, and item rows with
   * every transferable item selected. The live guard is never touched; the
   * model retains shared ownership of the graph and immutable root document.
   *
   * A replay failure of the initial Version still returns true: the dialog
   * opens with an empty node column and @p error_text_ set so the user can
   * pick another Version. Returns false only when no source catalog exists.
   */
  auto OpenSource(std::shared_ptr<const alcedo::CommitGraph>      graph,
                  std::shared_ptr<const alcedo::PipelineDocument> root_document, std::string* error)
      -> bool;

  // Commands invoked by QML delegates with stable identities.
  Q_INVOKABLE void   SelectVersion(const QString& versionId);
  Q_INVOKABLE void   FocusNode(const QString& nodeId);
  Q_INVOKABLE void   SetNodeChecked(const QString& nodeId, bool checked);
  Q_INVOKABLE void   SetItemChecked(const QString& nodeId, const QString& itemKey, bool checked);
  Q_INVOKABLE void   SetAllNodesChecked(bool checked);
  Q_INVOKABLE void   SetAllFocusedNodeItemsChecked(bool checked);
  Q_INVOKABLE void   ClearAll();
  Q_INVOKABLE void   ClearFocusedNode();

  // List model accessors (Q_PROPERTY reads + tests).
  [[nodiscard]] auto versions() -> AdjustmentTransferVersionListModel* { return versions_; }
  [[nodiscard]] auto nodes() -> AdjustmentTransferNodeListModel* { return nodes_; }
  [[nodiscard]] auto items() -> AdjustmentTransferItemListModel* { return items_; }

  // Scalar state (Q_PROPERTY reads + tests).
  [[nodiscard]] auto selected_version_id() const -> QString;
  [[nodiscard]] auto selected_version_name() const -> QString;
  [[nodiscard]] auto focused_node_id() const -> QString;
  [[nodiscard]] auto can_copy() const -> bool;
  [[nodiscard]] auto all_nodes_check_state() const -> int;
  [[nodiscard]] auto focused_items_check_state() const -> int;
  [[nodiscard]] auto error_text() const -> QString { return error_text_; }
  [[nodiscard]] auto has_source() const -> bool { return read_.has_value(); }

  /// Stable typed selection of every checked, enabled item.
  [[nodiscard]] auto BuildSelection() const -> alcedo::AdjustmentTransferSelection;
  /// Validated v6 package for the current selection. A failed build leaves the
  /// caller's prior package untouched; @p error receives the exact reason.
  [[nodiscard]] auto BuildPackage(std::string* error) const
      -> std::optional<alcedo::AdjustmentTransferPackage>;
  /// Read-only paste-summary rows: one row per selected item, grouped under
  /// its node's display name. Used by the existing flat paste list.
  [[nodiscard]] auto SelectionSummary() const -> QVariantList;

  /// Test-only read of one node's derived Qt::CheckState value.
  [[nodiscard]] auto NodeCheckStateForTesting(const alcedo::NodeId& node_id) const -> int;

 signals:
  void StateChanged();

 private:
  struct ItemState {
    alcedo::AdjustmentTransferItemDescriptor descriptor;
    /// Opaque identity QML hands back to SetItemChecked.
    QString                                  key;
    bool                                     checked = true;
  };
  struct NodeState {
    alcedo::AdjustmentTransferNodeDescriptor descriptor;
    std::vector<ItemState>                   items;
  };

  void                      ApplyRead(alcedo::AdjustmentTransferCatalogRead read);
  void                      PublishNodeRows();
  void                      PublishFocusedItems();
  void                      PublishDerivedStates();
  void                      SetNodeItemsChecked(std::size_t node_index, bool checked);
  void                      SetError(const QString& text);

  [[nodiscard]] auto        FindNode(const alcedo::NodeId& id) const -> std::optional<std::size_t>;
  [[nodiscard]] static auto ItemKeyFor(const alcedo::AdjustmentTransferItemDescriptor& descriptor)
      -> QString;
  [[nodiscard]] static auto NodeCheckState(const NodeState& node) -> int;
  [[nodiscard]] static auto NodeRowFor(const NodeState& node, bool focused)
      -> AdjustmentTransferNodeListModel::Row;

  AdjustmentTransferVersionListModel*                  versions_ = nullptr;
  AdjustmentTransferNodeListModel*                     nodes_    = nullptr;
  AdjustmentTransferItemListModel*                     items_    = nullptr;

  std::shared_ptr<const alcedo::CommitGraph>           graph_;
  std::shared_ptr<const alcedo::PipelineDocument>      root_document_;
  std::optional<alcedo::AdjustmentTransferCatalogRead> read_;
  std::vector<NodeState>                               node_states_;
  std::size_t                                          focused_node_ = 0;
  QString                                              error_text_;
};

}  // namespace alcedo::ui
