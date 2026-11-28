//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QString>
#include <cstddef>
#include <vector>

#include "app/preset_library_service.hpp"

namespace alcedo::ui {

/**
 * @brief Rows of the preset browser over the inventory of PresetLibraryService (edit presets
 *        plan, PS2).
 *
 * The service owns the entries; the model keeps only their display order (the folder itself
 * first, then each group by name, each name in case-insensitive order) and resets on each
 * InventoryChanged. No role reads a preset file. Owner thread only.
 */
class PresetListModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ rowCount NOTIFY CountChanged)

 public:
  enum Role {
    PresetIdRole = Qt::UserRole + 1,
    NameRole,
    GroupRole,
    AvailableRole,
    ReasonRole,
    FingerprintRole,
  };
  Q_ENUM(Role)

  explicit PresetListModel(alcedo::PresetLibraryService* library, QObject* parent = nullptr);

  [[nodiscard]] auto   rowCount(const QModelIndex& parent = QModelIndex()) const -> int override;
  [[nodiscard]] auto   data(const QModelIndex& index, int role) const -> QVariant override;
  [[nodiscard]] auto   roleNames() const -> QHash<int, QByteArray> override;

  /// Row of the preset @p presetId, or -1.
  Q_INVOKABLE int      rowOfPreset(const QString& presetId) const;
  /// The value of @p role at @p row, or an invalid value; for QML tiles that read by row.
  Q_INVOKABLE QVariant roleAt(int row, int role) const;

 signals:
  void CountChanged();

 private:
  void                                   Reset();
  [[nodiscard]] auto                     EntryAt(int row) const -> const alcedo::PresetEntry*;

  QPointer<alcedo::PresetLibraryService> library_;
  /// Indexes into PresetLibraryService::Entries() in display order; valid until InventoryChanged.
  std::vector<std::size_t>               order_;
};

}  // namespace alcedo::ui
