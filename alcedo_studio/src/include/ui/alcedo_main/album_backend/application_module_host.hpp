//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class QQmlEngine;

#include "app/ai_provider_profile.hpp"
#include "app/download_service.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/image_analysis_service.hpp"
#include "app/lut_library_service.hpp"
#include "app/lut_package_service.hpp"
#include "app/model_download_service.hpp"
#include "app/update_service.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
#include "ui/alcedo_main/album_backend/application_close_coordinator.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_behavior_preferences.hpp"
#include "ui/alcedo_main/album_backend/editor_comparison_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/image_analysis_controller.hpp"
#include "ui/alcedo_main/album_backend/image_analysis_sink.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/library_mutation_operations.hpp"
#include "ui/alcedo_main/album_backend/lut_library_controller.hpp"
#include "ui/alcedo_main/album_backend/lut_library_model.hpp"
#include "ui/alcedo_main/album_backend/model_download_controller.hpp"
#include "ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp"
#include "ui/alcedo_main/album_backend/project_db_write_barrier.hpp"
#include "ui/alcedo_main/album_backend/project_launch_coordinator.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/album_backend/workspace_router.hpp"

namespace alcedo::ui {

/// Process-local UI module composition root. Owns module lifecycle and exposes
/// typed module accessors for QML (`appModules`). Does not forward module
/// actions or mirror module-specific state.
class ApplicationModuleHost final : public QObject {
  Q_OBJECT
  Q_PROPERTY(ProjectModule* project READ project CONSTANT)
  Q_PROPERTY(LibraryModule* library READ library CONSTANT)
  Q_PROPERTY(FolderController* folders READ folders CONSTANT)
  Q_PROPERTY(ImageController* images READ images CONSTANT)
  Q_PROPERTY(StatsEngine* stats READ stats CONSTANT)
  Q_PROPERTY(SearchController* search READ search CONSTANT)
  Q_PROPERTY(ImportExportHandler* importExport READ import_export CONSTANT)
  Q_PROPERTY(NikonHeRecoveryController* nikonHeRecovery READ nikon_he_recovery CONSTANT)
  Q_PROPERTY(BackgroundTaskController* backgroundTasks READ background_tasks CONSTANT)
  Q_PROPERTY(InteractionPolicyController* interactionPolicy READ interaction_policy CONSTANT)
  Q_PROPERTY(ModelDownloadController* modelDownload READ model_download CONSTANT)
  Q_PROPERTY(alcedo::UpdateService* updates READ updates CONSTANT)
  Q_PROPERTY(alcedo::LutLibraryService* lutLibrary READ lut_library CONSTANT)
  Q_PROPERTY(alcedo::LutPackageService* lutPackages READ lut_packages CONSTANT)
  /// LUT browser rows over lutLibrary; one instance shared by every LUT surface (plan 6.4).
  Q_PROPERTY(LutLibraryModel* lutBrowser READ lut_browser CONSTANT)
  /// The Color Grade target of LUT application, read from editorSession.
  Q_PROPERTY(LutLibraryController* lutTarget READ lut_target CONSTANT)
  Q_PROPERTY(SemanticGenerationController* semanticGeneration READ semantic_generation CONSTANT)
  Q_PROPERTY(
      alcedo::AiProviderProfileController* aiProviderProfiles READ ai_provider_profiles CONSTANT)
  Q_PROPERTY(ImageAnalysisController* imageAnalysis READ image_analysis CONSTANT)
  Q_PROPERTY(AdjustmentTransferController* adjustmentTransfer READ adjustment_transfer CONSTANT)
  Q_PROPERTY(EditorSessionController* editorSession READ editor_session CONSTANT)
  /// The editor comparison of the open image (Compare actions, panel, and view).
  Q_PROPERTY(EditorComparisonController* editorComparison READ editor_comparison CONSTANT)
  Q_PROPERTY(WorkspaceRouter* workspaceRouter READ workspace_router CONSTANT)
  Q_PROPERTY(EditorBehaviorPreferences* editorBehavior READ editor_behavior CONSTANT)
  /// Project open and create launch state and requests.
  Q_PROPERTY(ProjectLaunchCoordinator* projectLaunch READ project_launch CONSTANT)
  /// Application quit decision and the editor finalize sequence of a window or project close.
  Q_PROPERTY(ApplicationCloseCoordinator* applicationClose READ application_close CONSTANT)
  /// Library delete targets, delete scope, post-delete pruning, and rating rules.
  Q_PROPERTY(LibraryMutationOperations* libraryMutations READ library_mutations CONSTANT)

