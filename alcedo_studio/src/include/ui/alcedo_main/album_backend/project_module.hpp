//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "edit/pipeline/pipeline_accelerator.hpp"
#include "ui/alcedo_main/album_backend/project_handler.hpp"
#include "ui/alcedo_main/album_backend/ui_status_sink.hpp"
#include "ui/alcedo_main/i18n.hpp"
#include "app/semantic_generation_service.hpp"

namespace alcedo::ui {

/// Narrow lifecycle callbacks used by ProjectModule and ProjectHandler. These
/// are behavior seams, not a service bag: project code can ask the composition
/// root to perform one lifecycle operation without acquiring sibling modules.
struct ProjectLifecycleHooks {
  std::function<QString()> project_switch_block_reason;
  std::function<void()> finalize_editor_session;
  std::function<void()> clear_project_ui_state;
  std::function<void()> project_opened;
  std::function<bool(const QString&)> should_keep_semantic_model_data;
  std::function<void()> refresh_semantic_state;
  std::function<bool()> export_inflight;
  std::function<void()> refresh_translations;
};

/// Project lifecycle module: open/create/save, accelerator preference, recent
/// projects, and the legacy task status surface. Implements IUiStatusSink so
/// other modules report status without depending on the host.
class ProjectModule final : public QObject, public IUiStatusSink {
  Q_OBJECT
  Q_PROPERTY(bool serviceReady READ ServiceReady NOTIFY ServiceStateChanged)
  Q_PROPERTY(QString serviceMessage READ ServiceMessage NOTIFY ServiceStateChanged)
  Q_PROPERTY(QVariantList recentProjects READ RecentProjects NOTIFY RecentProjectsChanged)
  Q_PROPERTY(QVariantList acceleratorOptions READ AcceleratorOptions NOTIFY AcceleratorStateChanged)
  Q_PROPERTY(QString acceleratorBackend READ AcceleratorBackend NOTIFY AcceleratorStateChanged)
  Q_PROPERTY(QString acceleratorWarning READ AcceleratorWarning NOTIFY AcceleratorStateChanged)
  Q_PROPERTY(
      bool acceleratorPreparing READ AcceleratorPreparing NOTIFY AcceleratorPreparationStateChanged)
  Q_PROPERTY(QString acceleratorPreparationStatus READ AcceleratorPreparationStatus NOTIFY
                 AcceleratorPreparationStateChanged)
  Q_PROPERTY(bool projectLoading READ ProjectLoading NOTIFY ProjectLoadStateChanged)
  Q_PROPERTY(
      QString projectLoadingMessage READ ProjectLoadingMessage NOTIFY ProjectLoadStateChanged)
  Q_PROPERTY(QString taskStatus READ TaskStatus NOTIFY TaskStateChanged)
  Q_PROPERTY(int taskProgress READ TaskProgress NOTIFY TaskStateChanged)
  Q_PROPERTY(bool taskCancelVisible READ TaskCancelVisible NOTIFY TaskStateChanged)

 public:
  explicit ProjectModule(QObject* parent = nullptr);
  ~ProjectModule() override = default;

  void SetLifecycleHooks(ProjectLifecycleHooks hooks);

  // ── Core accessors ─────────────────────────────────────────────────────
  [[nodiscard]] auto handler() -> ProjectHandler& { return handler_; }
  [[nodiscard]] auto handler() const -> const ProjectHandler& { return handler_; }
  [[nodiscard]] auto accelerator_preference() const -> AcceleratorBackendPreference {
    return accelerator_preference_;
  }
  // ── Q_PROPERTY getters ─────────────────────────────────────────────────
  bool         ServiceReady() const { return service_ready_; }
  QString      ServiceMessage() const { return service_message_text_.Render(); }
  QVariantList RecentProjects() const { return recent_projects_; }
  QVariantList AcceleratorOptions() const { return accelerator_options_; }
  QString      AcceleratorBackend() const { return accelerator_backend_key_; }
  QString      AcceleratorWarning() const {
    return IsAcceleratorWarningAcknowledged() ? QString{} : accelerator_warning_text_.Render();
  }
  bool    AcceleratorPreparing() const { return accelerator_preparing_; }
  QString AcceleratorPreparationStatus() const {
    return accelerator_preparation_status_text_.Render();
  }
  bool    ProjectLoading() const { return handler_.project_loading(); }
  QString ProjectLoadingMessage() const { return handler_.project_loading_message(); }
  QString TaskStatus() const { return task_status_text_.Render(); }
  int     TaskProgress() const { return task_progress_; }
  bool    TaskCancelVisible() const { return task_cancel_visible_; }

