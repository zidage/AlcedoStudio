//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/album_backend.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QInputDialog>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <thread>

#include "app/album_browse_service.hpp"
#include "app/project_package_service.hpp"
#include "image/image.hpp"
#ifdef HAVE_OPENCL
#include "opencl/opencl_runtime.hpp"
#endif
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "utils/cuda/cuda_driver_requirements.hpp"

namespace alcedo::ui {

using namespace album_util;
#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {

constexpr auto   kRecentProjectsKey                 = "projects/recent";
constexpr auto   kAcceleratorBackendKey             = "gpu/acceleratorBackend";
constexpr auto   kAcceleratorWarningAcknowledgedKey = "gpu/acceleratorWarningAcknowledged";
constexpr int    kMaxRecentProjects                 = 12;
constexpr size_t kAlbumMetadataPageSize             = 1000;
constexpr size_t kSearchMetadataPageSize            = 120;

auto FormatCacheSize(size_t bytes) -> QString {
  if (bytes == 0) {
    return QStringLiteral("0 KiB");
  }
  if (bytes < 1024) {
    return QStringLiteral("< 1 KiB");
  }

  static constexpr const char* kUnits[] = {"KiB", "MiB", "GiB", "TiB"};
  double                       value    = static_cast<double>(bytes) / 1024.0;
  int                          unit_idx = 0;
  while (value >= 1024.0 && unit_idx < 3) {
    value /= 1024.0;
    ++unit_idx;
  }

  const int decimals = value >= 10.0 ? 1 : 2;
  return QStringLiteral("%1 %2").arg(value, 0, 'f', decimals).arg(QLatin1String(kUnits[unit_idx]));
}

class ThumbnailModelLoadingGuard {
 public:
  explicit ThumbnailModelLoadingGuard(AlbumThumbnailModel& model) : model_(model) {
    model_.setLoading(true);
  }

  ~ThumbnailModelLoadingGuard() { model_.setLoading(false); }

  ThumbnailModelLoadingGuard(const ThumbnailModelLoadingGuard&)            = delete;
  ThumbnailModelLoadingGuard& operator=(const ThumbnailModelLoadingGuard&) = delete;

 private:
  AlbumThumbnailModel& model_;
};

[[maybe_unused]] constexpr const char* kAcceleratorTranslationStrings[] = {
    QT_TRANSLATE_NOOP("Alcedo", "CPU"),
    QT_TRANSLATE_NOOP("Alcedo", "Auto"),
    QT_TRANSLATE_NOOP("Alcedo", "Detected CUDA driver compatibility: %1."),
    QT_TRANSLATE_NOOP("Alcedo", "No usable NVIDIA CUDA driver was detected."),
    QT_TRANSLATE_NOOP("Alcedo", "Failed to query the installed NVIDIA CUDA driver."),
    QT_TRANSLATE_NOOP("Alcedo",
                      "This machine has an NVIDIA graphics card, but it does not meet Alcedo's "
                      "CUDA runtime requirements. CUDA requires an NVIDIA graphics driver with "
                      "CUDA %1 or newer.\n\n%2\n\nAlcedo will use %3 instead."),
    QT_TRANSLATE_NOOP("Alcedo",
                      "CUDA acceleration is not supported on this machine because no NVIDIA "
                      "graphics card was detected.\n\nAlcedo will use %1 instead."),
    QT_TRANSLATE_NOOP("Alcedo", "Unknown accelerator backend."),
    QT_TRANSLATE_NOOP("Alcedo", "Selected accelerator backend is unavailable."),
    QT_TRANSLATE_NOOP("Alcedo", "Failed to switch accelerator backend: %1"),
    QT_TRANSLATE_NOOP("Alcedo", "Failed to switch accelerator backend."),
    QT_TRANSLATE_NOOP("Alcedo", "Using %1 acceleration."),
    QT_TRANSLATE_NOOP("Alcedo", "Preparing OpenCL acceleration..."),
    QT_TRANSLATE_NOOP("Alcedo", "Compiling OpenCL kernels. This happens every launch."),
    QT_TRANSLATE_NOOP("Alcedo", "OpenCL acceleration is ready."),
    QT_TRANSLATE_NOOP("Alcedo", "OpenCL preparation failed: %1"),
};

auto AcceleratorPreferenceKey(AcceleratorBackendPreference preference) -> QString {
  switch (preference) {
    case AcceleratorBackendPreference::CPU:
      return QStringLiteral("cpu");
    case AcceleratorBackendPreference::Auto:
      return QStringLiteral("auto");
    case AcceleratorBackendPreference::CUDA:
      return QStringLiteral("cuda");
    case AcceleratorBackendPreference::OpenCL:
      return QStringLiteral("opencl");
    case AcceleratorBackendPreference::Metal:
      return QStringLiteral("metal");
  }
  return QStringLiteral("auto");
}

auto AcceleratorPreferenceLabel(AcceleratorBackendPreference preference) -> QString {
  switch (preference) {
    case AcceleratorBackendPreference::CUDA:
      return QStringLiteral("CUDA");
    case AcceleratorBackendPreference::OpenCL:
      return QStringLiteral("OpenCL");
    case AcceleratorBackendPreference::Metal:
      return QStringLiteral("Metal");
    case AcceleratorBackendPreference::CPU:
      return Tr("CPU");
    case AcceleratorBackendPreference::Auto:
      return Tr("Auto");
  }
  return Tr("Auto");
}

auto AcceleratorPreferenceFromKey(const QString& key)
    -> std::optional<AcceleratorBackendPreference> {
  const QString normalized = key.trimmed().toLower();
  if (normalized == QLatin1String("cuda")) {
    return AcceleratorBackendPreference::CUDA;
  }
  if (normalized == QLatin1String("opencl")) {
    return AcceleratorBackendPreference::OpenCL;
  }
  if (normalized == QLatin1String("metal")) {
    return AcceleratorBackendPreference::Metal;
  }
  if (normalized == QLatin1String("cpu")) {
    return AcceleratorBackendPreference::CPU;
  }
  if (normalized == QLatin1String("auto")) {
    return AcceleratorBackendPreference::Auto;
  }
  return std::nullopt;
}

auto AcceleratorOption(AcceleratorBackendPreference preference) -> QVariantMap {
  return QVariantMap{
      {QStringLiteral("label"), AcceleratorPreferenceLabel(preference)},
      {QStringLiteral("value"), AcceleratorPreferenceKey(preference)},
  };
}

auto CanResolveAccelerator(AcceleratorBackendPreference preference) -> bool {
  try {
    (void)ResolveAcceleratorBackend(preference);
    return true;
  } catch (...) {
    return false;
  }
}

auto FindOptionIndex(const QVariantList& options, const QString& backendKey) -> int {
  for (int i = 0; i < options.size(); ++i) {
    if (options[i].toMap().value(QStringLiteral("value")).toString() == backendKey) {
      return i;
    }
  }
  return -1;
}

auto NormalizeRecentProjectPath(const std::filesystem::path& projectPath) -> QString {
  if (projectPath.empty()) {
    return {};
  }
  return QDir::cleanPath(QDir::fromNativeSeparators(
      QFileInfo(PathToQString(projectPath.lexically_normal())).absoluteFilePath()));
}

auto BuildRecentProjectEntry(const QString& normalizedPath, qint64 lastOpenedMs) -> QVariantMap {
  const QFileInfo info(normalizedPath);
  QString         display_name = info.completeBaseName();
  if (display_name.isEmpty()) {
    display_name = info.fileName();
  }
  if (display_name.isEmpty()) {
    display_name = normalizedPath;
  }

  return QVariantMap{
      {"path", normalizedPath},
      {"name", display_name},
      {"folderPath", info.absolutePath()},
      {"lastOpenedMs", lastOpenedMs},
  };
}

}  // namespace

