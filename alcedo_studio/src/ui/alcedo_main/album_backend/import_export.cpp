//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/import_export.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/album_backend/project_db_write_barrier.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/album_backend/ui_status_sink.hpp"

namespace alcedo::ui {

using namespace album_util;

#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {

constexpr auto kExportSdrQualityKey      = "export/sdrQuality";
constexpr auto kExportUltraHdrQualityKey = "export/ultraHdrQuality";
constexpr auto kExportFileNamePresetsKey = "export/fileNamePresets";
constexpr int  kDefaultExportQuality     = 95;
constexpr int  kMaxExportPresetCount     = 64;
constexpr int  kMaxExportPresetNameSize  = 80;
constexpr int  kMaxExportPatternSize     = 2048;

auto           SanitizeBitDepth(ImageFormatType format, ExportFormatOptions::BIT_DEPTH requested)
    -> ExportFormatOptions::BIT_DEPTH {
  switch (format) {
    case ImageFormatType::JPEG:
    case ImageFormatType::WEBP:
      return ExportFormatOptions::BIT_DEPTH::BIT_8;
    case ImageFormatType::PNG:
      return requested == ExportFormatOptions::BIT_DEPTH::BIT_8
                 ? ExportFormatOptions::BIT_DEPTH::BIT_8
                 : ExportFormatOptions::BIT_DEPTH::BIT_16;
    case ImageFormatType::EXR:
      return requested == ExportFormatOptions::BIT_DEPTH::BIT_32
                 ? ExportFormatOptions::BIT_DEPTH::BIT_32
                 : ExportFormatOptions::BIT_DEPTH::BIT_16;
    case ImageFormatType::TIFF:
      return requested;
    default:
      return ExportFormatOptions::BIT_DEPTH::BIT_8;
  }
}

auto ExportStatusKey(const sl_element_id_t elementId, const image_id_t imageId) -> QString {
  return QString::number(static_cast<qulonglong>(elementId)) + ":" +
         QString::number(static_cast<qulonglong>(imageId));
}

}  // namespace

ImportExportHandler::ImportExportHandler(ProjectModule* project, LibraryModule* library,
                                         FolderController* folders, IUiStatusSink* status,
                                         ProjectDbWriteBarrier* barrier, QObject* parent)
    : QObject(parent),
      project_(project),
      library_(library),
      folders_(folders),
      status_(status),
      barrier_(barrier) {
  // default export folder init continues below

  const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
  if (!pictures.isEmpty()) {
    default_export_folder_ = pictures;
  }
  export_status_text_ = PL_TEXT("Ready to export.");
}

void ImportExportHandler::BindCollaborators(StatsEngine* stats, NikonHeRecoveryController* nikon,
                                            SemanticGenerationController* semantic) {
  stats_    = stats;
  nikon_    = nikon;
  semantic_ = semantic;
}

void ImportExportHandler::StartImport(const QStringList& fileUrlsOrPaths) {
  std::vector<image_path_t>        paths;
  std::unordered_set<std::wstring> seen;

  for (const QString& raw : fileUrlsOrPaths) {
    const auto pathOpt = InputToPath(raw);
    if (!pathOpt.has_value()) {
      continue;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pathOpt.value(), ec) || ec) {
      continue;
    }

    const std::wstring key = pathOpt->wstring();
    if (!seen.insert(key).second) {
      continue;
    }
    paths.push_back(pathOpt.value());
  }

  if (paths.empty()) {
    status_->SetTaskState(PL_TEXT("No files selected."), 0, false);
    return;
  }
  StartImportResolvedPaths(std::move(paths), false);
}

