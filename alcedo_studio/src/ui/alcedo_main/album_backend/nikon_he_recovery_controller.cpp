//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp"

#include "ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/album_backend/ui_status_sink.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"

#include <QCoreApplication>
#include <QFileDialog>
#include <QProcess>

#include <algorithm>
#include <system_error>
#include <utility>

namespace alcedo::ui {

using namespace album_util;

#define PL_TEXT(text, ...)                                                    \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT,                              \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text)      \
                              __VA_OPT__(, ) __VA_ARGS__)

namespace {

#if defined(Q_OS_MACOS)
auto MacOsDngConverterBundlePath() -> const std::filesystem::path& {
  static const std::filesystem::path kPath("/Applications/Adobe DNG Converter.app");
  return kPath;
}

auto MacOsDngConverterExecutablePath() -> const std::filesystem::path& {
  static const std::filesystem::path kPath(
      "/Applications/Adobe DNG Converter.app/Contents/MacOS/Adobe DNG Converter");
  return kPath;
}

auto RefreshMacOsConverterPath(QString* converter_path) -> bool {
  std::error_code ec;
  if (!std::filesystem::exists(MacOsDngConverterBundlePath(), ec) || ec) {
    converter_path->clear();
    return false;
  }

  ec.clear();
  if (!std::filesystem::is_regular_file(MacOsDngConverterExecutablePath(), ec) || ec) {
    converter_path->clear();
    return false;
  }

  *converter_path = PathToQString(MacOsDngConverterExecutablePath());
  return true;
}

auto MissingMacOsConverterStatus() -> i18n::LocalizedText {
  return PL_TEXT(
      "Adobe DNG Converter was not found at /Applications/Adobe DNG Converter.app. Install it, exit this dialog, then reimport these Nikon HE/HE* files.");
}
#endif

#if defined(Q_OS_WIN)
auto WindowsDngConverterDefaultPath() -> const std::filesystem::path& {
  static const std::filesystem::path kPath(
      LR"(C:\Program Files\Adobe\Adobe DNG Converter\Adobe DNG Converter.exe)");
  return kPath;
}

auto RefreshWindowsDefaultConverterPath(QString* converter_path) -> bool {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(WindowsDngConverterDefaultPath(), ec) || ec) {
    converter_path->clear();
    return false;
  }

  *converter_path = PathToQString(WindowsDngConverterDefaultPath());
  return true;
}
#endif

auto ExitStatusToQtEnum(const int exit_status) -> QProcess::ExitStatus {
  return static_cast<QProcess::ExitStatus>(exit_status);
}

auto ConverterFileDialogFilter() -> QString {
  return QCoreApplication::translate(ALCEDO_I18N_CONTEXT,
                                     "Executable Files (*.exe);;All Files (*)");
}

auto FileNameForRecoveryItem(const NikonHeRecoveryItem& item) -> QString {
  if (!item.file_name_.empty()) {
    return WStringToQString(item.file_name_);
  }
  if (!item.source_path_.empty()) {
    return PathToQString(item.source_path_.filename());
  }
  return QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "(unnamed)");
}

}  // namespace

NikonHeRecoveryController::NikonHeRecoveryController(ProjectModule* project,
                                                     ImageController* images,
                                                     IUiStatusSink* status,
                                                     QObject* parent)
    : QObject(parent), project_(project), images_(images), status_(status) {}

void NikonHeRecoveryController::BindCollaborators(ImportExportHandler* import_export,
                                                  SemanticGenerationController* semantic) {
  import_export_ = import_export;
  semantic_ = semantic;
}

void NikonHeRecoveryController::BrowseNikonHeConverter() { BrowseConverter(); }
void NikonHeRecoveryController::StartNikonHeConversion() { StartConversion(); }
void NikonHeRecoveryController::ExitNikonHeRecovery() { ExitRecovery(); }

