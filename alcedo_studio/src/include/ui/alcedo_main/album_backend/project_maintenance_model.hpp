// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVariantList>
#include <unordered_set>
#include <vector>

#include "app/project_maintenance_service.hpp"

namespace alcedo::ui {
/// Bounded review page over service-owned findings. Selection survives filters and pages.
/// UI thread only. Protected or incomplete findings cannot be selected through any API.
class ProjectMaintenanceModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int selectedCount READ SelectedCount NOTIFY Changed)
  Q_PROPERTY(int selectedDuplicates READ SelectedDuplicates NOTIFY Changed)
  Q_PROPERTY(int selectedMissing READ SelectedMissing NOTIFY Changed)
  Q_PROPERTY(int matchingCount READ MatchingCount NOTIFY Changed)
  Q_PROPERTY(int page READ Page WRITE SetPage NOTIFY Changed)
  Q_PROPERTY(QString search READ Search WRITE SetSearch NOTIFY Changed)
  Q_PROPERTY(int filter READ Filter WRITE SetFilter NOTIFY Changed)
 public:
  enum Role {
    EntryId = Qt::UserRole + 1,
    SourcePath,
    Rating,
    EditingWork,
    Finding,
    Reason,
    RetainedId,
    Removable,
    Selected
  };
  explicit ProjectMaintenanceModel(ProjectMaintenanceService& service, QObject* parent = nullptr);
  int                    rowCount(const QModelIndex& parent = {}) const override;
  QVariant               data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;
  /// Detach visible rows and selection before the service starts changing findings.
  void                   Clear();
  /// Publish a completed service scan; starts with no selection.
  void                   Refresh();
  void                   SetReadOnly(bool read_only);
  int                    SelectedCount() const { return static_cast<int>(selected_.size()); }
  int                    SelectedDuplicates() const;
  int                    SelectedMissing() const { return SelectedCount() - SelectedDuplicates(); }
  int                    MatchingCount() const { return static_cast<int>(visible_.size()); }
  int                    Page() const { return page_; }
  void                   SetPage(int page);
  QString                Search() const { return search_; }
  void                   SetSearch(const QString& search);
  int                    Filter() const { return filter_; }
  void                   SetFilter(int filter);
  Q_INVOKABLE bool       SetSelected(uint entry_id, bool selected);
  Q_INVOKABLE void       SelectKind(int kind);
  Q_INVOKABLE void       ClearSelection();
  auto                   SelectedIds() const -> std::vector<sl_element_id_t>;
  /// Bounded command read using exactly the same roles as the review.
  Q_INVOKABLE QVariantList ReadPage() const;
  /// Read an independent bounded command page without changing the visible review or selection.
  /// Throws std::out_of_range for an unavailable page or invalid filter.
  auto ReadFindingsPage(int page, int filter, const QString& search, int& matching_count) const
      -> QVariantList;
 signals:
  void Changed();

 private:
  void        Rebuild();
  auto        Value(const MaintenanceFinding& finding, int role) const -> QVariant;
  static bool Matches(const MaintenanceFinding& finding, int filter, const QString& search);
  ProjectMaintenanceService&          service_;
  std::vector<size_t>                 visible_;
  std::unordered_set<sl_element_id_t> selected_;
  bool                                read_only_ = true;
  int                                 page_      = 0;
  int                                 filter_    = -1;
  QString                             search_;
  static constexpr int                kPageSize = 100;
};
}  // namespace alcedo::ui