QStringList ImportExportHandler::CollectFolderFiles(const QString& folderUrlOrPath) {
  QStringList result;
  const auto  folder_opt = InputToPath(folderUrlOrPath);
  if (!folder_opt.has_value()) {
    return result;
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(folder_opt.value(), ec) || ec) {
    return result;
  }

  std::vector<std::filesystem::path> files;
  for (auto it = std::filesystem::recursive_directory_iterator(
           folder_opt.value(), std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    std::error_code file_ec;
    if (it->is_regular_file(file_ec) && !file_ec) {
      files.push_back(it->path());
    }
  }

  std::sort(files.begin(), files.end());
  result.reserve(static_cast<int>(files.size()));
  for (const auto& f : files) {
#if defined(_WIN32)
    result.append(QString::fromStdWString(f.wstring()));
#else
    result.append(QString::fromStdString(f.string()));
#endif
  }
  return result;
}

void ImportExportHandler::StartImportPaths(const std::vector<image_path_t>& paths,
                                           const bool                       preserveTarget) {
  std::vector<image_path_t>        deduped_paths;
  std::unordered_set<std::wstring> seen;
  deduped_paths.reserve(paths.size());
  for (const auto& path : paths) {
    if (path.empty()) {
      continue;
    }
    const std::wstring key = path.wstring();
    if (!seen.insert(key).second) {
      continue;
    }
    deduped_paths.push_back(path);
  }
  if (deduped_paths.empty()) {
    status_->SetTaskState(PL_TEXT("No supported files selected."), 0, false);
    return;
  }
  StartImportResolvedPaths(std::move(deduped_paths), preserveTarget);
}

void ImportExportHandler::StartImportResolvedPaths(std::vector<image_path_t> paths,
                                                   const bool                preserveTarget) {
  if (project_->handler().project_loading()) {
    status_->SetTaskState(PL_TEXT("Project is loading. Please wait."), 0, false);
    return;
  }
  auto* isvc = project_->handler().import_service();
  if (!isvc) {
    status_->SetTaskState(PL_TEXT("Import service is unavailable."), 0, false);
    return;
  }
  if (current_import_job_ && !current_import_job_->IsCancelationAcked()) {
    status_->SetTaskState(PL_TEXT("Import already running."), 0, true);
    return;
  }

  if (!preserveTarget) {
    import_target_folder_id_   = folders_->CurrentFolderElementId().value_or(0);
    import_target_folder_path_ = folders_->CurrentFolderFsPath();
  }

  auto job            = std::make_shared<ImportJob>();
  current_import_job_ = job;

  import_running_     = true;
  import_total_       = static_cast<int>(paths.size());
  import_completed_   = 0;
  import_failed_      = 0;
  import_status_text_ = PL_TEXT("Importing %1 file(s)...", import_total_);
  emit ImportStateChanged();
  emit importStateChanged();

  status_->SetTaskState(import_status_text_, 0, true);

  QPointer<ImportExportHandler> self(this);
  job->on_progress_ = [self](const ImportProgress& progress) {
    if (!self) return;
    const uint32_t total        = std::max<uint32_t>(progress.total_, 1);
    const uint32_t metadataDone = progress.metadata_done_.load();
    const uint32_t failed       = progress.failed_.load();
    const uint32_t done         = metadataDone + failed;
    const int      pct          = static_cast<int>((done * 100U) / total);

    QMetaObject::invokeMethod(
        self,
        [self, metadataDone, total, failed, pct]() {
          if (!self) return;
          self->import_completed_ = static_cast<int>(metadataDone);
          self->import_failed_    = static_cast<int>(failed);
          self->import_status_text_ =
              PL_TEXT("Importing... %1/%2 (failed %3)", metadataDone, total, failed);
          emit self->ImportStateChanged();
          emit self->importStateChanged();
          if (self->status_) {
            self->status_->SetTaskState(self->import_status_text_, pct, true);
          }
          if (self->nikon_ && self->nikon_->is_reimporting()) {
            self->nikon_->UpdateReimportProgress(metadataDone, total, failed);
          }
        },
        Qt::QueuedConnection);
  };

  job->on_finished_ = [self](const ImportResult& result) {
    if (!self) return;
    QMetaObject::invokeMethod(
        self,
        [self, result]() {
          if (!self) return;
          self->FinishImport(result);
        },
        Qt::QueuedConnection);
  };

  try {
    ImportOptions options;
    current_import_job_ = isvc->ImportToFolder(paths, import_target_folder_path_, options, job);
  } catch (const std::exception& e) {
    current_import_job_.reset();
    import_running_     = false;
    import_status_text_ = PL_TEXT("Import failed: %1", QString::fromUtf8(e.what()));
    emit ImportStateChanged();
    emit importStateChanged();
    status_->SetTaskState(import_status_text_, 0, false);
  }
}

void ImportExportHandler::CancelImport() {
  if (!current_import_job_) return;
  current_import_job_->canceled_.store(true);
  import_status_text_ = PL_TEXT("Cancelling import...");
  emit ImportStateChanged();
  emit importStateChanged();
  status_->SetTaskState(PL_TEXT("Cancelling import..."), 0, true);
}

void ImportExportHandler::StartExport(const QString& outputDirUrlOrPath) {
  StartExportWithSplitOptionsForTargets(outputDirUrlOrPath, false, 0, 8192, "JPEG", 95, 16, 5,
                                        "NONE", 95, true, {});
}

void ImportExportHandler::StartExportWithOptions(const QString& outputDirUrlOrPath,
                                                 const QString& formatName,
                                                 const QString& hdrExportMode, bool resizeEnabled,
                                                 int maxLengthSide, int quality, int bitDepth,
                                                 int            pngCompressionLevel,
                                                 const QString& tiffCompression) {
  StartExportWithOptionsForTargets(outputDirUrlOrPath, formatName, hdrExportMode, resizeEnabled,
                                   maxLengthSide, quality, bitDepth, pngCompressionLevel,
                                   tiffCompression, {});
}

void ImportExportHandler::StartExportWithOptionsForTargets(
    const QString& outputDirUrlOrPath, const QString& formatName, const QString& hdrExportMode,
    bool resizeEnabled, int maxLengthSide, int quality, int bitDepth, int pngCompressionLevel,
    const QString& tiffCompression, const QVariantList& targetEntries) {
  (void)hdrExportMode;
  StartExportWithSplitOptionsForTargets(outputDirUrlOrPath, resizeEnabled, maxLengthSide, 8192,
                                        formatName, quality, bitDepth, pngCompressionLevel,
                                        tiffCompression, quality, true, targetEntries);
}

void ImportExportHandler::StartExportWithSplitOptionsForTargets(
    const QString& outputDirUrlOrPath, bool sdrResizeEnabled, int sdrMaxLengthSide,
    int ultraHdrMaxLengthSide, const QString& sdrFormatName, int sdrQuality, int sdrBitDepth,
    int sdrPngCompressionLevel, const QString& sdrTiffCompression, int ultraHdrQuality,
    bool ultraHdrDitherEnabled, const QVariantList& targetEntries) {
  StartExportWithRecipeOptionsForTargets(outputDirUrlOrPath, sdrResizeEnabled, sdrMaxLengthSide,
                                         ultraHdrMaxLengthSide, sdrFormatName, sdrQuality,
                                         sdrBitDepth, sdrPngCompressionLevel, sdrTiffCompression,
                                         ultraHdrQuality, ultraHdrDitherEnabled, {}, targetEntries);
}

void ImportExportHandler::StartExportWithRecipeOptionsForTargets(
    const QString& outputDirUrlOrPath, bool sdrResizeEnabled, int sdrMaxLengthSide,
    int ultraHdrMaxLengthSide, const QString& sdrFormatName, int sdrQuality, int sdrBitDepth,
    int sdrPngCompressionLevel, const QString& sdrTiffCompression, int ultraHdrQuality,
    bool ultraHdrDitherEnabled, const QVariantMap& recipeOptions,
    const QVariantList& targetEntries) {
  if (project_->handler().project_loading()) {
    SetExportFailureState(PL_TEXT("Project is loading. Please wait."));
    return;
  }

  const auto& esvc = project_->handler().export_service();
  auto        proj = project_->handler().project();
  if (!esvc || !proj) {
    SetExportFailureState(PL_TEXT("Export service is unavailable."));
    return;
  }
  if (export_inflight_) {
    SetExportFailureState(PL_TEXT("Export already running."));
    return;
  }

  ResetExportProgressState(PL_TEXT("Preparing export queue..."));

  const auto outDirOpt = InputToPath(outputDirUrlOrPath);
  if (!outDirOpt.has_value()) {
    SetExportFailureState(PL_TEXT("No export folder selected."));
    return;
  }

  std::error_code ec;
  if (!std::filesystem::exists(outDirOpt.value(), ec)) {
    std::filesystem::create_directories(outDirOpt.value(), ec);
  }
  if (ec || !std::filesystem::is_directory(outDirOpt.value(), ec) || ec) {
    SetExportFailureState(PL_TEXT("Export folder is invalid."));
    return;
  }

  const auto targets = CollectExportTargets(targetEntries);
  if (targets.empty()) {
    SetExportFailureState(PL_TEXT("No images to export."));
    return;
  }

  const ImageFormatType sdr_format = FormatFromName(sdrFormatName);
  const int clamped_sdr_max = sdrResizeEnabled ? std::clamp(sdrMaxLengthSide, 256, 16384) : 0;
  const int clamped_ultra_hdr_max =
      std::clamp(ultraHdrMaxLengthSide > 0 ? ultraHdrMaxLengthSide : 8192, 256, 8192);
  const int  clamped_sdr_quality       = std::clamp(sdrQuality, 1, 100);
  const int  clamped_ultra_hdr_quality = std::clamp(ultraHdrQuality, 1, 100);
  const auto sdr_bit_depth             = SanitizeBitDepth(sdr_format, BitDepthFromInt(sdrBitDepth));
  const int  clamped_png               = std::clamp(sdrPngCompressionLevel, 0, 9);
  const auto tiff_compress             = TiffCompressFromName(sdrTiffCompression);
  esvc->ClearAllExportTasks();
  // Phase 2 (Step 4): hold the project DB write barrier across the export so
  // background image-analysis result commits queue in memory and flush after
  // export releases it. Export itself does not wait for analysis to finish.
  if (barrier_) barrier_->Acquire();
  const auto queue_result = BuildExportQueue(
      targets, outDirOpt.value(), sdrResizeEnabled, clamped_sdr_max, clamped_ultra_hdr_max,
      sdr_format, clamped_sdr_quality, sdr_bit_depth, clamped_png, tiff_compress,
      clamped_ultra_hdr_quality, ultraHdrDitherEnabled, recipeOptions);

  if (queue_result.queued_count_ == 0) {
    // Nothing to export; release the barrier we just acquired.
    if (barrier_) barrier_->Release();
    export_status_text_ = PL_TEXT("No export tasks were queued.");
    if (!queue_result.first_error_.isEmpty()) {
      export_error_summary_text_ = PL_TEXT("%1", queue_result.first_error_);
    }
    emit ExportStateChanged();
    emit exportStateChanged();
    status_->SetTaskState(PL_TEXT("No valid export tasks could be created."), 0, false);
    return;
  }

  export_item_statuses_.clear();
  for (const auto& [elementId, imageId] : queue_result.queued_targets_) {
    export_item_statuses_.insert(ExportStatusKey(elementId, imageId), QStringLiteral("queued"));
  }

  export_inflight_ = true;
  export_total_    = queue_result.queued_count_;
  export_skipped_  = queue_result.skipped_count_;
  if (queue_result.skipped_count_ > 0) {
    export_status_text_ = PL_TEXT("Exporting %1 image(s). Skipped %2 invalid item(s).",
                                  queue_result.queued_count_, queue_result.skipped_count_);
  } else {
    export_status_text_ = PL_TEXT("Exporting %1 image(s)...", queue_result.queued_count_);
  }
  emit ExportStateChanged();
  emit exportStateChanged();
  status_->SetTaskState(export_status_text_, 0, false);

  QPointer<ImportExportHandler> self(this);
  esvc->ExportAll(
      [self](const ExportProgress& progress) {
        if (!self) return;
        QMetaObject::invokeMethod(
            self,
            [self, progress]() {
              if (!self) return;
              bool state_changed = false;

              if (progress.sleeve_id_ != 0 && progress.image_id_ != 0 &&
                  (progress.task_started_ || progress.task_finished_)) {
                const QString status_key = ExportStatusKey(progress.sleeve_id_, progress.image_id_);
                if (progress.task_started_) {
                  self->export_item_statuses_.insert(status_key, QStringLiteral("running"));
                  state_changed = true;
                }
                if (progress.task_finished_) {
                  self->export_item_statuses_.insert(status_key, progress.task_success_
                                                                     ? QStringLiteral("succeeded")
                                                                     : QStringLiteral("failed"));
                  state_changed = true;
                }
              }

              if (progress.task_finished_) {
                const int completed =
                    static_cast<int>(std::min(progress.completed_, progress.total_));
                if (completed >= self->export_completed_) {
                  self->export_total_     = static_cast<int>(std::max<size_t>(progress.total_, 1));
                  self->export_completed_ = completed;
                  self->export_succeeded_ = static_cast<int>(progress.succeeded_);
                  self->export_failed_    = static_cast<int>(progress.failed_);
                  self->export_status_text_ =
                      PL_TEXT("Exporting... processed %1/%2, written %3, failed %4.",
                              self->export_completed_, self->export_total_, self->export_succeeded_,
                              self->export_failed_);

                  const int percent = self->export_total_ > 0
                                          ? (self->export_completed_ * 100) / self->export_total_
                                          : 0;
                  if (self->status_) {
                    self->status_->SetTaskState(self->export_status_text_, percent, false);
                  }
                  state_changed = true;
                }
              }

              if (state_changed) {
                emit self->ExportStateChanged();
                emit self->exportStateChanged();
              }
            },
            Qt::QueuedConnection);
      },
      [self,
       skipped = queue_result.skipped_count_](std::shared_ptr<std::vector<ExportResult>> results) {
        if (!self) return;
        QMetaObject::invokeMethod(
            self,
            [self, results, skipped]() {
              if (!self) return;
              self->FinishExport(results, skipped);
            },
            Qt::QueuedConnection);
      });
}

void ImportExportHandler::ResetExportState() {
  if (export_inflight_) return;
  ResetExportProgressState(PL_TEXT("Ready to export."));
}

auto ImportExportHandler::LoadExportSdrQuality() const -> int {
  return std::clamp(
      QSettings{}.value(QLatin1String(kExportSdrQualityKey), kDefaultExportQuality).toInt(), 1,
      100);
}

void ImportExportHandler::SaveExportSdrQuality(int quality) {
  QSettings{}.setValue(QLatin1String(kExportSdrQualityKey), std::clamp(quality, 1, 100));
}

auto ImportExportHandler::LoadExportUltraHdrQuality() const -> int {
  return std::clamp(
      QSettings{}.value(QLatin1String(kExportUltraHdrQualityKey), kDefaultExportQuality).toInt(), 1,
      100);
}

void ImportExportHandler::SaveExportUltraHdrQuality(int quality) {
  QSettings{}.setValue(QLatin1String(kExportUltraHdrQualityKey), std::clamp(quality, 1, 100));
}

auto ImportExportHandler::LoadExportFileNamePresets() const -> QVariantList {
  QVariantList valid_presets;
  const auto stored_presets = QSettings{}.value(QLatin1String(kExportFileNamePresetsKey)).toList();
  valid_presets.reserve(std::min(stored_presets.size(), qsizetype{kMaxExportPresetCount}));
  for (const QVariant& entry : stored_presets) {
    const QVariantMap map     = entry.toMap();
    const QString     name    = map.value(QStringLiteral("name")).toString().trimmed();
    const QString     pattern = map.value(QStringLiteral("pattern")).toString();
    if (name.isEmpty() || name.size() > kMaxExportPresetNameSize || pattern.isEmpty() ||
        pattern.size() > kMaxExportPatternSize ||
        !ParseExportFileNamePattern(pattern.toStdWString()).success_) {
      continue;
    }
    valid_presets.push_back(
        QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("pattern"), pattern}});
    if (valid_presets.size() >= kMaxExportPresetCount) {
      break;
    }
  }
  return valid_presets;
}

