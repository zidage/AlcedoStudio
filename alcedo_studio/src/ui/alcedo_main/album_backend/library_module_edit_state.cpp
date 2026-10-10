//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LibraryModule behavior that follows the edit history: the refresh of presentations that read
// the edit state (edit-time sort, edit-day groups) after an edit is written, and the reveal of
// the last edited photo when a project opens. Every database read runs on the query worker.

#include <QDebug>
#include <exception>
#include <optional>

#include "app/album_browse_service.hpp"
#include "app/project_service.hpp"
#include "sleeve/album_query.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"

namespace alcedo::ui {
namespace {

auto ReadsEditState(const AlbumQueryOptions& options) -> bool {
  return options.sort_field_ == AlbumSortField::kEditTime ||
         options.group_field_ == AlbumGroupField::kEditDay;
}

}  // namespace

void LibraryModule::NoteEditHistoryChanged(bool library_hidden) {
  if (!ReadsEditState(RequestedOptions())) {
    return;
  }
  // While the library is hidden (the editor is open, and its filmstrip shows this order), a
  // refresh would move photos under the user; the library refreshes when it is shown again.
  if (library_hidden) {
    edit_order_stale_ = true;
    return;
  }
  RequestLibraryRefresh();
}

void LibraryModule::RefreshStaleEditOrder() {
  if (edit_order_stale_) {
    RequestLibraryRefresh();
  }
}

void LibraryModule::SetPendingReveal(uint fileId) {
  if (pending_reveal_file_id_ == fileId) {
    return;
  }
  pending_reveal_file_id_ = fileId;
  emit PendingRevealChanged();
  emit pendingRevealChanged();
}

void LibraryModule::RevealLastEditedFile() {
  SetPendingReveal(0);
  reveal_scope_widened_ = false;
  auto proj             = project_ ? project_->handler().project() : nullptr;
  auto browse           = proj ? proj->GetAlbumBrowseService() : nullptr;
  if (!browse) {
    return;
  }
  query_worker_->Submit(SearchRequestKind::kLibraryIds, [this, browse](std::uint64_t generation) {
    std::optional<LastEditedFile> file;
    QString                       error;
    try {
      file = browse->ReadLastEditedFile();
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
    }
    QMetaObject::invokeMethod(
        this,
        [this, generation, browse, file, error]() {
          if (!query_worker_->IsCurrent(SearchRequestKind::kLibraryIds, generation)) {
            return;
          }
          // A project opened after this read started has its own read.
          auto current = project_ ? project_->handler().project() : nullptr;
          if (!current || current->GetAlbumBrowseService() != browse) {
            return;
          }
          if (!error.isEmpty()) {
            qWarning().noquote() << "Last edited photo read failed:" << error;
            SetQueryError(error);
            return;
          }
          if (!file.has_value()) {
            return;
          }
          emit LastEditedFileFound(static_cast<uint>(file->file_id_),
                                   static_cast<uint>(file->image_id_));
          SetPendingReveal(static_cast<uint>(file->file_id_));
        },
        Qt::QueuedConnection);
  });
}

void LibraryModule::RequestPendingReveal() {
  if (pending_reveal_file_id_ != 0) {
    RequestFocusPosition(pending_reveal_file_id_);
  }
}

auto LibraryModule::SettlePendingReveal(uint fileId, bool found) -> bool {
  if (pending_reveal_file_id_ == 0 || fileId != pending_reveal_file_id_) {
    return false;
  }
  // The root folder (ui id 0) reads every project file, so a miss there is final.
  if (found || reveal_scope_widened_ || folders_ == nullptr || folders_->CurrentFolderId() == 0) {
    SetPendingReveal(0);
    return false;
  }
  reveal_scope_widened_ = true;
  folders_->SelectFolder(0);
  // Runs after the refresh that the folder selection requested.
  RequestFocusPosition(fileId);
  return true;
}

}  // namespace alcedo::ui
