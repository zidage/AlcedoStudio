//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <QString>
#include <json.hpp>

#include "app/history_mgmt_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/version.hpp"
#include "ui/alcedo_main/editor_dialog/modules/versioning.hpp"

class QListWidgetItem;

namespace alcedo::ui {

/// History operations for the editor. Reusable logic (apply params, undo cursor,
/// version checkout) does not own QWidget*; UI prompts are injected via Callbacks
/// so QWidget dialogs stay in the dialog shell (Phase 5A-Fix).
class EditorHistoryCoordinator {
 public:
  struct Dependencies {
    std::shared_ptr<EditHistoryMgmtService> history_service;
    std::shared_ptr<EditHistoryGuard>       history_guard;
    std::shared_ptr<PipelineGuard>          pipeline_guard;
    sl_element_id_t                         element_id = 0;
  };

  struct Callbacks {
    /// Show an informational or warning message (title, text, is_warning).
    std::function<void(const QString&, const QString&, bool)> show_message;
    /// Prompt for a string; returns nullopt if cancelled.
    std::function<std::optional<QString>(const QString& title, const QString& label,
                                         const QString& current)>
        prompt_text;
    std::function<bool(bool)> reload_ui_state_from_pipeline;
    std::function<void()>     after_pipeline_params_imported;
    std::function<void()>     refresh_version_log_selection_styles;
  };

  EditorHistoryCoordinator(Dependencies dependencies, Callbacks callbacks);

  auto WorkingVersion() -> alcedo::WorkingVersion&;
  auto WorkingVersion() const -> const alcedo::WorkingVersion&;

  void SetUiContext(const versioning::VersionUiContext& ui);
  void SeedWorkingVersionFromActive();

  auto ReconstructPipelineParamsForVersion(Version& version) const
      -> std::optional<nlohmann::json>;
  auto ReloadUiStateFromPipeline(bool reset_to_defaults_if_missing) -> bool;
  auto ApplyPipelineParamsToEditor(const nlohmann::json& params) -> bool;
  auto ReloadEditorFromHistoryVersion(Version& version, QString* error) -> bool;

  /// UI entry: list item → version id. Widgets only at the call site.
  void CheckoutSelectedVersion(QListWidgetItem* item);
  void CheckoutVersionById(const QString& version_id);
  void RenameVersionById(const QString& version_id);
  void UndoLastTransaction();
  void MoveCursorTo(size_t target_cursor);
  void UpdateVersionUi();
  void CreateVersion();

 private:
  void ShowMessage(const QString& title, const QString& text, bool is_warning) const;

  Dependencies                   dependencies_;
  Callbacks                      callbacks_;
  versioning::VersionUiContext   ui_{};
  versioning::VersionUiCallbacks ui_callbacks_{};
  alcedo::WorkingVersion         working_version_{};
};

}  // namespace alcedo::ui
