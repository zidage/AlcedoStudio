//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/project_handler.hpp"

#include <QMetaObject>
#include <QPointer>

#include <stdexcept>
#include <thread>

#include "app/project_package_service.hpp"
#include "image/image.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"

namespace alcedo::ui {

#define PL_TEXT(text, ...)                                                                      \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) \
                                                     __VA_OPT__(, ) __VA_ARGS__)

ProjectHandler::ProjectHandler(ProjectModule& project_module) : project_module_(project_module) {}

bool ProjectHandler::InitializeServices(const std::filesystem::path& dbPath,
                                        const std::filesystem::path& metaPath,
                                        ProjectOpenMode              openMode,
                                        const std::filesystem::path& packagePath,
                                        const std::filesystem::path& workspaceDir,
                                        const std::filesystem::path& recentProjectPath) {
  if (project_loading_) {
    project_module_.SetServiceMessageForCurrentProject(PL_TEXT("A project load is already in progress."));
    return false;
  }

  const QString switch_block_reason = project_module_.ProjectSwitchBlockReason();
  if (!switch_block_reason.isEmpty()) {
    project_module_.SetServiceMessageForCurrentProject(PL_TEXT("%1", switch_block_reason));
    return false;
  }

  project_module_.FinalizeEditorSession();

  project_module_.SetServiceMessageForCurrentProject((openMode == ProjectOpenMode::kCreateNew)
                                                  ? PL_TEXT("Creating project...")
                                                  : PL_TEXT("Loading project..."));
  SetProjectLoadingState(true, (openMode == ProjectOpenMode::kCreateNew)
                                   ? PL_TEXT("Creating project...")
                                   : PL_TEXT("Loading project..."));
  project_module_.SetTaskState(PL_TEXT("Opening project..."), 0, false);

  const auto request_id = ++project_load_request_id_;
  const auto accelerator_preference = project_module_.accelerator_preference();

  auto old_project   = project_;
  auto old_pipeline  = pipeline_service_;
  auto old_thumbnail = thumbnail_service_;
  auto old_meta      = meta_path_;
  auto old_package   = project_package_path_;
  auto old_workspace = project_workspace_dir_;

  QPointer<ProjectModule> self(&project_module_);
  std::thread([self, request_id, old_project = std::move(old_project),
               old_pipeline = std::move(old_pipeline),
               old_thumbnail = std::move(old_thumbnail), old_meta = std::move(old_meta),
               old_package = std::move(old_package), old_workspace = std::move(old_workspace),
               dbPath, metaPath, packagePath, workspaceDir, recentProjectPath, openMode,
               accelerator_preference]() mutable {
    struct LoadResult {
      bool                                    success_ = false;
      QString                                 error_{};
      std::shared_ptr<ProjectService>         project_{};
      std::shared_ptr<PipelineMgmtService>    pipeline_{};
      std::shared_ptr<ThumbnailService>       thumbnail_{};
      std::unique_ptr<ImportServiceImpl>      import_{};
      std::shared_ptr<ExportService>          export_{};
      std::filesystem::path                   db_path_{};
      std::filesystem::path                   meta_path_{};
      std::filesystem::path                   package_path_{};
      std::filesystem::path                   workspace_dir_{};
      std::filesystem::path                   recent_project_path_{};
      std::filesystem::path                   workspace_to_cleanup_{};
    };

    auto result = std::make_shared<LoadResult>();

    try {
      if (old_pipeline) {
        old_pipeline->Sync();
      }
      if (old_thumbnail) {
        auto stats = old_thumbnail->GetDiskCacheStats();
        if (stats.enabled) {
          old_thumbnail->FlushDiskCacheMetadata();
        }
      }

      if (old_project && !old_meta.empty()) {
        old_project->GetSleeveService()->Sync();
        old_project->GetImagePoolService()->SyncWithStorage();
        old_project->SaveProject(old_meta);

        if (!old_package.empty()) {
          auto package_service = old_project->GetProjectPackageService();
          if (!package_service) {
            throw std::runtime_error("Project package service is unavailable.");
          }

          QString               package_error;
          std::filesystem::path snapshot_path;
          if (!package_service->BuildTempDbSnapshotPath(&snapshot_path, &package_error) ||
              !package_service->CreateLiveDbSnapshot(old_project, snapshot_path, &package_error) ||
              !package_service->WritePackedProject(old_package, old_meta, snapshot_path,
                                                   &package_error)) {
            std::error_code ec;
            if (!snapshot_path.empty()) {
              std::filesystem::remove(snapshot_path, ec);
            }
            const QByteArray err = package_error.toUtf8();
            throw std::runtime_error(err.isEmpty() ? "Failed to pack previous project."
                                                   : err.constData());
          }

          std::error_code ec;
          std::filesystem::remove(snapshot_path, ec);
        }
      }

      result->workspace_to_cleanup_ = old_workspace;

      result->project_   = std::make_shared<ProjectService>(dbPath, metaPath, openMode);
      result->pipeline_  = std::make_shared<PipelineMgmtService>(result->project_->GetStorageService());
      result->pipeline_->SetAcceleratorBackendPreference(accelerator_preference);
      qInfo("pipeline.accelerator backend=%s source=active-editor-backend",
            AcceleratorBackendPreferenceToString(accelerator_preference).data());
      result->thumbnail_ = std::make_shared<ThumbnailService>(
          result->project_->GetSleeveService(), result->project_->GetImagePoolService(),
          result->pipeline_, result->project_->GetStorageService(), result->project_->GetProjectUUID());
      result->import_ = std::make_unique<ImportServiceImpl>(
          result->project_->GetSleeveService(), result->project_->GetImagePoolService(),
          [pipeline = result->pipeline_](sl_element_id_t element_id,
                                         const std::shared_ptr<Image>& image) {
            if (!pipeline) {
              throw std::runtime_error("Pipeline service is unavailable during root creation");
            }
            auto guard = pipeline->LoadPipeline(element_id);
            if (!guard || !guard->pipeline_) {
              throw std::runtime_error("Pipeline is unavailable during root creation");
            }
            if (image && image->HasRawColorContext()) {
              guard->pipeline_->InjectRawMetadata(image->GetRawColorContext());
            }
            pipeline->InitializeImageRoot(
                guard, image && image->HasRawColorContext() ? &image->GetRawColorContext()
                                                            : nullptr);
            guard->dirty_ = true;
            pipeline->SyncPipeline(element_id);
            pipeline->SavePipeline(guard);
          });
      result->export_ = std::make_shared<ExportService>(result->project_->GetSleeveService(),
                                                        result->project_->GetImagePoolService(),
                                                        result->pipeline_);

      if (openMode == ProjectOpenMode::kCreateNew) {
        result->project_->GetSleeveService()->Sync();
        result->project_->GetImagePoolService()->SyncWithStorage();
        result->project_->SaveProject(metaPath);
      }

      result->db_path_   = result->project_->GetDBPath();
      result->meta_path_ = result->project_->GetMetaPath();
      if (result->meta_path_.empty()) {
        result->meta_path_ = metaPath;
      }
      result->package_path_  = packagePath;
      result->workspace_dir_ = workspaceDir;
      result->recent_project_path_ = recentProjectPath;
      result->success_       = true;
    } catch (const std::exception& e) {
      result->success_ = false;
      result->error_   = QString::fromUtf8(e.what());
    } catch (...) {
      result->success_ = false;
      result->error_   = PL_TEXT("Unknown project load error.").Render();
    }

    if (!result->success_ && !workspaceDir.empty()) {
      album_util::CleanupWorkspaceDirectory(workspaceDir);
    }

    if (!self) {
      return;
    }

    QMetaObject::invokeMethod(
        self,
        [self, request_id, result]() mutable {
          if (!self || request_id != self->handler().project_load_request_id()) {
            return;
          }

          auto& ph = self->handler();

          if (!result->success_) {
            ph.SetProjectLoadingState(false, {});
            self->SetServiceMessageForCurrentProject(
                ph.project_ ? PL_TEXT("Requested project failed to open: %1", result->error_)
                            : PL_TEXT("Project open failed: %1", result->error_));
            self->SetTaskState(PL_TEXT("Project open failed."), 0, false);
            return;
          }

          ph.project_               = std::move(result->project_);
          ph.pipeline_service_      = std::move(result->pipeline_);
          ph.thumbnail_service_     = std::move(result->thumbnail_);
          ph.import_service_        = std::move(result->import_);
          ph.export_service_        = std::move(result->export_);
          ph.db_path_               = std::move(result->db_path_);
          ph.meta_path_             = std::move(result->meta_path_);
          ph.project_package_path_  = std::move(result->package_path_);
          ph.project_workspace_dir_ = std::move(result->workspace_dir_);

          if (ph.project_) {
            (void)ph.project_->GetAiSidecarRuntimeService();
          }

          ph.ClearProjectData();
          self->HandleProjectOpened();
          self->SetTaskState(PL_TEXT("No background tasks"), 0, false);

          self->SetServiceState(
              true, ph.project_package_path_.empty()
                        ? PL_TEXT("Loaded project. DB: %1  Meta: %2",
                                  album_util::PathToQString(ph.db_path_),
                                  album_util::PathToQString(ph.meta_path_))
                        : PL_TEXT("Loaded packed project: %1 (DB temp: %2)",
                                  album_util::PathToQString(ph.project_package_path_),
                                  album_util::PathToQString(ph.db_path_)));
          self->RegisterRecentProject(result->recent_project_path_.empty()
                                          ? (!ph.project_package_path_.empty() ? ph.project_package_path_
                                                                               : ph.meta_path_)
                                          : result->recent_project_path_);
          emit self->ProjectChanged();
          emit self->projectChanged();
          ph.SetProjectLoadingState(false, {});

          if (!result->workspace_to_cleanup_.empty() &&
              result->workspace_to_cleanup_ != ph.project_workspace_dir_) {
            album_util::CleanupWorkspaceDirectory(result->workspace_to_cleanup_);
          }
        },
        Qt::QueuedConnection);
  }).detach();

  return true;
}

