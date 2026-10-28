//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/model_download_controller.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPointer>
#include <QSettings>
#include <QUrl>
#include <QVariantMap>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "app/model_asset_catalog.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui {

#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {

auto QStringToPath(const QString& value) -> std::filesystem::path {
#ifdef _WIN32
  return std::filesystem::path(value.toStdWString());
#else
  return std::filesystem::path(value.toStdString());
#endif
}

auto PathString(const QString& value) -> std::string {
#ifdef _WIN32
  return QStringToPath(value).string();
#else
  return value.toStdString();
#endif
}

auto PathExists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

auto FirstPathElementIsParent(const std::filesystem::path& path) -> bool {
  auto it = path.begin();
  return it != path.end() && *it == "..";
}

auto ManifestAssetRelativePath(const std::filesystem::path& local_path,
                               const std::filesystem::path& model_root,
                               const std::filesystem::path& stored_model_root)
    -> std::filesystem::path {
  if (!local_path.is_absolute()) {
    return local_path;
  }
  auto relative = local_path.lexically_relative(model_root);
  if (!relative.empty() && !FirstPathElementIsParent(relative)) {
    return relative;
  }
  if (!stored_model_root.empty()) {
    relative = local_path.lexically_relative(stored_model_root);
    if (!relative.empty() && !FirstPathElementIsParent(relative)) {
      return relative;
    }
  }
  return {};
}

auto ValidateCatalogAssetPresence(const ModelAssetSpec&        asset,
                                  const std::filesystem::path& local_path)
    -> std::optional<QString> {
  std::error_code ec;
  if (!std::filesystem::exists(local_path, ec)) {
    return PL_TEXT("missing file: %1", QString::fromStdString(local_path.string())).Render();
  }
  const auto size = std::filesystem::file_size(local_path, ec);
  if (ec) {
    return PL_TEXT("failed to stat %1: %2", QString::fromStdString(local_path.string()),
                   QString::fromStdString(ec.message()))
        .Render();
  }
  if (size != asset.size_bytes) {
    return PL_TEXT("%1 size mismatch: expected %2 bytes, got %3 bytes",
                   QString::fromLatin1(asset.local_path),
                   QString::number(static_cast<qulonglong>(asset.size_bytes)),
                   QString::number(static_cast<qulonglong>(size)))
        .Render();
  }
  return std::nullopt;
}