// ── Constructor / Destructor ────────────────────────────────────────────────

AlbumBackend::AlbumBackend(QObject* parent)
    : QObject(parent),
      project_handler_(*this),
      thumb_(*this),
      folder_ctrl_(*this),
      image_ctrl_(*this),
      stats_(*this),
      search_(*this),
      model_download_controller_(*this),
      semantic_generation_(*this),
      import_export_(*this),
      nikon_he_recovery_(*this),
      editor_(*this),
      adjustment_transfer_(*this) {
  LoadRecentProjectsFromSettings();
  LoadThumbnailDiskCacheSettings();
  QObject::connect(&i18n::TranslationNotifier::Instance(),
                   &i18n::TranslationNotifier::LanguageChanged, this,
                   &AlbumBackend::RefreshTranslations);
  InitializeAcceleratorSettings();
  editor_.InitializeEditorLuts();
  SetServiceState(false, PL_TEXT("Select a project: load a .alcd package or create a new "
                                 "packed project."));
  task_status_text_ = PL_TEXT("Open or create a project to begin.");
}

AlbumBackend::~AlbumBackend() {
  try {
    semantic_generation_.CancelGeneration();
    search_.CancelSearchPreviewThumbnails();
    thumb_.ReleaseVisibleThumbnailPins();
    editor_.FinalizeEditorSession(true);
    auto job = import_export_.current_import_job();
    if (job) {
      job->canceled_.store(true);
    }
    auto psvc = project_handler_.pipeline_service();
    if (psvc) {
      psvc->Sync();
    }
    if (project_handler_.PersistCurrentProjectState()) {
      QString ignored_error;
      (void)project_handler_.PackageCurrentProjectFiles(&ignored_error);
    }
    CleanupWorkspaceDirectory(project_handler_.workspace_dir());
  } catch (...) {
  }
}

// ── Q_PROPERTY getters that compute ─────────────────────────────────────────

auto AlbumBackend::FilterInfo() const -> QString {
  return stats_.FormatPhotoInfo(ShownCount(), TotalCount());
}

int AlbumBackend::TotalCount() const {
  return static_cast<int>(
      std::min<size_t>(view_state_.total_count_, std::numeric_limits<int>::max()));
}

bool         AlbumBackend::HasMoreThumbnails() const { return thumbnail_model_.hasMore(); }

QVariantList AlbumBackend::Thumbnails() const {
  QVariantList rows;
  rows.reserve(static_cast<qsizetype>(thumbnail_model_.items().size()));
  int index = 0;
  for (const AlbumItem& image : thumbnail_model_.items()) {
    rows.push_back(stats_.MakeThumbMap(image, index++));
  }
  return rows;
}

// ── Q_INVOKABLE: Folder delegation ──────────────────────────────────────────

void AlbumBackend::SelectFolder(uint folderId) { folder_ctrl_.SelectFolder(folderId); }

void AlbumBackend::CreateFolder(const QString& folderName) {
  folder_ctrl_.CreateFolder(folderName);
}
void AlbumBackend::DeleteFolder(uint folderId) { folder_ctrl_.DeleteFolder(folderId); }
auto AlbumBackend::DeleteImages(const QVariantList& targetEntries) -> QVariantMap {
  return image_ctrl_.DeleteImages(targetEntries);
}
auto AlbumBackend::AddImagesToFolder(const QVariantList& targetEntries, uint targetFolderId)
    -> QVariantMap {
  return image_ctrl_.AddImagesToFolder(targetEntries, targetFolderId);
}
auto AlbumBackend::GetImageDetails(uint elementId, uint imageId) -> QVariantMap {
  return image_ctrl_.GetImageDetails(elementId, imageId);
}
auto AlbumBackend::GetImageRating(uint elementId, uint imageId) -> QVariantMap {
  return image_ctrl_.GetImageRating(elementId, imageId);
}
auto AlbumBackend::SetImageRating(uint elementId, uint imageId, int rating) -> QVariantMap {
  return image_ctrl_.SetImageRating(elementId, imageId, rating);
}
bool AlbumBackend::OpenDirectoryInFileManager(const QString& dirUrlOrPath) {
  const auto dir_path_opt = InputToPath(dirUrlOrPath);
  if (!dir_path_opt.has_value()) {
    SetServiceMessageForCurrentProject(PL_TEXT("Source directory is unavailable."));
    return false;
  }

  const std::filesystem::path dir_path = dir_path_opt.value().lexically_normal();
  std::error_code             ec;
  if (!std::filesystem::exists(dir_path, ec) || ec ||
      !std::filesystem::is_directory(dir_path, ec) || ec) {
    SetServiceMessageForCurrentProject(PL_TEXT("Source directory is unavailable."));
    return false;
  }

  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(PathToQString(dir_path)))) {
    SetServiceMessageForCurrentProject(PL_TEXT("Failed to open source directory."));
    return false;
  }

  return true;
}

// ── Q_INVOKABLE: Stats-bar filter ──────────────────────────────────────────

void AlbumBackend::ToggleStatsFilter(const QString& category, const QString& label) {
  stats_.ToggleFilter(category, label);
  stats_.RebuildThumbnailView();
  emit StatsFilterChanged();
}

void AlbumBackend::ClearStatsFilter() {
  stats_.ClearFilters();
  stats_.RebuildThumbnailView();
  emit StatsFilterChanged();
}

// ── Q_INVOKABLE: Import / export delegation ─────────────────────────────────

void AlbumBackend::StartImport(const QStringList& fileUrlsOrPaths) {
  import_export_.StartImport(fileUrlsOrPaths);
}
void AlbumBackend::CancelImport() { import_export_.CancelImport(); }

void AlbumBackend::SetAcceleratorPreparationState(bool preparing,
                                                  const i18n::LocalizedText& status) {
  accelerator_preparing_               = preparing;
  accelerator_preparation_status_text_ = status;
  emit AcceleratorPreparationStateChanged();
}

void AlbumBackend::StartOpenClPreparationIfNeeded() {
  if (accelerator_prepare_started_ || accelerator_preparing_ ||
      accelerator_preference_ != AcceleratorBackendPreference::OpenCL) {
    return;
  }

  accelerator_prepare_started_ = true;

#ifdef HAVE_OPENCL
  SetAcceleratorPreparationState(true, PL_TEXT("Compiling OpenCL kernels. This happens every launch."));
  QPointer<AlbumBackend> self(this);
  std::thread([self]() {
    QString error_text;
    try {
      WarmUpOpenClRuntime();
    } catch (const std::exception& error) {
      error_text = QString::fromUtf8(error.what());
    } catch (...) {
      error_text = QStringLiteral("Unknown OpenCL preparation error.");
    }

    if (!self) {
      return;
    }
    QMetaObject::invokeMethod(
        self,
        [self, error_text]() {
          if (!self) {
            return;
          }
          if (error_text.isEmpty()) {
            self->SetAcceleratorPreparationState(false, PL_TEXT("OpenCL acceleration is ready."));
            self->SetServiceMessageForCurrentProject(PL_TEXT("OpenCL acceleration is ready."));
            return;
          }

          self->accelerator_prepare_started_ = false;
          self->SetAcceleratorPreparationState(
              false, PL_TEXT("OpenCL preparation failed: %1", error_text));
          self->SetServiceMessageForCurrentProject(
              PL_TEXT("OpenCL preparation failed: %1", error_text));
        },
        Qt::QueuedConnection);
  }).detach();
#else
  SetAcceleratorPreparationState(false, {});
#endif
}

