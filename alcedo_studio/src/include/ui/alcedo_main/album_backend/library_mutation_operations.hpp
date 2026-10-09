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
#include "ui/alcedo_main/album_backend/library_selection.hpp"
#include "ui/alcedo_main/album_backend/workspace_router.hpp"

namespace alcedo::ui {

/// The library delete and rating rules that the image context menu and automation share. Owned
/// by ApplicationModuleHost. GUI thread only.
///
/// A target is a map with elementId, imageId, and optional fileId, folderId, scopeType, and
/// fileName, the entry form of ImageController::DeleteImages.
class LibraryMutationOperations final : public QObject {
  Q_OBJECT

 public:
  LibraryMutationOperations(ImageController* images, LibrarySelection* selection,
                            FolderController* folders, EditorSessionController* editor_session,
                            WorkspaceRouter* workspace_router, QObject* parent = nullptr);

  /// The targets of a delete, rating, or paste from the context menu of @p clickedItem: with
  /// @p includeSelection, the selected images when the selection is not empty; otherwise the
  /// clicked image alone. The editor filmstrip has no selection surface and passes false.
  Q_INVOKABLE QVariantList ResolveTargets(const QVariantMap& clickedItem,
                                          bool               includeSelection) const;
  /// `project` when the current folder is the root (a delete removes the photos from the
  /// project), `album` otherwise (a delete removes them from the album).
  Q_INVOKABLE QString      DeleteScope() const;
  /// Deletes @p targets through ImageController::DeleteImages, then removes the deleted elements
  /// from the selection and closes the editor image when it was deleted: the last edited image
  /// is forgotten, the Editor workspace shows the empty editor, and an image that the session
  /// keeps open outside the Editor workspace closes without its changes. Returns the
  /// DeleteImages result.
  Q_INVOKABLE QVariantMap  DeleteTargets(const QVariantList& targets);
  /// Sets the star rating of @p targets. One target uses ImageController::SetImageRating and
  /// returns its result (success, rating, message). Several targets use the batch path
  /// ImageController::StartSetImageRatings and return its result (started, taskId, message)
  /// with `batch: true`. A rating outside 0..5 returns `success: false` and changes nothing.
  Q_INVOKABLE QVariantMap  RateTargets(const QVariantList& targets, int rating);

 private:
  void                       CloseDeletedEditorImage(const QVariantList& deleted_element_ids);

  QPointer<ImageController>  images_;
  QPointer<LibrarySelection> selection_;
  QPointer<FolderController> folders_;
  QPointer<EditorSessionController> editor_session_;
  QPointer<WorkspaceRouter>         workspace_router_;
};

}  // namespace alcedo::ui
