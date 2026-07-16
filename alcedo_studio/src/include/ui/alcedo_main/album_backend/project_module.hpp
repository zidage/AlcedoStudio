//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "edit/pipeline/pipeline_accelerator.hpp"
#include "ui/alcedo_main/album_backend/project_handler.hpp"
#include "ui/alcedo_main/album_backend/ui_status_sink.hpp"
#include "ui/alcedo_main/i18n.hpp"
#include "app/semantic_generation_service.hpp"

namespace alcedo::ui {

class EditorController;
class FolderController;
class ImportExportHandler;
class LibraryModule;
class ModelDownloadController;
class SemanticGenerationController;
class StatsEngine;

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

  // ── Late collaborator binding (host wires after all modules exist) ─────
  void BindCollaborators(ImportExportHandler* import_export, EditorController* editor,
                         LibraryModule* library, FolderController* folders, StatsEngine* stats,
                         SemanticGenerationController* semantic_generation,
                         ModelDownloadController*      model_download);

  // ── Typed accessors for sibling modules ────────────────────────────────
  [[nodiscard]] auto handler() -> ProjectHandler& { return handler_; }
  [[nodiscard]] auto handler() const -> const ProjectHandler& { return handler_; }
  [[nodiscard]] auto accelerator_preference() const -> AcceleratorBackendPreference {
    return accelerator_preference_;
  }
  [[nodiscard]] auto import_export() -> ImportExportHandler* { return import_export_; }
  [[nodiscard]] auto import_export() const -> const ImportExportHandler* {
    return import_export_;
  }
  [[nodiscard]] auto editor() -> EditorController* { return editor_; }
  [[nodiscard]] auto editor() const -> const EditorController* { return editor_; }
  [[nodiscard]] auto library() -> LibraryModule* { return library_; }
  [[nodiscard]] auto library() const -> const LibraryModule* { return library_; }
  [[nodiscard]] auto folders() -> FolderController* { return folders_; }
  [[nodiscard]] auto folders() const -> const FolderController* { return folders_; }
  [[nodiscard]] auto stats() -> StatsEngine* { return stats_; }
  [[nodiscard]] auto stats() const -> const StatsEngine* { return stats_; }
  [[nodiscard]] auto semantic_generation() -> SemanticGenerationController* {
    return semantic_generation_;
  }
  [[nodiscard]] auto semantic_generation() const -> const SemanticGenerationController* {
    return semantic_generation_;
  }
  [[nodiscard]] auto model_download() -> ModelDownloadController* { return model_download_; }
  [[nodiscard]] auto model_download() const -> const ModelDownloadController* {
    return model_download_;
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
  void QueueSemanticGenerationPrompt(std::vector<SemanticGenerationItem> items);
  void ResumeQueuedSemanticGenerationWorkflow();
  auto ActiveSemanticModelKey() const -> std::string;
  auto SemanticLabelDisplayText(sl_element_id_t elementId) const -> QString;

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

  // Bound after host constructs sibling modules (null until BindCollaborators).
  ImportExportHandler*           import_export_        = nullptr;
  EditorController*              editor_               = nullptr;
  LibraryModule*                 library_              = nullptr;
  FolderController*              folders_              = nullptr;
  StatsEngine*                   stats_                = nullptr;
  SemanticGenerationController*  semantic_generation_  = nullptr;
  ModelDownloadController*       model_download_       = nullptr;

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