bool ProjectHandler::PurgeUninstalledSemanticModels() {
  if (!project_) {
    return false;
  }
  auto&      semantic = project_->GetStorageService()->GetSemanticStorageController();
  std::string err;
  const auto  models = semantic.ListModels(&err);
  bool        changed = false;
  for (const auto& model : models) {
    const auto lookup = model.profile_id_.empty() ? model.model_id_ : model.profile_id_;
    if (project_module_.ShouldKeepSemanticModelData(QString::fromStdString(lookup))) {
      continue;  // still installed (or unknown to the catalog) — keep its rows
    }
    // If this was the active model, deleting its SemanticModel row leaves no
    // active row; the runtime child process (if still holding it in memory)
    // self-cleans on the next activation. Nothing queries the runtime with the
    // stale key between now and then.
    if (semantic.PurgeModel(model.model_key_, &err)) {
      changed = true;
    }
  }
  return changed;
}

bool ProjectHandler::PersistCurrentProjectState() {
  try {
    if (pipeline_service_) {
      pipeline_service_->Sync();
    }
    if (thumbnail_service_) {
      auto stats = thumbnail_service_->GetDiskCacheStats();
      if (stats.enabled) {
        thumbnail_service_->FlushDiskCacheMetadata();
      }
    }
    if (project_) {
      project_->GetSleeveService()->Sync();
      project_->GetImagePoolService()->SyncWithStorage();
      if (!meta_path_.empty()) {
        project_->SaveProject(meta_path_);
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool ProjectHandler::PackageCurrentProjectFiles(QString* errorOut) const {
  if (!project_ || db_path_.empty() || meta_path_.empty() || project_package_path_.empty()) {
    return true;
  }

  auto package_service = project_->GetProjectPackageService();
  if (!package_service) {
    if (errorOut) {
      *errorOut = QStringLiteral("Project package service is unavailable.");
    }
    return false;
  }

  std::filesystem::path snapshot_path;
  if (!package_service->BuildTempDbSnapshotPath(&snapshot_path, errorOut)) {
    return false;
  }

  const bool snapshot_ok = package_service->CreateLiveDbSnapshot(project_, snapshot_path, errorOut);
  if (!snapshot_ok) {
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);
    return false;
  }

  const bool packed_ok =
      package_service->WritePackedProject(project_package_path_, meta_path_, snapshot_path, errorOut);
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  return packed_ok;
}

void ProjectHandler::SetProjectLoadingState(bool loading, const i18n::LocalizedText& message) {
  const i18n::LocalizedText next_message = loading ? message : i18n::LocalizedText{};
  if (project_loading_ == loading &&
      project_loading_message_text_.source_ == next_message.source_ &&
      project_loading_message_text_.args_ == next_message.args_) {
    return;
  }
  project_loading_              = loading;
  project_loading_message_text_ = next_message;
  project_module_.NotifyProjectLoadStateChanged();
}

void ProjectHandler::ClearProjectData() {
  project_module_.ClearProjectUiState();
}

}  // namespace alcedo::ui

#undef PL_TEXT