  // ── IUiStatusSink ──────────────────────────────────────────────────────
  void SetServiceMessage(const i18n::LocalizedText& message) override;
  void SetTaskState(const i18n::LocalizedText& status, int progress,
                    bool cancelVisible) override;
  void ScheduleIdleTaskStateReset(int delayMs) override;

  void SetServiceState(bool ready, const i18n::LocalizedText& message);
  void SetServiceMessageForCurrentProject(const i18n::LocalizedText& message);

  // ── Project lifecycle Q_INVOKABLE ──────────────────────────────────────
  Q_INVOKABLE bool PromptAndLoadProject();
  Q_INVOKABLE bool PromptAndCreateProject();
  Q_INVOKABLE bool SetAcceleratorBackend(const QString& backendKey);
  Q_INVOKABLE void StartAcceleratorPreparation();
  Q_INVOKABLE void AcknowledgeAcceleratorWarning();
  Q_INVOKABLE bool LoadProject(const QString& metaFileUrlOrPath);
  Q_INVOKABLE bool CreateProjectInFolder(const QString& folderUrlOrPath);
  Q_INVOKABLE bool CreateProjectInFolderNamed(const QString& folderUrlOrPath,
                                              const QString& projectName);
  Q_INVOKABLE bool SaveProject();

  // ── Internals used by ProjectHandler / host ────────────────────────────
  void InitializeAcceleratorSettings();
  void RefreshTranslations();
  void LoadRecentProjectsFromSettings();
  void RegisterRecentProject(const std::filesystem::path& projectPath);
  void RemoveRecentProject(const std::filesystem::path& projectPath);
  void ApplyAcceleratorPreferenceToServices();
  void NotifyProjectLoadStateChanged();
  void HandleProjectOpened();
  void ClearProjectUiState();
  [[nodiscard]] auto ProjectSwitchBlockReason() const -> QString;
  void              FinalizeEditorSession();
  [[nodiscard]] bool ShouldKeepSemanticModelData(const QString& profileId) const;

 signals:
  void ServiceStateChanged();
  void RecentProjectsChanged();
  void AcceleratorStateChanged();
  void AcceleratorPreparationStateChanged();
  void TaskStateChanged();
  void ProjectChanged();
  void projectChanged();
  void ProjectLoadStateChanged();

 private:
  void StartOpenClPreparationIfNeeded();
  void SetAcceleratorPreparationState(bool preparing, const i18n::LocalizedText& status);
  void RebuildAcceleratorOptions();
  bool IsAcceleratorWarningAcknowledged() const;
  void PersistAcceleratorWarningAcknowledgement() const;
  void PersistRecentProjects() const;

  ProjectHandler handler_;

  ProjectLifecycleHooks lifecycle_hooks_{};

  i18n::LocalizedText service_message_text_{};
  bool                service_ready_ = false;
  QVariantList        recent_projects_{};
  AcceleratorBackendPreference accelerator_preference_ = AcceleratorBackendPreference::Auto;
  QString                      accelerator_backend_key_{};
  QString                      accelerator_warning_id_{};
  QVariantList                 accelerator_options_{};
  i18n::LocalizedText          accelerator_warning_text_{};
  i18n::LocalizedText          accelerator_preparation_status_text_{};
  bool                         accelerator_preparing_       = false;
  bool                         accelerator_prepare_started_ = false;
  bool                         cuda_backend_available_      = false;
  bool                         opencl_backend_available_    = false;
  bool                         metal_backend_available_     = false;
  i18n::LocalizedText          task_status_text_{};
  int                          task_progress_       = 0;
  bool                         task_cancel_visible_ = false;
};

}  // namespace alcedo::ui
