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

#include "image/color_label.hpp"
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

  /**
   * @brief Deletes the images of @p targetEntries (from the project in the root folder, from the
   *        album otherwise) without blocking the UI thread.
   *
   * @details The database delete, the pipeline, export and image pool cleanup, the project save
   * and the package write run on a worker thread as an ImageDelete background task. The task
   * holds the DeleteImages lock, which refuses a second delete until this one is complete, and
   * per-image locks for the rating, description and analysis of the targets. When the worker is
   * done, the UI thread removes the deleted images from the thumbnails and the selection, reloads
   * the view and emits DeleteImagesFinished with the result `{success, deletedCount,
   * failedCount, deletedElementIds, failedElementIds, message}`.
   *
   * Returns `{started, taskId, message}`. When the delete does not start, `message` gives the
   * reason and no DeleteImagesFinished follows. `locked: true` marks a delete that the
   * interaction policy refuses because a background task holds the DeleteImages lock of the
   * project or of a target; it can start once that task is complete.
   */
  Q_INVOKABLE QVariantMap StartDeleteImages(const QVariantList& targetEntries);
  /// True while a delete started by StartDeleteImages runs.
  [[nodiscard]] auto      delete_running() const -> bool { return delete_running_; }
  /// User action: refused with the reason in `message` while an automation agent holds control.
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
  // Returns {started, taskId, message}; taskId is the RatingUpdate background task.
  // ImageRatingsFinished carries the SetImageRatings result.
  Q_INVOKABLE QVariantMap StartSetImageRatings(const QVariantList& targetEntries, int rating);

  /**
   * @brief Sets (@p on) or clears the color label @p code (0 to 6, see ColorLabel) on every
   *        image of @p targetEntries. The other codes of each image stay.
   *
   * @details One batched image-row update, one statistics read (or a library refresh when the
   * Inspector filter reads a mark), and one project save, on the caller's thread. The library
   * rows show the new marks before the write; a failed write puts the old marks back. Returns
   * `{success, message, markedCount, failedCount, markedElementIds, failedElementIds}`.
   */
  Q_INVOKABLE QVariantMap SetImageColorLabel(const QVariantList& targetEntries, int code, bool on);
  /// Same batch as SetImageColorLabel; the write and the save run on a worker thread as a
  /// LibraryMarkUpdate background task. Returns `{started, taskId, message}`;
  /// ImageMarksFinished carries the SetImageColorLabel result.
  Q_INVOKABLE QVariantMap StartSetImageColorLabel(const QVariantList& targetEntries, int code,
                                                  bool on);
  /// Sets or clears the Delete mark on every image of @p targetEntries (see SetImageColorLabel).
  Q_INVOKABLE QVariantMap SetImageDeleteMark(const QVariantList& targetEntries, bool on);
  /// Same batch as SetImageDeleteMark on a worker thread (see StartSetImageColorLabel).
  Q_INVOKABLE QVariantMap StartSetImageDeleteMark(const QVariantList& targetEntries, bool on);
  /// User edits of the inspector: refused with the reason in `message` while an automation agent
  /// holds control.
  Q_INVOKABLE QVariantMap SetImageDescription(uint elementId, const QString& caption);
  Q_INVOKABLE QVariantMap SetImageRatingReasons(uint elementId, const QString& reasons);
  Q_INVOKABLE QVariantMap GetImageRatingReasons(uint elementId);
  Q_INVOKABLE QVariantMap GetImageDescription(uint elementId);
  Q_INVOKABLE bool        OpenDirectoryInFileManager(const QString& dirUrlOrPath);

  /// Deletes @p targets on the calling thread (same steps and result as StartDeleteImages).
  /// Only for flows that must continue after the delete in the same call (Nikon HE recovery).
  auto DeleteTargets(const std::vector<DeleteTarget>& targets) -> DeleteExecutionResult;
  /// The QVariantMap form of @p result (see StartDeleteImages).
  [[nodiscard]] static auto DeleteResultMap(const DeleteExecutionResult& result) -> QVariantMap;

  // Phase 7a: light star-rating path for AI batch scoring (no full save/package).
  void ApplyStarRatingLight(uint elementId, uint imageId, int rating);
  void FlushPendingStarRatings();

 signals:
  void ImageRatingsFinished(const QVariantMap& result);
  // QML Connections maps a handler onFoo to the signal foo, so a signal that starts with an
  // uppercase letter needs a lowercase twin for QML.
  void imageRatingsFinished(const QVariantMap& result);
  /// A batch started by StartSetImageColorLabel or StartSetImageDeleteMark is complete.
  void ImageMarksFinished(const QVariantMap& result);
  void imageMarksFinished(const QVariantMap& result);
  /// StartDeleteImages accepted the delete of @p elementIds. Emitted on the UI thread before the
  /// worker starts, so a receiver can close what must not be open while the images are deleted.
  void DeleteImagesStarting(const QVariantList& elementIds);
  /// A delete started by StartDeleteImages is complete (see there for @p result).
  void DeleteImagesFinished(const QVariantMap& result);

 private:
  struct RatingTarget {
    sl_element_id_t element_id_ = 0;
    image_id_t      image_id_   = 0;
  };

  // The element ids of @p targets (0 for a target without one).
  [[nodiscard]] static auto TargetElementIds(const std::vector<RatingTarget>& targets)
      -> std::vector<sl_element_id_t>;

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

  // One change of a library mark batch: one color label code set or cleared, or the Delete mark
  // set or cleared.
  struct MarkChange {
    enum class Kind { kColorLabel, kDeleteMark };
    Kind       kind_ = Kind::kColorLabel;
    ColorLabel code_ = ColorLabel::kRed;  // kColorLabel only.
    bool       on_   = false;
  };

  // The marks of one library row.
  struct ViewMarks {
    ColorLabelMask color_labels_ = 0;
    bool           delete_mark_  = false;
  };

  // A validated mark batch. `previous_view_marks_` holds, per target, the marks the library view
  // showed before the batch (empty when the view has no row for the target).
  struct MarkBatch {
    MarkChange                            change_{};
    std::vector<RatingTarget>             targets_{};
    std::vector<std::optional<ViewMarks>> previous_view_marks_{};
    std::vector<sl_element_id_t>          unresolved_element_ids_{};
  };

  struct MarkBatchOutcome {
    std::vector<bool> marked_{};  // per target
    bool              save_ok_ = false;
  };

  // One target per image of @p targetEntries, in entry order (the first entry of an image wins).
  // Entries without an image id add their element id to @p unresolved_element_ids.
  [[nodiscard]] auto ResolveBatchTargets(const QVariantList&           targetEntries,
                                         std::vector<sl_element_id_t>* unresolved_element_ids) const
      -> std::vector<RatingTarget>;
  [[nodiscard]] static auto ApplyMarkChange(ViewMarks marks, const MarkChange& change)
      -> ViewMarks;
  [[nodiscard]] auto PrepareMarkBatch(const QVariantList& targetEntries, MarkChange change,
                                      QVariantMap* error_result) -> std::optional<MarkBatch>;
  [[nodiscard]] static auto PersistMarkBatch(const std::shared_ptr<ProjectService>& project,
                                             const MarkBatch&                       batch,
                                             const std::function<bool(QString*)>&   save_job)
      -> MarkBatchOutcome;
  [[nodiscard]] auto FinishMarkBatch(const MarkBatch& batch, const MarkBatchOutcome& outcome)
      -> QVariantMap;
  // Applies @p change to the library view state and the thumbnail model rows of @p targets.
  // Returns the marks each row showed before (empty where the view has no row).
  auto ApplyViewMarkChange(const std::vector<RatingTarget>& targets, const MarkChange& change)
      -> std::vector<std::optional<ViewMarks>>;
  // Shows @p marks[i] for @p targets[i] (a revert of ApplyViewMarkChange).
  void SetViewMarks(const std::vector<RatingTarget>& targets, const std::vector<ViewMarks>& marks);
  [[nodiscard]] auto SetImageMarks(const QVariantList& targetEntries, MarkChange change)
      -> QVariantMap;
  [[nodiscard]] auto StartSetImageMarks(const QVariantList& targetEntries, MarkChange change)
      -> QVariantMap;

  // A validated delete: the targets with the ids that the library rows supply, and the scope.
  struct DeleteBatch {
    sl_element_id_t           folder_id_           = 0;
    bool                      delete_from_library_ = false;
    std::vector<DeleteTarget> targets_{};
  };

  // What the delete job wrote. Produced on the thread that runs the job.
  struct DeleteBatchOutcome {
    std::vector<sl_element_id_t> deleted_element_ids_{};
    bool                         save_ok_ = true;
  };

  [[nodiscard]] auto CollectDeleteTargets(const QVariantList& targetEntries) const
      -> std::vector<DeleteTarget>;
  // Checks the project state and resolves @p targets on the UI thread. Without a batch,
  // @p refusal holds the message.
  [[nodiscard]] auto PrepareDeleteBatch(const std::vector<DeleteTarget>& targets, QString* refusal)
      -> std::optional<DeleteBatch>;
  // The job that writes @p batch: database delete, cleanup, save and package. It takes the
  // services of the open project now and may run on any thread.
  [[nodiscard]] auto MakeDeleteJob(DeleteBatch batch) const -> std::function<DeleteBatchOutcome()>;
  // UI-thread part after the job: thumbnails, selection, view reload, status message.
  auto FinishDeleteBatch(const DeleteBatch& batch, const DeleteBatchOutcome& outcome)
      -> DeleteExecutionResult;
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
  QString                       mark_task_id_{};
  bool                          mark_batch_running_ = false;
  QString                       delete_task_id_{};
  bool                          delete_running_ = false;
};

}  // namespace alcedo::ui