auto ImportExportHandler::SaveExportFileNamePreset(const QString& name, const QString& pattern,
                                                   const QString& replacedName) -> bool {
  const QString normalized_name = name.trimmed();
  if (normalized_name.isEmpty() || normalized_name.size() > kMaxExportPresetNameSize ||
      pattern.isEmpty() || pattern.size() > kMaxExportPatternSize ||
      !ParseExportFileNamePattern(pattern.toStdWString()).success_) {
    return false;
  }

  QVariantList presets = LoadExportFileNamePresets();
  QVariantMap  saved{{QStringLiteral("name"), normalized_name},
                     {QStringLiteral("pattern"), pattern}};

  const auto   index_of_name = [&presets](const QString& candidate) {
    for (qsizetype index = 0; index < presets.size(); ++index) {
      if (presets[index]
              .toMap()
              .value(QStringLiteral("name"))
              .toString()
              .compare(candidate, Qt::CaseInsensitive) == 0) {
        return index;
      }
    }
    return qsizetype{-1};
  };

  qsizetype replace_index =
      replacedName.trimmed().isEmpty() ? qsizetype{-1} : index_of_name(replacedName.trimmed());
  qsizetype same_name_index = index_of_name(normalized_name);
  if (replace_index >= 0 && same_name_index >= 0 && replace_index != same_name_index) {
    presets.removeAt(same_name_index);
    if (same_name_index < replace_index) {
      --replace_index;
    }
  } else if (replace_index < 0) {
    replace_index = same_name_index;
  }

  if (replace_index >= 0) {
    presets[replace_index] = saved;
  } else {
    if (presets.size() >= kMaxExportPresetCount) {
      return false;
    }
    presets.push_back(std::move(saved));
  }
  QSettings{}.setValue(QLatin1String(kExportFileNamePresetsKey), presets);
  return true;
}

