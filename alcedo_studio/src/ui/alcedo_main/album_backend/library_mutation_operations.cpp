//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/library_mutation_operations.hpp"

namespace alcedo::ui {

LibraryMutationOperations::LibraryMutationOperations(
    ImageController* images, LibrarySelection* selection, FolderController* folders,
    EditorSessionController* editor_session, WorkspaceRouter* workspace_router, QObject* parent)
    : QObject(parent),
      images_(images),
      selection_(selection),
      folders_(folders),
      editor_session_(editor_session),
      workspace_router_(workspace_router) {}

auto LibraryMutationOperations::ResolveTargets(const QVariantMap& clickedItem,
                                               bool includeSelection) const -> QVariantList {
  if (includeSelection && selection_ != nullptr && selection_->SelectedCount() > 0) {
    return selection_->SelectedImagesById().values();
  }
  const uint element_id = clickedItem.value(QStringLiteral("elementId")).toUInt();
  if (element_id == 0) {
    return {};
  }
  const uint file_id   = clickedItem.value(QStringLiteral("fileId")).toUInt();
  uint       folder_id = clickedItem.value(QStringLiteral("folderId")).toUInt();
  if (folder_id == 0 && folders_ != nullptr) {
    folder_id = folders_->CurrentFolderId();
  }
  QString file_name = clickedItem.value(QStringLiteral("fileName")).toString();
  if (file_name.isEmpty()) {
    file_name = tr("(unnamed)");
  }
  return QVariantList{QVariantMap{
      {QStringLiteral("elementId"), element_id},
      {QStringLiteral("fileId"), file_id != 0 ? file_id : element_id},
      {QStringLiteral("imageId"), clickedItem.value(QStringLiteral("imageId")).toUInt()},
      {QStringLiteral("folderId"), folder_id},
      {QStringLiteral("scopeType"), clickedItem.value(QStringLiteral("scopeType")).toString()},
      {QStringLiteral("fileName"), file_name}}};
}

auto LibraryMutationOperations::DeleteScope() const -> QString {
  return folders_ == nullptr || folders_->CurrentFolderId() == 0 ? QStringLiteral("project")
                                                                 : QStringLiteral("album");
}

auto LibraryMutationOperations::DeleteTargets(const QVariantList& targets) -> QVariantMap {
  if (images_ == nullptr) {
    return QVariantMap{{QStringLiteral("success"), false},
                       {QStringLiteral("deletedElementIds"), QVariantList{}},
                       {QStringLiteral("failedElementIds"), QVariantList{}}};
  }
  const QVariantMap  result  = images_->DeleteImages(targets);
  const QVariantList deleted = result.value(QStringLiteral("deletedElementIds")).toList();
  if (!deleted.isEmpty()) {
    if (selection_ != nullptr) {
      selection_->PruneElements(deleted);
    }
    CloseDeletedEditorImage(deleted);
  }
  return result;
}

void LibraryMutationOperations::CloseDeletedEditorImage(const QVariantList& deleted_element_ids) {
  if (editor_session_ == nullptr) {
    return;
  }
  const uint editor_element_id  = editor_session_->element_id();
  bool       deleted_open_image = false;
  for (const QVariant& value : deleted_element_ids) {
    deleted_open_image =
        deleted_open_image || (editor_element_id != 0 && value.toUInt() == editor_element_id);
  }
  const uint last_element_id    = editor_session_->last_element_id();
  bool       deleted_last_image = false;
  for (const QVariant& value : deleted_element_ids) {
    deleted_last_image =
        deleted_last_image || (last_element_id != 0 && value.toUInt() == last_element_id);
  }
  if (!deleted_open_image && !deleted_last_image) {
    return;
  }
  // Re-entering the editor must not restore a deleted image.
  editor_session_->clearLastEditedImage();
  if (!deleted_open_image) {
    return;
  }
  if (workspace_router_ != nullptr && workspace_router_->workspace() == QStringLiteral("editor")) {
    // Open(0, 0) in the Editor workspace finalizes the session and shows the empty editor.
    workspace_router_->OpenEditor(0, 0);
    return;
  }
  // The session keeps the image open for an immediate return to the Editor. The image is gone,
  // so it closes without writing its history.
  editor_session_->Finalize(false);
}

auto LibraryMutationOperations::RateTargets(const QVariantList& targets, int rating)
    -> QVariantMap {
  if (images_ == nullptr || targets.isEmpty()) {
    return QVariantMap{{QStringLiteral("success"), false},
                       {QStringLiteral("message"), tr("No valid image was selected.")}};
  }
  if (rating < 0 || rating > 5) {
    return QVariantMap{{QStringLiteral("success"), false},
                       {QStringLiteral("message"), tr("Rating must be between 0 and 5.")}};
  }
  if (targets.size() > 1) {
    QVariantMap result = images_->StartSetImageRatings(targets, rating);
    result.insert(QStringLiteral("batch"), true);
    return result;
  }
  const QVariantMap target = targets.front().toMap();
  QVariantMap       result =
      images_->SetImageRating(target.value(QStringLiteral("elementId")).toUInt(),
                              target.value(QStringLiteral("imageId")).toUInt(), rating);
  result.insert(QStringLiteral("batch"), false);
  return result;
}

}  // namespace alcedo::ui
