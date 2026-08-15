//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/workspace_router.hpp"

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::ui {

WorkspaceRouter::WorkspaceRouter(EditorSessionController* editor_session, QObject* parent)
    : QObject(parent), editor_session_(editor_session) {}

void WorkspaceRouter::OpenLibrary() {
  if (workspace_ == QStringLiteral("library") && element_id_ == 0 && image_id_ == 0) {
    return;
  }
  if (editor_session_) {
    // Parent visibility changes do not reliably notify the retained viewport.
    // Pause producer handshakes before hiding its workspace.
    editor_session_->SetWorkspacePresentationActive(false);
  }
  workspace_  = QStringLiteral("library");
  element_id_ = 0;
  image_id_   = 0;
  emit RouteChanged();
}

void WorkspaceRouter::OpenEditor(uint elementId, uint imageId) {
  workspace_  = QStringLiteral("editor");
  element_id_ = elementId;
  image_id_   = imageId;
  // Publish the visual route first. The viewport must be visible before Open
  // can produce a frame that requires a presentation acknowledgement.
  emit RouteChanged();
  if (editor_session_) {
    editor_session_->SetWorkspacePresentationActive(true);
    // EditorSessionController chooses Open, Switch, or Close. Do not pre-close
    // here: A→B must remain one service-owned switch operation.
    editor_session_->Open(elementId, imageId);
  }
}

}  // namespace alcedo::ui