auto ImportExportHandler::DeleteExportFileNamePreset(const QString& name) -> bool {
  const QString normalized_name = name.trimmed();
  if (normalized_name.isEmpty()) {
    return false;
  }
  QVariantList presets = LoadExportFileNamePresets();
  for (qsizetype index = 0; index < presets.size(); ++index) {
    if (presets[index]
            .toMap()
            .value(QStringLiteral("name"))
            .toString()
            .compare(normalized_name, Qt::CaseInsensitive) == 0) {
      presets.removeAt(index);
      QSettings{}.setValue(QLatin1String(kExportFileNamePresetsKey), presets);
      return true;
    }
  }
  return false;
}

void ImportExportHandler::FinishImport(const ImportResult& result) {
  const auto importJob = current_import_job_;
  current_import_job_.reset();

  if (!importJob || !importJob->import_log_) {
    status_->SetTaskState(PL_TEXT("Import finished but no log snapshot is available."), 0, false);
    return;
  }

  const auto snapshot                    = importJob->import_log_->Snapshot();
  const bool reimporting_nikon_he        = nikon_ && nikon_->is_reimporting();
  const auto recovery_target_folder_id   = import_target_folder_id_;
  const auto recovery_target_folder_path = import_target_folder_path_;

  bool       state_saved                 = true;
  try {
    auto* isvc = project_->handler().import_service();
    if (isvc) {
      isvc->SyncImports(snapshot, import_target_folder_path_);
    }
    auto proj = project_->handler().project();
    if (proj) {
      proj->GetSleeveService()->Sync();
      proj->GetImagePoolService()->SyncWithStorage();
      proj->SaveProject(project_->handler().meta_path());
    }
  } catch (...) {
    state_saved = false;
  }

  QString package_error;
  bool    package_saved = true;
  if (state_saved) {
    package_saved = project_->handler().PackageCurrentProjectFiles(&package_error);
  }

  library_->ReloadCurrentFolder();
  stats_->ClearFilters();
  if (stats_) emit stats_->StatsFilterChanged();

  import_target_folder_id_   = folders_->CurrentFolderElementId().value_or(0);
  import_target_folder_path_ = folders_->CurrentFolderFsPath();

  auto task_text =
      PL_TEXT("Import complete: %1 imported, %2 failed", result.imported_, result.failed_);
  if (!state_saved) {
    status_->SetServiceMessage(PL_TEXT("Import finished, but saving project state failed."));
  } else if (!package_saved) {
    status_->SetServiceMessage(package_error.isEmpty()
                                   ? PL_TEXT("Import finished, but project packing failed.")
                                   : PL_TEXT("%1", package_error));
  }
  import_running_     = false;
  import_completed_   = static_cast<int>(result.imported_);
  import_failed_      = static_cast<int>(result.failed_);
  import_status_text_ = task_text;
  emit ImportStateChanged();
  emit importStateChanged();

  status_->SetTaskState(task_text, 100, false);
  status_->ScheduleIdleTaskStateReset(1800);

  std::unordered_set<sl_element_id_t> nikon_he_ids;
  nikon_he_ids.reserve(snapshot.unsupported_nikon_he_.size());
  for (const auto& entry : snapshot.unsupported_nikon_he_) {
    nikon_he_ids.insert(entry.element_id_);
  }
  std::vector<SemanticGenerationItem> semantic_items;
  semantic_items.reserve(snapshot.created_.size());
  for (const auto& created : snapshot.created_) {
    if (created.element_id_ == 0 || created.image_id_ == 0 ||
        nikon_he_ids.contains(created.element_id_)) {
      continue;
    }
    semantic_items.push_back(SemanticGenerationItem{created.element_id_, created.image_id_});
  }
  if (semantic_) semantic_->QueuePrompt(std::move(semantic_items));

  if (reimporting_nikon_he) {
    if (nikon_) {
      nikon_->HandleReimportFinished(result);
    }
    return;
  }

  if (!snapshot.unsupported_nikon_he_.empty() && nikon_) {
    nikon_->BeginRecovery(snapshot.unsupported_nikon_he_, recovery_target_folder_id,
                          recovery_target_folder_path);
  }
}

