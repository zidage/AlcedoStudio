// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "ui/alcedo_main/album_backend/project_maintenance_model.hpp"

#include <QStringList>
#include <QVariantMap>
#include <algorithm>
#include <stdexcept>

namespace alcedo::ui {
ProjectMaintenanceModel::ProjectMaintenanceModel(ProjectMaintenanceService& service,
                                                 QObject*                   parent)
    : QAbstractListModel(parent), service_(service) {}

int ProjectMaintenanceModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return std::max(0, std::min(kPageSize, MatchingCount() - page_ * kPageSize));
}
QHash<int, QByteArray> ProjectMaintenanceModel::roleNames() const {
  return {{EntryId, "entryId"},         {SourcePath, "sourcePath"}, {Rating, "rating"},
          {EditingWork, "editingWork"}, {Finding, "findingKind"},   {Reason, "reason"},
          {RetainedId, "retainedId"},   {Removable, "removable"},   {Selected, "selected"}};
}
QVariant ProjectMaintenanceModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
  const auto& finding = service_.Findings()[visible_[page_ * kPageSize + index.row()]];
  return Value(finding, role);
}
auto ProjectMaintenanceModel::Value(const MaintenanceFinding& finding, int role) const -> QVariant {
  const auto& entry = finding.evidence;
  switch (role) {
    case EntryId:
      return entry.entry_id;
    case SourcePath:
      return QString::fromStdWString(entry.source_path.wstring());
    case Rating:
      return entry.rating;
    case EditingWork:
      return tr("%1 commits; %2 Versions").arg(entry.commit_count).arg(entry.version_count);
    case Finding:
      return static_cast<int>(finding.kind);
    case Reason: {
      QStringList reasons{finding.kind == MaintenanceFindingKind::SourceChanged
                              ? tr("Source changed since import")
                              : QString::fromStdString(finding.reason)};
      for (const auto& reason : entry.protection_reasons) reasons << QString::fromStdString(reason);
      return reasons.join(QStringLiteral("\n"));
    }
    case RetainedId:
      return finding.retained_id ? QVariant(*finding.retained_id) : QVariant{};
    case Removable:
      return finding.removable && !read_only_ && service_.Complete();
    case Selected:
      return selected_.contains(entry.entry_id);
    default:
      return {};
  }
}
void ProjectMaintenanceModel::Clear() {
  beginResetModel();
  visible_.clear();
  selected_.clear();
  page_      = 0;
  read_only_ = true;
  endResetModel();
  emit Changed();
}
void ProjectMaintenanceModel::Refresh() {
  selected_.clear();
  read_only_ = !service_.Complete();
  Rebuild();
}
void ProjectMaintenanceModel::Rebuild() {
  beginResetModel();
  visible_.clear();
  if (service_.Complete()) {
    for (size_t i = 0; i < service_.Findings().size(); ++i) {
      const auto& item = service_.Findings()[i];
      if (!Matches(item, filter_, search_)) continue;
      visible_.push_back(i);
    }
  }
  page_ = std::clamp(page_, 0, std::max(0, (MatchingCount() - 1) / kPageSize));
  endResetModel();
  emit Changed();
}
void ProjectMaintenanceModel::SetReadOnly(bool read_only) {
  read_only_ = read_only;
  if (rowCount()) emit dataChanged(index(0), index(rowCount() - 1), {Removable});
  emit Changed();
}
void ProjectMaintenanceModel::SetPage(int page) {
  if (page < 0 || page * kPageSize >= MatchingCount() || page == page_) return;
  beginResetModel();
  page_ = page;
  endResetModel();
  emit Changed();
}
void ProjectMaintenanceModel::SetSearch(const QString& search) {
  if (search_ == search) return;
  search_ = search;
  page_   = 0;
  Rebuild();
}
void ProjectMaintenanceModel::SetFilter(int filter) {
  if (filter_ == filter || filter < -1 ||
      filter > static_cast<int>(MaintenanceFindingKind::SourceHistoryUnknown))
    return;
  filter_ = filter;
  page_   = 0;
  Rebuild();
}
bool ProjectMaintenanceModel::SetSelected(uint entry_id, bool selected) {
  if (read_only_ || !service_.Complete()) return false;
  const auto found =
      std::find_if(service_.Findings().begin(), service_.Findings().end(),
                   [entry_id](const auto& item) { return item.evidence.entry_id == entry_id; });
  if (found == service_.Findings().end() || !found->removable) return false;
  if (selected)
    selected_.insert(entry_id);
  else
    selected_.erase(entry_id);
  if (rowCount()) emit dataChanged(index(0), index(rowCount() - 1), {Selected});
  emit Changed();
  return true;
}
void ProjectMaintenanceModel::SelectKind(int kind) {
  if (read_only_ || !service_.Complete()) return;
  for (const auto& item : service_.Findings()) {
    if (item.removable && static_cast<int>(item.kind) == kind)
      selected_.insert(item.evidence.entry_id);
  }
  if (rowCount()) emit dataChanged(index(0), index(rowCount() - 1), {Selected});
  emit Changed();
}
void ProjectMaintenanceModel::ClearSelection() {
  if (read_only_) return;
  selected_.clear();
  if (rowCount()) emit dataChanged(index(0), index(rowCount() - 1), {Selected});
  emit Changed();
}
auto ProjectMaintenanceModel::SelectedIds() const -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids(selected_.begin(), selected_.end());
  std::sort(ids.begin(), ids.end());
  return ids;
}
int ProjectMaintenanceModel::SelectedDuplicates() const {
  if (selected_.empty()) return 0;
  return static_cast<int>(std::count_if(
      service_.Findings().begin(), service_.Findings().end(), [this](const auto& finding) {
        return finding.kind == MaintenanceFindingKind::Duplicate &&
               selected_.contains(finding.evidence.entry_id);
      }));
}
QVariantList ProjectMaintenanceModel::ReadPage() const {
  QVariantList rows;
  for (int row = 0; row < rowCount(); ++row) {
    QVariantMap values;
    const auto  roles = roleNames();
    for (auto role = roles.begin(); role != roles.end(); ++role)
      values[QString::fromUtf8(role.value())] = data(index(row), role.key());
    rows.push_back(values);
  }
  return rows;
}
bool ProjectMaintenanceModel::Matches(const MaintenanceFinding& finding, int filter,
                                      const QString& search) {
  return (filter < 0 || static_cast<int>(finding.kind) == filter) &&
         (search.isEmpty() ||
          QString::fromStdWString(finding.evidence.source_path.wstring())
              .contains(search, Qt::CaseInsensitive) ||
          QString::fromStdString(finding.reason).contains(search, Qt::CaseInsensitive) ||
          QString::number(finding.evidence.entry_id).contains(search));
}
auto ProjectMaintenanceModel::ReadFindingsPage(int page, int filter, const QString& search,
                                               int& matching_count) const -> QVariantList {
  if (page < 0 || filter < -1 ||
      filter > static_cast<int>(MaintenanceFindingKind::SourceHistoryUnknown))
    throw std::out_of_range("Invalid maintenance page or filter.");
  QVariantList rows;
  matching_count = 0;
  if (service_.Complete()) {
    const auto roles = roleNames();
    for (const auto& finding : service_.Findings()) {
      if (!Matches(finding, filter, search)) continue;
      const auto position = matching_count++;
      if (position / kPageSize != page) continue;
      QVariantMap values;
      for (auto role = roles.begin(); role != roles.end(); ++role)
        values[QString::fromUtf8(role.value())] = Value(finding, role.key());
      rows.push_back(values);
    }
  }
  if (page > 0 && (matching_count == 0 || page > (matching_count - 1) / kPageSize))
    throw std::out_of_range("Maintenance findings page does not exist.");
  return rows;
}
}  // namespace alcedo::ui
