//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <vector>

#include "type/type.hpp"

namespace alcedo::ui {

class FolderController;
class ImportExportHandler;
class InteractionPolicyController;
class LibraryModule;
class ProjectModule;
class SemanticGenerationController;
class StatsEngine;
class IUiStatusSink;

/// Handles single/batch image deletion and related project data cleanup.
class ImageController final : public QObject {
  Q_OBJECT

 public:
  struct DeleteTarget {
    sl_element_id_t       element_id_ = 0;
    image_id_t            image_id_   = 0;
    sl_element_id_t       folder_id_  = 0;
    std::filesystem::path file_path_{};
  };

  struct DeleteExecutionResult {
    bool                         success_       = false;
    int                          deleted_count_ = 0;
    int                          failed_count_  = 0;
    std::vector<sl_element_id_t> deleted_element_ids_{};
    std::vector<sl_element_id_t> failed_element_ids_{};
    QString                      message_{};
  };

  ImageController(ProjectModule* project, LibraryModule* library, FolderController* folders,
                  IUiStatusSink* status, QObject* parent = nullptr);

  void                    BindCollaborators(StatsEngine* stats, ImportExportHandler* import_export,
                                            SemanticGenerationController* semantic,
                                            InteractionPolicyController*  policy);

  Q_INVOKABLE QVariantMap DeleteImages(const QVariantList& targetEntries);
  Q_INVOKABLE QVariantMap AddImagesToFolder(const QVariantList& targetEntries, uint targetFolderId);
  Q_INVOKABLE QVariantMap GetImageDetails(uint elementId, uint imageId);
  Q_INVOKABLE QVariantMap GetFocusedImageInspection(uint elementId, uint imageId);
  Q_INVOKABLE QVariantMap GetImageRating(uint elementId, uint imageId);
  Q_INVOKABLE QVariantMap SetImageRating(uint elementId, uint imageId, int rating);
  Q_INVOKABLE QVariantMap SetImageDescription(uint elementId, const QString& caption);
  Q_INVOKABLE QVariantMap SetImageRatingReasons(uint elementId, const QString& reasons);
  Q_INVOKABLE QVariantMap GetImageRatingReasons(uint elementId);
  Q_INVOKABLE QVariantMap GetImageDescription(uint elementId);
  Q_INVOKABLE bool        OpenDirectoryInFileManager(const QString& dirUrlOrPath);

  auto DeleteTargets(const std::vector<DeleteTarget>& targets) -> DeleteExecutionResult;

  // Phase 7a: light star-rating path for AI batch scoring (no full save/package).
  void ApplyStarRatingLight(uint elementId, uint imageId, int rating);
  void FlushPendingStarRatings();

 private:
  struct RatingTarget {
    sl_element_id_t element_id_ = 0;
    image_id_t      image_id_   = 0;
  };

  [[nodiscard]] auto CollectDeleteTargets(const QVariantList& targetEntries) const
      -> std::vector<DeleteTarget>;
  [[nodiscard]] auto   ResolveRatingTarget(uint elementId, uint imageId) const -> RatingTarget;
  [[nodiscard]] auto   SaveProjectSnapshot() -> bool;

  ProjectModule*       project_           = nullptr;
  LibraryModule*       library_           = nullptr;
  FolderController*    folders_           = nullptr;
  IUiStatusSink*       status_            = nullptr;
  StatsEngine*         stats_             = nullptr;
  ImportExportHandler* import_export_     = nullptr;
  SemanticGenerationController* semantic_ = nullptr;
  InteractionPolicyController*  policy_   = nullptr;
};

}  // namespace alcedo::ui
