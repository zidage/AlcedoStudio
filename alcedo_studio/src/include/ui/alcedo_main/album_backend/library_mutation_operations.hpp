//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/alcedo_main/album_backend/library_selection.hpp"
#include "ui/alcedo_main/album_backend/workspace_router.hpp"

namespace alcedo::ui {

/// The library delete and rating rules that the image context menu and automation share. Owned
/// by ApplicationModuleHost. GUI thread only.
///
/// A target is a map with elementId, imageId, and optional fileId, folderId, scopeType, and
/// fileName, the entry form of ImageController::StartDeleteImages.
class LibraryMutationOperations final : public QObject {
  Q_OBJECT

 public:
  LibraryMutationOperations(ImageController* images, LibrarySelection* selection,
                            FolderController* folders, EditorSessionController* editor_session,
                            WorkspaceRouter* workspace_router, QObject* parent = nullptr);

  /// The policy whose agent control refuses the user variants StartDeleteTargetsAsUser and
  /// RateTargetsAsUser. Null refuses nothing.
  void SetInteractionPolicy(InteractionPolicyController* interaction_policy);

  /// The targets of a delete, rating, or paste from the context menu of @p clickedItem: with
  /// @p includeSelection, the selected images when the selection is not empty; otherwise the
  /// clicked image alone. The editor filmstrip has no selection surface and passes false.
  Q_INVOKABLE QVariantList ResolveTargets(const QVariantMap& clickedItem,
                                          bool               includeSelection) const;
  /// `project` when the current folder is the root (a delete removes the photos from the
  /// project), `album` otherwise (a delete removes them from the album).
  Q_INVOKABLE QString      DeleteScope() const;
  /**
   * @brief Starts the delete of @p targets through ImageController::StartDeleteImages and
   *        returns its result (started, taskId, message, and `locked` when a task holds the
   *        delete lock).
   *
   * @details When the delete starts (ImageController::DeleteImagesStarting), the editor image
   * closes when it is a target: the last edited image is forgotten, the Editor workspace shows
   * the empty editor, and an image that the session keeps open outside the Editor workspace
   * closes without its changes. So the pipeline delete on the worker never runs while the editor
   * can still write that pipeline. A delete that does not start closes nothing. When the delete
   * is complete,
   * ImageController has removed the deleted elements from the selection; this object emits
   * ElementsDeleted (when an element was deleted) and then DeleteTargetsFinished.
   */
  Q_INVOKABLE QVariantMap  StartDeleteTargets(const QVariantList& targets);
  /// Sets the star rating of @p targets. One target uses ImageController::SetImageRating and
  /// returns its result (success, rating, message). Several targets use the batch path
  /// ImageController::StartSetImageRatings and return its result (started, taskId, message)
  /// with `batch: true`. A rating outside 0..5 returns `success: false` and changes nothing.
  Q_INVOKABLE QVariantMap  RateTargets(const QVariantList& targets, int rating);
  /// The user delete and rating of the library context menu: StartDeleteTargets and RateTargets,
  /// but refused (`started: false` and `success: false`) with the reason in `message` while an
  /// automation agent holds control. The automation commands call StartDeleteTargets and
  /// RateTargets directly.
  Q_INVOKABLE QVariantMap  StartDeleteTargetsAsUser(const QVariantList& targets);
  Q_INVOKABLE QVariantMap  RateTargetsAsUser(const QVariantList& targets, int rating);
  /// Sets (@p on) or clears the color label @p code (0 to 6) on every image of @p targets
  /// through ImageController::StartSetImageColorLabel and returns its result (started, taskId,
  /// message). ImageController::ImageMarksFinished reports the write.
  Q_INVOKABLE QVariantMap  SetColorLabelOfTargets(const QVariantList& targets, int code, bool on);
  /// Sets or clears the Delete mark on every image of @p targets through
  /// ImageController::StartSetImageDeleteMark (see SetColorLabelOfTargets).
  Q_INVOKABLE QVariantMap  SetDeleteMarkOfTargets(const QVariantList& targets, bool on);
  /// The marks control of the Library inspector: SetColorLabelOfTargets and
  /// SetDeleteMarkOfTargets, refused (`started: false`) with the reason in `message` while an
  /// automation agent holds control.
  Q_INVOKABLE QVariantMap  SetColorLabelOfTargetsAsUser(const QVariantList& targets, int code,
                                                        bool on);
  Q_INVOKABLE QVariantMap  SetDeleteMarkOfTargetsAsUser(const QVariantList& targets, bool on);

 signals:
  /// The element ids that a delete deleted.
  void ElementsDeleted(const QVariantList& element_ids);
  /// A delete started by StartDeleteTargets is complete. @p result is the
  /// ImageController::DeleteImagesFinished result.
  void DeleteTargetsFinished(const QVariantMap& result);
  // QML Connections maps a handler onFoo to the signal foo, so a signal that starts with an
  // uppercase letter needs a lowercase twin for QML.
  void deleteTargetsFinished(const QVariantMap& result);

 private:
  void                       CloseDeletedEditorImage(const QVariantList& deleted_element_ids);
  void                       OnDeleteImagesFinished(const QVariantMap& result);

  QPointer<ImageController>  images_;
  QPointer<LibrarySelection> selection_;
  QPointer<FolderController> folders_;
  QPointer<EditorSessionController> editor_session_;
  QPointer<WorkspaceRouter>         workspace_router_;
  InteractionPolicyController*      interaction_policy_ = nullptr;
};

}  // namespace alcedo::ui
