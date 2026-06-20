//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QDate>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/semantic_generation_service.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
#include "ui/alcedo_main/album_backend/album_thumbnail_model.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/editor_controller.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/model_download_controller.hpp"
#include "ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp"
#include "ui/alcedo_main/album_backend/nikon_he_recovery_types.hpp"
#include "ui/alcedo_main/album_backend/project_handler.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/album_backend/thumbnail_manager.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

class AlbumBackend final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList thumbnails READ Thumbnails NOTIFY ThumbnailsChanged)
  Q_PROPERTY(QObject* thumbnailModel READ ThumbnailModel CONSTANT)
  Q_PROPERTY(QObject* adjustmentTransferController READ AdjustmentTransferControllerObject CONSTANT)
  Q_PROPERTY(QObject* modelDownloadController READ ModelDownloadControllerObject CONSTANT)
  Q_PROPERTY(QObject* semanticGenerationController READ SemanticGenerationControllerObject CONSTANT)
  Q_PROPERTY(QVariantList folders READ Folders NOTIFY FoldersChanged)
  Q_PROPERTY(uint currentFolderId READ CurrentFolderId NOTIFY FolderSelectionChanged)
  Q_PROPERTY(QString currentFolderPath READ CurrentFolderPath NOTIFY FolderSelectionChanged)
  Q_PROPERTY(int shownCount READ ShownCount NOTIFY CountsChanged)
  Q_PROPERTY(int totalCount READ TotalCount NOTIFY CountsChanged)
  Q_PROPERTY(bool hasMoreThumbnails READ HasMoreThumbnails NOTIFY CountsChanged)
  Q_PROPERTY(QString filterInfo READ FilterInfo NOTIFY CountsChanged)
  Q_PROPERTY(QObject* searchController READ SearchControllerObject CONSTANT)
  Q_PROPERTY(QVariantList dateStats READ DateStats NOTIFY StatsChanged)
  Q_PROPERTY(QVariantList cameraStats READ CameraStats NOTIFY StatsChanged)
  Q_PROPERTY(QVariantList lensStats READ LensStats NOTIFY StatsChanged)
  Q_PROPERTY(QVariantList labelStats READ LabelStats NOTIFY StatsChanged)
  Q_PROPERTY(int totalPhotoCount READ TotalPhotoCount NOTIFY StatsChanged)
  Q_PROPERTY(QString statsFilterDate READ StatsFilterDate NOTIFY StatsFilterChanged)
  Q_PROPERTY(QString statsFilterCamera READ StatsFilterCamera NOTIFY StatsFilterChanged)
  Q_PROPERTY(QString statsFilterLens READ StatsFilterLens NOTIFY StatsFilterChanged)
  Q_PROPERTY(QString statsFilterLabel READ StatsFilterLabel NOTIFY StatsFilterChanged)
  Q_PROPERTY(QVariantList ratingStats READ RatingStats NOTIFY StatsChanged)
  Q_PROPERTY(QString statsFilterRating READ StatsFilterRating NOTIFY StatsFilterChanged)
  Q_PROPERTY(bool serviceReady READ ServiceReady NOTIFY ServiceStateChanged)
  Q_PROPERTY(QString serviceMessage READ ServiceMessage NOTIFY ServiceStateChanged)
  Q_PROPERTY(QVariantList recentProjects READ RecentProjects NOTIFY RecentProjectsChanged)
  Q_PROPERTY(QVariantList acceleratorOptions READ AcceleratorOptions NOTIFY AcceleratorStateChanged)
  Q_PROPERTY(QString acceleratorBackend READ AcceleratorBackend NOTIFY AcceleratorStateChanged)
  Q_PROPERTY(QString acceleratorWarning READ AcceleratorWarning NOTIFY AcceleratorStateChanged)
  Q_PROPERTY(bool acceleratorPreparing READ AcceleratorPreparing NOTIFY AcceleratorPreparationStateChanged)
  Q_PROPERTY(QString acceleratorPreparationStatus READ AcceleratorPreparationStatus NOTIFY AcceleratorPreparationStateChanged)
  Q_PROPERTY(bool projectLoading READ ProjectLoading NOTIFY ProjectLoadStateChanged)
  Q_PROPERTY(
      QString projectLoadingMessage READ ProjectLoadingMessage NOTIFY ProjectLoadStateChanged)
  Q_PROPERTY(QString taskStatus READ TaskStatus NOTIFY TaskStateChanged)
  Q_PROPERTY(int taskProgress READ TaskProgress NOTIFY TaskStateChanged)
  Q_PROPERTY(bool taskCancelVisible READ TaskCancelVisible NOTIFY TaskStateChanged)
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
  Q_PROPERTY(
      bool nikonHeRecoveryActive READ NikonHeRecoveryActive NOTIFY NikonHeRecoveryStateChanged)
  Q_PROPERTY(bool nikonHeRecoveryBusy READ NikonHeRecoveryBusy NOTIFY NikonHeRecoveryStateChanged)
  Q_PROPERTY(
      QString nikonHeRecoveryPhase READ NikonHeRecoveryPhase NOTIFY NikonHeRecoveryStateChanged)
  Q_PROPERTY(
      QString nikonHeRecoveryStatus READ NikonHeRecoveryStatus NOTIFY NikonHeRecoveryStateChanged)
  Q_PROPERTY(QVariantList nikonHeUnsupportedFiles READ NikonHeUnsupportedFiles NOTIFY
                 NikonHeRecoveryStateChanged)
  Q_PROPERTY(
      QString nikonHeConverterPath READ NikonHeConverterPath NOTIFY NikonHeRecoveryStateChanged)
  Q_PROPERTY(bool nikonHeConverterPathFromDefault READ NikonHeConverterPathFromDefault NOTIFY
                 NikonHeRecoveryStateChanged)
  Q_PROPERTY(bool editorActive READ EditorActive NOTIFY EditorStateChanged)
  Q_PROPERTY(bool editorBusy READ EditorBusy NOTIFY EditorStateChanged)
  Q_PROPERTY(uint editorElementId READ EditorElementId NOTIFY EditorStateChanged)
  Q_PROPERTY(QString editorTitle READ EditorTitle NOTIFY EditorStateChanged)
  Q_PROPERTY(QString editorStatus READ EditorStatus NOTIFY EditorStateChanged)
  Q_PROPERTY(bool thumbnailDiskCacheEnabled READ ThumbnailDiskCacheEnabled NOTIFY ThumbnailDiskCacheStateChanged)
  Q_PROPERTY(QString thumbnailDiskCacheRoot READ ThumbnailDiskCacheRoot NOTIFY ThumbnailDiskCacheStateChanged)
  Q_PROPERTY(int thumbnailDiskCacheMaxEntries READ ThumbnailDiskCacheMaxEntries NOTIFY ThumbnailDiskCacheStateChanged)
  Q_PROPERTY(int thumbnailDiskCacheJpegQuality READ ThumbnailDiskCacheJpegQuality NOTIFY ThumbnailDiskCacheStateChanged)
  Q_PROPERTY(QString thumbnailDiskCacheStats READ ThumbnailDiskCacheStats NOTIFY ThumbnailDiskCacheStateChanged)
  Q_PROPERTY(QString editorPreviewUrl READ EditorPreviewUrl NOTIFY EditorPreviewChanged)
  Q_PROPERTY(QVariantList editorLutOptions READ EditorLutOptions NOTIFY EditorStateChanged)
  Q_PROPERTY(int editorLutIndex READ EditorLutIndex NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorExposure READ EditorExposure NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorContrast READ EditorContrast NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorSaturation READ EditorSaturation NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorTint READ EditorTint NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorBlacks READ EditorBlacks NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorWhites READ EditorWhites NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorShadows READ EditorShadows NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorHighlights READ EditorHighlights NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorSharpen READ EditorSharpen NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorClarity READ EditorClarity NOTIFY EditorStateChanged)

 public:
  explicit AlbumBackend(QObject* parent = nullptr);
  ~AlbumBackend() override;

  // ── Q_PROPERTY getters ──────────────────────────────────────────────
  QVariantList Thumbnails() const;
  QObject*     ThumbnailModel() { return &thumbnail_model_; }
  QObject*     AdjustmentTransferControllerObject() { return &adjustment_transfer_; }
  QObject*     ModelDownloadControllerObject() { return &model_download_controller_; }
  QObject*     SemanticGenerationControllerObject() { return &semantic_generation_; }
  QVariantList Folders() const { return folder_ctrl_.folders(); }
  uint CurrentFolderId() const { return static_cast<uint>(folder_ctrl_.current_folder_id()); }
  const QString& CurrentFolderPath() const { return folder_ctrl_.current_folder_path_text(); }
  int            ShownCount() const { return thumbnail_model_.count(); }
  int            TotalCount() const;
  bool           HasMoreThumbnails() const;
  QString        FilterInfo() const;
  QObject*       SearchControllerObject() { return &search_; }
  QVariantList   DateStats() const { return stats_.date_stats(); }
  QVariantList   CameraStats() const { return stats_.camera_stats(); }
  QVariantList   LensStats() const { return stats_.lens_stats(); }
  QVariantList   LabelStats() const { return stats_.label_stats(); }
  QVariantList   RatingStats() const { return stats_.rating_stats(); }
  int            TotalPhotoCount() const { return stats_.total_photo_count(); }
  const QString& StatsFilterDate() const { return stats_.filter_date(); }
  const QString& StatsFilterCamera() const { return stats_.filter_camera(); }
  const QString& StatsFilterLens() const { return stats_.filter_lens(); }
  const QString& StatsFilterLabel() const { return stats_.filter_label(); }
  const QString& StatsFilterRating() const { return stats_.filter_rating(); }
  bool           ServiceReady() const { return service_ready_; }
  QString        ServiceMessage() const { return service_message_text_.Render(); }
  QVariantList   RecentProjects() const { return recent_projects_; }
  QVariantList   AcceleratorOptions() const { return accelerator_options_; }
  QString        AcceleratorBackend() const { return accelerator_backend_key_; }
  QString        AcceleratorWarning() const {
    return IsAcceleratorWarningAcknowledged() ? QString{} : accelerator_warning_text_.Render();
  }
  bool    AcceleratorPreparing() const { return accelerator_preparing_; }
  QString AcceleratorPreparationStatus() const { return accelerator_preparation_status_text_.Render(); }
  bool    ProjectLoading() const { return project_handler_.project_loading(); }
  QString ProjectLoadingMessage() const { return project_handler_.project_loading_message(); }
  QString TaskStatus() const { return task_status_text_.Render(); }
  int     TaskProgress() const { return task_progress_; }
  bool    TaskCancelVisible() const { return task_cancel_visible_; }
  const QString& DefaultExportFolder() const { return import_export_.default_export_folder(); }
  bool           ImportRunning() const { return import_export_.import_running(); }
  int            ImportTotal() const { return import_export_.import_total(); }
  int            ImportCompleted() const { return import_export_.import_completed(); }
  int            ImportFailed() const { return import_export_.import_failed(); }
  QString        ImportStatus() const { return import_export_.import_status(); }
  bool           ExportInFlight() const { return import_export_.export_inflight(); }
  QString        ExportStatus() const { return import_export_.export_status(); }
  QVariantMap    ExportItemStatuses() const { return import_export_.export_item_statuses(); }
  int            ExportTotal() const { return import_export_.export_total(); }
  int            ExportCompleted() const { return import_export_.export_completed(); }
  int            ExportSucceeded() const { return import_export_.export_succeeded(); }
  int            ExportFailed() const { return import_export_.export_failed(); }
  int            ExportSkipped() const { return import_export_.export_skipped(); }
  QString        ExportErrorSummary() const { return import_export_.export_error_summary(); }
  bool           NikonHeRecoveryActive() const { return nikon_he_recovery_.is_active(); }
  bool           NikonHeRecoveryBusy() const { return nikon_he_recovery_.is_busy(); }
  QString        NikonHeRecoveryPhase() const { return nikon_he_recovery_.phase_text(); }
  QString        NikonHeRecoveryStatus() const { return nikon_he_recovery_.status_text(); }
  QVariantList   NikonHeUnsupportedFiles() const { return nikon_he_recovery_.unsupported_files(); }
  QString        NikonHeConverterPath() const { return nikon_he_recovery_.converter_path(); }
  bool           NikonHeConverterPathFromDefault() const {
    return nikon_he_recovery_.converter_path_from_default();
  }
  bool           EditorActive() const { return editor_.editor_active(); }
  bool           EditorBusy() const { return editor_.editor_busy(); }
  uint           EditorElementId() const { return static_cast<uint>(editor_.editor_element_id()); }
  QString        EditorTitle() const { return editor_.editor_title(); }
  QString        EditorStatus() const { return editor_.editor_status(); }
  bool    ThumbnailDiskCacheEnabled() const;
  QString ThumbnailDiskCacheRoot() const;
  int     ThumbnailDiskCacheMaxEntries() const;
  int     ThumbnailDiskCacheJpegQuality() const;
  QString ThumbnailDiskCacheStats() const;
  const QString& EditorPreviewUrl() const { return editor_.editor_preview_url(); }
  QVariantList   EditorLutOptions() const { return editor_.editor_lut_options(); }
  int            EditorLutIndex() const { return editor_.editor_lut_index(); }
  double         EditorExposure() const { return editor_.editor_state().exposure_; }
  double         EditorContrast() const { return editor_.editor_state().contrast_; }
  double         EditorSaturation() const { return editor_.editor_state().saturation_; }
  double         EditorTint() const { return editor_.editor_state().tint_; }
  double         EditorBlacks() const { return editor_.editor_state().blacks_; }
  double         EditorWhites() const { return editor_.editor_state().whites_; }
  double         EditorShadows() const { return editor_.editor_state().shadows_; }
  double         EditorHighlights() const { return editor_.editor_state().highlights_; }
  double         EditorSharpen() const { return editor_.editor_state().sharpen_; }
  double         EditorClarity() const { return editor_.editor_state().clarity_; }

  Q_INVOKABLE void        SelectFolder(uint folderId);
  Q_INVOKABLE void        CreateFolder(const QString& folderName);
  Q_INVOKABLE void        DeleteFolder(uint folderId);
  Q_INVOKABLE QVariantMap DeleteImages(const QVariantList& targetEntries);
  Q_INVOKABLE QVariantMap AddImagesToFolder(const QVariantList& targetEntries, uint targetFolderId);
  Q_INVOKABLE QVariantMap GetImageDetails(uint elementId, uint imageId);
  Q_INVOKABLE QVariantMap GetImageRating(uint elementId, uint imageId);
  Q_INVOKABLE QVariantMap SetImageRating(uint elementId, uint imageId, int rating);
  Q_INVOKABLE bool        OpenDirectoryInFileManager(const QString& dirUrlOrPath);

  Q_INVOKABLE void        StartImport(const QStringList& fileUrlsOrPaths);
  Q_INVOKABLE void        CancelImport();
  Q_INVOKABLE bool        PromptAndLoadProject();
  Q_INVOKABLE bool        PromptAndCreateProject();
  Q_INVOKABLE bool        SetAcceleratorBackend(const QString& backendKey);
  Q_INVOKABLE void        StartAcceleratorPreparation();
  Q_INVOKABLE void        AcknowledgeAcceleratorWarning();
  Q_INVOKABLE bool        LoadProject(const QString& metaFileUrlOrPath);
  Q_INVOKABLE bool        CreateProjectInFolder(const QString& folderUrlOrPath);
  Q_INVOKABLE bool        CreateProjectInFolderNamed(const QString& folderUrlOrPath,
                                                     const QString& projectName);
  Q_INVOKABLE bool        SaveProject();
  Q_INVOKABLE void        StartExport(const QString& outputDirUrlOrPath);
  Q_INVOKABLE void        StartExportWithOptions(const QString& outputDirUrlOrPath,
                                                 const QString& formatName, const QString& hdrExportMode,
                                                 bool resizeEnabled, int maxLengthSide, int quality,
                                                 int bitDepth, int pngCompressionLevel,
                                                 const QString& tiffCompression);
  Q_INVOKABLE void        StartExportWithOptionsForTargets(
             const QString& outputDirUrlOrPath, const QString& formatName, const QString& hdrExportMode,
             bool resizeEnabled, int maxLengthSide, int quality, int bitDepth, int pngCompressionLevel,
             const QString& tiffCompression, const QVariantList& targetEntries);
  Q_INVOKABLE void        StartExportWithSplitOptionsForTargets(
             const QString& outputDirUrlOrPath, bool sdrResizeEnabled, int sdrMaxLengthSide,
             int ultraHdrMaxLengthSide,
             const QString& sdrFormatName, int sdrQuality, int sdrBitDepth,
             int sdrPngCompressionLevel, const QString& sdrTiffCompression,
             int ultraHdrQuality, bool ultraHdrDitherEnabled, const QVariantList& targetEntries);
  Q_INVOKABLE void         ResetExportState();
  Q_INVOKABLE bool         CanUseHdrExportForTargets(const QVariantList& targetEntries) const;
  Q_INVOKABLE void         BrowseNikonHeConverter();
  Q_INVOKABLE void         StartNikonHeConversion();
  Q_INVOKABLE void         ExitNikonHeRecovery();
  Q_INVOKABLE void         OpenEditor(uint elementId, uint imageId);
  Q_INVOKABLE void         CloseEditor();
  Q_INVOKABLE void         ResetEditorAdjustments();
  Q_INVOKABLE void         RequestEditorFullPreview();
  Q_INVOKABLE void         SetEditorLutIndex(int index);
  Q_INVOKABLE void         SetEditorExposure(double value);
  Q_INVOKABLE void         SetEditorContrast(double value);
  Q_INVOKABLE void         SetEditorSaturation(double value);
  Q_INVOKABLE void         SetEditorTint(double value);
  Q_INVOKABLE void         SetEditorBlacks(double value);
  Q_INVOKABLE void         SetEditorWhites(double value);
  Q_INVOKABLE void         SetEditorShadows(double value);
  Q_INVOKABLE void         SetEditorHighlights(double value);
  Q_INVOKABLE void         SetEditorSharpen(double value);
  Q_INVOKABLE void         SetEditorClarity(double value);
  Q_INVOKABLE void         SetThumbnailVisible(uint elementId, uint imageId, bool visible,
                                               uint maxEdge = 1024);
  Q_INVOKABLE void         SetThumbnailCacheHint(uint visibleCells, uint maxEdge = 1024);
  Q_INVOKABLE bool         LoadMoreThumbnails();
  Q_INVOKABLE bool         LoadThumbnailsThroughIndex(int index);
  Q_INVOKABLE void         ToggleStatsFilter(const QString& category, const QString& label);
  Q_INVOKABLE void         ClearStatsFilter();

  // ── Phase 4: Thumbnail disk cache control ───────────────────────────
  Q_INVOKABLE void SetThumbnailDiskCacheEnabled(bool enabled);
  Q_INVOKABLE void SetThumbnailDiskCacheRoot(const QString& rootPath);
  Q_INVOKABLE void SetThumbnailDiskCacheMaxEntries(int maxEntries);
  Q_INVOKABLE void SetThumbnailDiskCacheJpegQuality(int quality);
  Q_INVOKABLE void ClearAllThumbnailDiskCache();
  Q_INVOKABLE void ClearProjectThumbnailDiskCache();
  Q_INVOKABLE int  PromptForInt(const QString& title, const QString& label,
                                int defaultValue, int minValue, int maxValue);

 signals:
  void ThumbnailsChanged();
  void thumbnailsChanged();
  void ThumbnailUpdated(uint elementId, const QString& dataUrl, bool loading, bool missingSource,
                        const QString& errorText);
  void thumbnailUpdated(uint elementId, const QString& dataUrl, bool loading, bool missingSource,
                        const QString& errorText);
  void CountsChanged();
  void StatsChanged();
  void ServiceStateChanged();
  void RecentProjectsChanged();
  void AcceleratorStateChanged();
  void AcceleratorPreparationStateChanged();
  void TaskStateChanged();
  void ImportStateChanged();
  void importStateChanged();
  void ExportStateChanged();
  void exportStateChanged();
  void NikonHeRecoveryStateChanged();
  void EditorStateChanged();
  void EditorPreviewChanged();
  void ProjectChanged();
  void projectChanged();
  void ProjectLoadStateChanged();
  void FoldersChanged();
  void FolderSelectionChanged();
  void folderSelectionChanged();
  void StatsFilterChanged();
  void ThumbnailDiskCacheStateChanged();

 private:
  friend class ProjectHandler;
  friend class ThumbnailManager;
  friend class FolderController;
  friend class ImageController;
  friend class StatsEngine;
  friend class SearchController;
  friend class ModelDownloadController;
  friend class SemanticGenerationController;
  friend class ImportExportHandler;
  friend class NikonHeRecoveryController;
  friend class EditorController;
  friend class AdjustmentTransferController;

  void SetServiceState(bool ready, const i18n::LocalizedText& message);
  void SetServiceMessageForCurrentProject(const i18n::LocalizedText& message);
  void ScheduleIdleTaskStateReset(int delayMs);
  void SetTaskState(const i18n::LocalizedText& status, int progress, bool cancelVisible);
  void RefreshTranslations();
  void InitializeAcceleratorSettings();
  void StartOpenClPreparationIfNeeded();
  void SetAcceleratorPreparationState(bool preparing, const i18n::LocalizedText& status);
  void RebuildAcceleratorOptions();
  void ApplyAcceleratorPreferenceToServices();
  bool IsAcceleratorWarningAcknowledged() const;
  void PersistAcceleratorWarningAcknowledgement() const;
  void QueueSemanticGenerationPrompt(std::vector<SemanticGenerationItem> items);
  void ResumeQueuedSemanticGenerationWorkflow();
  auto ActiveSemanticModelKey() const -> std::string;
  auto SemanticLabelDisplayText(sl_element_id_t elementId) const -> QString;
  void LoadRecentProjectsFromSettings();
  void PersistRecentProjects() const;
  void RegisterRecentProject(const std::filesystem::path& projectPath);
  void RemoveRecentProject(const std::filesystem::path& projectPath);
  void ReloadFolderTree(const std::filesystem::path& preferredFolderPath = {});
  void ReloadCurrentFolder();
  bool LoadThumbnailWindow(const std::optional<std::wstring>& filterWhere, bool reset);
  auto EffectiveFilterWhere(const std::optional<std::wstring>& filterWhere) const
      -> std::optional<std::wstring>;
  void AddOrUpdateAlbumItem(sl_element_id_t elementId, image_id_t imageId, sl_element_id_t folderId,
                            const QString& scopeType, const file_name_t& fallbackName,
                            const std::filesystem::path& filePath);
  void SetAlbumItemHdrFlag(sl_element_id_t elementId, image_id_t imageId, bool isHdr);
  void PersistImageHdrFlag(sl_element_id_t elementId, image_id_t imageId, bool isHdr);
  auto FindAlbumItem(sl_element_id_t elementId) -> AlbumItem*;
  auto FindAlbumItem(sl_element_id_t elementId) const -> const AlbumItem*;

  // ── Helper modules ──────────────────────────────────────────────────
  ProjectHandler               project_handler_;
  ThumbnailManager             thumb_;
  FolderController             folder_ctrl_;
  ImageController              image_ctrl_;
  StatsEngine                  stats_;
  SearchController             search_;
  alcedo::ModelDownloadService model_download_service_;
  ModelDownloadController      model_download_controller_;
  SemanticGenerationController semantic_generation_;
  ImportExportHandler          import_export_;
  NikonHeRecoveryController    nikon_he_recovery_;
  EditorController             editor_;
  AdjustmentTransferController adjustment_transfer_;
  AlbumThumbnailModel          thumbnail_model_{};

  // ── Shared data (accessed by helpers via friend) ────────────────────
  AlbumViewState               view_state_{};
  i18n::LocalizedText          service_message_text_{};
  bool                         service_ready_ = false;
  QVariantList                 recent_projects_{};
  AcceleratorBackendPreference accelerator_preference_ = AcceleratorBackendPreference::Auto;
  QString                      accelerator_backend_key_{};
  QString                      accelerator_warning_id_{};
  QVariantList                 accelerator_options_{};
  i18n::LocalizedText          accelerator_warning_text_{};
  i18n::LocalizedText          accelerator_preparation_status_text_{};
  bool                         accelerator_preparing_      = false;
  bool                         accelerator_prepare_started_ = false;
  bool                         cuda_backend_available_   = false;
  bool                         opencl_backend_available_ = false;
  bool                         metal_backend_available_  = false;
  i18n::LocalizedText          task_status_text_{};
  int                          task_progress_       = 0;
  bool                         task_cancel_visible_ = false;
  // ── Phase 4: Thumbnail disk cache settings ──────────────────────────
  void LoadThumbnailDiskCacheSettings();
  void SaveThumbnailDiskCacheSettings();
  void ApplyThumbnailDiskCacheSettingsToService();

  bool   thumbnail_disk_cache_enabled_     = true;
  QString thumbnail_disk_cache_root_;
  int    thumbnail_disk_cache_max_entries_ = 10000;
  int    thumbnail_disk_cache_jpeg_quality_ = 85;
};

}  // namespace alcedo::ui