 public:
  struct LifecycleEvent {
    enum class Kind { Constructed, Destroyed };
    Kind        kind;
    std::string type_name;
    const void* object = nullptr;
  };
  using LifecycleObserver = std::function<void(const LifecycleEvent&)>;

  /// Construction options of the two LUT services. An empty factory selects the production
  /// options: the home library root and the build-configured signed feed. When the package
  /// options have no downloader, the host supplies the shared DownloadService admission.
  /// Tests use these factories to point the services at a temporary root and a local feed.
  struct LutServiceFactories {
    std::function<alcedo::LutLibraryServiceOptions()> library_options;
    std::function<alcedo::LutPackageServiceOptions()> package_options;
  };

  explicit ApplicationModuleHost(QObject* parent = nullptr, LifecycleObserver observer = {},
                                 LutServiceFactories lut_services = {});
  ~ApplicationModuleHost() override;

  // Typed accessors (C++ / tests).
  [[nodiscard]] auto project() -> ProjectModule* { return project_.get(); }
  [[nodiscard]] auto project() const -> const ProjectModule* { return project_.get(); }
  [[nodiscard]] auto library() -> LibraryModule* { return library_.get(); }
  [[nodiscard]] auto library() const -> const LibraryModule* { return library_.get(); }
  [[nodiscard]] auto folders() -> FolderController* { return folders_.get(); }
  [[nodiscard]] auto images() -> ImageController* { return images_.get(); }
  [[nodiscard]] auto stats() -> StatsEngine* { return stats_.get(); }
  [[nodiscard]] auto search() -> SearchController* { return search_.get(); }
  [[nodiscard]] auto import_export() -> ImportExportHandler* { return import_export_.get(); }
  [[nodiscard]] auto nikon_he_recovery() -> NikonHeRecoveryController* {
    return nikon_he_recovery_.get();
  }
  [[nodiscard]] auto background_tasks() -> BackgroundTaskController* {
    return background_tasks_.get();
  }
  [[nodiscard]] auto interaction_policy() -> InteractionPolicyController* {
    return interaction_policy_.get();
  }
  [[nodiscard]] auto model_download() -> ModelDownloadController* { return model_download_.get(); }
  [[nodiscard]] auto updates() -> alcedo::UpdateService* { return updates_.get(); }
  [[nodiscard]] auto lut_library() -> alcedo::LutLibraryService* { return lut_library_.get(); }
  [[nodiscard]] auto lut_packages() -> alcedo::LutPackageService* { return lut_packages_.get(); }
  [[nodiscard]] auto semantic_generation() -> SemanticGenerationController* {
    return semantic_generation_.get();
  }
  [[nodiscard]] auto ai_provider_profiles() -> alcedo::AiProviderProfileController* {
    return ai_provider_profiles_.get();
  }
  [[nodiscard]] auto image_analysis() -> ImageAnalysisController* { return image_analysis_.get(); }
  [[nodiscard]] auto adjustment_transfer() -> AdjustmentTransferController* {
    return adjustment_transfer_.get();
  }
  [[nodiscard]] auto db_write_barrier() -> ProjectDbWriteBarrier& { return *db_write_barrier_; }
  [[nodiscard]] auto image_analysis_sink() -> IImageAnalysisSink* {
    return image_analysis_sink_.get();
  }
  [[nodiscard]] auto image_analysis_concurrency_limit()
      -> const std::shared_ptr<alcedo::ImageAnalysisConcurrencyLimit>& {
    return image_analysis_concurrency_limit_;
  }

  [[nodiscard]] auto editor_session() -> EditorSessionController* { return editor_session_.get(); }
  [[nodiscard]] auto editor_comparison() -> EditorComparisonController* {
    return editor_comparison_.get();
  }
  [[nodiscard]] auto lut_browser() -> LutLibraryModel* { return lut_browser_.get(); }
  [[nodiscard]] auto lut_target() -> LutLibraryController* { return lut_target_.get(); }
  [[nodiscard]] auto workspace_router() -> WorkspaceRouter* { return workspace_router_.get(); }
  [[nodiscard]] auto editor_behavior() -> EditorBehaviorPreferences* {
    return editor_behavior_.get();
  }
  [[nodiscard]] auto project_launch() -> ProjectLaunchCoordinator* { return project_launch_.get(); }
  [[nodiscard]] auto application_close() -> ApplicationCloseCoordinator* {
    return application_close_.get();
  }
  [[nodiscard]] auto library_mutations() -> LibraryMutationOperations* {
    return library_mutations_.get();
  }
  /// Phase 5A application-layer editor session (owned by the host, not QML).
  [[nodiscard]] auto editor_session_service() -> alcedo::EditorSessionService* {
    return editor_session_runtime_ ? editor_session_runtime_->service.get() : nullptr;
  }
  [[nodiscard]] auto editor_render_coordinator() -> alcedo::EditorRenderCoordinator* {
    return editor_session_runtime_ ? editor_session_runtime_->coordinator.get() : nullptr;
  }
  /// First-frame scheduler (null when runtime uses bootstrap only).
  [[nodiscard]] auto editor_session_scheduler() -> EditorSessionRenderSchedulerPort* {
    return editor_session_scheduler_.get();
  }

