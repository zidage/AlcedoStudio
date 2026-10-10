//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QVariantList>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "ui/alcedo_main/i18n.hpp"
#include "app/export_service.hpp"
#include "app/import_service.hpp"
#include "app/mask_thumbnail_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "app/thumbnail_service.hpp"
#include "concurrency/thread_pool.hpp"
#include "storage/store/sleeve/element_store.hpp"

namespace alcedo::ui {

class ProjectModule;

/// How the user reaches the project of one load request.
/// - kPreview: the welcome surface stays visible and shows the project overview. The user has
///   not entered the project, so the project has no user changes.
/// - kEnter: the welcome surface closes and the Library shows the project.
enum class ProjectEntryMode : uint8_t { kPreview, kEnter };

/// Owns all back-end services and manages lifetime of a single project.
class ProjectHandler {
 public:
  explicit ProjectHandler(ProjectModule& project_module);

  bool InitializeServices(const std::filesystem::path& dbPath,
                          const std::filesystem::path& metaPath,
                          ProjectOpenMode              openMode,
                          const std::filesystem::path& packagePath,
                          const std::filesystem::path& workspaceDir,
                          const std::filesystem::path& recentProjectPath,
                          ProjectEntryMode             entryMode);
  // Opens the packed project at @p packagePath: the loader thread unpacks it into
  // @p workspaceDir (created by the caller) and then starts the services as
  // InitializeServices does for an existing project.
  bool OpenPackedProject(const std::filesystem::path& packagePath,
                         const std::filesystem::path& workspaceDir, const QString& projectName,
                         ProjectEntryMode entryMode);
  /// Enters the previewed project. UI thread only.
  /// - Loaded, unentered project and no running load: sets the entered state and registers the
  ///   recent entry.
  /// - Running kPreview load: changes the load to kEnter; the completion enters the project.
  /// Returns false and changes nothing in every other state.
  bool RequestEnterLoadedProject();
  bool PersistCurrentProjectState();
  /// Saves the loaded project before it closes. For an entered project: Mini-Git sync and
  /// garbage collection of unreachable commits, the purge of uninstalled semantic models, the
  /// metadata save, and the package write. A project that the user did not enter has no user
  /// changes, so only its thumbnail cache index is flushed. Returns false and writes @p error
  /// when a step fails. UI thread only.
  bool PersistProjectForClose(QString* error);
  /// Closes the loaded project without saving it: clears the project UI state and the project
  /// paths, and retires the services and the unpacked workspace. Call PersistProjectForClose
  /// first to keep the changes. Does nothing when no project is loaded. UI thread only.
  void CloseProject();
  bool PackageCurrentProjectFiles(QString* errorOut = nullptr) const;
  /// Returns a job that saves the project metadata and then packs the project file. The job
  /// keeps its own references to the project and paths, so a worker thread can run it
  /// without this handler. It packs under the same lock as PackageCurrentProjectFiles.
  [[nodiscard]] auto MakeSaveAndPackageJob() const -> std::function<bool(QString*)>;
  void SetProjectLoadingState(bool loading, const i18n::LocalizedText& message);
  void ClearProjectData();

  // Drops DuckDB embeddings/labels/prototypes and the SemanticModel registry row
  // for every registered model whose profile is no longer installed locally.
  // Keeps rows for still-installed models (so switching back stays free) and for
  // profiles absent from the catalog (install state indeterminate). Returns true
  // if any rows were purged.
  bool PurgeUninstalledSemanticModels();

  [[nodiscard]] auto project() const -> const std::shared_ptr<ProjectService>& { return project_; }
  [[nodiscard]] auto pipeline_service() const -> const std::shared_ptr<PipelineMgmtService>& {
    return pipeline_service_;
  }
  [[nodiscard]] auto thumbnail_service() const -> const std::shared_ptr<ThumbnailService>& {
    return thumbnail_service_;
  }
  [[nodiscard]] auto mask_thumbnail_service() const
      -> const std::shared_ptr<MaskThumbnailService>& {
    return mask_thumbnail_service_;
  }
  [[nodiscard]] auto import_service() const -> ImportServiceImpl* { return import_service_.get(); }
  [[nodiscard]] auto export_service() const -> const std::shared_ptr<ExportService>& {
    return export_service_;
  }