auto ValidateLocalCatalogModelProfile(const ModelProfileSpec&      profile,
                                      const std::filesystem::path& root) -> std::optional<QString> {
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    return PL_TEXT("missing model root directory: %1", QString::fromStdString(root.string()))
        .Render();
  }

  for (const auto& asset : profile.assets) {
    const auto asset_path = root / asset.local_path;
    if (auto error = ValidateCatalogAssetPresence(asset, asset_path); error.has_value()) {
      return error;
    }
  }

  const auto manifest_path = root / kSemanticResolvedManifestFile;
  QFile      file(QString::fromStdString(manifest_path.string()));
  if (!file.open(QIODevice::ReadOnly)) {
    return PL_TEXT("Model manifest was not found at %1",
                   QString::fromStdString(manifest_path.string()))
        .Render();
  }

  QJsonParseError parse_error;
  const auto      document = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return PL_TEXT("Model manifest is invalid: %1", parse_error.errorString()).Render();
  }

  const auto object      = document.object();
  const auto read_string = [&object](const char* key) {
    return object.value(QString::fromLatin1(key)).toString().toStdString();
  };
  const auto read_u32 = [&object](const char* key) {
    return static_cast<uint32_t>(object.value(QString::fromLatin1(key)).toInt());
  };

  if (read_string("profile_id") != profile.profile_id ||
      read_string("model_id") != profile.model_id || read_string("revision") != profile.revision ||
      read_string("engine_profile_id") != profile.engine_profile_id ||
      read_string("language") != ToString(profile.language) ||
      read_u32("embedding_dimension") != profile.embedding_dimension ||
      read_u32("native_embedding_dimension") != profile.native_embedding_dimension ||
      read_u32("image_size") != profile.image_size ||
      read_string("embedding_transform") != profile.embedding_transform) {
    return PL_TEXT("Model manifest does not match the selected model.").Render();
  }

  const auto assets = object.value(QStringLiteral("assets")).toArray();
  if (assets.size() != static_cast<qsizetype>(profile.assets.size())) {
    return PL_TEXT("Model manifest lists %1 file(s), but the selected model expects %2.",
                   assets.size(), static_cast<int>(profile.assets.size()))
        .Render();
  }

  const auto stored_model_root_text = read_string("model_root");
  const auto stored_model_root =
      stored_model_root_text.empty()
          ? std::filesystem::path{}
          : QStringToPath(QString::fromStdString(stored_model_root_text));
  for (qsizetype i = 0; i < assets.size(); ++i) {
    const auto& expected    = profile.assets[static_cast<size_t>(i)];
    const auto  actual      = assets.at(i).toObject();
    const auto  actual_role = actual.value(QStringLiteral("role")).toString().toStdString();
    const auto  actual_remote_path =
        actual.value(QStringLiteral("remote_path")).toString().toStdString();
    const auto actual_local_path =
        actual.value(QStringLiteral("local_path")).toString().toStdString();
    const auto actual_size =
        static_cast<uint64_t>(actual.value(QStringLiteral("size_bytes")).toDouble());
    const auto actual_sha256 = actual.value(QStringLiteral("sha256")).toString().toStdString();

    if (actual_role != ToString(expected.role) || actual_remote_path != expected.remote_path ||
        actual_size != expected.size_bytes ||
        actual_sha256 != (expected.sha256 == nullptr ? std::string{} : expected.sha256)) {
      return PL_TEXT("Model manifest asset mismatch: expected %1, found %2.",
                     QString::fromLatin1(expected.remote_path),
                     QString::fromStdString(actual_remote_path.empty() ? actual_local_path
                                                                       : actual_remote_path))
          .Render();
    }

    const auto relative_local = ManifestAssetRelativePath(
        QStringToPath(QString::fromStdString(actual_local_path)), root, stored_model_root);
    if (relative_local.empty() || relative_local.generic_string() != expected.local_path) {
      return PL_TEXT("Model manifest file path mismatch: expected %1, found %2.",
                     QString::fromLatin1(expected.local_path),
                     QString::fromStdString(actual_local_path))
          .Render();
    }
  }

  return std::nullopt;
}

