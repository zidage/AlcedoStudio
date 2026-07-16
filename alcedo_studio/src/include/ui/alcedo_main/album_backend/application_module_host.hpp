//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <memory>
#include <string>
#include <vector>

#include "app/ai_provider_profile.hpp"
#include "app/image_analysis_service.hpp"
#include "app/model_download_service.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_controller.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/image_analysis_controller.hpp"
#include "ui/alcedo_main/album_backend/image_analysis_sink.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/model_download_controller.hpp"
#include "ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp"
#include "ui/alcedo_main/album_backend/project_db_write_barrier.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"

namespace alcedo::ui {

/// Process-local UI module composition root. Owns module lifecycle and exposes
/// typed module accessors for QML (`appModules`). Does not forward module
/// actions or mirror module-specific state.
class ApplicationModuleHost final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject* project READ ProjectObject CONSTANT)
  Q_PROPERTY(QObject* library READ LibraryObject CONSTANT)
  Q_PROPERTY(QObject* folders READ FoldersObject CONSTANT)
  Q_PROPERTY(QObject* images READ ImagesObject CONSTANT)
  Q_PROPERTY(QObject* stats READ StatsObject CONSTANT)
  Q_PROPERTY(QObject* search READ SearchObject CONSTANT)
  Q_PROPERTY(QObject* importExport READ ImportExportObject CONSTANT)
  Q_PROPERTY(QObject* nikonHeRecovery READ NikonHeRecoveryObject CONSTANT)
  Q_PROPERTY(QObject* editor READ EditorObject CONSTANT)
  Q_PROPERTY(QObject* backgroundTasks READ BackgroundTasksObject CONSTANT)
  Q_PROPERTY(QObject* interactionPolicy READ InteractionPolicyObject CONSTANT)
  Q_PROPERTY(QObject* modelDownload READ ModelDownloadObject CONSTANT)
  Q_PROPERTY(QObject* semanticGeneration READ SemanticGenerationObject CONSTANT)
  Q_PROPERTY(QObject* aiProviderProfiles READ AiProviderProfilesObject CONSTANT)
  Q_PROPERTY(QObject* imageAnalysis READ ImageAnalysisObject CONSTANT)
  Q_PROPERTY(QObject* adjustmentTransfer READ AdjustmentTransferObject CONSTANT)

 public:
  explicit ApplicationModuleHost(QObject* parent = nullptr);
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
  [[nodiscard]] auto editor() -> EditorController* { return editor_.get(); }
  [[nodiscard]] auto background_tasks() -> BackgroundTaskController* {
    return background_tasks_.get();
  }
  [[nodiscard]] auto interaction_policy() -> InteractionPolicyController* {
    return interaction_policy_.get();
  }
  [[nodiscard]] auto model_download() -> ModelDownloadController* {
    return model_download_.get();
  }
  [[nodiscard]] auto semantic_generation() -> SemanticGenerationController* {
    return semantic_generation_.get();
  }
  [[nodiscard]] auto ai_provider_profiles() -> alcedo::AiProviderProfileController* {
    return ai_provider_profiles_.get();
  }
  [[nodiscard]] auto image_analysis() -> ImageAnalysisController* {
    return image_analysis_.get();
  }
  [[nodiscard]] auto adjustment_transfer() -> AdjustmentTransferController* {
    return adjustment_transfer_.get();
  }
  [[nodiscard]] auto db_write_barrier() -> ProjectDbWriteBarrier& { return *db_write_barrier_; }
  [[nodiscard]] auto image_analysis_gate()
      -> const std::shared_ptr<alcedo::ImageAnalysisInFlightGate>& {
    return image_analysis_gate_;
  }

  // QML CONSTANT property getters.
  QObject* ProjectObject() { return project_.get(); }
  QObject* LibraryObject() { return library_.get(); }
  QObject* FoldersObject() { return folders_.get(); }
  QObject* ImagesObject() { return images_.get(); }
  QObject* StatsObject() { return stats_.get(); }
  QObject* SearchObject() { return search_.get(); }
  QObject* ImportExportObject() { return import_export_.get(); }
  QObject* NikonHeRecoveryObject() { return nikon_he_recovery_.get(); }
  QObject* EditorObject() { return editor_.get(); }
  QObject* BackgroundTasksObject() { return background_tasks_.get(); }
  QObject* InteractionPolicyObject() { return interaction_policy_.get(); }
  QObject* ModelDownloadObject() { return model_download_.get(); }
  QObject* SemanticGenerationObject() { return semantic_generation_.get(); }
  QObject* AiProviderProfilesObject() { return ai_provider_profiles_.get(); }
  QObject* ImageAnalysisObject() { return image_analysis_.get(); }
  QObject* AdjustmentTransferObject() { return adjustment_transfer_.get(); }

  /// Deterministic construction order used by the lifecycle test.
  /// Returns module type names in construction order.
  [[nodiscard]] static auto ConstructionOrder() -> std::vector<std::string>;
  /// Reverse of ConstructionOrder (destruction order).
  [[nodiscard]] static auto DestructionOrder() -> std::vector<std::string>;

 private:
  void ShutdownModules();

  // Owned in construction dependency order. Destroyed in reverse.
  std::unique_ptr<BackgroundTaskController>          background_tasks_;
  std::unique_ptr<InteractionPolicyController>       interaction_policy_;
  std::unique_ptr<alcedo::ModelDownloadService>      model_download_service_;
  std::unique_ptr<ProjectModule>                     project_;
  std::unique_ptr<LibraryModule>                     library_;
  std::unique_ptr<FolderController>                  folders_;
  std::unique_ptr<ImageController>                   images_;
  std::unique_ptr<StatsEngine>                       stats_;
  std::unique_ptr<SearchController>                  search_;
  std::unique_ptr<ModelDownloadController>           model_download_;
  std::unique_ptr<SemanticGenerationController>      semantic_generation_;
  std::unique_ptr<alcedo::AiProviderProfileController> ai_provider_profiles_;
  std::shared_ptr<alcedo::ImageAnalysisInFlightGate> image_analysis_gate_;
  std::unique_ptr<ProjectDbWriteBarrier>             db_write_barrier_;
  std::shared_ptr<IImageAnalysisSink>                image_analysis_sink_;
  std::unique_ptr<ImageAnalysisController>           image_analysis_;
  std::unique_ptr<ImportExportHandler>               import_export_;
  std::unique_ptr<NikonHeRecoveryController>         nikon_he_recovery_;
  std::unique_ptr<EditorController>                  editor_;
  std::unique_ptr<AdjustmentTransferController>      adjustment_transfer_;

  bool shutting_down_ = false;
};

}  // namespace alcedo::ui
