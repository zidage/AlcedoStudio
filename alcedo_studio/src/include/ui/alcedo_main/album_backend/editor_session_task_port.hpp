//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "app/editor_session_ports.hpp"

namespace alcedo::ui {

class BackgroundTaskController;

/// Publishes the editor-save task and its navigation locks to the shared task
/// controller. It owns only task IDs and never coordinates save persistence.
class EditorSessionTaskPort final : public alcedo::IEditorTaskPort {
 public:
  /// Construct a task port around the optional application task controller.
  explicit EditorSessionTaskPort(BackgroundTaskController* background_tasks = nullptr);

  /// Replace the task controller used for subsequent task registrations.
  void SetBackgroundTasks(BackgroundTaskController* background_tasks);
  /// Register one logical editor operation and return its local ID.
  auto BeginTask(const std::string& name, sl_element_id_t element_id) -> std::uint64_t override;
  /// Finish a previously registered logical editor operation.
  void EndTask(std::uint64_t task_id, bool success, const std::string& message) override;

 private:
  BackgroundTaskController*                  background_tasks_ = nullptr;
  mutable std::mutex                         mutex_;
  std::uint64_t                              next_id_ = 0;
  std::unordered_map<std::uint64_t, QString> active_task_ids_;
};

}  // namespace alcedo::ui
