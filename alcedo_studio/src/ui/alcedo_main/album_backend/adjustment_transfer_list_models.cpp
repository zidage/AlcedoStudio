//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/adjustment_transfer_list_models.hpp"

namespace alcedo::ui {

// ============================================================================
// AdjustmentTransferVersionListModel
// ============================================================================

AdjustmentTransferVersionListModel::AdjustmentTransferVersionListModel(QObject* parent)
    : QAbstractListModel(parent) {}

auto AdjustmentTransferVersionListModel::rowCount(const QModelIndex& parent) const -> int {
  if (parent.isValid()) {
    return 0;
  }
  return count();
}

auto AdjustmentTransferVersionListModel::data(const QModelIndex& index, int role) const
    -> QVariant {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
    return {};
  }
  const auto& row = rows_.at(static_cast<std::size_t>(index.row()));
  switch (role) {
    case VersionIdRole:
      return QString::fromStdString(row.version_id.ToString());
    case DisplayNameRole:
      return QString::fromStdString(row.display_name);
    case CreatedAtRole:
      return static_cast<qlonglong>(row.created_at);
    case UpdatedAtRole:
      return static_cast<qlonglong>(row.updated_at);
    case ActiveRole:
      return row.active;
    case SelectedRole:
      return row.version_id == selected_id_;
    default:
      return {};
  }
}

auto AdjustmentTransferVersionListModel::roleNames() const -> QHash<int, QByteArray> {
  return {
      {VersionIdRole, "versionId"}, {DisplayNameRole, "displayName"}, {CreatedAtRole, "createdAt"},
      {UpdatedAtRole, "updatedAt"}, {ActiveRole, "active"},           {SelectedRole, "selected"},
  };
}

void AdjustmentTransferVersionListModel::SetRows(
    std::vector<alcedo::AdjustmentTransferVersionDescriptor> rows,
    const alcedo::version_ref_id_t&                          selected_id) {
  beginResetModel();
  rows_        = std::move(rows);
  selected_id_ = selected_id;
  endResetModel();
  emit CountChanged();
}

void AdjustmentTransferVersionListModel::SetSelected(const alcedo::version_ref_id_t& selected_id) {
  if (selected_id == selected_id_) {
    return;
  }
  const int previous = IndexOf(selected_id_);
  selected_id_       = selected_id;
  const int next     = IndexOf(selected_id_);
  for (const int row : {previous, next}) {
    if (row >= 0) {
      const auto model_index = index(row);
      emit       dataChanged(model_index, model_index, {SelectedRole});
    }
  }
}

auto AdjustmentTransferVersionListModel::IndexOf(const alcedo::version_ref_id_t& id) const -> int {
  for (std::size_t row = 0; row < rows_.size(); ++row) {
    if (rows_[row].version_id == id) {
      return static_cast<int>(row);
    }
  }
  return -1;
}

// ============================================================================
// AdjustmentTransferNodeListModel
// ============================================================================

AdjustmentTransferNodeListModel::AdjustmentTransferNodeListModel(QObject* parent)
    : QAbstractListModel(parent) {}

auto AdjustmentTransferNodeListModel::rowCount(const QModelIndex& parent) const -> int {
  if (parent.isValid()) {
    return 0;
  }
  return count();
}

auto AdjustmentTransferNodeListModel::data(const QModelIndex& index, int role) const -> QVariant {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
    return {};
  }
  const auto& row = rows_.at(static_cast<std::size_t>(index.row()));
  switch (role) {
    case NodeIdRole:
      return QString::fromStdString(std::string{row.node_id.Value()});
    case DisplayNameRole:
      return row.display_name;
    case NodeKindRole:
      return row.node_kind;
    case DefaultGradeRole:
      return row.default_grade;
    case CheckStateRole:
      return row.check_state;
    case FocusedRole:
      return row.focused;
    default:
      return {};
  }
}

auto AdjustmentTransferNodeListModel::roleNames() const -> QHash<int, QByteArray> {
  return {
      {NodeIdRole, "nodeId"},         {DisplayNameRole, "displayName"},
      {NodeKindRole, "nodeKind"},     {DefaultGradeRole, "defaultGrade"},
      {CheckStateRole, "checkState"}, {FocusedRole, "focused"},
  };
}

