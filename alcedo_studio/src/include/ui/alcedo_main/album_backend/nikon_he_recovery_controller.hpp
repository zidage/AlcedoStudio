//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QVariantList>

#include <filesystem>
#include <memory>
#include <vector>

#include "app/import_service.hpp"
#include "ui/alcedo_main/i18n.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/nikon_he_recovery_types.hpp"
#include "utils/import/import_log.hpp"

namespace alcedo::ui {

class ImportExportHandler;
class ProjectModule;
class SemanticGenerationController;
class IUiStatusSink;

class NikonHeRecoveryController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool nikonHeRecoveryActive READ NikonHeRecoveryActive NOTIFY NikonHeRecoveryStateChanged)
  Q_PROPERTY(bool nikonHeRecoveryBusy READ NikonHeRecoveryBusy NOTIFY NikonHeRecoveryStateChanged)
  Q_PROPERTY(
      QString nikonHeRecoveryPhase READ NikonHeRecoveryPhaseText NOTIFY NikonHeRecoveryStateChanged)
  Q_PROPERTY(
      QString nikonHeRecoveryStatus READ NikonHeRecoveryStatus NOTIFY NikonHeRecoveryStateChanged)
  Q_PROPERTY(QVariantList nikonHeUnsupportedFiles READ NikonHeUnsupportedFiles NOTIFY
                 NikonHeRecoveryStateChanged)
  Q_PROPERTY(
      QString nikonHeConverterPath READ NikonHeConverterPath NOTIFY NikonHeRecoveryStateChanged)
  Q_PROPERTY(bool nikonHeConverterPathFromDefault READ NikonHeConverterPathFromDefault NOTIFY
                 NikonHeRecoveryStateChanged)

 public:
  NikonHeRecoveryController(ProjectModule* project, ImageController* images, IUiStatusSink* status,
                            QObject* parent = nullptr);

  void BindCollaborators(ImportExportHandler* import_export,
                         SemanticGenerationController* semantic);

  void BeginRecovery(const std::vector<ImportLogEntry>& unsupported_entries,
                     sl_element_id_t                    import_target_folder_id,
                     const std::filesystem::path&       import_target_folder_path);
  Q_INVOKABLE void BrowseNikonHeConverter();
  Q_INVOKABLE void StartNikonHeConversion();
  Q_INVOKABLE void ExitNikonHeRecovery();
  void BrowseConverter();
  void StartConversion();
  void ExitRecovery();
  void UpdateReimportProgress(uint32_t completed, uint32_t total, uint32_t failed);
  void HandleReimportFinished(const ImportResult& result);

  [[nodiscard]] bool NikonHeRecoveryActive() const { return active_; }
  [[nodiscard]] bool is_active() const { return active_; }
  [[nodiscard]] bool NikonHeRecoveryBusy() const { return busy_; }
  [[nodiscard]] bool is_busy() const { return busy_; }
  [[nodiscard]] bool is_reimporting() const {
    return active_ && phase_ == NikonHeRecoveryPhase::REIMPORTING_DNG;
  }
  [[nodiscard]] auto phase() const -> NikonHeRecoveryPhase { return phase_; }
  [[nodiscard]] auto NikonHeRecoveryPhaseText() const -> QString { return phase_text(); }
  [[nodiscard]] auto phase_text() const -> QString;
  [[nodiscard]] auto NikonHeRecoveryStatus() const -> QString { return status_text_.Render(); }
  [[nodiscard]] auto status_text() const -> QString { return status_text_.Render(); }
  [[nodiscard]] auto NikonHeUnsupportedFiles() const -> QVariantList { return unsupported_files(); }
  [[nodiscard]] auto unsupported_files() const -> QVariantList;
  [[nodiscard]] auto NikonHeConverterPath() const -> QString { return converter_path_; }
  [[nodiscard]] const QString& converter_path() const { return converter_path_; }
  [[nodiscard]] bool NikonHeConverterPathFromDefault() const {
    return converter_path_from_default_;
  }
  [[nodiscard]] bool converter_path_from_default() const { return converter_path_from_default_; }

 signals:
  void NikonHeRecoveryStateChanged();

 private:
  void NotifyStateChanged();
  void SetPhase(NikonHeRecoveryPhase phase, const i18n::LocalizedText& status, int progress = 0);
  void FinishAndClose(const i18n::LocalizedText& status, int progress = 100);
  void ClearState();
  void HandleConverterFinished(int exit_code, int exit_status);
  auto ValidateConvertedDngs(QString* warning_summary) -> std::vector<image_path_t>;
  auto BuildDeleteTargets() const -> std::vector<ImageController::DeleteTarget>;
  void RemoveUnsupportedEntriesAndContinue(const std::vector<image_path_t>& converted_paths,
                                           const QString&                   completion_note);

  ProjectModule*                 project_       = nullptr;
  ImageController*               images_        = nullptr;
  IUiStatusSink*                 status_        = nullptr;
  ImportExportHandler*           import_export_ = nullptr;
  SemanticGenerationController*  semantic_      = nullptr;

  NikonHeRecoveryItemList    items_{};
  sl_element_id_t            import_target_folder_id_ = 0;
  std::filesystem::path      import_target_folder_path_{};
  QString                    converter_path_{};
  bool                       converter_path_from_default_ = false;
  std::unique_ptr<QProcess>  process_{};
  NikonHeRecoveryPhase       phase_ = NikonHeRecoveryPhase::IDLE;
  i18n::LocalizedText        status_text_{};
  QString                    completion_note_{};
  bool                       active_ = false;
  bool                       busy_   = false;
};

}  // namespace alcedo::ui