auto LoadLocalResolvedModelManifestImpl(const ModelProfileSpec* profile,
                                        const QString& base_directory, QString* error)
    -> std::optional<SemanticResolvedModelManifest> {
  if (profile == nullptr) {
    if (error) {
      *error = PL_TEXT("Unknown semantic model profile.").Render();
    }
    return std::nullopt;
  }
  const auto root = QDir(base_directory).filePath(QString::fromLatin1(profile->profile_id));
  if (const auto catalog_error = ValidateLocalCatalogModelProfile(*profile, QStringToPath(root));
      catalog_error.has_value()) {
    if (error) {
      *error = *catalog_error;
    }
    return std::nullopt;
  }
  const auto path = QDir(root).filePath(QString::fromLatin1(kSemanticResolvedManifestFile));
  QFile      file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error) {
      *error = PL_TEXT("Model manifest was not found at %1", path).Render();
    }
    return std::nullopt;
  }

  QJsonParseError parse_error;
  const auto      document = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    if (error) {
      *error = PL_TEXT("Model manifest is invalid: %1", parse_error.errorString()).Render();
    }
    return std::nullopt;
  }

  const auto object      = document.object();
  const auto read_string = [&object](const char* key) {
    return object.value(QString::fromLatin1(key)).toString().toStdString();
  };
  const auto read_u32 = [&object](const char* key) {
    return static_cast<uint32_t>(object.value(QString::fromLatin1(key)).toInt());
  };

  SemanticResolvedModelManifest manifest;
  manifest.profile_id                 = read_string("profile_id");
  manifest.model_id                   = read_string("model_id");
  manifest.revision                   = read_string("revision");
  manifest.engine_profile_id          = read_string("engine_profile_id");
  manifest.language                   = read_string("language");
  manifest.embedding_dimension        = read_u32("embedding_dimension");
  manifest.native_embedding_dimension = read_u32("native_embedding_dimension");
  manifest.image_size                 = read_u32("image_size");
  manifest.embedding_transform        = read_string("embedding_transform");
  const auto stored_model_root        = read_string("model_root");
  manifest.model_root                 = PathString(root);

  const auto assets                   = object.value(QStringLiteral("assets")).toArray();
  manifest.assets.reserve(static_cast<size_t>(assets.size()));
  for (const auto& value : assets) {
    const auto             asset_object = value.toObject();
    SemanticModelAssetInfo asset;
    const auto             read_asset_string = [&asset_object](const char* key) {
      return asset_object.value(QString::fromLatin1(key)).toString().toStdString();
    };
    asset.role        = read_asset_string("role");
    asset.repo_id     = read_asset_string("repo_id");
    asset.revision    = read_asset_string("revision");
    asset.remote_path = read_asset_string("remote_path");
    asset.local_path  = read_asset_string("local_path");
    asset.size_bytes =
        static_cast<uint64_t>(asset_object.value(QStringLiteral("size_bytes")).toDouble());
    asset.sha256 = read_asset_string("sha256");
    manifest.assets.push_back(std::move(asset));
  }

  if (manifest.profile_id != profile->profile_id || manifest.model_id != profile->model_id ||
      manifest.revision != profile->revision || manifest.engine_profile_id.empty() ||
      manifest.embedding_dimension == 0 || manifest.native_embedding_dimension == 0 ||
      manifest.image_size == 0 || manifest.embedding_transform.empty()) {
    if (error) {
      *error = PL_TEXT("Model manifest does not match the selected model.").Render();
    }
    return std::nullopt;
  }
  if (manifest.assets.empty()) {
    if (error) {
      *error = PL_TEXT("Model manifest does not list any model files.").Render();
    }
    return std::nullopt;
  }

  const auto current_root = QStringToPath(root);
  const auto old_root     = stored_model_root.empty()
                                ? std::filesystem::path{}
                                : QStringToPath(QString::fromStdString(stored_model_root));
  for (const auto& asset : manifest.assets) {
    if (asset.local_path.empty()) {
      if (error) {
        *error = PL_TEXT("Model manifest contains an asset without a local path.").Render();
      }
      return std::nullopt;
    }

    const auto local_path = QStringToPath(QString::fromStdString(asset.local_path));
    bool       exists     = PathExists(local_path);
    if (!exists && local_path.is_absolute() && !old_root.empty()) {
      const auto relative = local_path.lexically_relative(old_root);
      if (!relative.empty() && !FirstPathElementIsParent(relative)) {
        exists = PathExists(current_root / relative);
      }
    }
    if (!exists && !local_path.is_absolute()) {
      exists = PathExists(current_root / local_path);
    }
    if (!exists) {
      if (error) {
        *error = PL_TEXT("Model file is missing: %1",
                         QString::fromStdString(asset.local_path.empty() ? asset.remote_path
                                                                         : asset.local_path))
                     .Render();
      }
      return std::nullopt;
    }
  }

  return manifest;
}

auto DownloadProgressPercent(const alcedo::ModelDownloadProgress& progress) -> int {
  if (progress.bytes_total == 0) {
    return progress.phase == "installed" ? 100 : 0;
  }
  return static_cast<int>(
      std::min<uint64_t>(100, (progress.bytes_downloaded * 100) / progress.bytes_total));
}