void NikonHeRecoveryController::BeginRecovery(
    const std::vector<ImportLogEntry>& unsupported_entries,
    const sl_element_id_t              import_target_folder_id,
    const std::filesystem::path&       import_target_folder_path) {
#if defined(Q_OS_MACOS)
  RefreshMacOsConverterPath(&converter_path_);
  converter_path_from_default_ = false;
#elif defined(Q_OS_WIN)
  if (converter_path_.trimmed().isEmpty() || converter_path_from_default_) {
    converter_path_from_default_ = RefreshWindowsDefaultConverterPath(&converter_path_);
  }
#endif

  items_.clear();
  items_.reserve(unsupported_entries.size());
  for (const auto& entry : unsupported_entries) {
    NikonHeRecoveryItem item;
    item.element_id_  = entry.element_id_;
    item.image_id_    = entry.image_id_;
    item.file_name_   = entry.file_name_;
    item.source_path_ = entry.source_path_;
    items_.push_back(std::move(item));
  }

  import_target_folder_id_   = import_target_folder_id;
  import_target_folder_path_ = import_target_folder_path;
  completion_note_.clear();
  active_ = !items_.empty();
  busy_   = false;

  if (!active_) {
    ClearState();
    return;
  }

#if defined(Q_OS_MACOS)
  if (converter_path_.trimmed().isEmpty()) {
    SetPhase(NikonHeRecoveryPhase::REVIEW_UNSUPPORTED, MissingMacOsConverterStatus(), 0);
    return;
  }
#endif

  SetPhase(NikonHeRecoveryPhase::REVIEW_UNSUPPORTED,
           PL_TEXT("These Nikon HE/HE* files need Adobe DNG Converter before they can be imported."),
           0);
}

void NikonHeRecoveryController::BrowseConverter() {
#if defined(Q_OS_MACOS)
  return;
#endif

  const QString selected_path = QFileDialog::getOpenFileName(
      nullptr, QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "Select Adobe DNG Converter"),
      converter_path_, ConverterFileDialogFilter());
  if (!selected_path.isEmpty()) {
    converter_path_              = selected_path;
    converter_path_from_default_ = false;
    NotifyStateChanged();
  }
}

void NikonHeRecoveryController::StartConversion() {
  if (!active_ || busy_ || items_.empty()) {
    return;
  }

#if defined(Q_OS_MACOS)
  if (converter_path_.trimmed().isEmpty()) {
    RefreshMacOsConverterPath(&converter_path_);
    NotifyStateChanged();
    if (converter_path_.trimmed().isEmpty()) {
      SetPhase(NikonHeRecoveryPhase::REVIEW_UNSUPPORTED, MissingMacOsConverterStatus(), 0);
      return;
    }
  }
#else
  if (converter_path_.trimmed().isEmpty()) {
#if defined(Q_OS_WIN)
    converter_path_from_default_ = RefreshWindowsDefaultConverterPath(&converter_path_);
    NotifyStateChanged();
#endif
  }
  if (converter_path_.trimmed().isEmpty()) {
    SetPhase(NikonHeRecoveryPhase::SELECTING_CONVERTER,
             PL_TEXT("Choose the Adobe DNG Converter executable to continue."), 5);
    BrowseConverter();
  }
#endif

  const auto converter_path_opt = InputToPath(converter_path_);
  std::error_code ec;
  if (!converter_path_opt.has_value() ||
      !std::filesystem::is_regular_file(converter_path_opt.value(), ec) || ec) {
#if defined(Q_OS_MACOS)
    RefreshMacOsConverterPath(&converter_path_);
    NotifyStateChanged();
    SetPhase(NikonHeRecoveryPhase::REVIEW_UNSUPPORTED, MissingMacOsConverterStatus(), 0);
#else
    converter_path_from_default_ = false;
    SetPhase(NikonHeRecoveryPhase::REVIEW_UNSUPPORTED,
             PL_TEXT("Adobe DNG Converter was not found. Choose a valid executable or exit."), 0);
#endif
    return;
  }

  process_ = std::make_unique<QProcess>();
  process_->setProgram(converter_path_);
  QStringList arguments;
  arguments << QStringLiteral("-c");
  for (const auto& item : items_) {
    arguments << PathToQString(item.source_path_);
  }
  process_->setArguments(arguments);

  QObject::connect(process_.get(), &QProcess::finished, this,
                   [this](int exit_code, QProcess::ExitStatus exit_status) {
                     HandleConverterFinished(exit_code, static_cast<int>(exit_status));
                   });
  QObject::connect(process_.get(), &QProcess::errorOccurred, this,
                   [this](QProcess::ProcessError) {
                     if (!process_) {
                       return;
                     }
                     const QString error_text = process_->errorString().trimmed();
                     RemoveUnsupportedEntriesAndContinue(
                         {},
                         error_text.isEmpty()
                             ? QCoreApplication::translate(
                                   ALCEDO_I18N_CONTEXT,
                                   "Adobe DNG Converter failed to start. Unsupported Nikon HE images were removed from the project.")
                             : QCoreApplication::translate(
                                   ALCEDO_I18N_CONTEXT,
                                   "Adobe DNG Converter failed to start (%1). Unsupported Nikon HE images were removed from the project.")
                                   .arg(error_text));
                   });

  SetPhase(NikonHeRecoveryPhase::RUNNING_CONVERTER,
           PL_TEXT("Running Adobe DNG Converter for %1 file(s)...",
                   static_cast<int>(items_.size())),
           20);
  process_->start();
}