void ImportExportHandler::FinishExport(const std::shared_ptr<std::vector<ExportResult>>& results,
                                       int skippedCount) {
  export_inflight_ = false;
  // Phase 2 (Step 4): release the barrier; the 1->0 transition fires on_release_,
  // which drains any analysis-result writes that queued behind it.
  if (barrier_) barrier_->Release();

  int         ok   = 0;
  int         fail = 0;
  QStringList errors;
  if (results) {
    for (const auto& r : *results) {
      if (r.success_) {
        ++ok;
      } else {
        ++fail;
        if (!r.message_.empty() && errors.size() < 8) {
          errors << QString::fromUtf8(r.message_.c_str());
        }
      }
    }
  }

  const int total            = ok + fail;
  export_total_              = std::max(export_total_, total);
  export_completed_          = total;
  export_succeeded_          = ok;
  export_failed_             = fail;
  export_skipped_            = skippedCount;
  export_error_summary_text_ = {};
  if (!errors.isEmpty()) {
    export_error_summary_text_ = PL_TEXT("%1", errors.join('\n'));
  }

  export_status_text_ =
      PL_TEXT("Export complete. Written %1/%2 image(s), failed %3.", ok, total, fail);
  if (skippedCount > 0) {
    export_status_text_ =
        PL_TEXT("Export complete. Written %1/%2 image(s), failed %3. Skipped %4 invalid item(s).",
                ok, total, fail, skippedCount);
  }
  emit ExportStateChanged();
  emit exportStateChanged();

  status_->SetTaskState(PL_TEXT("Export complete: %1 ok, %2 failed", ok, fail), 100, false);
  status_->ScheduleIdleTaskStateReset(1800);
}

