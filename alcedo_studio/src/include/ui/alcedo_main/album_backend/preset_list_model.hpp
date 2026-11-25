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

#include "app/adjustment_transfer_types.hpp"
#include "app/preset_library_service.hpp"

namespace alcedo::ui {

/// Plain-text summary of @p package for a preset tooltip: one line for each node,
/// `<node>: <item>, <item>`, with the node and item names of the Copy dialog.
[[nodiscard]] auto PresetPackageSummaryText(const alcedo::AdjustmentTransferPackage& package)
    -> QString;

/**
 * @brief Rows of the preset browser over the inventory of PresetLibraryService (edit presets
 *        plan, PS2).
 *
 * The service owns the entries; the model keeps only their display order (the folder itself
 * first, then each group by name, each name in case-insensitive order) and resets on each
 * InventoryChanged. The summary role reads the package through PresetLibraryService::PackageFor,
 * so it costs a file read only when the package cache does not hold it; QML reads it when a
 * tooltip opens. Owner thread only.
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
    SummaryRole,
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