void AlbumBackend::StartAcceleratorPreparation() { StartOpenClPreparationIfNeeded(); }

void AlbumBackend::InitializeAcceleratorSettings() {
  const QString stored_key = QSettings{}.value(QLatin1String(kAcceleratorBackendKey)).toString();
  const auto    stored_preference = AcceleratorPreferenceFromKey(stored_key);
  const bool    stored_cuda_preference =
      stored_preference.has_value() && *stored_preference == AcceleratorBackendPreference::CUDA;

#if defined(__APPLE__)
  metal_backend_available_ = CanResolveAccelerator(AcceleratorBackendPreference::Metal);
#else
  cuda_backend_available_   = CanResolveAccelerator(AcceleratorBackendPreference::CUDA);
  opencl_backend_available_ = CanResolveAccelerator(AcceleratorBackendPreference::OpenCL);
#if defined(_WIN32) && defined(HAVE_CUDA)
  const auto cuda_support = cuda::CheckDriverSupport();
  if (!cuda_backend_available_) {
    const QString fallback_backend =
        opencl_backend_available_ ? QStringLiteral("OpenCL")
                                  : AcceleratorPreferenceLabel(AcceleratorBackendPreference::CPU);

    if (cuda_support.nvidia_adapter_detected) {
      accelerator_warning_id_ = QStringLiteral("cuda-driver:%1:%2:%3")
                                    .arg(static_cast<int>(cuda_support.status))
                                    .arg(cuda_support.detected_cuda_driver_version)
                                    .arg(cuda::kMinimumSupportedCudaDriverVersion);
      const QString required_version =
          QString::fromStdString(cuda::FormatCudaVersion(cuda::kMinimumSupportedCudaDriverVersion));
      const QString detected_version = QString::fromStdString(
          cuda::FormatCudaVersion(cuda_support.detected_cuda_driver_version));

      QString detail;
      switch (cuda_support.status) {
        case cuda::DriverSupportStatus::kDriverTooOld:
          detail = PL_TEXT("Detected CUDA driver compatibility: %1.", detected_version).Render();
          break;
        case cuda::DriverSupportStatus::kDriverUnavailable:
          detail = PL_TEXT("No usable NVIDIA CUDA driver was detected.").Render();
          break;
        case cuda::DriverSupportStatus::kQueryFailed:
          detail = PL_TEXT("Failed to query the installed NVIDIA CUDA driver.").Render();
          break;
        case cuda::DriverSupportStatus::kSupported:
          break;
      }
      if (!cuda_support.detail.empty()) {
        const QString raw_detail = QString::fromStdString(cuda_support.detail);
        detail = detail.isEmpty() ? raw_detail : detail + QStringLiteral("\n") + raw_detail;
      }

      accelerator_warning_text_ = PL_TEXT(
          "This machine has an NVIDIA graphics card, but it does not meet Alcedo's CUDA "
          "runtime requirements. CUDA requires an NVIDIA graphics driver with CUDA %1 or newer.\n\n"
          "%2\n\nAlcedo will use %3 instead.",
          required_version, detail, fallback_backend);
    } else {
      accelerator_warning_id_   = QStringLiteral("cuda-no-nvidia");
      accelerator_warning_text_ = PL_TEXT(
          "CUDA acceleration is not supported on this machine because no NVIDIA graphics card was "
          "detected.\n\nAlcedo will use %1 instead.",
          fallback_backend);
    }

    if (stored_cuda_preference) {
      const QString stored_cuda_warning =
          PL_TEXT("The saved CUDA accelerator setting is unavailable. Alcedo will use %1 instead.",
                  fallback_backend)
              .Render();
      accelerator_warning_text_ =
          accelerator_warning_text_.IsEmpty()
              ? PL_TEXT("%1", stored_cuda_warning)
              : PL_TEXT("%1\n\n%2", accelerator_warning_text_.Render(), stored_cuda_warning);
    }
  }
#endif
#endif

  RebuildAcceleratorOptions();

  if (stored_preference.has_value() &&
      FindOptionIndex(accelerator_options_, AcceleratorPreferenceKey(*stored_preference)) >= 0) {
    accelerator_preference_  = *stored_preference;
    accelerator_backend_key_ = AcceleratorPreferenceKey(accelerator_preference_);
    return;
  }

  if (!accelerator_options_.isEmpty()) {
    const QString first_key =
        accelerator_options_.front().toMap().value(QStringLiteral("value")).toString();
    accelerator_preference_ =
        AcceleratorPreferenceFromKey(first_key).value_or(AcceleratorBackendPreference::Auto);
    accelerator_backend_key_ = first_key;
  } else {
    accelerator_preference_  = AcceleratorBackendPreference::CPU;
    accelerator_backend_key_ = AcceleratorPreferenceKey(accelerator_preference_);
  }
  QSettings{}.setValue(QLatin1String(kAcceleratorBackendKey), accelerator_backend_key_);
}

void AlbumBackend::RebuildAcceleratorOptions() {
  QVariantList options;
#if defined(__APPLE__)
  if (metal_backend_available_) {
    options.push_back(AcceleratorOption(AcceleratorBackendPreference::Metal));
  }
#else
  if (cuda_backend_available_) {
    options.push_back(AcceleratorOption(AcceleratorBackendPreference::CUDA));
  }
  if (opencl_backend_available_) {
    options.push_back(AcceleratorOption(AcceleratorBackendPreference::OpenCL));
  }
#endif
  if (options.isEmpty()) {
    options.push_back(AcceleratorOption(AcceleratorBackendPreference::CPU));
  }
  accelerator_options_ = options;
}

void AlbumBackend::ApplyAcceleratorPreferenceToServices() {
  const auto& psvc = project_handler_.pipeline_service();
  if (psvc) {
    psvc->SetAcceleratorBackendPreference(accelerator_preference_);
  }
}

bool AlbumBackend::SetAcceleratorBackend(const QString& backendKey) {
  const auto preference = AcceleratorPreferenceFromKey(backendKey);
  if (!preference.has_value()) {
    SetServiceMessageForCurrentProject(PL_TEXT("Unknown accelerator backend."));
    return false;
  }

  const QString normalized_key = AcceleratorPreferenceKey(*preference);
  if (FindOptionIndex(accelerator_options_, normalized_key) < 0) {
    SetServiceMessageForCurrentProject(PL_TEXT("Selected accelerator backend is unavailable."));
    return false;
  }

  try {
    (void)ResolveAcceleratorBackend(*preference);
    accelerator_preference_  = *preference;
    accelerator_backend_key_ = normalized_key;
    QSettings{}.setValue(QLatin1String(kAcceleratorBackendKey), accelerator_backend_key_);
    ApplyAcceleratorPreferenceToServices();
    StartOpenClPreparationIfNeeded();
  } catch (const std::exception& e) {
    SetServiceMessageForCurrentProject(
        PL_TEXT("Failed to switch accelerator backend: %1", QString::fromUtf8(e.what())));
    return false;
  } catch (...) {
    SetServiceMessageForCurrentProject(PL_TEXT("Failed to switch accelerator backend."));
    return false;
  }

  emit AcceleratorStateChanged();
  SetServiceMessageForCurrentProject(
      PL_TEXT("Using %1 acceleration.", AcceleratorPreferenceLabel(accelerator_preference_)));
  return true;
}