void ImportExportHandler::AddImportedEntries(const ImportLogSnapshot& snapshot) {
  (void)snapshot;
  // Deprecated path: folder content is now reloaded through AlbumBrowseService.
}

auto ImportExportHandler::CollectExportTargets(const QVariantList& targetEntries) const
    -> std::vector<ExportTarget> {
  std::vector<ExportTarget>    targets;

  std::unordered_set<uint64_t> dedupe;
  if (targetEntries.empty()) {
    const auto& source = library_->model().items();
    targets.reserve(source.size());
    dedupe.reserve(source.size() * 2 + 1);

    for (const AlbumItem& item : source) {
      if (item.element_id == 0 || item.image_id == 0) continue;
      if (!dedupe.insert(ExportTargetKey(item.element_id, item.image_id)).second) continue;
      targets.emplace_back(item.element_id, item.image_id);
    }
    return targets;
  }

  targets.reserve(static_cast<size_t>(targetEntries.size()));
  dedupe.reserve(static_cast<size_t>(targetEntries.size()) * 2 + 1);

  for (const QVariant& entry : targetEntries) {
    const auto map       = entry.toMap();
    const auto elementId = static_cast<sl_element_id_t>(map.value("elementId").toUInt());
    const auto imageId   = static_cast<image_id_t>(map.value("imageId").toUInt());
    if (elementId == 0 || imageId == 0) continue;
    if (!dedupe.insert(ExportTargetKey(elementId, imageId)).second) continue;
    targets.emplace_back(elementId, imageId);
  }
  return targets;
}