void NikonHeRecoveryController::ExitRecovery() {
  if (!active_ || busy_) {
    return;
  }
  RemoveUnsupportedEntriesAndContinue(
      {},
      QCoreApplication::translate(
          ALCEDO_I18N_CONTEXT,
          "Unsupported Nikon HE images were removed from the project. Source files were left untouched."));
}

void NikonHeRecoveryController::UpdateReimportProgress(uint32_t completed, uint32_t total,
                                                       uint32_t failed) {
  if (!is_reimporting()) {
    return;
  }
  status_text_ = PL_TEXT("Reimporting converted DNG files... %1/%2 (failed %3)",
                         static_cast<int>(completed), static_cast<int>(total),
                         static_cast<int>(failed));
  NotifyStateChanged();
}

void NikonHeRecoveryController::HandleReimportFinished(const ImportResult& result) {
  if (!is_reimporting()) {
    return;
  }

  QString summary = QCoreApplication::translate(
      ALCEDO_I18N_CONTEXT, "Converted Nikon HE files were reimported: %1 imported, %2 failed.")
                        .arg(result.imported_)
                        .arg(result.failed_);
  if (!completion_note_.trimmed().isEmpty()) {
    summary += QStringLiteral(" ") + completion_note_.trimmed();
  }
  FinishAndClose(PL_TEXT("%1", summary), 100);
}

auto NikonHeRecoveryController::phase_text() const -> QString {
  switch (phase_) {
    case NikonHeRecoveryPhase::REVIEW_UNSUPPORTED:
      return QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "Review Unsupported Files");
    case NikonHeRecoveryPhase::SELECTING_CONVERTER:
      return QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "Select Converter");
    case NikonHeRecoveryPhase::RUNNING_CONVERTER:
      return QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "Running Converter");
    case NikonHeRecoveryPhase::VALIDATING_DNG:
      return QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "Validating DNG");
    case NikonHeRecoveryPhase::REMOVING_PROJECT_ITEMS:
      return QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "Removing Project Items");
    case NikonHeRecoveryPhase::REIMPORTING_DNG:
      return QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "Reimporting DNG");
    case NikonHeRecoveryPhase::FINISHED:
      return QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "Finished");
    case NikonHeRecoveryPhase::ABORTED:
      return QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "Aborted");
    case NikonHeRecoveryPhase::IDLE:
    default:
      return QString();
  }
}

auto NikonHeRecoveryController::unsupported_files() const -> QVariantList {
  QVariantList rows;
  rows.reserve(static_cast<qsizetype>(items_.size()));
  for (const auto& item : items_) {
    rows.push_back(QVariantMap{
        {"elementId", static_cast<uint>(item.element_id_)},
        {"imageId", static_cast<uint>(item.image_id_)},
        {"fileName", FileNameForRecoveryItem(item)},
        {"sourcePath", PathToQString(item.source_path_)},
        {"convertedPath", PathToQString(item.converted_dng_path_)},
    });
  }
  return rows;
}

void NikonHeRecoveryController::NotifyStateChanged() {
  emit NikonHeRecoveryStateChanged();
}

void NikonHeRecoveryController::SetPhase(const NikonHeRecoveryPhase phase,
                                         const i18n::LocalizedText& status,
                                         const int                  progress) {
  phase_       = phase;
  status_text_ = status;
  active_      = phase != NikonHeRecoveryPhase::IDLE;
  busy_        = phase == NikonHeRecoveryPhase::RUNNING_CONVERTER ||
          phase == NikonHeRecoveryPhase::VALIDATING_DNG ||
          phase == NikonHeRecoveryPhase::REMOVING_PROJECT_ITEMS ||
          phase == NikonHeRecoveryPhase::REIMPORTING_DNG;
  status_->SetTaskState(status, progress, false);
  NotifyStateChanged();
}

void NikonHeRecoveryController::FinishAndClose(const i18n::LocalizedText& status,
                                               const int                  progress) {
  status_->SetTaskState(status, progress, false);
  status_->SetServiceMessage(status);
  status_->ScheduleIdleTaskStateReset(2200);
  ClearState();
}

void NikonHeRecoveryController::ClearState() {
  items_.clear();
  import_target_folder_id_ = 0;
  import_target_folder_path_.clear();
  completion_note_.clear();
  process_.reset();
  phase_       = NikonHeRecoveryPhase::IDLE;
  status_text_ = {};
  active_      = false;
  busy_        = false;
  NotifyStateChanged();
  if (semantic_) { semantic_->ResumeQueuedWorkflow(); };
}