// "280 MB / 1.54 GB" — pre-formatted in C++ so the QML binding reads a plain
// string property instead of calling a Q_INVOKABLE from inside a binding (which
// is fragile w.r.t. JS-number -> quint64 argument conversion).
auto FormatBytesLabel(quint64 done, quint64 total) -> QString {
  const QLocale locale;
  if (total == 0) {
    return {};
  }
  return locale.formattedDataSize(static_cast<qint64>(done), 1, QLocale::DataSizeBase1000)
         + QStringLiteral(" / ")
         + locale.formattedDataSize(static_cast<qint64>(total), 1, QLocale::DataSizeBase1000);
}

// "2.3 MB/s" — formatted off an exponentially-smoothed byte rate so the number
// doesn't jitter on every 250 ms aria2 poll.
auto FormatSpeedLabel(double bytes_per_sec) -> QString {
  if (!(bytes_per_sec > 0) || !std::isfinite(bytes_per_sec)) {
    return {};
  }
  const QLocale locale;
  return locale.formattedDataSize(static_cast<qint64>(std::round(bytes_per_sec)), 1,
                                  QLocale::DataSizeBase1000)
         + QStringLiteral("/s");
}

// "~2m 30s left" from a remaining-seconds estimate.
auto FormatEtaLabel(double seconds) -> QString {
  if (!(seconds > 0) || !std::isfinite(seconds) || seconds > 24 * 3600) {
    return {};
  }
  const int total = static_cast<int>(std::round(seconds));
  if (total <= 0) {
    return {};
  }
  const int h = total / 3600;
  const int m = (total % 3600) / 60;
  const int s = total % 60;
  if (h > 0) {
    return QStringLiteral("~%1h %2m left").arg(h).arg(m);
  }
  if (m > 0) {
    return QStringLiteral("~%1m %2s left").arg(m).arg(s);
  }
  return QStringLiteral("~%1s left").arg(s);
}

}  // namespace

namespace detail {

auto LoadLocalResolvedModelManifestForActivation(const QString& profileId,
                                                 const QString& baseDirectory, QString* error)
    -> std::optional<SemanticResolvedModelManifest> {
  return LoadLocalResolvedModelManifestImpl(FindSemanticProfile(profileId.toStdString()),
                                            baseDirectory, error);
}

}  // namespace detail

