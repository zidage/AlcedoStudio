//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

#include "ui/alcedo_main/album_backend/editor_controller.hpp"

namespace alcedo::ui {

EditorSessionController::EditorSessionController(EditorController* editor, QObject* parent)
    : QObject(parent), editor_(editor) {}

bool EditorSessionController::active() const { return editor_ && editor_->editor_active(); }

uint EditorSessionController::element_id() const {
  return editor_ ? static_cast<uint>(editor_->editor_element_id()) : 0;
}

uint EditorSessionController::image_id() const {
  return editor_ ? static_cast<uint>(editor_->editor_image_id()) : 0;
}

void EditorSessionController::Open(uint elementId, uint imageId) {
  if (editor_) {
    editor_->OpenEditor(elementId, imageId);
  }
  emit StateChanged();
}

void EditorSessionController::Close() {
  if (editor_) {
    editor_->CloseEditor();
  }
  emit StateChanged();
}

void EditorSessionController::Finalize(bool persistChanges) {
  if (editor_) {
    editor_->FinalizeEditorSession(persistChanges);
  }
  emit StateChanged();
}

}  // namespace alcedo::ui
