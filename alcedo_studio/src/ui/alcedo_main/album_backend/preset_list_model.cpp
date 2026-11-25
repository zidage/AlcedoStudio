//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/preset_list_model.hpp"

#include <algorithm>
#include <numeric>

namespace alcedo::ui {

PresetListModel::PresetListModel(alcedo::PresetLibraryService* library, QObject* parent)
    : QAbstractListModel(parent), library_(library) {
  if (library_ != nullptr) {
    connect(library_, &alcedo::PresetLibraryService::InventoryChanged, this,
            &PresetListModel::Reset);
  }
  Reset();
}

void PresetListModel::Reset() {
  beginResetModel();
  order_.clear();
  if (library_ != nullptr) {
    const auto& entries = library_->Entries();
    order_.resize(entries.size());
    std::iota(order_.begin(), order_.end(), std::size_t{0});
    const auto key = [&entries](std::size_t index) {
      const auto& entry = entries[index];
      return std::pair{QString::fromStdString(entry.group).toCaseFolded(),
                       QString::fromStdString(entry.name).toCaseFolded()};
    };
    // The folder itself (empty group) sorts first; then the groups by name.
    std::stable_sort(order_.begin(), order_.end(), [&key](std::size_t left, std::size_t right) {
      return key(left) < key(right);
    });
  }
  endResetModel();
  emit CountChanged();
}

auto PresetListModel::EntryAt(int row) const -> const alcedo::PresetEntry* {
  if (library_ == nullptr || row < 0 || static_cast<std::size_t>(row) >= order_.size()) {
    return nullptr;
  }
  const auto& entries = library_->Entries();
  const auto  index   = order_[static_cast<std::size_t>(row)];
  return index < entries.size() ? &entries[index] : nullptr;
}

auto PresetListModel::rowCount(const QModelIndex& parent) const -> int {
  return parent.isValid() ? 0 : static_cast<int>(order_.size());
}

auto PresetListModel::data(const QModelIndex& index, int role) const -> QVariant {
  const auto* entry = EntryAt(index.row());
  if (entry == nullptr) {
    return {};
  }
  switch (role) {
    case PresetIdRole:
      return QString::fromStdString(entry->id);
    case Qt::DisplayRole:
    case NameRole:
      return QString::fromStdString(entry->name);
    case GroupRole:
      return QString::fromStdString(entry->group);
    case AvailableRole:
      return entry->available;
    case ReasonRole:
      return QString::fromStdString(entry->reason);
    case FingerprintRole:
      return QString::fromStdString(entry->fingerprint);
    default:
      return {};
  }
}

auto PresetListModel::roleNames() const -> QHash<int, QByteArray> {
  return {{PresetIdRole, "presetId"},   {NameRole, "name"},     {GroupRole, "group"},
          {AvailableRole, "available"}, {ReasonRole, "reason"}, {FingerprintRole, "fingerprint"}};
}

auto PresetListModel::rowOfPreset(const QString& presetId) const -> int {
  const auto id = presetId.toStdString();
  for (int row = 0; row < rowCount(); ++row) {
    if (EntryAt(row)->id == id) {
      return row;
    }
  }
  return -1;
}

auto PresetListModel::roleAt(int row, int role) const -> QVariant {
  return data(index(row, 0), role);
}

}  // namespace alcedo::ui
