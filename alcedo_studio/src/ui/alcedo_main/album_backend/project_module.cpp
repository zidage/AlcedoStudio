//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/project_module.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <thread>

#include "app/project_package_service.hpp"
#ifdef HAVE_OPENCL
#include "opencl/opencl_runtime.hpp"
#endif
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "utils/cuda/cuda_driver_requirements.hpp"

namespace alcedo::ui {

using namespace album_util;
using namespace std::chrono_literals;
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

auto             FormatCacheSize(size_t bytes) -> QString {
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
    QT_TRANSLATE_NOOP("Alcedo", "The accelerator change will apply after Alcedo restarts."),
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



ProjectModule::ProjectModule(QObject* parent)
    : QObject(parent), handler_(*this) {
  LoadRecentProjectsFromSettings();
  QObject::connect(&i18n::TranslationNotifier::Instance(),
                   &i18n::TranslationNotifier::LanguageChanged, this,
                   &ProjectModule::RefreshTranslations);
  InitializeAcceleratorSettings();
  SetServiceState(false, PL_TEXT("Select a project: load a .alcd package or create a new "
                                 "packed project."));
  task_status_text_ = PL_TEXT("Open or create a project to begin.");
}

void ProjectModule::SetLifecycleHooks(ProjectLifecycleHooks hooks) {
  lifecycle_hooks_ = std::move(hooks);
}

void ProjectModule::SetRuntimeAcceleratorPreference(
    const AcceleratorBackendPreference preference) {
  // The RHI backend selected by main.cpp is the process-wide source of truth.
  // QSettings is only the next-launch request and may differ when a command-line
  // override is active or when settings changed during the previous process.
  accelerator_preference_  = preference;
  accelerator_backend_key_ = AcceleratorPreferenceKey(preference);
  // OpenCL kernel registration/compile used to run when the user selected the
  // backend (pre PR #72). Backend is now locked before QApplication; kick the
  // same warm-up here so programs are registered and compiled even if QML never
  // reaches ProjectLaunchController.start(). The QML timer remains a no-op once
  // accelerator_prepare_started_ is set.
  StartOpenClPreparationIfNeeded();
}

void ProjectModule::SetServiceMessage(const i18n::LocalizedText& message) {
  SetServiceMessageForCurrentProject(message);
}

void ProjectModule::NotifyProjectLoadStateChanged() { emit ProjectLoadStateChanged(); }

void ProjectModule::SetAcceleratorPreparationState(bool                       preparing,
                                                  const i18n::LocalizedText& status) {
  accelerator_preparing_               = preparing;
  accelerator_preparation_status_text_ = status;
  emit AcceleratorPreparationStateChanged();
}


void ProjectModule::StartOpenClPreparationIfNeeded() {
  if (accelerator_prepare_started_ || accelerator_preparing_ ||
      accelerator_preference_ != AcceleratorBackendPreference::OpenCL) {
    return;
  }

  accelerator_prepare_started_ = true;

#ifdef HAVE_OPENCL
  SetAcceleratorPreparationState(true,
                                 PL_TEXT("Compiling OpenCL kernels. This happens every launch."));
  QPointer<ProjectModule> self(this);
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


void ProjectModule::StartAcceleratorPreparation() { StartOpenClPreparationIfNeeded(); }


void ProjectModule::InitializeAcceleratorSettings() {
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


void ProjectModule::RebuildAcceleratorOptions() {
  QVariantList options;
#if defined(__APPLE__)
  if (metal_backend_available_) {
    options.push_back(AcceleratorOption(AcceleratorBackendPreference::Metal));
  }
#else
  // OpenCL is the Windows baseline (the only viable backend on non-NVIDIA
  // machines); it is the first option and the first-launch default. CUDA is
  // offered only when the runtime actually supports it, and only as a
  // user-selected alternative.
  if (opencl_backend_available_) {
    options.push_back(AcceleratorOption(AcceleratorBackendPreference::OpenCL));
  }
  if (cuda_backend_available_) {
    options.push_back(AcceleratorOption(AcceleratorBackendPreference::CUDA));
  }
#endif
  if (options.isEmpty()) {
    options.push_back(AcceleratorOption(AcceleratorBackendPreference::CPU));
  }
  accelerator_options_ = options;
}


bool ProjectModule::SetAcceleratorBackend(const QString& backendKey) {
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

  if (normalized_key == accelerator_backend_key_) {
    return true;
  }

  // The graphics API and frame presentation backend were selected before the
  // first QQuickWindow. Keep the active pipeline unchanged and persist only the
  // next-launch preference; main.cpp consumes it on the next process start.
  accelerator_backend_key_ = normalized_key;
  QSettings settings;
  settings.setValue(QLatin1String(kAcceleratorBackendKey), accelerator_backend_key_);
  settings.sync();
  emit AcceleratorStateChanged();
  SetServiceMessageForCurrentProject(
      PL_TEXT("The accelerator change will apply after Alcedo restarts."));
  return true;
}


void ProjectModule::AcknowledgeAcceleratorWarning() {
  if (accelerator_warning_id_.isEmpty()) {
    return;
  }
  PersistAcceleratorWarningAcknowledgement();
  emit AcceleratorStateChanged();
}


bool ProjectModule::IsAcceleratorWarningAcknowledged() const {
  return !accelerator_warning_id_.isEmpty() &&
         QSettings{}.value(QLatin1String(kAcceleratorWarningAcknowledgedKey)).toString() ==
             accelerator_warning_id_;
}


void ProjectModule::PersistAcceleratorWarningAcknowledgement() const {
  if (!accelerator_warning_id_.isEmpty()) {
    QSettings{}.setValue(QLatin1String(kAcceleratorWarningAcknowledgedKey),
                         accelerator_warning_id_);
  }
}


bool ProjectModule::PromptAndLoadProject() {
  const QString start_dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString selected_path =
      QFileDialog::getOpenFileName(nullptr, tr("Select Project Package"), start_dir,
                                   tr("Packed Project (*.alcd);;All Files (*)"));
  if (selected_path.isEmpty()) {
    return false;
  }
  return LoadProject(QUrl::fromLocalFile(selected_path).toString());
}


bool ProjectModule::PromptAndCreateProject() {
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


bool ProjectModule::LoadProject(const QString& metaFileUrlOrPath) {
  if (handler_.project_loading()) {
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

    return handler_.InitializeServices(unpacked_db_path, unpacked_meta_path,
                                               ProjectOpenMode::kLoadExisting, project_path,
                                               workspace_dir, project_path);
  }

  SetServiceMessageForCurrentProject(PL_TEXT("Unsupported project format. Choose a .alcd file."));
  return false;
}


bool ProjectModule::CreateProjectInFolder(const QString& folderUrlOrPath) {
  return CreateProjectInFolderNamed(folderUrlOrPath, "album_editor_project");
}


bool ProjectModule::CreateProjectInFolderNamed(const QString& folderUrlOrPath,
                                              const QString& projectName) {
  if (handler_.project_loading()) {
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
  const bool started      = handler_.InitializeServices(
      runtime_pair.first, runtime_pair.second, ProjectOpenMode::kCreateNew, packed_path_opt.value(),
      workspace_dir, packed_path_opt.value());
  if (!started) {
    CleanupWorkspaceDirectory(workspace_dir);
  }
  return started;
}


bool ProjectModule::SaveProject() {
  if (handler_.project_loading()) {
    SetServiceMessageForCurrentProject(PL_TEXT("Please wait until project loading finishes."));
    return false;
  }

  if (!handler_.project() || handler_.meta_path().empty()) {
    SetServiceState(false, PL_TEXT("No project is loaded yet."));
    SetTaskState(PL_TEXT("No project to save."), 0, false);
    return false;
  }

  FinalizeEditorSession();

  // Drop DB rows for models no longer installed locally before the metadata
  // save + packaging, then refresh the badge/counts so the open app reflects it.
  if (handler_.PurgeUninstalledSemanticModels()) {
    if (lifecycle_hooks_.refresh_semantic_state) {
      lifecycle_hooks_.refresh_semantic_state();
    }
  }

  if (!handler_.PersistCurrentProjectState()) {
    SetServiceMessageForCurrentProject(PL_TEXT("Project save failed."));
    SetTaskState(PL_TEXT("Project save failed."), 0, false);
    return false;
  }

  QString package_error;
  if (!handler_.PackageCurrentProjectFiles(&package_error)) {
    SetServiceMessageForCurrentProject(package_error.isEmpty()
                                           ? PL_TEXT("Project saved, but packing failed.")
                                           : PL_TEXT("%1", package_error));
    SetTaskState(PL_TEXT("Project packing failed."), 0, false);
    return false;
  }

  SetServiceMessageForCurrentProject(
      handler_.package_path().empty()
          ? PL_TEXT("Project saved to %1", PathToQString(handler_.meta_path()))
          : PL_TEXT("Project saved and packed to %1",
                    PathToQString(handler_.package_path())));
  SetTaskState(handler_.package_path().empty() ? PL_TEXT("Project saved.")
                                                       : PL_TEXT("Project saved and packed."),
               100, false);
  ScheduleIdleTaskStateReset(1200);
  return true;
}

// ── Shared internal methods ─────────────────────────────────────────────────


void ProjectModule::SetServiceState(bool ready, const i18n::LocalizedText& message) {
  if (service_ready_ == ready && service_message_text_.source_ == message.source_ &&
      service_message_text_.args_ == message.args_) {
    return;
  }
  service_ready_        = ready;
  service_message_text_ = message;
  emit ServiceStateChanged();
}


void ProjectModule::SetServiceMessageForCurrentProject(const i18n::LocalizedText& message) {
  SetServiceState(handler_.project() != nullptr, message);
}


void ProjectModule::ScheduleIdleTaskStateReset(int delayMs) {
  QTimer::singleShot(std::max(delayMs, 0), this, [this]() {
    if ((!lifecycle_hooks_.export_inflight || !lifecycle_hooks_.export_inflight()) &&
        !task_cancel_visible_) {
      SetTaskState(PL_TEXT("No background tasks"), 0, false);
    }
  });
}


void ProjectModule::SetTaskState(const i18n::LocalizedText& status, int progress,
                                bool cancelVisible) {
  task_status_text_    = status;
  task_progress_       = std::clamp(progress, 0, 100);
  task_cancel_visible_ = cancelVisible;
  emit TaskStateChanged();
}


void ProjectModule::RefreshTranslations() {
  if (lifecycle_hooks_.refresh_translations) {
    lifecycle_hooks_.refresh_translations();
  }
  emit ServiceStateChanged();
  RebuildAcceleratorOptions();
  emit AcceleratorStateChanged();
  emit TaskStateChanged();
  emit ProjectLoadStateChanged();
  emit RecentProjectsChanged();
}

void ProjectModule::HandleProjectOpened() {
  if (lifecycle_hooks_.project_opened) {
    lifecycle_hooks_.project_opened();
  }
}

void ProjectModule::ClearProjectUiState() {
  if (lifecycle_hooks_.clear_project_ui_state) {
    lifecycle_hooks_.clear_project_ui_state();
  }
}

auto ProjectModule::ProjectSwitchBlockReason() const -> QString {
  return lifecycle_hooks_.project_switch_block_reason
             ? lifecycle_hooks_.project_switch_block_reason()
             : QString{};
}

void ProjectModule::FinalizeEditorSession() {
  if (lifecycle_hooks_.finalize_editor_session) {
    lifecycle_hooks_.finalize_editor_session();
  }
}

bool ProjectModule::ShouldKeepSemanticModelData(const QString& profileId) const {
  return !lifecycle_hooks_.should_keep_semantic_model_data ||
         lifecycle_hooks_.should_keep_semantic_model_data(profileId);
}


void ProjectModule::LoadRecentProjectsFromSettings() {
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


void ProjectModule::PersistRecentProjects() const {
  QSettings{}.setValue(QLatin1String(kRecentProjectsKey), recent_projects_);
}


void ProjectModule::RegisterRecentProject(const std::filesystem::path& projectPath) {
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


void ProjectModule::RemoveRecentProject(const std::filesystem::path& projectPath) {
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


}  // namespace alcedo::ui

#undef PL_TEXT