ModelDownloadController::ModelDownloadController(alcedo::ModelDownloadService& service,
                                                 BackgroundTaskController*     background_tasks,
                                                 QObject*                      parent)
    : QObject(parent), service_(service), background_tasks_(background_tasks) {
  connect(&service_, &alcedo::ModelDownloadService::ProgressChanged, this,
          [this](const alcedo::ModelDownloadProgress& progress) {
            if (downloading_profile_id_.isEmpty()) {
              return;
            }
            model_download_progress_     = DownloadProgressPercent(progress);
            model_download_phase_        = QString::fromStdString(progress.phase);
            model_download_current_file_ = QString::fromStdString(progress.current_file);
            model_download_bytes_done_   = progress.bytes_downloaded;
            model_download_bytes_total_  = progress.bytes_total;
            model_download_bytes_label_ =
                FormatBytesLabel(static_cast<quint64>(progress.bytes_downloaded),
                                 static_cast<quint64>(progress.bytes_total));
            model_download_files_done_  = static_cast<int>(progress.files_completed);
            model_download_files_total_ = static_cast<int>(progress.files_total);
            UpdateDownloadSpeed(progress);
            if (download_canceling_) {
              emit StateChanged();
              return;
            }
            const QString message = QString::fromStdString(progress.message);
            const auto    text    = message.isEmpty()
                                        ? PL_TEXT("Downloading model... %1%", model_download_progress_)
                                        : PL_TEXT("%1 (%2%)", message, model_download_progress_);
            SetRowMessage(downloading_profile_id_, text, false);
            emit StateChanged();
            if (!background_task_id_.isEmpty()) {
              background_tasks_->UpdateTask(background_task_id_, text.Render(),
                                            model_download_bytes_label_, model_download_progress_);
            }
          });
  connect(&service_, &alcedo::ModelDownloadService::Finished, this,
          [this](bool ok, const QString& error) {
            const QString profile_id = downloading_profile_id_;
            if (profile_id.isEmpty()) {
              return;
            }
            const bool was_canceled = !ok && download_canceling_;
            downloading_profile_id_.clear();
            download_canceling_ = false;
            ResetDownloadProgress();
            installed_.insert(profile_id, IsModelInstalled(profile_id));
            if (ok) {
              SetRowMessage(profile_id, PL_TEXT("Model download complete."), false);
            } else if (was_canceled) {
              SetRowMessage(profile_id, PL_TEXT("Model download canceled."), false);
            } else {
              SetRowMessage(profile_id,
                            error.isEmpty() ? PL_TEXT("Model download failed.")
                                            : PL_TEXT("Model download failed: %1", error),
                            true);
            }
            if (!background_task_id_.isEmpty()) {
              const BackgroundTaskState final_state =
                  was_canceled ? BackgroundTaskState::Canceled
                               : (ok ? BackgroundTaskState::Succeeded
                                     : BackgroundTaskState::Failed);
              background_tasks_->FinishTask(background_task_id_, final_state,
                                            messages_.value(profile_id).text.Render());
              background_task_id_.clear();
            }
            emit StateChanged();
          });
  RefreshModels();
}

QVariantList ModelDownloadController::Models() const {
  QVariantList  rows;
  const QLocale locale;
  for (const auto& profile : service_.Profiles()) {
    const QString profile_id = QString::fromLatin1(profile.profile_id);
    const bool    busy       = profile_id == downloading_profile_id_;
    const bool    installed  = installed_.value(profile_id, false);
    const quint64 size_bytes = ProfileTotalBytes(profile);
    QVariantMap   row;
    row.insert(QStringLiteral("profileId"), profile_id);
    row.insert(QStringLiteral("displayName"), QString::fromLatin1(profile.display_name));
    row.insert(QStringLiteral("modelId"), QString::fromLatin1(profile.model_id));
    row.insert(QStringLiteral("language"), QString::fromLatin1(ToString(profile.language)));
    row.insert(QStringLiteral("inferenceBackend"),
               QString::fromLatin1(ToString(profile.inference_backend)));
    row.insert(QStringLiteral("imageSize"), static_cast<int>(profile.image_size));
    row.insert(QStringLiteral("sizeBytes"), static_cast<double>(size_bytes));
    row.insert(
        QStringLiteral("sizeLabel"),
        locale.formattedDataSize(static_cast<qint64>(size_bytes), 1, QLocale::DataSizeBase1000));
    row.insert(QStringLiteral("licenseName"), QString::fromLatin1(profile.license_name));
    row.insert(QStringLiteral("licenseUrl"), QString::fromLatin1(profile.license_url));
    row.insert(QStringLiteral("installed"), installed);
    QString status = installed ? QStringLiteral("installed") : QStringLiteral("notInstalled");
    if (busy) {
      status = download_canceling_ ? QStringLiteral("canceling") : QStringLiteral("downloading");
    }
    row.insert(QStringLiteral("status"), status);
    row.insert(QStringLiteral("busy"), busy);
    row.insert(QStringLiteral("progress"), busy ? model_download_progress_ : (installed ? 100 : 0));
    row.insert(QStringLiteral("phase"), busy ? model_download_phase_ : QString{});
    row.insert(QStringLiteral("currentFile"), busy ? model_download_current_file_ : QString{});
    row.insert(QStringLiteral("bytesLabel"), busy ? model_download_bytes_label_ : QString{});
    row.insert(QStringLiteral("speedLabel"), busy ? model_download_speed_label_ : QString{});
    row.insert(QStringLiteral("etaLabel"), busy ? model_download_eta_label_ : QString{});
    row.insert(QStringLiteral("filesDone"), busy ? model_download_files_done_ : 0);
    row.insert(QStringLiteral("filesTotal"),
               busy ? model_download_files_total_ : static_cast<int>(profile.assets.size()));
    const auto    message = messages_.value(profile_id);
    const QString text    = message.text.IsEmpty() ? QString{} : message.text.Render();
    row.insert(QStringLiteral("message"), message.is_error ? QString{} : text);
    row.insert(QStringLiteral("error"), message.is_error ? text : QString{});
    rows.push_back(row);
  }
  return rows;
}