  [[nodiscard]] auto db_path() const -> const std::filesystem::path& { return db_path_; }
  [[nodiscard]] auto meta_path() const -> const std::filesystem::path& { return meta_path_; }
  [[nodiscard]] auto package_path() const -> const std::filesystem::path& {
    return project_package_path_;
  }
  [[nodiscard]] auto workspace_dir() const -> const std::filesystem::path& {
    return project_workspace_dir_;
  }
  [[nodiscard]] bool project_loading() const { return project_loading_; }
  [[nodiscard]] auto project_loading_message() const -> QString {
    return project_loading_message_text_.Render();
  }
  [[nodiscard]] auto project_load_request_id() const -> uint64_t {
    return project_load_request_id_;
  }
  /// True when the user entered the loaded project. False for a preview and with no project.
  [[nodiscard]] bool project_entered() const { return project_entered_; }
  /// Entry mode of the running load, or of the last load when no load runs.
  [[nodiscard]] auto load_entry_mode() const -> ProjectEntryMode { return load_entry_mode_; }
  /// Overview counts of the loaded project, read on the loader thread at load time. The value
  /// stays valid while the project is not entered because no user edit can run in that state.
  [[nodiscard]] auto project_overview() const -> const std::optional<ProjectOverviewCounts>& {
    return project_overview_;
  }
  /// Path that the recent-project list uses for the loaded project.
  [[nodiscard]] auto recent_project_path() const -> const std::filesystem::path& {
    return recent_project_path_;
  }

 private:
  /// Services of a project that a load replaced, and its unpacked workspace to remove.
  struct RetiredProject {
    std::shared_ptr<ProjectService>       project_{};
    std::shared_ptr<PipelineMgmtService>  pipeline_service_{};
    std::shared_ptr<ThumbnailService>     thumbnail_service_{};
    std::shared_ptr<MaskThumbnailService> mask_thumbnail_service_{};
    std::unique_ptr<ImportServiceImpl>    import_service_{};
    std::shared_ptr<ExportService>        export_service_{};
    std::filesystem::path                 workspace_dir_{};
  };

  /// Stop @p retired's AI sidecar on this (UI) thread, then destroy its services and remove its
  /// workspace on project_retirement_thread_.
  void RetireProject(std::shared_ptr<RetiredProject> retired);

  // Shared by InitializeServices and OpenPackedProject. With @p unpackProjectName, the loader
  // thread first unpacks @p packagePath into @p workspaceDir and opens the unpacked files
  // instead of @p dbPath and @p metaPath.
  bool StartProjectLoad(const std::filesystem::path& dbPath, const std::filesystem::path& metaPath,
                        ProjectOpenMode openMode, const std::filesystem::path& packagePath,
                        const std::filesystem::path& workspaceDir,
                        const std::filesystem::path& recentProjectPath,
                        std::optional<QString> unpackProjectName, ProjectEntryMode entryMode);

  ProjectModule& project_module_;

  std::shared_ptr<ProjectService>         project_{};
  std::shared_ptr<PipelineMgmtService>    pipeline_service_{};
  std::shared_ptr<ThumbnailService>       thumbnail_service_{};
  std::shared_ptr<MaskThumbnailService>   mask_thumbnail_service_{};
  std::unique_ptr<ImportServiceImpl>      import_service_{};
  std::shared_ptr<ExportService>          export_service_{};

  std::filesystem::path db_path_{};
  std::filesystem::path meta_path_{};
  std::filesystem::path project_package_path_{};
  std::filesystem::path project_workspace_dir_{};
  std::filesystem::path recent_project_path_{};
  // Two packers would share the same "<package>.tmp" file, so packing is serialized.
  std::shared_ptr<std::mutex> package_mutex_ = std::make_shared<std::mutex>();

  bool                project_loading_ = false;
  i18n::LocalizedText project_loading_message_text_{};
  uint64_t            project_load_request_id_ = 0;
  // Written on the UI thread only: at load start, at load completion, and in
  // RequestEnterLoadedProject.
  bool                                 project_entered_ = false;
  ProjectEntryMode                     load_entry_mode_ = ProjectEntryMode::kEnter;
  std::optional<ProjectOverviewCounts> project_overview_{};

  /// Destroys the services of a project replaced by a load (closing its database and stopping
  /// its workers) and then removes its unpacked workspace, off the UI thread: both scale with
  /// the project size and ran on the UI thread after every Welcome preview switch. The AI
  /// sidecar is stopped on the UI thread before the handoff, so a retirement never waits for
  /// the UI thread and the destructor can drain this pool from it. Declared last: destroyed
  /// first.
  ThreadPool project_retirement_thread_{1};
};

}  // namespace alcedo::ui