void AdjustmentTransferNodeListModel::SetRows(std::vector<Row> rows) {
  beginResetModel();
  rows_ = std::move(rows);
  endResetModel();
  emit CountChanged();
}

void AdjustmentTransferNodeListModel::UpdateRow(int index_value, int check_state, bool focused) {
  if (index_value < 0 || index_value >= static_cast<int>(rows_.size())) {
    return;
  }
  auto& row              = rows_.at(static_cast<std::size_t>(index_value));
  row.check_state        = check_state;
  row.focused            = focused;
  const auto model_index = index(index_value);
  emit       dataChanged(model_index, model_index, {CheckStateRole, FocusedRole});
}

void AdjustmentTransferNodeListModel::SetAllCheckStates(int check_state) {
  if (rows_.empty()) {
    return;
  }
  for (auto& row : rows_) {
    row.check_state = check_state;
  }
  emit dataChanged(index(0), index(static_cast<int>(rows_.size()) - 1), {CheckStateRole});
}

auto AdjustmentTransferNodeListModel::IndexOfNode(const alcedo::NodeId& id) const -> int {
  for (std::size_t row = 0; row < rows_.size(); ++row) {
    if (rows_[row].node_id == id) {
      return static_cast<int>(row);
    }
  }
  return -1;
}

// ============================================================================
// AdjustmentTransferItemListModel
// ============================================================================

AdjustmentTransferItemListModel::AdjustmentTransferItemListModel(QObject* parent)
    : QAbstractListModel(parent) {}

auto AdjustmentTransferItemListModel::rowCount(const QModelIndex& parent) const -> int {
  if (parent.isValid()) {
    return 0;
  }
  return count();
}

auto AdjustmentTransferItemListModel::data(const QModelIndex& index, int role) const -> QVariant {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
    return {};
  }
  const auto& row = rows_.at(static_cast<std::size_t>(index.row()));
  switch (role) {
    case ItemKeyRole:
      return row.key;
    case DisplayNameRole:
      return row.display_name;
    case DisplayValueRole:
      return row.display_value;
    case ItemSectionRole:
      return row.item_section;
    case ItemKindRole:
      return row.item_kind;
    case CheckedRole:
      return row.checked;
    case EnabledRole:
      return row.enabled;
    default:
      return {};
  }
}

auto AdjustmentTransferItemListModel::roleNames() const -> QHash<int, QByteArray> {
  return {
      {ItemKeyRole, "itemKey"},           {DisplayNameRole, "displayName"},
      {DisplayValueRole, "displayValue"}, {ItemSectionRole, "itemSection"},
      {ItemKindRole, "itemKind"},         {CheckedRole, "checked"},
      {EnabledRole, "enabled"},
  };
}

void AdjustmentTransferItemListModel::SetRows(std::vector<Row> rows) {
  beginResetModel();
  rows_ = std::move(rows);
  endResetModel();
  emit CountChanged();
}

void AdjustmentTransferItemListModel::SetChecked(int index_value, bool checked) {
  if (index_value < 0 || index_value >= static_cast<int>(rows_.size())) {
    return;
  }
  auto& row              = rows_.at(static_cast<std::size_t>(index_value));
  row.checked            = checked;
  const auto model_index = index(index_value);
  emit       dataChanged(model_index, model_index, {CheckedRole});
}

void AdjustmentTransferItemListModel::SetAllChecked(bool checked) {
  if (rows_.empty()) {
    return;
  }
  for (auto& row : rows_) {
    if (row.enabled) {
      row.checked = checked;
    }
  }
  emit dataChanged(index(0), index(static_cast<int>(rows_.size()) - 1), {CheckedRole});
}

auto AdjustmentTransferItemListModel::IndexOfKey(const QString& key) const -> int {
  for (std::size_t row = 0; row < rows_.size(); ++row) {
    if (rows_[row].key == key) {
      return static_cast<int>(row);
    }
  }
  return -1;
}

}  // namespace alcedo::ui