void ModelDownloadController::RefreshModels() {
  for (const auto& profile : service_.Profiles()) {
    const QString profile_id = QString::fromLatin1(profile.profile_id);
    if (profile_id != downloading_profile_id_) {
      installed_.insert(profile_id, IsModelInstalled(profile_id));
    }
  }
  emit StateChanged();
}

auto ModelDownloadController::FindProfile(const QString& profileId) const
    -> const ModelProfileSpec* {
  return service_.FindProfile(profileId.trimmed().toStdString());
}

bool ModelDownloadController::IsModelInstalled(const QString& profileId) const {
  const auto* profile = FindProfile(profileId);
  return profile != nullptr &&
         !ValidateLocalCatalogModelProfile(
              *profile, ModelRootForProfile(QString::fromLatin1(profile->profile_id)))
              .has_value();
}

auto ModelDownloadController::LoadResolvedManifest(const QString& profileId, QString* error) const
    -> std::optional<SemanticResolvedModelManifest> {
  return LoadLocalResolvedModelManifestImpl(FindProfile(profileId), ModelDownloadDirectory(),
                                            error);
}

void ModelDownloadController::SetRowMessage(const QString&             profileId,
                                            const i18n::LocalizedText& text, bool is_error) {
  messages_.insert(profileId, RowMessage{text, is_error});
}

void ModelDownloadController::SetModelMessage(const QString&             profileId,
                                              const i18n::LocalizedText& text, bool is_error) {
  const auto* profile = FindProfile(profileId);
  if (profile == nullptr) {
    return;
  }
  SetRowMessage(QString::fromLatin1(profile->profile_id), text, is_error);
  emit StateChanged();
}

void ModelDownloadController::ResetDownloadProgress() {
  model_download_progress_ = 0;
  model_download_phase_.clear();
  model_download_current_file_.clear();
  model_download_bytes_done_  = 0;
  model_download_bytes_total_ = 0;
  model_download_bytes_label_.clear();
  model_download_speed_label_.clear();
  model_download_eta_label_.clear();
  model_download_files_done_  = 0;
  model_download_files_total_ = 0;
  download_speed_ema_         = 0.0;
  download_speed_primed_      = false;
}

