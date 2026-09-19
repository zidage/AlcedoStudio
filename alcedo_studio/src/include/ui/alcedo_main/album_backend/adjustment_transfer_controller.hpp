//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

#include "app/adjustment_transfer_types.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.hpp"

namespace alcedo::ui {

class ImportExportHandler;
class LibraryModule;
class ProjectModule;

/**
 * @brief Thin QML façade for Adjustment Transfer.
 *
 * Owns the copied package and its read-only summary, prepares the dialog model
 * for one source image, and routes Copy/Paste commands. Selection state lives
 * in @ref AdjustmentTransferDialogModel; multi-target apply work lives in
 * @ref AdjustmentTransferApplyCoordinator. The controller never inspects
 * pipeline operators and never switches the live source Version.
 */
class AdjustmentTransferController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool packageAvailable READ package_available NOTIFY PackageChanged)
  Q_PROPERTY(QVariantList packageSummary READ package_summary NOTIFY PackageChanged)
  Q_PROPERTY(QString packageSourceTitle READ package_source_title NOTIFY PackageChanged)
  Q_PROPERTY(QString packageSourceVersion READ package_source_version NOTIFY PackageChanged)
  Q_PROPERTY(alcedo::ui::AdjustmentTransferDialogModel* dialogModel READ dialog_model CONSTANT)

 public:
  AdjustmentTransferController(ProjectModule* project, LibraryModule* library,
                               ImportExportHandler* import_export, QObject* parent = nullptr);
  ~AdjustmentTransferController() override = default;

  [[nodiscard]] bool package_available() const { return copied_package_.has_value(); }
  [[nodiscard]] auto package_summary() const -> QVariantList { return copied_summary_; }
  [[nodiscard]] auto package_source_title() const -> QString { return copied_source_title_; }
  [[nodiscard]] auto package_source_version() const -> QString { return copied_source_version_; }
  [[nodiscard]] auto dialog_model() -> AdjustmentTransferDialogModel* { return dialog_model_; }
  /// Test accessor for the multi-target apply owner.
  [[nodiscard]] auto apply_coordinator() -> AdjustmentTransferApplyCoordinator* {
    return apply_coordinator_.get();
  }

  /// Load the source image catalog into the dialog model. Returns
  /// `{success, message?, sourceTitle?, activeVersionId?}` for the caller.
  Q_INVOKABLE QVariantMap PrepareCopy(uint elementId);
  /// Build the v6 package from the dialog model's selection and publish it as
  /// the copied package. A failed build keeps the prior package.
  Q_INVOKABLE QVariantMap CommitCopy();
  Q_INVOKABLE QVariantMap Paste(const QVariantList& targetEntries, const QString& strategy);
  Q_INVOKABLE QVariantMap PasteIntoEditor(QObject* editorSession);
  Q_INVOKABLE void        Discard();

 signals:
  void PackageChanged();

 private:
  ProjectModule*                                      project_       = nullptr;
  LibraryModule*                                      library_       = nullptr;
  ImportExportHandler*                                import_export_ = nullptr;

  AdjustmentTransferDialogModel*                      dialog_model_  = nullptr;  // owned child
  std::unique_ptr<AdjustmentTransferApplyCoordinator> apply_coordinator_;

  std::optional<alcedo::AdjustmentTransferPackage>    copied_package_{};
  QVariantList                                        copied_summary_{};
  QString                                             copied_source_title_{};
  QString                                             copied_source_version_{};
  /// Title of the image the dialog model currently inspects.
  QString                                             prepared_source_title_{};
};

}  // namespace alcedo::ui