auto ImportExportHandler::BuildExportQueue(
    const std::vector<ExportTarget>& targets, const std::filesystem::path& outputDir,
    bool sdrResizeEnabled, int sdrMaxLengthSide, int ultraHdrMaxLengthSide,
    ImageFormatType sdrFormat, int sdrQuality, ExportFormatOptions::BIT_DEPTH sdrBitDepth,
    int sdrPngCompressionLevel, ExportFormatOptions::TIFF_COMPRESS sdrTiffCompression,
    int ultraHdrQuality, bool ultraHdrDitherEnabled, const QVariantMap& recipeOptions)
    -> ExportQueueBuildResult {
  (void)ultraHdrMaxLengthSide;
  ExportQueueBuildResult summary;
  auto                   proj = project_->handler().project();
  const auto&            esvc = project_->handler().export_service();
  if (!proj || !esvc) {
    summary.first_error_ = PL_TEXT("Export service is unavailable.").Render();
    return summary;
  }

  const bool include_metadata =
      recipeOptions.value(QStringLiteral("includeMetadata"), true).toBool();
  const bool embed_icc_profile =
      recipeOptions.value(QStringLiteral("embedIccProfile"), true).toBool();
  const QString file_name_pattern =
      recipeOptions.value(QStringLiteral("fileNamePattern"), QStringLiteral("{source}")).toString();
  const QString resize_mode =
      recipeOptions
          .value(QStringLiteral("resizeMode"),
                 sdrResizeEnabled ? QStringLiteral("longEdge") : QStringLiteral("original"))
          .toString();
  const int    width_pixels    = recipeOptions.value(QStringLiteral("widthPixels")).toInt();
  const int    height_pixels   = recipeOptions.value(QStringLiteral("heightPixels")).toInt();
  const double physical_width  = recipeOptions.value(QStringLiteral("physicalWidth")).toDouble();
  const double physical_height = recipeOptions.value(QStringLiteral("physicalHeight")).toDouble();
  const double dpi             = recipeOptions.value(QStringLiteral("dpi")).toDouble();

  if (resize_mode != QStringLiteral("original") && resize_mode != QStringLiteral("longEdge") &&
      resize_mode != QStringLiteral("bounds") && resize_mode != QStringLiteral("physical")) {
    summary.skipped_count_ = static_cast<int>(targets.size());
    summary.first_error_   = PL_TEXT("The export size mode is not valid.").Render();
    return summary;
  }
  if (resize_mode == QStringLiteral("bounds") && (width_pixels <= 0 || height_pixels <= 0)) {
    summary.skipped_count_ = static_cast<int>(targets.size());
    summary.first_error_ = PL_TEXT("The export width and height must be more than zero.").Render();
    return summary;
  }
  if (resize_mode == QStringLiteral("physical") &&
      (!(physical_width > 0.0) || physical_width > 100000.0 || !(physical_height > 0.0) ||
       physical_height > 100000.0 || !(dpi > 0.0) || dpi > 10000.0)) {
    summary.skipped_count_ = static_cast<int>(targets.size());
    summary.first_error_ =
        PL_TEXT("The print size and resolution must be more than zero.").Render();
    return summary;
  }

  const auto parsed_name = ParseExportFileNamePattern(file_name_pattern.toStdWString());
  if (!parsed_name.success_) {
    summary.skipped_count_ = static_cast<int>(targets.size());
    summary.first_error_   = QString::fromStdString(parsed_name.message_);
    return summary;
  }

  std::unordered_set<std::wstring> planned_export_paths;
  planned_export_paths.reserve(targets.size() * 2 + 1);

  for (size_t target_index = 0; target_index < targets.size(); ++target_index) {
    const auto [elementId, imageId] = targets[target_index];
    try {
      const auto source_info =
          proj->GetImagePoolService()
              ->Read<std::tuple<std::filesystem::path, std::wstring, bool,
                                std::optional<ExifDisplayMetaData>>>(
                  imageId, [](const std::shared_ptr<Image>& image) {
                    if (!image) {
                      return std::tuple<std::filesystem::path, std::wstring, bool,
                                        std::optional<ExifDisplayMetaData>>{};
                    }
                    std::wstring image_name = image->image_name_;
                    if (image_name.empty() && !image->image_path_.empty()) {
                      image_name = image->image_path_.filename().wstring();
                    }
                    std::optional<ExifDisplayMetaData> metadata;
                    bool                               is_hdr = image->exif_display_.is_hdr_;
                    if (image->has_exif_display_.load()) {
                      metadata = image->exif_display_;
                    }
                    if (!image->has_exif_display_.load() && image->has_exif_json_.load()) {
                      metadata.emplace();
                      metadata->FromJson(image->exif_json_);
                      is_hdr = metadata->is_hdr_;
                    }
                    return std::make_tuple(image->image_path_, std::move(image_name), is_hdr,
                                           std::move(metadata));
                  });
      const auto& srcPath = std::get<0>(source_info);
      if (srcPath.empty()) {
        ++summary.skipped_count_;
        if (summary.first_error_.isEmpty()) {
          summary.first_error_ = PL_TEXT("Image source path is empty.").Render();
        }
        continue;
      }

      std::filesystem::path name_source_path;
      if (!std::get<1>(source_info).empty()) {
        name_source_path = std::filesystem::path(std::get<1>(source_info)).filename();
      }
      if (name_source_path.empty()) {
        name_source_path = srcPath.filename();
      }
      if (name_source_path.empty()) {
        name_source_path = std::filesystem::path(L"image");
      }

      const bool            is_hdr_export = std::get<2>(source_info);

      const ImageFormatType task_format   = is_hdr_export ? ImageFormatType::JPEG : sdrFormat;
      ExportFileNameContext name_context;
      name_context.source_stem_ = name_source_path.stem().wstring();
      name_context.sequence_    = target_index + 1;
      if (const auto& metadata = std::get<3>(source_info); metadata.has_value()) {
        name_context.capture_date_time_ =
            QString::fromUtf8(metadata->date_time_str_).toStdWString();
        name_context.camera_make_         = QString::fromUtf8(metadata->make_).toStdWString();
        name_context.camera_model_        = QString::fromUtf8(metadata->model_).toStdWString();
        name_context.lens_model_          = QString::fromUtf8(metadata->lens_).toStdWString();
        name_context.iso_                 = metadata->iso_;
        name_context.aperture_            = metadata->aperture_;
        name_context.shutter_numerator_   = metadata->shutter_speed_.first;
        name_context.shutter_denominator_ = metadata->shutter_speed_.second;
        name_context.focal_length_mm_     = metadata->focal_;
        name_context.rating_              = ExifDisplayMetaData::NormalizeRating(metadata->rating_);
      }
      const auto resolved_name =
          ExportService::ResolveFileName(parsed_name.name_template_, name_context, task_format);
      if (!resolved_name.success_) {
        throw std::runtime_error(resolved_name.message_);
      }
      auto       export_path = outputDir / resolved_name.file_name_;
      const auto path_exists = [](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
      };
      if (planned_export_paths.contains(export_path.wstring()) || path_exists(export_path)) {
        const std::wstring stem       = export_path.stem().wstring();
        const std::wstring ext        = export_path.extension().wstring();
        int                suffix_idx = 1;
        while (true) {
          const auto candidate =
              outputDir / (stem + L" (" + std::to_wstring(suffix_idx) + L")" + ext);
          if (!planned_export_paths.contains(candidate.wstring()) && !path_exists(candidate)) {
            export_path = candidate;
            break;
          }
          ++suffix_idx;
        }
      }
      planned_export_paths.insert(export_path.wstring());

      ExportTask task;
      task.sleeve_id_                = elementId;
      task.image_id_                 = imageId;
      task.options_.format_          = task_format;
      task.options_.resize_enabled_  = sdrResizeEnabled;
      task.options_.max_length_side_ = sdrResizeEnabled ? sdrMaxLengthSide : 0;
      task.options_.quality_         = is_hdr_export ? ultraHdrQuality : sdrQuality;
      task.options_.bit_depth_ =
          is_hdr_export ? ExportFormatOptions::BIT_DEPTH::BIT_8 : sdrBitDepth;
      task.options_.compression_level_        = sdrPngCompressionLevel;
      task.options_.tiff_compress_            = sdrTiffCompression;
      task.options_.hdr_export_mode_          = ExportFormatOptions::HDR_EXPORT_MODE::ULTRA_HDR;
      task.options_.ultra_hdr_quality_        = ultraHdrQuality;
      task.options_.ultra_hdr_dither_enabled_ = ultraHdrDitherEnabled;
      task.options_.export_path_              = std::move(export_path);
      task.recipe_                            = ExportRecipe::FromLegacyOptions(task.options_);
      task.recipe_->metadata_.mode_ =
          include_metadata ? ExportMetadataMode::STANDARD : ExportMetadataMode::NONE;
      task.recipe_->icc_ =
          embed_icc_profile ? ExportIccPolicy::EMBED_OUTPUT_PROFILE : ExportIccPolicy::OMIT;
      task.recipe_->file_name_ = parsed_name.name_template_;
      if (resize_mode == QStringLiteral("bounds")) {
        task.recipe_->resize_.mode_          = ExportResizeMode::BOUNDING_BOX_PIXELS;
        task.recipe_->resize_.width_pixels_  = width_pixels;
        task.recipe_->resize_.height_pixels_ = height_pixels;
      } else if (resize_mode == QStringLiteral("physical")) {
        task.recipe_->resize_.mode_            = ExportResizeMode::PHYSICAL_SIZE;
        task.recipe_->resize_.physical_width_  = physical_width;
        task.recipe_->resize_.physical_height_ = physical_height;
        task.recipe_->resize_.dpi_             = dpi;
        const QString unit = recipeOptions.value(QStringLiteral("physicalUnit")).toString();
        task.recipe_->resize_.physical_unit_ =
            unit == QStringLiteral("in")
                ? ExportPhysicalUnit::INCHES
                : (unit == QStringLiteral("cm") ? ExportPhysicalUnit::CENTIMETERS
                                                : ExportPhysicalUnit::MILLIMETERS);
      } else if (resize_mode == QStringLiteral("longEdge")) {
        task.recipe_->resize_.mode_             = ExportResizeMode::LONG_EDGE_PIXELS;
        task.recipe_->resize_.long_edge_pixels_ = sdrMaxLengthSide;
      } else {
        task.recipe_->resize_.mode_ = ExportResizeMode::ORIGINAL_PIXELS;
      }
      if (is_hdr_export) task.recipe_->resize_.maximum_edge_pixels_ = 8192;

      esvc->EnqueueExportTask(task);
      ++summary.queued_count_;
      summary.queued_targets_.emplace_back(elementId, imageId);
    } catch (const std::exception& e) {
      ++summary.skipped_count_;
      if (summary.first_error_.isEmpty()) {
        summary.first_error_ = QString::fromUtf8(e.what());
      }
    } catch (...) {
      ++summary.skipped_count_;
      if (summary.first_error_.isEmpty()) {
        summary.first_error_ = PL_TEXT("Unknown error while preparing export task.").Render();
      }
    }
  }
  return summary;
}

void ImportExportHandler::ResetExportProgressState(const i18n::LocalizedText& status) {
  export_status_text_        = status;
  export_error_summary_text_ = {};
  export_item_statuses_.clear();
  export_total_     = 0;
  export_completed_ = 0;
  export_succeeded_ = 0;
  export_failed_    = 0;
  export_skipped_   = 0;
  emit ExportStateChanged();
  emit exportStateChanged();
}

void ImportExportHandler::SetExportFailureState(const i18n::LocalizedText& message) {
  export_status_text_        = message;
  export_error_summary_text_ = {};
  export_item_statuses_.clear();
  emit ExportStateChanged();
  emit exportStateChanged();
  status_->SetTaskState(message, 0, false);
}

bool ImportExportHandler::CanUseHdrExportForTargets(const QVariantList& targetEntries) const {
  const auto targets = CollectExportTargets(targetEntries);
  if (targets.empty() || !library_) {
    return false;
  }
  for (const auto& [elementId, imageId] : targets) {
    if (const auto* item = library_->FindAlbumItem(elementId);
        item != nullptr && (imageId == 0 || item->image_id == imageId) && item->is_hdr) {
      return true;
    }
  }
  return false;
}

}  // namespace alcedo::ui

#undef PL_TEXT