void ModelDownloadController::UpdateDownloadSpeed(const alcedo::ModelDownloadProgress& progress) {
  // Only the "downloading" phase moves bytes across the wire; reused/validated
  // ticks add whole staged files instantly and would otherwise report absurd
  // multi-GB/s spikes. While not downloading we drop the labels (the EMA is
  // kept so it resumes smoothly) and force a re-prime on the next real tick.
  if (progress.phase != "downloading") {
    model_download_speed_label_.clear();
    model_download_eta_label_.clear();
    download_speed_primed_ = false;
    return;
  }

  const auto now       = std::chrono::steady_clock::now();
  const auto bytes_now = static_cast<quint64>(progress.bytes_downloaded);

  if (!download_speed_primed_) {
    download_sample_bytes_ = bytes_now;
    download_sample_time_  = now;
    download_speed_primed_ = true;
  } else {
    const double dt = std::chrono::duration<double>(now - download_sample_time_).count();
    if (dt > 0.05) {
      const double instant = static_cast<double>(bytes_now - download_sample_bytes_) / dt;
      // A zero delta (mirror stall) is skipped rather than folded into the
      // average, so a pause shows the last good rate instead of decaying to 0
      // and flickering the label. The baseline still advances so the resume
      // doesn't produce a one-shot spike.
      if (instant > 0) {
        constexpr double kAlpha = 0.3;
        download_speed_ema_     = download_speed_ema_ == 0.0
                                      ? instant
                                      : (kAlpha * instant + (1.0 - kAlpha) * download_speed_ema_);
      }
      download_sample_bytes_ = bytes_now;
      download_sample_time_  = now;
    }
  }

  if (download_speed_ema_ > 0 && progress.bytes_total > 0) {
    model_download_speed_label_ = FormatSpeedLabel(download_speed_ema_);
    const auto   done           = std::min<quint64>(bytes_now, progress.bytes_total);
    const double remaining      = static_cast<double>(progress.bytes_total - done);
    model_download_eta_label_   = FormatEtaLabel(remaining / download_speed_ema_);
  } else {
    model_download_speed_label_.clear();
    model_download_eta_label_.clear();
  }
}

QString ModelDownloadController::ModelDownloadDirectory() const {
  return ConfiguredSemanticModelDirectory();
}

QString ModelDownloadController::ModelEndpointPreset() const {
  return ConfiguredModelEndpointPreset();
}

QString ModelDownloadController::CustomModelEndpoint() const {
  return ConfiguredCustomModelEndpoint();
}

QString ModelDownloadController::EffectiveModelEndpoint() const {
  return EffectiveSemanticModelEndpoint();
}

auto ModelDownloadController::ModelRootForProfile(const QString& profileId) const
    -> std::filesystem::path {
  return QStringToPath(QDir(ModelDownloadDirectory()).filePath(profileId.trimmed()));
}

void ModelDownloadController::SetModelDownloadDirectory(const QString& directory) {
  QString value = directory.trimmed();
  if (value.startsWith(QLatin1String("file:"))) {
    value = QUrl(value).toLocalFile();
  }
  QSettings{}.setValue(QLatin1String(kSemanticModelDirectoryKey),
                       value.isEmpty() ? DefaultSemanticModelDirectory() : value);
  messages_.clear();
  RefreshModels();
}

void ModelDownloadController::SetModelEndpointPreset(const QString& preset) {
  QSettings{}.setValue(QLatin1String(kSemanticModelEndpointPresetKey),
                       NormalizedModelEndpointPreset(preset));
  emit StateChanged();
}

void ModelDownloadController::SetCustomModelEndpoint(const QString& endpoint) {
  QSettings{}.setValue(QLatin1String(kSemanticCustomModelEndpointKey), endpoint.trimmed());
  emit StateChanged();
}

void ModelDownloadController::ResetModelDownloadDirectory() {
  QSettings{}.remove(QLatin1String(kSemanticModelDirectoryKey));
  messages_.clear();
  RefreshModels();
}

bool ModelDownloadController::ShouldKeepSemanticModelData(const QString& profileId) const {
  if (FindProfile(profileId) == nullptr) {
    // Unknown to the catalog — install state is indeterminate, so keep the rows
    // rather than risk irreversible label loss if the profile is ever re-added.
    return true;
  }
  return IsModelInstalled(profileId);
}

