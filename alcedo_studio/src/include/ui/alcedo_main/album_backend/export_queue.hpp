//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"

namespace alcedo::ui {

/// The export queue of the export panel: the photos that the next export of the panel writes.
/// Owned by ApplicationModuleHost. GUI thread only.
///
/// - One entry for each element id. Adding an element that is in the queue replaces its entry
///   and keeps its position and its export run.
/// - An entry joins an export run when the run starts while the entry is in the queue and the
///   run has a status for the entry (ImportExportHandler::exportItemStatuses). Only the entries
///   of the current run show a run status; every other entry is queued for the next export.
/// - An entry leaves the queue when its run reports it as succeeded or failed, when its element
///   is deleted, and when the project changes.
class ExportQueue final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int count READ Count NOTIFY QueueChanged)
  /// Entry maps (elementId, imageId, fileName, isHdr) keyed by the element id as text.
  Q_PROPERTY(QVariantMap entriesById READ EntriesById NOTIFY QueueChanged)
  /// One row (statusKey, summaryRow, label, isHdr) for each entry; sorted by file name when the
  /// queue has at most 200 entries, in queue order otherwise. statusKey is the
  /// exportItemStatuses key of an entry of the current export run, and empty for the others.
  Q_PROPERTY(QVariantList previewRows READ PreviewRows NOTIFY QueueChanged)
  Q_PROPERTY(bool hasHdrItems READ HasHdrItems NOTIFY QueueChanged)

 public:
  struct Entry {
    sl_element_id_t element_id = 0;
    image_id_t      image_id   = 0;
    QString         file_name;
    bool            is_hdr = false;
    /// The export run (ImportExportHandler::ExportRunId) of the entry; 0 until a run takes it.
    quint64         run_id = 0;
  };

  /// The sort limit of previewRows.
  static constexpr int kSortedPreviewLimit = 200;

  /// @p project and @p import_export can be null (tests); then the queue changes only through
  /// its own operations.
  ExportQueue(ProjectModule* project, ImportExportHandler* import_export,
              QObject* parent = nullptr);

  [[nodiscard]] auto       Count() const -> int { return static_cast<int>(entries_.size()); }
  [[nodiscard]] auto       Entries() const -> const std::vector<Entry>& { return entries_; }
  [[nodiscard]] auto       EntriesById() const -> QVariantMap;
  [[nodiscard]] auto       PreviewRows() const -> QVariantList;
  [[nodiscard]] bool       HasHdrItems() const;

  /// Adds the maps of @p items (elementId, imageId, fileName, isHdr). An item with element id 0
  /// is ignored. An empty file name gets the placeholder name.
  Q_INVOKABLE void         AddTargets(const QVariantList& items);
  Q_INVOKABLE void         Clear();
  /// Removes the entries of the element ids in @p element_ids.
  Q_INVOKABLE void         RemoveElements(const QVariantList& element_ids);
  /// The export targets (elementId, imageId, isHdr) of the entries, in queue order.
  Q_INVOKABLE QVariantList Targets() const;

  /// Applies the export state of ImportExportHandler: when @p run_id is a new run, the entries
  /// with a status in @p statuses join it; then the entries of the run whose status is
  /// succeeded or failed leave the queue. Called on ImportExportHandler::ExportStateChanged.
  void                     ApplyExportState(quint64 run_id, const QVariantMap& statuses);

 signals:
  void QueueChanged();

 private:
  QPointer<ImportExportHandler> import_export_;
  std::vector<Entry>            entries_;
  /// The run id of the last ApplyExportState.
  quint64                       current_run_id_ = 0;
};

}  // namespace alcedo::ui