  /// True when no background task runs, no import runs, and no export is in flight.
  [[nodiscard]] auto IsIdle() const -> bool;

  // Explicitly idempotent so the application can shut down modules before the
  // QML engine is torn down. The destructor calls the same path.
  void Shutdown();

  /// Register the album thumbnail `image://alcedo-thumb/` provider on @p engine.
  /// Safe to call once per engine; the engine takes ownership of the provider.
  void AttachQmlEngine(QQmlEngine* engine);

 private:
  void RecordConstruction(const char* type_name, const void* object);
  void RecordDestruction(const char* type_name, const void* object);
  void ShutdownModules();
  /// Removes the runtime workspace that ShutdownModules recorded in
  /// pending_workspace_removal_. Called by the destructor after ProjectModule (the last owner
  /// of the project services and their DuckDB file) is destroyed, because Windows cannot
  /// delete an open file. A failure is written with qWarning (path and OS error), followed by
  /// one qWarning per file that still cannot be removed (on Windows with the processes that
  /// hold it). It is not retried. Clears pending_workspace_removal_.
  void RemovePendingWorkspace();

  // Owned in construction dependency order. Destroyed in reverse.
  std::unique_ptr<BackgroundTaskController>            background_tasks_;
  std::unique_ptr<InteractionPolicyController>         interaction_policy_;
  std::unique_ptr<alcedo::DownloadService>             download_service_;
  std::unique_ptr<alcedo::ModelDownloadService>        model_download_service_;
  std::unique_ptr<alcedo::UpdateService>               updates_;
  std::unique_ptr<alcedo::LutLibraryService>           lut_library_;
  std::unique_ptr<alcedo::LutPackageService>           lut_packages_;
  std::unique_ptr<ProjectModule>                       project_;
  std::unique_ptr<LibraryModule>                       library_;
  std::unique_ptr<FolderController>                    folders_;
  std::unique_ptr<ImageController>                     images_;
  std::unique_ptr<StatsEngine>                         stats_;
  std::unique_ptr<SearchController>                    search_;
  std::unique_ptr<ModelDownloadController>             model_download_;
  std::unique_ptr<alcedo::AiProviderProfileController> ai_provider_profiles_;
  std::unique_ptr<SemanticGenerationController>        semantic_generation_;
  std::shared_ptr<alcedo::ImageAnalysisConcurrencyLimit> image_analysis_concurrency_limit_;
  std::unique_ptr<ProjectDbWriteBarrier>               db_write_barrier_;
  std::shared_ptr<IImageAnalysisSink>                  image_analysis_sink_;
  std::unique_ptr<ImageAnalysisController>             image_analysis_;
  std::unique_ptr<ImportExportHandler>                 import_export_;
  std::unique_ptr<NikonHeRecoveryController>           nikon_he_recovery_;
  std::unique_ptr<AdjustmentTransferController>        adjustment_transfer_;
  std::unique_ptr<alcedo::EditorSessionRuntime>        editor_session_runtime_;
  std::shared_ptr<EditorSessionRenderSchedulerPort>    editor_session_scheduler_;
  std::unique_ptr<EditorSessionController>             editor_session_;
  std::unique_ptr<EditorComparisonController>          editor_comparison_;
  std::unique_ptr<LutLibraryModel>                     lut_browser_;
  std::unique_ptr<LutLibraryController>                lut_target_;
  std::unique_ptr<WorkspaceRouter>                     workspace_router_;
  std::unique_ptr<EditorBehaviorPreferences>           editor_behavior_;
  std::unique_ptr<ProjectLaunchCoordinator>            project_launch_;
  std::unique_ptr<ApplicationCloseCoordinator>         application_close_;
  std::unique_ptr<LibraryMutationOperations>           library_mutations_;

  LifecycleObserver                                    lifecycle_observer_{};
  bool                                                 shutting_down_ = false;
  // Runtime workspace of the loaded project at shutdown; the destructor removes it.
  std::filesystem::path                                pending_workspace_removal_{};
};

}  // namespace alcedo::ui