auto ModelDownloadController::RegisterBackgroundTask(const QString& profileId) -> QString {
  if (background_tasks_ == nullptr) {
    return {};
  }
  BackgroundTaskSnapshot snapshot;
  snapshot.kind_             = BackgroundTaskKind::ModelDownload;
  snapshot.state_            = BackgroundTaskState::Running;
  snapshot.title_            = messages_.value(profileId).text.Render();
  snapshot.progress_percent_ = 0;
  snapshot.cancelable_       = true;
  snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::CancelAndWait;
  snapshot.affected_targets_ = QVariantList{profileId};
  // The download blocks changes of the download settings (directory, source, removal) and the
  // activation of a model while its files change. Both are global locks.
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::ChangeModelDownloadSettings, 0,
      PL_TEXT("A model download is running; wait for it to finish or cancel.").Render()});
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::ChangeSemanticModel, 0,
      PL_TEXT("A model download is running; change the model after it finishes.").Render()});
  QPointer<ModelDownloadController> self(this);
  return background_tasks_->RegisterTask(snapshot, [self, profileId]() {
    if (self) {
      self->CancelInstall(profileId);
    }
  });
}

bool ModelDownloadController::InstallModel(const QString& profileId) {
  const auto* profile = FindProfile(profileId);
  if (profile == nullptr) {
    return false;
  }
  const QString profile_id = QString::fromLatin1(profile->profile_id);
  // One download at a time (plan P9). The shared DownloadService also refuses a second job of
  // another owner (application update, AI Mask package).
  if (!downloading_profile_id_.isEmpty() || service_.IsRunning() ||
      !service_.StartDownload(profile->profile_id, QStringToPath(ModelDownloadDirectory()),
                              EffectiveModelEndpoint().toStdString())) {
    SetRowMessage(profile_id, PL_TEXT("Another download is running."), true);
    emit StateChanged();
    return false;
  }

  // The totals come from the catalog so the card shows the full size before the first
  // progress tick.
  ResetDownloadProgress();
  downloading_profile_id_     = profile_id;
  download_canceling_         = false;
  model_download_phase_       = QStringLiteral("preparing");
  model_download_bytes_total_ = ProfileTotalBytes(*profile);
  model_download_bytes_label_ = FormatBytesLabel(0, model_download_bytes_total_);
  model_download_files_total_ = static_cast<int>(profile->assets.size());
  installed_.insert(profile_id, false);
  SetRowMessage(profile_id, PL_TEXT("Model download queued from %1", EffectiveModelEndpoint()),
                false);
  emit StateChanged();
  background_task_id_ = RegisterBackgroundTask(profile_id);
  return true;
}

bool ModelDownloadController::CancelInstall(const QString& profileId) {
  const auto* profile = FindProfile(profileId);
  if (profile == nullptr || downloading_profile_id_ != QLatin1String(profile->profile_id)) {
    return false;
  }
  if (download_canceling_) {
    return true;
  }
  download_canceling_ = true;
  service_.CancelDownload();
  model_download_current_file_.clear();
  SetRowMessage(downloading_profile_id_, PL_TEXT("Canceling model download..."), false);
  if (!background_task_id_.isEmpty()) {
    background_tasks_->UpdateTaskState(background_task_id_, BackgroundTaskState::Canceling);
  }
  emit StateChanged();
  return true;
}

bool ModelDownloadController::RemoveModel(const QString& profileId) {
  const auto* profile = FindProfile(profileId);
  if (profile == nullptr) {
    return false;
  }
  const QString profile_id = QString::fromLatin1(profile->profile_id);
  if (downloading_profile_id_ == profile_id) {
    SetRowMessage(profile_id, PL_TEXT("Cancel the download before you remove the model."), true);
    emit StateChanged();
    return false;
  }
  const auto      root    = ModelRootForProfile(profile_id);
  const auto      staging = StagingRoot(root);
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  if (!ec) {
    std::filesystem::remove_all(root, ec);
  }
  installed_.insert(profile_id, IsModelInstalled(profile_id));
  if (ec) {
    SetRowMessage(profile_id,
                  PL_TEXT("Model removal failed: %1", QString::fromStdString(ec.message())), true);
    emit StateChanged();
    return false;
  }
  SetRowMessage(profile_id, PL_TEXT("Model files removed."), false);
  emit StateChanged();
  emit ModelRemoved(profile_id);
  return true;
}

}  // namespace alcedo::ui

#undef PL_TEXT