void AlbumBackend::AcknowledgeAcceleratorWarning() {
  if (accelerator_warning_id_.isEmpty()) {
    return;
  }
  PersistAcceleratorWarningAcknowledgement();
  emit AcceleratorStateChanged();
}

bool AlbumBackend::IsAcceleratorWarningAcknowledged() const {
  return !accelerator_warning_id_.isEmpty() &&
         QSettings{}.value(QLatin1String(kAcceleratorWarningAcknowledgedKey)).toString() ==
             accelerator_warning_id_;
}

void AlbumBackend::PersistAcceleratorWarningAcknowledgement() const {
  if (!accelerator_warning_id_.isEmpty()) {
    QSettings{}.setValue(QLatin1String(kAcceleratorWarningAcknowledgedKey),
                         accelerator_warning_id_);
  }
}

void AlbumBackend::StartExport(const QString& outputDirUrlOrPath) {
  import_export_.StartExport(outputDirUrlOrPath);
}

void AlbumBackend::StartExportWithOptions(const QString& outputDirUrlOrPath,
                                          const QString& formatName, const QString& hdrExportMode,
                                          bool resizeEnabled, int maxLengthSide, int quality,
                                          int bitDepth, int pngCompressionLevel,
                                          const QString& tiffCompression) {
  import_export_.StartExportWithOptions(outputDirUrlOrPath, formatName, hdrExportMode,
                                        resizeEnabled, maxLengthSide, quality, bitDepth,
                                        pngCompressionLevel, tiffCompression);
}

void AlbumBackend::StartExportWithOptionsForTargets(
    const QString& outputDirUrlOrPath, const QString& formatName, const QString& hdrExportMode,
    bool resizeEnabled, int maxLengthSide, int quality, int bitDepth, int pngCompressionLevel,
    const QString& tiffCompression, const QVariantList& targetEntries) {
  import_export_.StartExportWithOptionsForTargets(
      outputDirUrlOrPath, formatName, hdrExportMode, resizeEnabled, maxLengthSide, quality,
      bitDepth, pngCompressionLevel, tiffCompression, targetEntries);
}

void AlbumBackend::StartExportWithSplitOptionsForTargets(
    const QString& outputDirUrlOrPath, bool sdrResizeEnabled, int sdrMaxLengthSide,
    int ultraHdrMaxLengthSide,
    const QString& sdrFormatName, int sdrQuality, int sdrBitDepth, int sdrPngCompressionLevel,
    const QString& sdrTiffCompression, int ultraHdrQuality, bool ultraHdrDitherEnabled,
    const QVariantList& targetEntries) {
  import_export_.StartExportWithSplitOptionsForTargets(
      outputDirUrlOrPath, sdrResizeEnabled, sdrMaxLengthSide, ultraHdrMaxLengthSide, sdrFormatName,
      sdrQuality, sdrBitDepth, sdrPngCompressionLevel, sdrTiffCompression, ultraHdrQuality,
      ultraHdrDitherEnabled, targetEntries);
}

void AlbumBackend::ResetExportState() { import_export_.ResetExportState(); }
bool AlbumBackend::CanUseHdrExportForTargets(const QVariantList& targetEntries) const {
  const auto targets = import_export_.CollectExportTargets(targetEntries);
  if (targets.empty()) {
    return false;
  }

  for (const auto& [elementId, imageId] : targets) {
    if (const auto* item = FindAlbumItem(elementId);
        item != nullptr && (imageId == 0 || item->image_id == imageId) && item->is_hdr) {
      return true;
    }
  }

  return false;
}
void AlbumBackend::BrowseNikonHeConverter() { nikon_he_recovery_.BrowseConverter(); }
void AlbumBackend::StartNikonHeConversion() { nikon_he_recovery_.StartConversion(); }
void AlbumBackend::ExitNikonHeRecovery() { nikon_he_recovery_.ExitRecovery(); }

// ── Q_INVOKABLE: Editor delegation ──────────────────────────────────────────

void AlbumBackend::OpenEditor(uint elementId, uint imageId) {
  editor_.OpenEditor(elementId, imageId);
}
void AlbumBackend::CloseEditor() { editor_.CloseEditor(); }
void AlbumBackend::ResetEditorAdjustments() { editor_.ResetEditorAdjustments(); }
void AlbumBackend::RequestEditorFullPreview() { editor_.RequestEditorFullPreview(); }
void AlbumBackend::SetEditorLutIndex(int index) { editor_.SetEditorLutIndex(index); }
void AlbumBackend::SetEditorExposure(double value) { editor_.SetEditorExposure(value); }
void AlbumBackend::SetEditorContrast(double value) { editor_.SetEditorContrast(value); }
void AlbumBackend::SetEditorSaturation(double value) { editor_.SetEditorSaturation(value); }
void AlbumBackend::SetEditorTint(double value) { editor_.SetEditorTint(value); }
void AlbumBackend::SetEditorBlacks(double value) { editor_.SetEditorBlacks(value); }
void AlbumBackend::SetEditorWhites(double value) { editor_.SetEditorWhites(value); }
void AlbumBackend::SetEditorShadows(double value) { editor_.SetEditorShadows(value); }
void AlbumBackend::SetEditorHighlights(double value) { editor_.SetEditorHighlights(value); }
void AlbumBackend::SetEditorSharpen(double value) { editor_.SetEditorSharpen(value); }
void AlbumBackend::SetEditorClarity(double value) { editor_.SetEditorClarity(value); }

// ── Q_INVOKABLE: Thumbnail delegation ───────────────────────────────────────

void AlbumBackend::SetThumbnailVisible(uint elementId, uint imageId, bool visible, uint maxEdge) {
  thumb_.SetThumbnailVisible(elementId, imageId, visible, maxEdge);
}

void AlbumBackend::SetThumbnailCacheHint(uint visibleCells, uint maxEdge) {
  auto thumb_svc = project_handler_.thumbnail_service();
  if (!thumb_svc) {
    return;
  }

  // Cache by count, but cap high-resolution tiers aggressively because
  // thumbnails are stored as float RGBA ImageBuffers before QML conversion.
  const uint32_t scroll_buffer = std::max<uint32_t>(visibleCells * 3, visibleCells + 4);
  uint32_t       tier_cap      = 96;
  if (maxEdge > 1024) {
    tier_cap = 8;
  } else if (maxEdge > 512) {
    tier_cap = 16;
  } else if (maxEdge > 256) {
    tier_cap = 48;
  }
  const uint32_t desired = std::clamp<uint32_t>(scroll_buffer, 4, tier_cap);
  try {
    thumb_svc->ResizeCache(desired);
  } catch (...) {
  }
}

bool AlbumBackend::LoadMoreThumbnails() {
  if (thumbnail_model_.loading() || !thumbnail_model_.hasMore()) {
    return false;
  }
  return stats_.LoadMoreThumbnailView();
}

