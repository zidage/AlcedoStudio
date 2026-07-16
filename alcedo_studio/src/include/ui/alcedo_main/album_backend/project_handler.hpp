//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QVariantList>

#include <filesystem>
#include <memory>
#include <vector>

#include "ui/alcedo_main/i18n.hpp"
#include "app/export_service.hpp"
#include "app/history_mgmt_service.hpp"
#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "app/thumbnail_service.hpp"

namespace alcedo::ui {

class ProjectModule;

/// Owns all back-end services and manages lifetime of a single project.
class ProjectHandler {
 public:
  explicit ProjectHandler(ProjectModule& project_module);

  bool InitializeServices(const std::filesystem::path& dbPath,
                          const std::filesystem::path& metaPath,
                          ProjectOpenMode              openMode,
                          const std::filesystem::path& packagePath = {},
                          const std::filesystem::path& workspaceDir = {},
                          const std::filesystem::path& recentProjectPath = {});
  bool PersistCurrentProjectState();
  bool PackageCurrentProjectFiles(QString* errorOut = nullptr) const;
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
  [[nodiscard]] auto history_service() const -> const std::shared_ptr<EditHistoryMgmtService>& {
    return history_service_;
  }
  [[nodiscard]] auto thumbnail_service() const -> const std::shared_ptr<ThumbnailService>& {
    return thumbnail_service_;
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

 private:
  ProjectModule& project_module_;

  std::shared_ptr<ProjectService>         project_{};
  std::shared_ptr<PipelineMgmtService>    pipeline_service_{};
  std::shared_ptr<EditHistoryMgmtService> history_service_{};
  std::shared_ptr<ThumbnailService>       thumbnail_service_{};
  std::unique_ptr<ImportServiceImpl>      import_service_{};
  std::shared_ptr<ExportService>          export_service_{};

  std::filesystem::path db_path_{};
  std::filesystem::path meta_path_{};
  std::filesystem::path project_package_path_{};
  std::filesystem::path project_workspace_dir_{};

  bool                project_loading_ = false;
  i18n::LocalizedText project_loading_message_text_{};
  uint64_t            project_load_request_id_ = 0;
};

}  // namespace alcedo::ui