void NikonHeRecoveryController::HandleConverterFinished(const int exit_code, const int exit_status) {
  if (!process_) {
    return;
  }

  const QString stdout_text =
      QString::fromLocal8Bit(process_->readAllStandardOutput()).trimmed();
  const QString stderr_text =
      QString::fromLocal8Bit(process_->readAllStandardError()).trimmed();

  if (ExitStatusToQtEnum(exit_status) != QProcess::NormalExit || exit_code != 0) {
    QString details = !stderr_text.isEmpty() ? stderr_text : stdout_text;
    if (details.isEmpty()) {
      details = QCoreApplication::translate(ALCEDO_I18N_CONTEXT, "unknown error");
    }
    RemoveUnsupportedEntriesAndContinue(
        {},
        QCoreApplication::translate(
            ALCEDO_I18N_CONTEXT,
            "Adobe DNG Converter failed (%1). Unsupported Nikon HE images were removed from the project.")
            .arg(details));
    return;
  }

  SetPhase(NikonHeRecoveryPhase::VALIDATING_DNG,
           PL_TEXT("Validating converted DNG files..."), 55);
  QString validation_warning;
  const auto converted_paths = ValidateConvertedDngs(&validation_warning);
  RemoveUnsupportedEntriesAndContinue(converted_paths, validation_warning);
}

auto NikonHeRecoveryController::ValidateConvertedDngs(QString* warning_summary)
    -> std::vector<image_path_t> {
  std::vector<image_path_t> converted_paths;
  converted_paths.reserve(items_.size());

  QStringList missing_names;
  for (auto& item : items_) {
    item.converted_dng_path_ =
        item.source_path_.parent_path() /
        std::filesystem::path(item.source_path_.stem().wstring() + L".dng");

    std::error_code ec;
    if (std::filesystem::is_regular_file(item.converted_dng_path_, ec) && !ec) {
      converted_paths.push_back(item.converted_dng_path_);
    } else {
      missing_names.push_back(FileNameForRecoveryItem(item));
    }
  }

  if (warning_summary) {
    if (converted_paths.empty()) {
      *warning_summary = QCoreApplication::translate(
          ALCEDO_I18N_CONTEXT,
          "Adobe DNG Converter finished, but no converted DNG files were found.");
    } else if (!missing_names.isEmpty()) {
      *warning_summary = QCoreApplication::translate(
          ALCEDO_I18N_CONTEXT,
          "%1 file(s) were converted. Missing DNG output for: %2")
                             .arg(converted_paths.size())
                             .arg(missing_names.join(QStringLiteral(", ")));
    } else {
      warning_summary->clear();
    }
  }

  return converted_paths;
}

auto NikonHeRecoveryController::BuildDeleteTargets() const
    -> std::vector<ImageController::DeleteTarget> {
  std::vector<ImageController::DeleteTarget> targets;
  targets.reserve(items_.size());
  for (const auto& item : items_) {
    ImageController::DeleteTarget target;
    target.element_id_ = item.element_id_;
    target.image_id_   = item.image_id_;
    const std::filesystem::path file_name =
        !item.file_name_.empty() ? std::filesystem::path(item.file_name_)
                                 : item.source_path_.filename();
    if (!import_target_folder_path_.empty() && !file_name.empty()) {
      target.file_path_ = import_target_folder_path_ / file_name;
    }
    targets.push_back(std::move(target));
  }
  return targets;
}

void NikonHeRecoveryController::RemoveUnsupportedEntriesAndContinue(
    const std::vector<image_path_t>& converted_paths, const QString& completion_note) {
  completion_note_ = completion_note;
  SetPhase(NikonHeRecoveryPhase::REMOVING_PROJECT_ITEMS,
           PL_TEXT("Removing unsupported Nikon HE items from the project..."), 75);

  const auto delete_result = images_->DeleteTargets(BuildDeleteTargets());

  if (!converted_paths.empty()) {
    SetPhase(NikonHeRecoveryPhase::REIMPORTING_DNG,
             PL_TEXT("Reimporting %1 converted DNG file(s)...",
                     static_cast<int>(converted_paths.size())),
             85);
    busy_ = true;
    NotifyStateChanged();
    import_export_->SetImportTarget(import_target_folder_id_,
                                            import_target_folder_path_);
    import_export_->StartImportPaths(converted_paths, true);
    return;
  }

  QString summary = completion_note_.trimmed();
  if (summary.isEmpty()) {
    summary = delete_result.message_;
  } else if (!delete_result.message_.trimmed().isEmpty()) {
    summary += QStringLiteral(" ") + delete_result.message_.trimmed();
  }
  FinishAndClose(PL_TEXT("%1", summary), delete_result.deleted_count_ > 0 ? 100 : 0);
}

}  // namespace alcedo::ui