bool AlbumBackend::LoadThumbnailsThroughIndex(int index) {
  if (index < 0 || thumbnail_model_.loading()) {
    return false;
  }

  const int target = TotalCount() > 0 ? std::min(index, TotalCount() - 1) : index;
  bool      loaded_any = false;
  while (thumbnail_model_.hasMore() && thumbnail_model_.count() <= target) {
    if (!stats_.LoadMoreThumbnailView()) {
      break;
    }
    loaded_any = true;
  }
  return loaded_any;
}

// ── Q_INVOKABLE: Project I/O ────────────────────────────────────────────────

bool AlbumBackend::PromptAndLoadProject() {
  const QString start_dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString selected_path =
      QFileDialog::getOpenFileName(nullptr, tr("Select Project Package"), start_dir,
                                   tr("Packed Project (*.alcd);;All Files (*)"));
  if (selected_path.isEmpty()) {
    return false;
  }
  return LoadProject(QUrl::fromLocalFile(selected_path).toString());
}

bool AlbumBackend::PromptAndCreateProject() {
  const QString start_dir    = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString selected_dir = QFileDialog::getExistingDirectory(
      nullptr, tr("Select Parent Folder for New Project"), start_dir,
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (selected_dir.isEmpty()) {
    return false;
  }

  bool    accepted = false;
  QString project_name =
      QInputDialog::getText(nullptr, tr("Name New Project"), tr("Project name"), QLineEdit::Normal,
                            QStringLiteral("album_editor_project"), &accepted);
  if (!accepted) {
    return false;
  }

  project_name = project_name.trimmed();
  if (project_name.isEmpty()) {
    SetServiceMessageForCurrentProject(PL_TEXT("Project name cannot be empty."));
    return false;
  }

  return CreateProjectInFolderNamed(QUrl::fromLocalFile(selected_dir).toString(), project_name);
}

bool AlbumBackend::LoadProject(const QString& metaFileUrlOrPath) {
  if (project_handler_.project_loading()) {
    SetServiceMessageForCurrentProject(PL_TEXT("A project load is already in progress."));
    return false;
  }

  const auto project_path_opt = InputToPath(metaFileUrlOrPath);
  if (!project_path_opt.has_value()) {
    SetServiceMessageForCurrentProject(PL_TEXT("Select a valid project file."));
    return false;
  }

  const auto      project_path = project_path_opt.value();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(project_path, ec) || ec) {
    RemoveRecentProject(project_path);
    SetServiceMessageForCurrentProject(PL_TEXT("Project file was not found."));
    return false;
  }

  ProjectPackageService package_service;
  if (!package_service.IsSupportedProjectFile(project_path)) {
    RemoveRecentProject(project_path);
    SetServiceMessageForCurrentProject(
        PL_TEXT("Unsupported or damaged project package. Choose a valid .alcd file."));
    return false;
  }

  if (package_service.IsPackedProjectPath(project_path)) {
    const QString         project_name = QFileInfo(PathToQString(project_path)).completeBaseName();
    std::filesystem::path workspace_dir;
    QString               workspace_error;
    if (!package_service.CreateProjectWorkspace(project_name, &workspace_dir, &workspace_error)) {
      SetServiceMessageForCurrentProject(workspace_error.isEmpty()
                                             ? PL_TEXT("Failed to prepare project temp workspace.")
                                             : PL_TEXT("%1", workspace_error));
      return false;
    }

    std::filesystem::path unpacked_db_path;
    std::filesystem::path unpacked_meta_path;
    QString               unpack_error;
    if (!package_service.UnpackProjectToWorkspace(project_path, workspace_dir, project_name,
                                                  &unpacked_db_path, &unpacked_meta_path,
                                                  &unpack_error)) {
      CleanupWorkspaceDirectory(workspace_dir);
      SetServiceMessageForCurrentProject(unpack_error.isEmpty()
                                             ? PL_TEXT("Failed to unpack project package.")
                                             : PL_TEXT("%1", unpack_error));
      return false;
    }

    return project_handler_.InitializeServices(unpacked_db_path, unpacked_meta_path,
                                               ProjectOpenMode::kLoadExisting, project_path,
                                               workspace_dir, project_path);
  }

  SetServiceMessageForCurrentProject(PL_TEXT("Unsupported project format. Choose a .alcd file."));
  return false;
}

bool AlbumBackend::CreateProjectInFolder(const QString& folderUrlOrPath) {
  return CreateProjectInFolderNamed(folderUrlOrPath, "album_editor_project");
}

bool AlbumBackend::CreateProjectInFolderNamed(const QString& folderUrlOrPath,
                                              const QString& projectName) {
  if (project_handler_.project_loading()) {
    SetServiceMessageForCurrentProject(PL_TEXT("A project load is already in progress."));
    return false;
  }

  const auto folder_path_opt = InputToPath(folderUrlOrPath);
  if (!folder_path_opt.has_value()) {
    SetServiceMessageForCurrentProject(PL_TEXT("Select a valid folder for the new project."));
    return false;
  }

  ProjectPackageService package_service;

  QString               build_error;
  const auto            packed_path_opt = package_service.BuildUniquePackedProjectPath(
      folder_path_opt.value(), projectName, &build_error);
  if (!packed_path_opt.has_value()) {
    SetServiceMessageForCurrentProject(
        build_error.isEmpty()
            ? PL_TEXT("Failed to prepare project package path in selected folder.")
            : PL_TEXT("%1", build_error));
    return false;
  }

  std::filesystem::path workspace_dir;
  QString               workspace_error;
  if (!package_service.CreateProjectWorkspace(projectName, &workspace_dir, &workspace_error)) {
    SetServiceMessageForCurrentProject(workspace_error.isEmpty()
                                           ? PL_TEXT("Failed to prepare project temp workspace.")
                                           : PL_TEXT("%1", workspace_error));
    return false;
  }

  const auto runtime_pair = package_service.BuildRuntimeProjectPair(workspace_dir, projectName);
  const bool started      = project_handler_.InitializeServices(
      runtime_pair.first, runtime_pair.second, ProjectOpenMode::kCreateNew, packed_path_opt.value(),
      workspace_dir, packed_path_opt.value());
  if (!started) {
    CleanupWorkspaceDirectory(workspace_dir);
  }
  return started;
}

bool AlbumBackend::SaveProject() {
  if (project_handler_.project_loading()) {
    SetServiceMessageForCurrentProject(PL_TEXT("Please wait until project loading finishes."));
    return false;
  }

  if (!project_handler_.project() || project_handler_.meta_path().empty()) {
    SetServiceState(false, PL_TEXT("No project is loaded yet."));
    SetTaskState(PL_TEXT("No project to save."), 0, false);
    return false;
  }

  if (editor_.editor_active()) {
    editor_.FinalizeEditorSession(true);
  }

  if (!project_handler_.PersistCurrentProjectState()) {
    SetServiceMessageForCurrentProject(PL_TEXT("Project save failed."));
    SetTaskState(PL_TEXT("Project save failed."), 0, false);
    return false;
  }

  QString package_error;
  if (!project_handler_.PackageCurrentProjectFiles(&package_error)) {
    SetServiceMessageForCurrentProject(package_error.isEmpty()
                                           ? PL_TEXT("Project saved, but packing failed.")
                                           : PL_TEXT("%1", package_error));
    SetTaskState(PL_TEXT("Project packing failed."), 0, false);
    return false;
  }

  SetServiceMessageForCurrentProject(
      project_handler_.package_path().empty()
          ? PL_TEXT("Project saved to %1", PathToQString(project_handler_.meta_path()))
          : PL_TEXT("Project saved and packed to %1",
                    PathToQString(project_handler_.package_path())));
  SetTaskState(project_handler_.package_path().empty() ? PL_TEXT("Project saved.")
                                                       : PL_TEXT("Project saved and packed."),
               100, false);
  ScheduleIdleTaskStateReset(1200);
  return true;
}

