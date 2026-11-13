//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/export_queue.hpp"

#include <algorithm>
#include <unordered_set>

namespace alcedo::ui {
namespace {

auto ElementKey(sl_element_id_t element_id) -> QString {
  return QString::number(static_cast<qulonglong>(element_id));
}

/// The key of ImportExportHandler::exportItemStatuses.
auto StatusKey(const ExportQueue::Entry& entry) -> QString {
  return ElementKey(entry.element_id) + QLatin1Char(':') +
         QString::number(static_cast<qulonglong>(entry.image_id));
}

}  // namespace

ExportQueue::ExportQueue(ProjectModule* project, ImportExportHandler* import_export,
                         QObject* parent)
    : QObject(parent), import_export_(import_export) {
  if (project != nullptr) {
    connect(project, &ProjectModule::ProjectChanged, this, &ExportQueue::Clear);
  }
  if (import_export != nullptr) {
    connect(import_export, &ImportExportHandler::ExportStateChanged, this,
            &ExportQueue::RemoveFinishedEntries);
  }
}

auto ExportQueue::EntriesById() const -> QVariantMap {
  QVariantMap by_id;
  for (const Entry& entry : entries_) {
    by_id.insert(
        ElementKey(entry.element_id),
        QVariantMap{{QStringLiteral("elementId"), static_cast<qulonglong>(entry.element_id)},
                    {QStringLiteral("imageId"), static_cast<qulonglong>(entry.image_id)},
                    {QStringLiteral("fileName"), entry.file_name},
                    {QStringLiteral("isHdr"), entry.is_hdr}});
  }
  return by_id;
}

auto ExportQueue::PreviewRows() const -> QVariantList {
  std::vector<const Entry*> ordered;
  ordered.reserve(entries_.size());
  for (const Entry& entry : entries_) {
    ordered.push_back(&entry);
  }
  if (ordered.size() <= static_cast<size_t>(kSortedPreviewLimit)) {
    std::stable_sort(ordered.begin(), ordered.end(), [](const Entry* left, const Entry* right) {
      return QString::localeAwareCompare(left->file_name, right->file_name) < 0;
    });
  }
  QVariantList rows;
  rows.reserve(static_cast<qsizetype>(ordered.size()));
  for (const Entry* entry : ordered) {
    rows.push_back(QVariantMap{{QStringLiteral("statusKey"), StatusKey(*entry)},
                               {QStringLiteral("summaryRow"), false},
                               {QStringLiteral("label"), entry->file_name},
                               {QStringLiteral("isHdr"), entry->is_hdr}});
  }
  return rows;
}

bool ExportQueue::HasHdrItems() const {
  return std::any_of(entries_.begin(), entries_.end(),
                     [](const Entry& entry) { return entry.is_hdr; });
}

void ExportQueue::AddTargets(const QVariantList& items) {
  bool changed = false;
  for (const QVariant& value : items) {
    const QVariantMap item = value.toMap();
    Entry             entry;
    entry.element_id =
        static_cast<sl_element_id_t>(item.value(QStringLiteral("elementId")).toULongLong());
    entry.image_id  = static_cast<image_id_t>(item.value(QStringLiteral("imageId")).toULongLong());
    entry.file_name = item.value(QStringLiteral("fileName")).toString();
    entry.is_hdr    = item.value(QStringLiteral("isHdr")).toBool();
    if (entry.element_id == 0) {
      continue;
    }
    if (entry.file_name.isEmpty()) {
      entry.file_name = tr("(unnamed)");
    }
    const auto existing = std::find_if(
        entries_.begin(), entries_.end(),
        [&entry](const Entry& queued) { return queued.element_id == entry.element_id; });
    if (existing != entries_.end()) {
      *existing = std::move(entry);
    } else {
      entries_.push_back(std::move(entry));
    }
    changed = true;
  }
  if (changed) {
    emit QueueChanged();
  }
}

void ExportQueue::Clear() {
  if (entries_.empty()) {
    return;
  }
  entries_.clear();
  emit QueueChanged();
}

void ExportQueue::RemoveElements(const QVariantList& element_ids) {
  std::unordered_set<sl_element_id_t> removed;
  for (const QVariant& value : element_ids) {
    removed.insert(static_cast<sl_element_id_t>(value.toULongLong()));
  }
  const auto erased = std::erase_if(
      entries_, [&removed](const Entry& entry) { return removed.contains(entry.element_id); });
  if (erased > 0) {
    emit QueueChanged();
  }
}

auto ExportQueue::Targets() const -> QVariantList {
  QVariantList targets;
  targets.reserve(static_cast<qsizetype>(entries_.size()));
  for (const Entry& entry : entries_) {
    targets.push_back(
        QVariantMap{{QStringLiteral("elementId"), static_cast<qulonglong>(entry.element_id)},
                    {QStringLiteral("imageId"), static_cast<qulonglong>(entry.image_id)},
                    {QStringLiteral("isHdr"), entry.is_hdr}});
  }
  return targets;
}

void ExportQueue::RemoveFinishedEntries() {
  if (import_export_ == nullptr || entries_.empty()) {
    return;
  }
  const QVariantMap statuses = import_export_->ExportItemStatuses();
  const auto        erased   = std::erase_if(entries_, [&statuses](const Entry& entry) {
    const QString status = statuses.value(StatusKey(entry)).toString();
    return status == QStringLiteral("succeeded") || status == QStringLiteral("failed");
  });
  if (erased > 0) {
    emit QueueChanged();
  }
}

}  // namespace alcedo::ui
