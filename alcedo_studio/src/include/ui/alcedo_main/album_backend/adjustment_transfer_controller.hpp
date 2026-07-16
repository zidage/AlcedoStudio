//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

#include "app/adjustment_transfer_service.hpp"

namespace alcedo::ui {

class ImportExportHandler;
class LibraryModule;
class ProjectModule;

class AdjustmentTransferController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool packageAvailable READ package_available NOTIFY PackageChanged)
  Q_PROPERTY(QVariantList packageSummary READ package_summary NOTIFY PackageChanged)
  Q_PROPERTY(QString packageSourceTitle READ package_source_title NOTIFY PackageChanged)

 public:
  AdjustmentTransferController(ProjectModule* project, LibraryModule* library,
                               ImportExportHandler* import_export, QObject* parent = nullptr);
  ~AdjustmentTransferController() override = default;

  [[nodiscard]] bool package_available() const { return copied_package_.has_value(); }
  [[nodiscard]] auto package_summary() const -> QVariantList { return copied_summary_; }
  [[nodiscard]] auto package_source_title() const -> QString { return copied_source_title_; }

  Q_INVOKABLE QVariantMap PrepareCopy(uint elementId);
  Q_INVOKABLE QVariantMap Copy(uint elementId, const QVariantList& selectedKeys);
  Q_INVOKABLE QVariantMap Paste(const QVariantList& targetEntries, const QString& strategy);
  Q_INVOKABLE void        Discard();

 signals:
  void PackageChanged();

 private:
  ProjectModule*       project_       = nullptr;
  LibraryModule*       library_       = nullptr;
  ImportExportHandler* import_export_ = nullptr;

  std::optional<AdjustmentTransferPackage> copied_package_{};
  QVariantList                             copied_summary_{};
  QString                                  copied_source_title_{};
};

}  // namespace alcedo::ui
