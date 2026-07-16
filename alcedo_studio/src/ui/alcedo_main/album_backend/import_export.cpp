//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/import_export.hpp"

#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/album_backend/project_db_write_barrier.hpp"
#include "ui/alcedo_main/album_backend/ui_status_sink.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>

#include <algorithm>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace alcedo::ui {

using namespace album_util;

#define PL_TEXT(text, ...)                                                    \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT,                              \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text)      \
                              __VA_OPT__(, ) __VA_ARGS__)

namespace {

auto SanitizeBitDepth(ImageFormatType format,
                      ExportFormatOptions::BIT_DEPTH requested) -> ExportFormatOptions::BIT_DEPTH {
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
    : QObject(parent), project_(project), library_(library), folders_(folders),
      status_(status), barrier_(barrier) {
  // default export folder init continues below

  const QString pictures =
      QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
  if (!pictures.isEmpty()) {
    default_export_folder_ = pictures;
  }
  export_status_text_ = PL_TEXT("Ready to export.");
}

void ImportExportHandler::BindCollaborators(StatsEngine* stats,
                                            NikonHeRecoveryController* nikon,
                                            SemanticGenerationController* semantic) {
  stats_ = stats;
  nikon_ = nikon;
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
    if (!is_supported_file(pathOpt.value())) {
      continue;
    }
    const std::wstring key = pathOpt->wstring();
    if (!seen.insert(key).second) {
      continue;
    }
    paths.push_back(pathOpt.value());
  }

  if (paths.empty()) {
    status_->SetTaskState(PL_TEXT("No supported files selected."), 0, false);
    return;
  }
  StartImportResolvedPaths(std::move(paths), false);
}

void ImportExportHandler::StartImportPaths(const std::vector<image_path_t>& paths,
                                           const bool                      preserveTarget) {
  std::vector<image_path_t> deduped_paths;
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
                                                   const bool preserveTarget) {
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

  import_running_   = true;
  import_total_     = static_cast<int>(paths.size());
  import_completed_ = 0;
  import_failed_    = 0;
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
    current_import_job_ =
        isvc->ImportToFolder(paths, import_target_folder_path_, options, job);
  } catch (const std::exception& e) {
    current_import_job_.reset();
    import_running_ = false;
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
                                                 const QString& hdrExportMode,
                                                 bool resizeEnabled, int maxLengthSide,
                                                 int quality, int bitDepth,
                                                 int pngCompressionLevel,
                                                 const QString& tiffCompression) {
  StartExportWithOptionsForTargets(outputDirUrlOrPath, formatName, hdrExportMode, resizeEnabled,
                                   maxLengthSide, quality, bitDepth, pngCompressionLevel,
                                   tiffCompression, {});
}

void ImportExportHandler::StartExportWithOptionsForTargets(
    const QString& outputDirUrlOrPath, const QString& formatName, const QString& hdrExportMode,
    bool resizeEnabled, int maxLengthSide, int quality, int bitDepth,
    int pngCompressionLevel, const QString& tiffCompression,
    const QVariantList& targetEntries) {
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
  if (project_->handler().project_loading()) {
    SetExportFailureState(PL_TEXT("Project is loading. Please wait."));
    return;
  }

  const auto& esvc = project_->handler().export_service();
  auto  proj = project_->handler().project();
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
  const int             clamped_sdr_max =
      sdrResizeEnabled ? std::clamp(sdrMaxLengthSide, 256, 16384) : 0;
  const int             clamped_ultra_hdr_max =
      std::clamp(ultraHdrMaxLengthSide > 0 ? ultraHdrMaxLengthSide : 8192, 256, 8192);
  const int             clamped_sdr_quality = std::clamp(sdrQuality, 1, 100);
  const int             clamped_ultra_hdr_quality = std::clamp(ultraHdrQuality, 1, 100);
  const auto            sdr_bit_depth =
      SanitizeBitDepth(sdr_format, BitDepthFromInt(sdrBitDepth));
  const int             clamped_png = std::clamp(sdrPngCompressionLevel, 0, 9);
  const auto            tiff_compress = TiffCompressFromName(sdrTiffCompression);

  esvc->ClearAllExportTasks();
  // Phase 2 (Step 4): hold the project DB write barrier across the export so
  // background image-analysis result commits queue in memory and flush after
  // export releases it. Export itself does not wait for analysis to finish.
  if (barrier_) barrier_->Acquire();
  const auto queue_result = BuildExportQueue(
      targets, outDirOpt.value(), sdrResizeEnabled, clamped_sdr_max, clamped_ultra_hdr_max,
      sdr_format, clamped_sdr_quality, sdr_bit_depth, clamped_png, tiff_compress,
      clamped_ultra_hdr_quality,
      ultraHdrDitherEnabled);

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
                  self->export_item_statuses_.insert(
                      status_key,
                      progress.task_success_ ? QStringLiteral("succeeded")
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

                  const int percent =
                      self->export_total_ > 0
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
      [self, skipped = queue_result.skipped_count_](
          std::shared_ptr<std::vector<ExportResult>> results) {
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

void ImportExportHandler::FinishImport(const ImportResult& result) {
  const auto importJob = current_import_job_;
  current_import_job_.reset();

  if (!importJob || !importJob->import_log_) {
    status_->SetTaskState(PL_TEXT("Import finished but no log snapshot is available."), 0, false);
    return;
  }

  const auto snapshot = importJob->import_log_->Snapshot();
  const bool reimporting_nikon_he = nikon_ && nikon_->is_reimporting();
  const auto recovery_target_folder_id = import_target_folder_id_;
  const auto recovery_target_folder_path = import_target_folder_path_;

  bool state_saved = true;
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

  auto task_text = PL_TEXT("Import complete: %1 imported, %2 failed", result.imported_,
                           result.failed_);
  if (!state_saved) {
    status_->SetServiceMessage(
        PL_TEXT("Import finished, but saving project state failed."));
  } else if (!package_saved) {
    status_->SetServiceMessage(
        package_error.isEmpty() ? PL_TEXT("Import finished, but project packing failed.")
                                : PL_TEXT("%1", package_error));
  }
  import_running_   = false;
  import_completed_ = static_cast<int>(result.imported_);
  import_failed_    = static_cast<int>(result.failed_);
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

void ImportExportHandler::FinishExport(
    const std::shared_ptr<std::vector<ExportResult>>& results, int skippedCount) {
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

  const int total   = ok + fail;
  export_total_     = std::max(export_total_, total);
  export_completed_ = total;
  export_succeeded_ = ok;
  export_failed_    = fail;
  export_skipped_   = skippedCount;
  export_error_summary_text_ = {};
  if (!errors.isEmpty()) {
    export_error_summary_text_ = PL_TEXT("%1", errors.join('\n'));
  }

  export_status_text_ = PL_TEXT("Export complete. Written %1/%2 image(s), failed %3.", ok, total,
                                fail);
  if (skippedCount > 0) {
    export_status_text_ = PL_TEXT(
        "Export complete. Written %1/%2 image(s), failed %3. Skipped %4 invalid item(s).", ok,
        total, fail, skippedCount);
  }
  emit ExportStateChanged();
  emit exportStateChanged();

  status_->SetTaskState(
      PL_TEXT("Export complete: %1 ok, %2 failed", ok, fail), 100, false);
  status_->ScheduleIdleTaskStateReset(1800);
}

void ImportExportHandler::AddImportedEntries(const ImportLogSnapshot& snapshot) {
  (void)snapshot;
  // Deprecated path: folder content is now reloaded through AlbumBrowseService.
}

auto ImportExportHandler::CollectExportTargets(const QVariantList& targetEntries) const
    -> std::vector<ExportTarget> {
  std::vector<ExportTarget> targets;

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
    ImageFormatType sdrFormat, int sdrQuality,
    ExportFormatOptions::BIT_DEPTH sdrBitDepth, int sdrPngCompressionLevel,
    ExportFormatOptions::TIFF_COMPRESS sdrTiffCompression, int ultraHdrQuality,
    bool ultraHdrDitherEnabled) -> ExportQueueBuildResult {
  ExportQueueBuildResult summary;
  auto                   proj = project_->handler().project();
  const auto&            esvc = project_->handler().export_service();
  if (!proj || !esvc) {
    summary.first_error_ = PL_TEXT("Export service is unavailable.").Render();
    return summary;
  }

  std::unordered_set<std::wstring> planned_export_paths;
  planned_export_paths.reserve(targets.size() * 2 + 1);

  for (const auto& [elementId, imageId] : targets) {
    try {
      const auto source_info =
          proj->GetImagePoolService()->Read<std::tuple<std::filesystem::path, std::wstring, bool>>(
              imageId, [](const std::shared_ptr<Image>& image) {
                if (!image) {
                  return std::tuple<std::filesystem::path, std::wstring, bool>{};
                }
                std::wstring image_name = image->image_name_;
                if (image_name.empty() && !image->image_path_.empty()) {
                  image_name = image->image_path_.filename().wstring();
                }
                bool is_hdr = image->exif_display_.is_hdr_;
                if (!image->has_exif_display_.load() && image->has_exif_json_.load()) {
                  ExifDisplayMetaData metadata;
                  metadata.FromJson(image->exif_json_);
                  is_hdr = metadata.is_hdr_;
                }
                return std::make_tuple(image->image_path_, std::move(image_name), is_hdr);
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

      const bool is_hdr_export = std::get<2>(source_info);

      const ImageFormatType task_format = is_hdr_export ? ImageFormatType::JPEG : sdrFormat;
      auto       export_path =
          ExportPathForOptions(name_source_path, outputDir, elementId, imageId, task_format);
      const auto path_exists = [](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
      };
      if (planned_export_paths.contains(export_path.wstring()) || path_exists(export_path)) {
        const std::wstring stem = export_path.stem().wstring();
        const std::wstring ext  = export_path.extension().wstring();
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
      task.sleeve_id_                  = elementId;
      task.image_id_                   = imageId;
      task.options_.format_            = task_format;
      task.options_.resize_enabled_    = is_hdr_export ? true : sdrResizeEnabled;
      task.options_.max_length_side_ =
          is_hdr_export ? ultraHdrMaxLengthSide : (sdrResizeEnabled ? sdrMaxLengthSide : 0);
      task.options_.quality_           = is_hdr_export ? ultraHdrQuality : sdrQuality;
      task.options_.bit_depth_         = is_hdr_export ? ExportFormatOptions::BIT_DEPTH::BIT_8
                                                       : sdrBitDepth;
      task.options_.compression_level_ = sdrPngCompressionLevel;
      task.options_.tiff_compress_     = sdrTiffCompression;
      task.options_.hdr_export_mode_   = ExportFormatOptions::HDR_EXPORT_MODE::ULTRA_HDR;
      task.options_.ultra_hdr_quality_ = ultraHdrQuality;
      task.options_.ultra_hdr_dither_enabled_ = ultraHdrDitherEnabled;
      task.options_.export_path_       = std::move(export_path);

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
  export_total_              = 0;
  export_completed_          = 0;
  export_succeeded_          = 0;
  export_failed_             = 0;
  export_skipped_            = 0;
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
