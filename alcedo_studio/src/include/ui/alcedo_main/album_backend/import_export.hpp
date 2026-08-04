//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>
#include <memory>
#include <vector>

#include "ui/alcedo_main/i18n.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/nikon_he_recovery_types.hpp"
#include "app/export_service.hpp"
#include "app/import_service.hpp"
#include "type/supported_file_type.hpp"

namespace alcedo::ui {

class FolderController;
class LibraryModule;
class NikonHeRecoveryController;
class ProjectDbWriteBarrier;
class ProjectModule;
class SemanticGenerationController;
class StatsEngine;
class IUiStatusSink;

/// Handles file import and image export workflows.
class ImportExportHandler final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString defaultExportFolder READ DefaultExportFolder CONSTANT)
  Q_PROPERTY(bool importRunning READ ImportRunning NOTIFY ImportStateChanged)
  Q_PROPERTY(int importTotal READ ImportTotal NOTIFY ImportStateChanged)
  Q_PROPERTY(int importCompleted READ ImportCompleted NOTIFY ImportStateChanged)
  Q_PROPERTY(int importFailed READ ImportFailed NOTIFY ImportStateChanged)
  Q_PROPERTY(QString importStatus READ ImportStatus NOTIFY ImportStateChanged)
  Q_PROPERTY(bool exportInFlight READ ExportInFlight NOTIFY ExportStateChanged)
  Q_PROPERTY(QString exportStatus READ ExportStatus NOTIFY ExportStateChanged)
  Q_PROPERTY(QVariantMap exportItemStatuses READ ExportItemStatuses NOTIFY ExportStateChanged)
  Q_PROPERTY(int exportTotal READ ExportTotal NOTIFY ExportStateChanged)
  Q_PROPERTY(int exportCompleted READ ExportCompleted NOTIFY ExportStateChanged)
  Q_PROPERTY(int exportSucceeded READ ExportSucceeded NOTIFY ExportStateChanged)
  Q_PROPERTY(int exportFailed READ ExportFailed NOTIFY ExportStateChanged)
  Q_PROPERTY(int exportSkipped READ ExportSkipped NOTIFY ExportStateChanged)
  Q_PROPERTY(QString exportErrorSummary READ ExportErrorSummary NOTIFY ExportStateChanged)

 public:
  ImportExportHandler(ProjectModule* project, LibraryModule* library, FolderController* folders,
                      IUiStatusSink* status, ProjectDbWriteBarrier* barrier,
                      QObject* parent = nullptr);

  void BindCollaborators(StatsEngine* stats, NikonHeRecoveryController* nikon,
                         SemanticGenerationController* semantic);

  Q_INVOKABLE void StartImport(const QStringList& fileUrlsOrPaths);
  Q_INVOKABLE QStringList CollectFolderFiles(const QString& folderUrlOrPath);
  Q_INVOKABLE void CancelImport();
  Q_INVOKABLE void StartExport(const QString& outputDirUrlOrPath);
  Q_INVOKABLE void StartExportWithOptions(const QString& outputDirUrlOrPath,
                                          const QString& formatName, const QString& hdrExportMode,
                                          bool resizeEnabled, int maxLengthSide, int quality,
                                          int bitDepth, int pngCompressionLevel,
                                          const QString& tiffCompression);
  Q_INVOKABLE void StartExportWithOptionsForTargets(
      const QString& outputDirUrlOrPath, const QString& formatName, const QString& hdrExportMode,
      bool resizeEnabled, int maxLengthSide, int quality, int bitDepth, int pngCompressionLevel,
      const QString& tiffCompression, const QVariantList& targetEntries);
  Q_INVOKABLE void StartExportWithSplitOptionsForTargets(
      const QString& outputDirUrlOrPath, bool sdrResizeEnabled, int sdrMaxLengthSide,
      int ultraHdrMaxLengthSide, const QString& sdrFormatName, int sdrQuality, int sdrBitDepth,
      int sdrPngCompressionLevel, const QString& sdrTiffCompression, int ultraHdrQuality,
      bool ultraHdrDitherEnabled, const QVariantList& targetEntries);
  Q_INVOKABLE void ResetExportState();
  Q_INVOKABLE bool CanUseHdrExportForTargets(const QVariantList& targetEntries) const;

  void StartImportPaths(const std::vector<image_path_t>& paths, bool preserveTarget = false);
  void FinishImport(const ImportResult& result);
  void FinishExport(const std::shared_ptr<std::vector<ExportResult>>& results, int skippedCount);
  void AddImportedEntries(const ImportLogSnapshot& snapshot);

  [[nodiscard]] auto CollectExportTargets(const QVariantList& targetEntries) const
      -> std::vector<ExportTarget>;
  auto BuildExportQueue(const std::vector<ExportTarget>& targets,
                        const std::filesystem::path& outputDir, bool sdrResizeEnabled,
                        int sdrMaxLengthSide, int ultraHdrMaxLengthSide, ImageFormatType sdrFormat,
                        int sdrQuality, ExportFormatOptions::BIT_DEPTH sdrBitDepth,
                        int sdrPngCompressionLevel,
                        ExportFormatOptions::TIFF_COMPRESS sdrTiffCompression, int ultraHdrQuality,
                        bool ultraHdrDitherEnabled) -> ExportQueueBuildResult;

  [[nodiscard]] bool export_inflight() const { return export_inflight_; }
  [[nodiscard]] bool ImportRunning() const { return import_running_; }
  [[nodiscard]] bool import_running() const { return import_running_; }
  [[nodiscard]] int  ImportTotal() const { return import_total_; }
  [[nodiscard]] int  import_total() const { return import_total_; }
  [[nodiscard]] int  ImportCompleted() const { return import_completed_; }
  [[nodiscard]] int  import_completed() const { return import_completed_; }
  [[nodiscard]] int  ImportFailed() const { return import_failed_; }
  [[nodiscard]] int  import_failed() const { return import_failed_; }
  [[nodiscard]] auto ImportStatus() const -> QString { return import_status_text_.Render(); }
  [[nodiscard]] auto import_status() const -> QString { return import_status_text_.Render(); }
  [[nodiscard]] auto current_import_job() const -> const std::shared_ptr<ImportJob>& {
    return current_import_job_;
  }
  [[nodiscard]] auto DefaultExportFolder() const -> QString { return default_export_folder_; }
  [[nodiscard]] auto default_export_folder() const -> const QString& {
    return default_export_folder_;
  }
  [[nodiscard]] bool ExportInFlight() const { return export_inflight_; }
  [[nodiscard]] auto ExportStatus() const -> QString { return export_status_text_.Render(); }
  [[nodiscard]] auto export_status() const -> QString { return export_status_text_.Render(); }
  [[nodiscard]] auto ExportErrorSummary() const -> QString {
    return export_error_summary_text_.Render();
  }
  [[nodiscard]] auto export_error_summary() const -> QString {
    return export_error_summary_text_.Render();
  }
  [[nodiscard]] auto ExportItemStatuses() const -> QVariantMap { return export_item_statuses_; }
  [[nodiscard]] auto export_item_statuses() const -> QVariantMap { return export_item_statuses_; }
  [[nodiscard]] int  ExportTotal() const { return export_total_; }
  [[nodiscard]] int  export_total() const { return export_total_; }
  [[nodiscard]] int  ExportCompleted() const { return export_completed_; }
  [[nodiscard]] int  export_completed() const { return export_completed_; }
  [[nodiscard]] int  ExportSucceeded() const { return export_succeeded_; }
  [[nodiscard]] int  export_succeeded() const { return export_succeeded_; }
  [[nodiscard]] int  ExportFailed() const { return export_failed_; }
  [[nodiscard]] int  export_failed() const { return export_failed_; }
  [[nodiscard]] int  ExportSkipped() const { return export_skipped_; }
  [[nodiscard]] int  export_skipped() const { return export_skipped_; }
  [[nodiscard]] auto import_target_folder_id() const -> sl_element_id_t {
    return import_target_folder_id_;
  }
  [[nodiscard]] auto import_target_folder_path() const -> const std::filesystem::path& {
    return import_target_folder_path_;
  }

  void SetImportTarget(sl_element_id_t folderId, const std::filesystem::path& folderPath) {
    import_target_folder_id_   = folderId;
    import_target_folder_path_ = folderPath;
  }
  void ClearImportTarget() {
    import_target_folder_id_ = 0;
    import_target_folder_path_.clear();
  }

 signals:
  void ImportStateChanged();
  void importStateChanged();
  void ExportStateChanged();
  void exportStateChanged();

 private:
  void StartImportResolvedPaths(std::vector<image_path_t> paths, bool preserveTarget);
  void ResetExportProgressState(const i18n::LocalizedText& status);
  void SetExportFailureState(const i18n::LocalizedText& message);

  ProjectModule*                 project_       = nullptr;
  LibraryModule*                 library_       = nullptr;
  FolderController*              folders_       = nullptr;
  IUiStatusSink*                 status_        = nullptr;
  ProjectDbWriteBarrier*         barrier_       = nullptr;
  StatsEngine*                   stats_         = nullptr;
  NikonHeRecoveryController*     nikon_         = nullptr;
  SemanticGenerationController*  semantic_      = nullptr;

  std::shared_ptr<ImportJob> current_import_job_{};
  bool                       import_running_   = false;
  int                        import_total_     = 0;
  int                        import_completed_ = 0;
  int                        import_failed_    = 0;
  i18n::LocalizedText        import_status_text_{};
  bool                       export_inflight_ = false;
  QString                    default_export_folder_{};
  i18n::LocalizedText        export_status_text_{};
  i18n::LocalizedText        export_error_summary_text_{};
  QVariantMap                export_item_statuses_{};
  int                        export_total_     = 0;
  int                        export_completed_ = 0;
  int                        export_succeeded_ = 0;
  int                        export_failed_    = 0;
  int                        export_skipped_   = 0;

  sl_element_id_t       import_target_folder_id_ = 0;
  std::filesystem::path import_target_folder_path_{};
};

}  // namespace alcedo::ui