// ── Shared internal methods ─────────────────────────────────────────────────

void AlbumBackend::SetServiceState(bool ready, const i18n::LocalizedText& message) {
  if (service_ready_ == ready && service_message_text_.source_ == message.source_ &&
      service_message_text_.args_ == message.args_) {
    return;
  }
  service_ready_        = ready;
  service_message_text_ = message;
  emit ServiceStateChanged();
}

void AlbumBackend::SetServiceMessageForCurrentProject(const i18n::LocalizedText& message) {
  SetServiceState(project_handler_.project() != nullptr, message);
}

void AlbumBackend::ScheduleIdleTaskStateReset(int delayMs) {
  QTimer::singleShot(std::max(delayMs, 0), this, [this]() {
    if (!import_export_.export_inflight() && !task_cancel_visible_) {
      SetTaskState(PL_TEXT("No background tasks"), 0, false);
    }
  });
}

void AlbumBackend::SetTaskState(const i18n::LocalizedText& status, int progress,
                                bool cancelVisible) {
  task_status_text_    = status;
  task_progress_       = std::clamp(progress, 0, 100);
  task_cancel_visible_ = cancelVisible;
  emit TaskStateChanged();
}

void AlbumBackend::QueueSemanticGenerationPrompt(std::vector<SemanticGenerationItem> items) {
  semantic_generation_.QueuePrompt(std::move(items));
}

void AlbumBackend::ResumeQueuedSemanticGenerationWorkflow() {
  semantic_generation_.ResumeQueuedWorkflow();
}

auto AlbumBackend::ActiveSemanticModelKey() const -> std::string {
  return semantic_generation_.ActiveModelKey();
}

auto AlbumBackend::SemanticLabelDisplayText(sl_element_id_t elementId) const -> QString {
  return semantic_generation_.LabelDisplayText(elementId);
}

void AlbumBackend::RefreshTranslations() {
  if (!folder_ctrl_.folder_entries().empty()) {
    folder_ctrl_.RebuildFolderView();
  }
  if (!thumbnail_model_.items().empty()) {
    stats_.RebuildThumbnailView();
  }
  stats_.RefreshStats();
  emit ServiceStateChanged();
  RebuildAcceleratorOptions();
  emit AcceleratorStateChanged();
  emit TaskStateChanged();
  emit ProjectLoadStateChanged();
  emit ImportStateChanged();
  emit importStateChanged();
  emit ExportStateChanged();
  emit exportStateChanged();
  emit RecentProjectsChanged();
  emit EditorStateChanged();
}

void AlbumBackend::LoadRecentProjectsFromSettings() {
  const QVariantList stored_entries = QSettings{}.value(QLatin1String(kRecentProjectsKey)).toList();

  QVariantList       normalized_entries;
  QStringList        seen_paths;
  bool               changed = false;
  ProjectPackageService package_service;

  for (const QVariant& entry_variant : stored_entries) {
    const QVariantMap entry_map = entry_variant.toMap();
    const QString     raw_path  = entry_map.value(QStringLiteral("path")).toString().trimmed();
    if (raw_path.isEmpty()) {
      changed = true;
      continue;
    }

    const QString normalized_path =
        NormalizeRecentProjectPath(std::filesystem::path(raw_path.toStdWString()));
    if (normalized_path.isEmpty() || seen_paths.contains(normalized_path)) {
      changed = true;
      continue;
    }

    const QFileInfo info(normalized_path);
    if (!info.exists() || !info.isFile()) {
      changed = true;
      continue;
    }
    if (!package_service.IsSupportedProjectFile(
            std::filesystem::path(normalized_path.toStdWString()))) {
      changed = true;
      continue;
    }

    const qint64 last_opened_ms = entry_map.value(QStringLiteral("lastOpenedMs")).toLongLong();
    normalized_entries.push_back(
        BuildRecentProjectEntry(normalized_path, last_opened_ms > 0 ? last_opened_ms : 0));
    seen_paths.push_back(normalized_path);

    if (normalized_entries.size() >= kMaxRecentProjects) {
      if (stored_entries.size() > normalized_entries.size()) {
        changed = true;
      }
      break;
    }
  }

  recent_projects_ = normalized_entries;
  if (changed || stored_entries != normalized_entries) {
    PersistRecentProjects();
  }
}

void AlbumBackend::PersistRecentProjects() const {
  QSettings{}.setValue(QLatin1String(kRecentProjectsKey), recent_projects_);
}

void AlbumBackend::RegisterRecentProject(const std::filesystem::path& projectPath) {
  const QString normalized_path = NormalizeRecentProjectPath(projectPath);
  if (normalized_path.isEmpty()) {
    return;
  }

  QVariantList next_entries;
  next_entries.push_back(BuildRecentProjectEntry(
      normalized_path, QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()));

  for (const QVariant& entry_variant : recent_projects_) {
    const QVariantMap entry_map  = entry_variant.toMap();
    const QString     entry_path = entry_map.value(QStringLiteral("path")).toString();
    if (entry_path.isEmpty() || entry_path == normalized_path) {
      continue;
    }
    next_entries.push_back(entry_map);
    if (next_entries.size() >= kMaxRecentProjects) {
      break;
    }
  }

  if (recent_projects_ == next_entries) {
    return;
  }

  recent_projects_ = next_entries;
  PersistRecentProjects();
  emit RecentProjectsChanged();
}

void AlbumBackend::RemoveRecentProject(const std::filesystem::path& projectPath) {
  const QString normalized_path = NormalizeRecentProjectPath(projectPath);
  if (normalized_path.isEmpty()) {
    return;
  }

  QVariantList next_entries;
  next_entries.reserve(recent_projects_.size());
  for (const QVariant& entry_variant : recent_projects_) {
    const QVariantMap entry_map = entry_variant.toMap();
    if (entry_map.value(QStringLiteral("path")).toString() == normalized_path) {
      continue;
    }
    next_entries.push_back(entry_map);
  }

  if (recent_projects_ == next_entries) {
    return;
  }

  recent_projects_ = next_entries;
  PersistRecentProjects();
  emit RecentProjectsChanged();
}

void AlbumBackend::ReloadFolderTree(const std::filesystem::path& preferredFolderPath) {
  auto proj = project_handler_.project();
  if (!proj) {
    folder_ctrl_.ClearState();
    emit FoldersChanged();
    emit FolderSelectionChanged();
    emit folderSelectionChanged();
    return;
  }

  auto browse = proj->GetAlbumBrowseService();
  if (!browse) {
    return;
  }

  folder_ctrl_.ReloadTree(preferredFolderPath.empty() ? folder_ctrl_.current_folder_path()
                                                      : preferredFolderPath);
}

void AlbumBackend::ReloadCurrentFolder() {
  stats_.RebuildThumbnailView();
  stats_.RefreshStats();
}

