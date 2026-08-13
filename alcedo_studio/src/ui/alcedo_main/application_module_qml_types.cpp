//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/application_module_qml_types.hpp"

#include <qqml.h>

#include "app/ai_provider_profile.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"

namespace alcedo::ui {

void RegisterApplicationModuleTypes() {
  qmlRegisterUncreatableType<ProjectModule>("Alcedo.Main", 1, 0, "ProjectModule",
                                            "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<LibraryModule>("Alcedo.Main", 1, 0, "LibraryModule",
                                            "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<FolderController>("Alcedo.Main", 1, 0, "FolderController",
                                               "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<ImageController>("Alcedo.Main", 1, 0, "ImageController",
                                              "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<StatsEngine>("Alcedo.Main", 1, 0, "StatsEngine",
                                          "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<SearchController>("Alcedo.Main", 1, 0, "SearchController",
                                               "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<ImportExportHandler>("Alcedo.Main", 1, 0, "ImportExportHandler",
                                                  "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<NikonHeRecoveryController>(
      "Alcedo.Main", 1, 0, "NikonHeRecoveryController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<BackgroundTaskController>(
      "Alcedo.Main", 1, 0, "BackgroundTaskController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<InteractionPolicyController>(
      "Alcedo.Main", 1, 0, "InteractionPolicyController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<ModelDownloadController>(
      "Alcedo.Main", 1, 0, "ModelDownloadController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::UpdateService>(
      "Alcedo.Main", 1, 0, "UpdateService", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<SemanticGenerationController>(
      "Alcedo.Main", 1, 0, "SemanticGenerationController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::AiProviderProfileController>(
      "Alcedo.Main", 1, 0, "AiProviderProfileController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<ImageAnalysisController>(
      "Alcedo.Main", 1, 0, "ImageAnalysisController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<AdjustmentTransferController>(
      "Alcedo.Main", 1, 0, "AdjustmentTransferController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<EditorSessionController>(
      "Alcedo.Main", 1, 0, "EditorSessionController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<EditorScopeController>("Alcedo.Main", 1, 0, "EditorScopeController",
                                                    "Owned by EditorSessionController");
  qmlRegisterUncreatableType<WorkspaceRouter>("Alcedo.Main", 1, 0, "WorkspaceRouter",
                                              "Owned by ApplicationModuleHost");
}

}  // namespace alcedo::ui
