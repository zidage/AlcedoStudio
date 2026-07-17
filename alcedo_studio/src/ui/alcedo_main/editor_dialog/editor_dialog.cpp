//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/editor_dialog.hpp"

#include "ui/alcedo_main/editor_dialog/dialog.hpp"

namespace alcedo::ui {

void ResetOpenEditorDialogCallCount() {}

auto OpenEditorDialogCallCount() -> int { return 0; }

auto OpenEditorDialog(std::shared_ptr<ImagePoolService> image_pool,
                      std::shared_ptr<PipelineGuard> pipeline_guard,
                      std::shared_ptr<EditHistoryMgmtService> history_service,
                      std::shared_ptr<EditHistoryGuard> history_guard,
                      sl_element_id_t element_id, image_id_t image_id,
                      QWidget* parent) -> bool {
  return RunEditorDialog(std::move(image_pool), std::move(pipeline_guard),
                         std::move(history_service), std::move(history_guard),
                         element_id, image_id, parent);
}

}  // namespace alcedo::ui