bool AlbumBackend::LoadThumbnailWindow(const std::optional<std::wstring>& filterWhere, bool reset) {
  if (thumbnail_model_.loading()) {
    return false;
  }
  ThumbnailModelLoadingGuard loading_guard(thumbnail_model_);
  const auto                 effective_filter_where = EffectiveFilterWhere(filterWhere);

  if (reset) {
    thumb_.ReleaseVisibleThumbnailPins();

    view_state_.all_images_.clear();
    view_state_.total_count_ = 0;
    thumbnail_model_.resetModel({}, 0);
    emit CountsChanged();
  }

  auto proj = project_handler_.project();
  if (!proj) {
    return false;
  }

  auto browse = proj->GetAlbumBrowseService();
  if (!browse) {
    return false;
  }

  const auto folder_id_opt = folder_ctrl_.CurrentFolderElementId();
  if (!folder_id_opt.has_value()) {
    return false;
  }

  const auto folder_id   = folder_id_opt.value();
  const auto folder_path = folder_ctrl_.CurrentFolderFsPath();
  if (reset || view_state_.total_count_ == 0) {
    view_state_.total_count_ = browse->CountFilesInFolderById(folder_id, effective_filter_where);
  }

  const size_t oldSize = view_state_.all_images_.size();
  if (oldSize >= view_state_.total_count_) {
    thumbnail_model_.setHasMore(false);
    emit CountsChanged();
    return false;
  }

  const auto page_size = search_.HasActiveSearchFilter() ? kSearchMetadataPageSize
                                                         : kAlbumMetadataPageSize;
  const auto files =
      browse->ListFilesInFolderById(folder_id, oldSize, page_size, effective_filter_where);
  for (const auto& file : files) {
    const auto file_path =
        file.file_path_.empty() ? folder_path / file.file_name_ : file.file_path_;
    AddOrUpdateAlbumItem(
        file.file_id_, file.image_id_, file.folder_id_,
        file.scope_type_ == AlbumScopeType::Root ? QStringLiteral("root") : QStringLiteral("album"),
        file.file_name_, file_path);
  }

  const size_t           newSize = view_state_.all_images_.size();
  std::vector<AlbumItem> newBatch;
  if (newSize > oldSize) {
    newBatch.reserve(newSize - oldSize);
    for (size_t i = oldSize; i < newSize; ++i) {
      newBatch.push_back(view_state_.all_images_[i]);
    }
  }

  if (oldSize == 0) {
    thumbnail_model_.resetModel(view_state_.all_images_, view_state_.total_count_);
  } else if (!newBatch.empty()) {
    thumbnail_model_.appendPage(newBatch);
  } else {
    thumbnail_model_.setHasMore(thumbnail_model_.items().size() < view_state_.total_count_);
  }

  emit CountsChanged();
  return !files.empty();
}

auto AlbumBackend::EffectiveFilterWhere(const std::optional<std::wstring>& filterWhere) const
    -> std::optional<std::wstring> {
  const auto& active_search_filter_where = search_.ActiveSearchFilterWhere();
  if (!active_search_filter_where.has_value() || active_search_filter_where->empty()) {
    return filterWhere;
  }
  if (!filterWhere.has_value() || filterWhere->empty()) {
    return active_search_filter_where;
  }
  return L"(" + *filterWhere + L") AND (" + *active_search_filter_where + L")";
}

void AlbumBackend::AddOrUpdateAlbumItem(sl_element_id_t elementId, image_id_t imageId,
                                        sl_element_id_t folderId, const QString& scopeType,
                                        const file_name_t&           fallbackName,
                                        const std::filesystem::path& filePath) {
  AlbumItem* item = FindAlbumItem(elementId);

  if (!item) {
    AlbumItem next;
    next.element_id = elementId;
    next.file_id    = elementId;
    next.image_id   = imageId;
    next.folder_id  = folderId;
    next.scope_type = scopeType;
    next.file_path_ = filePath;
    next.file_name  = WStringToQString(fallbackName);
    next.extension  = ExtensionFromFileName(next.file_name);
    next.accent     = AccentForIndex(view_state_.all_images_.size());

    view_state_.all_images_.push_back(std::move(next));
    item = &view_state_.all_images_.back();
  }

  if (!item) return;

  item->element_id = elementId;
  item->file_id    = elementId;
  item->image_id   = imageId;
  item->folder_id  = folderId;
  item->scope_type = scopeType;
  item->file_path_ = filePath;

  auto proj        = project_handler_.project();
  if (proj) {
    try {
      proj->GetImagePoolService()->Read<void>(imageId, [item](std::shared_ptr<Image> image) {
        if (!image) return;
        if (!image->image_name_.empty()) {
          item->file_name = WStringToQString(image->image_name_);
        }
        if (!image->image_path_.empty()) {
          item->extension = ExtensionUpper(image->image_path_);
        }

        const auto& exif        = image->exif_display_;
        item->camera_model      = QString::fromUtf8(exif.model_.c_str());
        item->lens              = QString::fromUtf8(exif.lens_.c_str());
        item->iso               = static_cast<int>(exif.iso_);
        item->aperture          = static_cast<double>(exif.aperture_);
        item->focal_length      = static_cast<double>(exif.focal_);
        item->rating            = exif.rating_;
        item->is_hdr            = exif.is_hdr_;
        const QDate captureDate = DateFromExifString(exif.date_time_str_);
        if (captureDate.isValid()) {
          item->capture_date = captureDate;
        }
      });
    } catch (...) {
    }
  }

  if (!item->import_date.isValid()) {
    item->import_date = QDate::currentDate();
  }
  if (item->extension.isEmpty()) {
    item->extension = ExtensionFromFileName(item->file_name);
  }
  item->tags = SemanticLabelDisplayText(elementId);
}

void AlbumBackend::SetAlbumItemHdrFlag(sl_element_id_t elementId, image_id_t imageId, bool isHdr) {
  if (auto* item = FindAlbumItem(elementId);
      item != nullptr && (imageId == 0 || item->image_id == imageId)) {
    item->is_hdr = isHdr;
  }
  thumbnail_model_.updateHdrFlag(elementId, imageId, isHdr);
}

void AlbumBackend::PersistImageHdrFlag(sl_element_id_t elementId, image_id_t imageId, bool isHdr) {
  auto proj = project_handler_.project();
  if (!proj || imageId == 0) {
    return;
  }

  try {
    proj->GetImagePoolService()->Write_NoSync<void>(
        imageId, [isHdr](const std::shared_ptr<Image>& image) {
          if (image) {
            image->SetHdrDisplayMetadata(isHdr);
          }
        });
    SetAlbumItemHdrFlag(elementId, imageId, isHdr);
  } catch (...) {
  }
}

auto AlbumBackend::FindAlbumItem(sl_element_id_t elementId) -> AlbumItem* {
  for (auto& item : view_state_.all_images_) {
    if (item.element_id == elementId) {
      return &item;
    }
  }
  return nullptr;
}

auto AlbumBackend::FindAlbumItem(sl_element_id_t elementId) const -> const AlbumItem* {
  for (const auto& item : view_state_.all_images_) {
    if (item.element_id == elementId) {
      return &item;
    }
  }

  const auto& visible_items = thumbnail_model_.items();
  for (const auto& item : visible_items) {
    if (item.element_id == elementId) {
      return &item;
    }
  }
  return nullptr;
}

// ── Phase 4: Thumbnail disk cache settings ─────────────────────────────────

