//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "type/type.hpp"

namespace alcedo {
class ProjectService;
}  // namespace alcedo

namespace alcedo::ui {

class BackgroundTaskController;
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
                                            InteractionPolicyController*  policy,
                                            BackgroundTaskController*     background_tasks);

  Q_INVOKABLE QVariantMap DeleteImages(const QVariantList& targetEntries);
  Q_INVOKABLE QVariantMap AddImagesToFolder(const QVariantList& targetEntries, uint targetFolderId);
  Q_INVOKABLE QVariantMap GetImageDetails(uint elementId, uint imageId);
  Q_INVOKABLE QVariantMap GetFocusedImageInspection(uint elementId, uint imageId);
  Q_INVOKABLE QVariantMap GetImageRating(uint elementId, uint imageId);
  Q_INVOKABLE QVariantMap SetImageRating(uint elementId, uint imageId, int rating);
  // Rates several images at once: one batched image-row update, one stats refresh,
  // and one project save, instead of one full save per image. Runs on the caller's thread.
  Q_INVOKABLE QVariantMap SetImageRatings(const QVariantList& targetEntries, int rating);
  // Same batch as SetImageRatings, but the library shows the new stars at once and the
  // database write and project save run on a worker thread as a background task.
  // Returns {started, message}; ImageRatingsFinished carries the SetImageRatings result.
  Q_INVOKABLE QVariantMap StartSetImageRatings(const QVariantList& targetEntries, int rating);
  Q_INVOKABLE QVariantMap SetImageDescription(uint elementId, const QString& caption);
  Q_INVOKABLE QVariantMap SetImageRatingReasons(uint elementId, const QString& reasons);
  Q_INVOKABLE QVariantMap GetImageRatingReasons(uint elementId);
  Q_INVOKABLE QVariantMap GetImageDescription(uint elementId);
  Q_INVOKABLE bool        OpenDirectoryInFileManager(const QString& dirUrlOrPath);

  auto DeleteTargets(const std::vector<DeleteTarget>& targets) -> DeleteExecutionResult;

  // Phase 7a: light star-rating path for AI batch scoring (no full save/package).
  void ApplyStarRatingLight(uint elementId, uint imageId, int rating);
  void FlushPendingStarRatings();

 signals:
  void ImageRatingsFinished(const QVariantMap& result);

 private:
  struct RatingTarget {
    sl_element_id_t element_id_ = 0;
    image_id_t      image_id_   = 0;
  };

  // A validated rating batch. `previous_view_ratings_` holds, per target, the rating the
  // library view showed before the batch (-1 when the view has no row for the target).
  struct RatingBatch {
    int                          rating_ = 0;
    std::vector<RatingTarget>    targets_{};
    std::vector<int>             previous_view_ratings_{};
    std::vector<sl_element_id_t> unresolved_element_ids_{};
  };

  struct RatingBatchOutcome {
    std::vector<bool> rated_{};  // per target
    bool              save_ok_ = false;
  };

  [[nodiscard]] auto PrepareRatingBatch(const QVariantList& targetEntries, int rating,
                                        QVariantMap* error_result) -> std::optional<RatingBatch>;
  [[nodiscard]] static auto PersistRatingBatch(const std::shared_ptr<ProjectService>& project,
                                               const RatingBatch&                     batch,
                                               const std::function<bool(QString*)>&   save_job)
      -> RatingBatchOutcome;
  [[nodiscard]] auto FinishRatingBatch(const RatingBatch&        batch,
                                       const RatingBatchOutcome& outcome) -> QVariantMap;
  // Shows ratings[i] for targets[i] in the library view state and the thumbnail model.
  // Returns the ratings the view showed before (-1 where the view has no row).
  auto SetViewRatings(const std::vector<RatingTarget>& targets, const std::vector<int>& ratings)
      -> std::vector<int>;

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
  BackgroundTaskController*     background_tasks_ = nullptr;
  QString                       rating_task_id_{};
  bool                          rating_batch_running_ = false;
};

}  // namespace alcedo::ui
