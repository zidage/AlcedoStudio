//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>
#include <vector>

#include "app/adjustment_transfer_catalog.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/history/commit_types.hpp"

namespace alcedo::ui {

/**
 * @brief Source-Version rows for the Adjustment Transfer Copy dialog.
 *
 * Row data is a read-only projection owned by AdjustmentTransferDialogModel.
 * The selected marker moves only through SetSelected; QML never writes rows.
 */
class AdjustmentTransferVersionListModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY CountChanged)

 public:
  enum Role {
    VersionIdRole = Qt::UserRole + 1,
    DisplayNameRole,
    CreatedAtRole,
    UpdatedAtRole,
    ActiveRole,
    SelectedRole,
  };

  explicit AdjustmentTransferVersionListModel(QObject* parent = nullptr);

  [[nodiscard]] auto rowCount(const QModelIndex& parent = {}) const -> int override;
  [[nodiscard]] auto data(const QModelIndex& index, int role) const -> QVariant override;
  [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;
  [[nodiscard]] auto count() const -> int { return static_cast<int>(rows_.size()); }

  /// Replace every row (model reset). @p selected_id marks the selected row.
  void               SetRows(std::vector<alcedo::AdjustmentTransferVersionDescriptor> rows,
                             const alcedo::version_ref_id_t&                          selected_id);
  /// Move the selected marker; emits dataChanged on the two affected rows.
  void               SetSelected(const alcedo::version_ref_id_t& selected_id);

 signals:
  void CountChanged();

 private:
  [[nodiscard]] auto IndexOf(const alcedo::version_ref_id_t& id) const -> int;

  std::vector<alcedo::AdjustmentTransferVersionDescriptor> rows_;
  alcedo::version_ref_id_t                                 selected_id_{};
};

/**
 * @brief Transferable-node rows for the Adjustment Transfer Copy dialog.
 *
 * One row per Color Grade on the source backbone plus one DRT/Post row last.
 * @ref Row::check_state carries a Qt::CheckState value derived from the owning
 * dialog model's item states; focus is a row flag, not selection state.
 */
class AdjustmentTransferNodeListModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY CountChanged)

 public:
  enum Role {
    NodeIdRole = Qt::UserRole + 1,
    DisplayNameRole,
    NodeKindRole,
    DefaultGradeRole,
    CheckStateRole,
    FocusedRole,
  };

  /// Read-only row projection pushed by the owning dialog model.
  struct Row {
    alcedo::NodeId node_id;
    QString        display_name;
    int            node_kind     = 0;
    bool           default_grade = false;
    /// Qt::CheckState value derived from child item states.
    int            check_state   = 0;
    bool           focused       = false;
  };

  explicit AdjustmentTransferNodeListModel(QObject* parent = nullptr);

  [[nodiscard]] auto rowCount(const QModelIndex& parent = {}) const -> int override;
  [[nodiscard]] auto data(const QModelIndex& index, int role) const -> QVariant override;
  [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;
  [[nodiscard]] auto count() const -> int { return static_cast<int>(rows_.size()); }

  void               SetRows(std::vector<Row> rows);
  /// Update one row's derived state and focus flag; emits a single-row
  /// dataChanged so the ListView keeps its scroll position.
  void               UpdateRow(int index, int check_state, bool focused);
  /// Bulk-state update used by Select All / Clear; emits one range dataChanged.
  void               SetAllCheckStates(int check_state);
  [[nodiscard]] auto IndexOfNode(const alcedo::NodeId& id) const -> int;

 signals:
  void CountChanged();

 private:
  std::vector<Row> rows_;
};

/**
 * @brief Transferable-item rows of the focused node.
 *
 * The owning dialog model swaps the whole row set when focus changes and emits
 * per-row dataChanged for checkbox edits so the ListView keeps its position.
 */
class AdjustmentTransferItemListModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY CountChanged)

 public:
  enum Role {
    ItemKeyRole = Qt::UserRole + 1,
    DisplayNameRole,
    DisplayValueRole,
    ItemSectionRole,
    ItemKindRole,
    CheckedRole,
    EnabledRole,
  };

  /// Read-only row projection pushed by the owning dialog model. @p key is the
  /// opaque item identity QML hands back to SetItemChecked commands.
  struct Row {
    QString key;
    QString display_name;
    QString display_value;
    int     item_section = 0;
    int     item_kind    = 0;
    bool    checked      = false;
    bool    enabled      = true;
  };

  explicit AdjustmentTransferItemListModel(QObject* parent = nullptr);

  [[nodiscard]] auto rowCount(const QModelIndex& parent = {}) const -> int override;
  [[nodiscard]] auto data(const QModelIndex& index, int role) const -> QVariant override;
  [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;
  [[nodiscard]] auto count() const -> int { return static_cast<int>(rows_.size()); }

  void               SetRows(std::vector<Row> rows);
  /// Single-row checked update; emits dataChanged on that row only.
  void               SetChecked(int index, bool checked);
  /// Bulk checked update; emits one range dataChanged.
  void               SetAllChecked(bool checked);
  [[nodiscard]] auto IndexOfKey(const QString& key) const -> int;

 signals:
  void CountChanged();

 private:
  std::vector<Row> rows_;
};

}  // namespace alcedo::ui