void AlbumBackend::LoadThumbnailDiskCacheSettings() {
  QSettings settings;
  thumbnail_disk_cache_enabled_ =
      settings.value(QStringLiteral("thumbnailCache/enabled"), true).toBool();
  thumbnail_disk_cache_root_ =
      settings.value(QStringLiteral("thumbnailCache/rootPath"), QString{}).toString();
  thumbnail_disk_cache_max_entries_ =
      settings.value(QStringLiteral("thumbnailCache/maxEntries"), 10000).toInt();
  thumbnail_disk_cache_jpeg_quality_ =
      settings.value(QStringLiteral("thumbnailCache/jpegQuality"), 85).toInt();
}

void AlbumBackend::SaveThumbnailDiskCacheSettings() {
  QSettings settings;
  settings.setValue(QStringLiteral("thumbnailCache/enabled"), thumbnail_disk_cache_enabled_);
  settings.setValue(QStringLiteral("thumbnailCache/rootPath"), thumbnail_disk_cache_root_);
  settings.setValue(QStringLiteral("thumbnailCache/maxEntries"), thumbnail_disk_cache_max_entries_);
  settings.setValue(QStringLiteral("thumbnailCache/jpegQuality"), thumbnail_disk_cache_jpeg_quality_);
}

void AlbumBackend::ApplyThumbnailDiskCacheSettingsToService() {
  auto thumb_svc = project_handler_.thumbnail_service();
  if (!thumb_svc) return;

  thumb_svc->SetDiskCacheEnabled(thumbnail_disk_cache_enabled_);
  if (!thumbnail_disk_cache_root_.isEmpty()) {
    thumb_svc->SetDiskCacheRoot(
        std::filesystem::path(thumbnail_disk_cache_root_.toStdWString()));
  }
  thumb_svc->SetDiskCacheMaxEntries(static_cast<size_t>(thumbnail_disk_cache_max_entries_));
  thumb_svc->SetDiskCacheJpegQuality(thumbnail_disk_cache_jpeg_quality_);
}

bool AlbumBackend::ThumbnailDiskCacheEnabled() const {
  return thumbnail_disk_cache_enabled_;
}

QString AlbumBackend::ThumbnailDiskCacheRoot() const {
  auto thumb_svc = project_handler_.thumbnail_service();
  if (thumb_svc) {
    const auto root = thumb_svc->GetDiskCacheRoot();
    if (!root.empty()) {
      return QString::fromStdWString(root.wstring());
    }
  }
  return thumbnail_disk_cache_root_;
}

int AlbumBackend::ThumbnailDiskCacheMaxEntries() const {
  auto thumb_svc = project_handler_.thumbnail_service();
  if (thumb_svc) {
    return static_cast<int>(thumb_svc->GetDiskCacheMaxEntries());
  }
  return thumbnail_disk_cache_max_entries_;
}

int AlbumBackend::ThumbnailDiskCacheJpegQuality() const {
  auto thumb_svc = project_handler_.thumbnail_service();
  if (thumb_svc) {
    return thumb_svc->GetDiskCacheJpegQuality();
  }
  return thumbnail_disk_cache_jpeg_quality_;
}

QString AlbumBackend::ThumbnailDiskCacheStats() const {
  auto thumb_svc = project_handler_.thumbnail_service();
  if (!thumb_svc) {
    return QStringLiteral("No thumbnail service.");
  }
  const auto stats = thumb_svc->GetDiskCacheStats();
  return QStringLiteral(
             "Enabled: %1\n"
             "Entries: %2\n"
             "Size: %3\n"
             "Max entries: %4\n"
             "Hits: %5 / Misses: %6\n"
             "Root: %7")
      .arg(stats.enabled ? QStringLiteral("Yes") : QStringLiteral("No"))
      .arg(stats.total_entries)
      .arg(FormatCacheSize(stats.total_size_bytes))
      .arg(stats.max_entries)
      .arg(stats.hit_count)
      .arg(stats.miss_count)
      .arg(QString::fromStdString(stats.cache_root_path));
}

void AlbumBackend::SetThumbnailDiskCacheEnabled(bool enabled) {
  thumbnail_disk_cache_enabled_ = enabled;
  SaveThumbnailDiskCacheSettings();
  auto thumb_svc = project_handler_.thumbnail_service();
  if (thumb_svc) {
    thumb_svc->SetDiskCacheEnabled(enabled);
  }
  emit ThumbnailDiskCacheStateChanged();
}

void AlbumBackend::SetThumbnailDiskCacheRoot(const QString& rootPath) {
  const auto root_path_opt = InputToPath(rootPath);
  const QString normalized_root =
      root_path_opt.has_value() ? PathToQString(root_path_opt.value().lexically_normal()) : rootPath;
  thumbnail_disk_cache_root_ = normalized_root;
  SaveThumbnailDiskCacheSettings();
  auto thumb_svc = project_handler_.thumbnail_service();
  if (thumb_svc && !thumbnail_disk_cache_root_.isEmpty()) {
    thumb_svc->SetDiskCacheRoot(std::filesystem::path(thumbnail_disk_cache_root_.toStdWString()));
  }
  emit ThumbnailDiskCacheStateChanged();
}

void AlbumBackend::SetThumbnailDiskCacheMaxEntries(int maxEntries) {
  thumbnail_disk_cache_max_entries_ = std::max(1, maxEntries);
  SaveThumbnailDiskCacheSettings();
  auto thumb_svc = project_handler_.thumbnail_service();
  if (thumb_svc) {
    thumb_svc->SetDiskCacheMaxEntries(static_cast<size_t>(thumbnail_disk_cache_max_entries_));
  }
  emit ThumbnailDiskCacheStateChanged();
}

void AlbumBackend::SetThumbnailDiskCacheJpegQuality(int quality) {
  thumbnail_disk_cache_jpeg_quality_ = std::clamp(quality, 1, 100);
  SaveThumbnailDiskCacheSettings();
  auto thumb_svc = project_handler_.thumbnail_service();
  if (thumb_svc) {
    thumb_svc->SetDiskCacheJpegQuality(thumbnail_disk_cache_jpeg_quality_);
  }
  emit ThumbnailDiskCacheStateChanged();
}

void AlbumBackend::ClearAllThumbnailDiskCache() {
  auto thumb_svc = project_handler_.thumbnail_service();
  if (thumb_svc) {
    thumb_svc->ClearAllDiskCache();
  }
  emit ThumbnailDiskCacheStateChanged();
  SetServiceMessageForCurrentProject(PL_TEXT("All thumbnail disk cache cleared."));
}

void AlbumBackend::ClearProjectThumbnailDiskCache() {
  auto thumb_svc = project_handler_.thumbnail_service();
  if (thumb_svc) {
    thumb_svc->ClearProjectDiskCache();
  }
  emit ThumbnailDiskCacheStateChanged();
  SetServiceMessageForCurrentProject(PL_TEXT("Current project thumbnail disk cache cleared."));
}

int AlbumBackend::PromptForInt(const QString& title, const QString& label,
                               int defaultValue, int minValue, int maxValue) {
  bool accepted = false;
  int  value    = QInputDialog::getInt(nullptr, title, label, defaultValue, minValue, maxValue, 1,
                                      &accepted);
  return accepted ? value : defaultValue;
}

}  // namespace alcedo::ui

#undef PL_TEXT
