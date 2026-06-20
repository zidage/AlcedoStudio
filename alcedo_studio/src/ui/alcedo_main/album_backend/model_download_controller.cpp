//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/model_download_controller.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSettings>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "app/model_asset_catalog.hpp"
#include "ui/alcedo_main/album_backend/album_backend.hpp"

namespace alcedo::ui {

#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {

constexpr auto kSemanticModelProfileKey      = "semantic/modelProfileId";
constexpr auto kSemanticModelDirectoryKey    = "semantic/modelDirectory";
constexpr auto kSemanticEndpointPresetKey    = "semantic/modelEndpointPreset";
constexpr auto kSemanticCustomEndpointKey    = "semantic/customModelEndpoint";
constexpr auto kSemanticResolvedManifestFile = "alcedo_model_manifest.json";

auto DefaultSemanticModelDirectory() -> QString {
  return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("model"));
}

auto NormalizedProfileId(QString profile_id) -> QString {
  profile_id = profile_id.trimmed();
  for (const auto& profile : SemanticModelProfiles()) {
    if (profile_id == QLatin1String(profile.profile_id)) {
      return profile_id;
    }
  }
  return QString::fromLatin1(SemanticModelProfiles().front().profile_id);
}

auto FindProfile(const QString& profile_id) -> const ModelProfileSpec* {
  const QString normalized = NormalizedProfileId(profile_id);
  for (const auto& profile : SemanticModelProfiles()) {
    if (normalized == QLatin1String(profile.profile_id)) {
      return &profile;
    }
  }
  return &SemanticModelProfiles().front();
}

auto ProfileRootPath(const QString& base_directory, const QString& profile_id) -> QString {
  return QDir(base_directory).filePath(NormalizedProfileId(profile_id));
}

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

auto LoadLocalResolvedModelManifestImpl(const QString& profile_id, const QString& base_directory,
                                        QString* error)
    -> std::optional<SemanticResolvedModelManifest> {
  const auto* profile = FindProfile(profile_id);
  const auto  root    = ProfileRootPath(base_directory, QString::fromLatin1(profile->profile_id));
  if (const auto* catalog_profile = FindSemanticProfile(profile->profile_id);
      catalog_profile != nullptr) {
    const auto catalog_error =
        ValidateLocalCatalogModelProfile(*catalog_profile, QStringToPath(root));
    if (catalog_error.has_value()) {
      if (error) {
        *error = *catalog_error;
      }
      return std::nullopt;
    }
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

auto NormalizedEndpointPreset(QString preset) -> QString {
  preset = preset.trimmed().toLower();
  if (preset == QLatin1String("huggingface") || preset == QLatin1String("sufy") ||
      preset == QLatin1String("custom")) {
    return preset;
  }
  return QStringLiteral("mirror");
}

auto EndpointForPreset(const QString& preset, const QString& custom_endpoint) -> QString {
  const QString normalized = NormalizedEndpointPreset(preset);
  if (normalized == QLatin1String("huggingface")) {
    return QStringLiteral("https://huggingface.co");
  }
  if (normalized == QLatin1String("sufy")) {
    return QStringLiteral("https://hf-cdn.sufy.com");
  }
  if (normalized == QLatin1String("custom") && !custom_endpoint.trimmed().isEmpty()) {
    return custom_endpoint.trimmed();
  }
  return QStringLiteral("https://hf-mirror.com");
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
  return LoadLocalResolvedModelManifestImpl(profileId, baseDirectory, error);
}

}  // namespace detail

ModelDownloadController::ModelDownloadController(AlbumBackend& backend, QObject* parent)
    : QObject(parent), backend_(backend) {
  model_download_status_text_ = PL_TEXT("Model status has not been checked.");

  connect(&backend_.model_download_service_, &alcedo::ModelDownloadService::ProgressChanged, this,
          [this](const alcedo::ModelDownloadProgress& progress) {
            model_download_progress_       = DownloadProgressPercent(progress);
            model_download_phase_          = QString::fromStdString(progress.phase);
            model_download_current_file_   = QString::fromStdString(progress.current_file);
            model_download_bytes_done_     = progress.bytes_downloaded;
            model_download_bytes_total_    = progress.bytes_total;
            model_download_bytes_label_    = FormatBytesLabel(
                static_cast<quint64>(progress.bytes_downloaded),
                static_cast<quint64>(progress.bytes_total));
            model_download_files_done_     = static_cast<int>(progress.files_completed);
            model_download_files_total_    = static_cast<int>(progress.files_total);
            UpdateDownloadSpeed(progress);
            const QString message          = QString::fromStdString(progress.message);
            model_download_status_text_ =
                message.isEmpty() ? PL_TEXT("Downloading model... %1%", model_download_progress_)
                                  : PL_TEXT("%1 (%2%)", message, model_download_progress_);
            emit StateChanged();
          });
  connect(&backend_.model_download_service_, &alcedo::ModelDownloadService::Finished, this,
          [this](bool ok, const QString& error) {
            model_download_running_ = false;
            if (ok) {
              model_download_progress_    = 100;
              model_download_phase_       = QStringLiteral("installed");
              model_download_current_file_.clear();
              model_download_bytes_done_  = model_download_bytes_total_;
              model_download_bytes_label_ =
                  FormatBytesLabel(model_download_bytes_total_, model_download_bytes_total_);
              model_download_files_done_  = model_download_files_total_;
              model_download_status_text_ = PL_TEXT("Model download complete.");
            } else {
              model_download_progress_    = 0;
              model_download_phase_       = QStringLiteral("failed");
              model_download_current_file_.clear();
              model_download_status_text_ = error.isEmpty()
                                                ? PL_TEXT("Model download failed.")
                                                : PL_TEXT("Model download failed: %1", error);
            }
            model_download_speed_label_.clear();
            model_download_eta_label_.clear();
            download_speed_ema_    = 0.0;
            download_speed_primed_ = false;
            RecomputeSelectedModelState();
            emit StateChanged();
          });
}

QVariantList ModelDownloadController::ModelProfileOptions() const {
  QVariantList options;
  for (const auto& profile : SemanticModelProfiles()) {
    QVariantMap entry;
    entry.insert(QStringLiteral("profileId"), QString::fromLatin1(profile.profile_id));
    entry.insert(QStringLiteral("label"), QString::fromLatin1(profile.display_name));
    entry.insert(QStringLiteral("modelId"), QString::fromLatin1(profile.model_id));
    entry.insert(QStringLiteral("revision"), QString::fromLatin1(profile.revision));
    entry.insert(QStringLiteral("language"), QString::fromLatin1(ToString(profile.language)));
    entry.insert(QStringLiteral("imageSize"), static_cast<int>(profile.image_size));
    entry.insert(QStringLiteral("nativeEmbeddingDim"),
                 static_cast<int>(profile.native_embedding_dimension));
    entry.insert(QStringLiteral("activatable"), true);
    options.push_back(entry);
  }
  return options;
}

QString ModelDownloadController::SelectedModelSizeLabel() const {
  const auto  profile_id = SelectedModelProfileId();
  const auto* profile    = FindSemanticProfile(profile_id.toStdString());
  if (profile == nullptr) {
    return {};
  }
  const quint64 total = ProfileTotalBytes(*profile);
  if (total == 0) {
    return {};
  }
  return QString(QChar(0x2248)) + QStringLiteral(" ")
         + QLocale().formattedDataSize(static_cast<qint64>(total), 1, QLocale::DataSizeBase1000);
}

void ModelDownloadController::RecomputeSelectedModelState() {
  const bool was_installed = selected_model_installed_;
  const auto profile_id    = SelectedModelProfileId();
  const auto* profile      = FindSemanticProfile(profile_id.toStdString());

  // Installed: local files present and validate against the catalog checksums.
  bool installed = false;
  if (profile != nullptr) {
    const auto root  = ModelRootForProfile(profile_id);
    const auto error = ValidateLocalCatalogModelProfile(*profile, root);
    installed        = !error.has_value();
  }
  selected_model_installed_ = installed;
  if (installed != was_installed) {
    emit SelectedModelInstallChanged();
  }
}

void ModelDownloadController::UpdateDownloadSpeed(
    const alcedo::ModelDownloadProgress& progress) {
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

  const auto  now       = std::chrono::steady_clock::now();
  const auto  bytes_now = static_cast<quint64>(progress.bytes_downloaded);

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
        download_speed_ema_ = download_speed_ema_ == 0.0
                                  ? instant
                                  : (kAlpha * instant + (1.0 - kAlpha) * download_speed_ema_);
      }
      download_sample_bytes_ = bytes_now;
      download_sample_time_  = now;
    }
  }

  if (download_speed_ema_ > 0 && progress.bytes_total > 0) {
    model_download_speed_label_ = FormatSpeedLabel(download_speed_ema_);
    const auto done = std::min<quint64>(bytes_now, progress.bytes_total);
    const double remaining = static_cast<double>(progress.bytes_total - done);
    model_download_eta_label_ = FormatEtaLabel(remaining / download_speed_ema_);
  } else {
    model_download_speed_label_.clear();
    model_download_eta_label_.clear();
  }
}

QString ModelDownloadController::SelectedModelProfileId() const {
  const auto& profiles = SemanticModelProfiles();
  return NormalizedProfileId(QSettings{}
                                 .value(QLatin1String(kSemanticModelProfileKey),
                                        QLatin1String(profiles.front().profile_id))
                                 .toString());
}

QString ModelDownloadController::ModelDownloadDirectory() const {
  const QString stored =
      QSettings{}.value(QLatin1String(kSemanticModelDirectoryKey), QString{}).toString().trimmed();
  return stored.isEmpty() ? DefaultSemanticModelDirectory() : stored;
}

QString ModelDownloadController::ModelEndpointPreset() const {
  return NormalizedEndpointPreset(
      QSettings{}
          .value(QLatin1String(kSemanticEndpointPresetKey), QStringLiteral("mirror"))
          .toString());
}

QString ModelDownloadController::CustomModelEndpoint() const {
  return QSettings{}.value(QLatin1String(kSemanticCustomEndpointKey), QString{}).toString();
}

QString ModelDownloadController::EffectiveModelEndpoint() const {
  return EndpointForPreset(ModelEndpointPreset(), CustomModelEndpoint());
}

auto ModelDownloadController::ModelRootForProfile(const QString& profileId) const
    -> std::filesystem::path {
  return QStringToPath(ProfileRootPath(ModelDownloadDirectory(), profileId));
}

auto ModelDownloadController::LoadSelectedResolvedManifest(QString* error) const
    -> std::optional<SemanticResolvedModelManifest> {
  return LoadLocalResolvedModelManifestImpl(SelectedModelProfileId(), ModelDownloadDirectory(),
                                            error);
}

void ModelDownloadController::SetStatusText(const i18n::LocalizedText& text) {
  model_download_status_text_ = text;
  emit StateChanged();
}

void ModelDownloadController::SetSelectedModelProfileId(const QString& profileId) {
  QSettings{}.setValue(QLatin1String(kSemanticModelProfileKey), NormalizedProfileId(profileId));
  model_download_status_text_ = PL_TEXT("Model status has not been checked.");
  model_download_progress_    = 0;
  model_download_phase_       = {};
  model_download_current_file_.clear();
  model_download_bytes_done_  = 0;
  model_download_bytes_total_ = 0;
  model_download_bytes_label_.clear();
  model_download_speed_label_.clear();
  model_download_eta_label_.clear();
  download_speed_ema_    = 0.0;
  download_speed_primed_ = false;
  model_download_files_done_  = 0;
  model_download_files_total_ = 0;
  RecomputeSelectedModelState();
  emit StateChanged();
}

void ModelDownloadController::SetModelDownloadDirectory(const QString& directory) {
  QString value = directory.trimmed();
  if (value.startsWith(QLatin1String("file:"))) {
    value = QUrl(value).toLocalFile();
  }
  QSettings{}.setValue(QLatin1String(kSemanticModelDirectoryKey),
                       value.isEmpty() ? DefaultSemanticModelDirectory() : value);
  model_download_status_text_ = PL_TEXT("Model directory updated.");
  model_download_progress_    = 0;
  RecomputeSelectedModelState();
  emit StateChanged();
}

void ModelDownloadController::SetModelEndpointPreset(const QString& preset) {
  QSettings{}.setValue(QLatin1String(kSemanticEndpointPresetKey), NormalizedEndpointPreset(preset));
  emit StateChanged();
}

void ModelDownloadController::SetCustomModelEndpoint(const QString& endpoint) {
  QSettings{}.setValue(QLatin1String(kSemanticCustomEndpointKey), endpoint.trimmed());
  emit StateChanged();
}

void ModelDownloadController::ResetModelDownloadDirectory() {
  QSettings{}.remove(QLatin1String(kSemanticModelDirectoryKey));
  model_download_status_text_ = PL_TEXT("Model directory reset to the executable folder.");
  model_download_progress_    = 0;
  RecomputeSelectedModelState();
  emit StateChanged();
}

void ModelDownloadController::RefreshSelectedModelStatus() {
  const auto  profile_id = SelectedModelProfileId();
  const auto* profile    = FindSemanticProfile(profile_id.toStdString());
  RecomputeSelectedModelState();
  if (profile == nullptr) {
    model_download_progress_    = 0;
    model_download_status_text_ = PL_TEXT("Unknown semantic model profile: %1", profile_id);
    emit StateChanged();
    return;
  }

  const auto root  = ModelRootForProfile(profile_id);
  const auto error = ValidateLocalCatalogModelProfile(*profile, root);
  if (!error.has_value()) {
    model_download_progress_ = 100;
    model_download_status_text_ =
        PL_TEXT("Model is installed at %1", QString::fromStdString(root.string()));
  } else {
    model_download_progress_    = 0;
    model_download_status_text_ = error->isEmpty() ? PL_TEXT("Model is not installed.")
                                                   : PL_TEXT("Model missing: %1", *error);
  }
  emit StateChanged();
}

void ModelDownloadController::StartSelectedModelDownload() {
  if (model_download_running_ || backend_.model_download_service_.IsRunning()) {
    return;
  }

  const auto profile_id = SelectedModelProfileId();
  const auto endpoint   = EffectiveModelEndpoint().toStdString();
  const bool started    = backend_.model_download_service_.StartDownload(
      profile_id.toStdString(), QStringToPath(ModelDownloadDirectory()), endpoint);
  if (!started) {
    model_download_status_text_ = PL_TEXT("Model download failed to start.");
    emit StateChanged();
    return;
  }

  // Seed the totals from the catalog so the card can show the full size before
  // the worker's first progress tick lands.
  const auto* profile       = FindSemanticProfile(profile_id.toStdString());
  const quint64 total_bytes = profile != nullptr ? ProfileTotalBytes(*profile) : 0;
  model_download_running_     = true;
  model_download_progress_    = 0;
  model_download_phase_       = QStringLiteral("preparing");
  model_download_current_file_.clear();
  model_download_bytes_done_  = 0;
  model_download_bytes_label_ =
      FormatBytesLabel(0, static_cast<quint64>(total_bytes));
  model_download_bytes_total_ = total_bytes;
  model_download_speed_label_.clear();
  model_download_eta_label_.clear();
  download_speed_ema_    = 0.0;
  download_speed_primed_ = false;
  model_download_files_done_  = 0;
  model_download_files_total_ = profile != nullptr ? static_cast<int>(profile->assets.size()) : 0;
  selected_model_installed_   = false;
  model_download_status_text_ = PL_TEXT("Model download queued from %1", EffectiveModelEndpoint());
  emit StateChanged();
}

void ModelDownloadController::CancelSelectedModelDownload() {
  if (!model_download_running_ && !backend_.model_download_service_.IsRunning()) {
    return;
  }
  if (!backend_.model_download_service_.IsRunning()) {
    // No active worker will emit Finished; clear local state immediately.
    model_download_running_     = false;
    model_download_progress_    = 0;
    model_download_phase_       = QStringLiteral("cancelled");
    model_download_current_file_.clear();
    model_download_bytes_done_  = 0;
    model_download_bytes_label_.clear();
    model_download_speed_label_.clear();
    model_download_eta_label_.clear();
    download_speed_ema_    = 0.0;
    download_speed_primed_ = false;
    model_download_status_text_ = PL_TEXT("Model download cancelled.");
    emit StateChanged();
    return;
  }
  backend_.model_download_service_.CancelDownload();
  model_download_phase_       = QStringLiteral("cancelled");
  model_download_current_file_.clear();
  model_download_status_text_ = PL_TEXT("Cancelling model download...");
  emit StateChanged();
}

void ModelDownloadController::DeleteSelectedModel() {
  if (model_download_running_) {
    CancelSelectedModelDownload();
  }
  const auto      profile_id = SelectedModelProfileId();
  const auto      root       = ModelRootForProfile(profile_id);
  const auto      staging    = StagingRoot(root);
  std::error_code ec;
  if (std::filesystem::exists(staging, ec)) {
    std::filesystem::remove_all(staging, ec);
  }
  if (!ec && std::filesystem::exists(root, ec)) {
    std::filesystem::remove_all(root, ec);
  }
  model_download_progress_    = 0;
  model_download_phase_       = {};
  model_download_current_file_.clear();
  model_download_bytes_done_  = 0;
  model_download_bytes_total_ = 0;
  model_download_bytes_label_.clear();
  model_download_speed_label_.clear();
  model_download_eta_label_.clear();
  download_speed_ema_    = 0.0;
  download_speed_primed_ = false;
  model_download_files_done_  = 0;
  model_download_files_total_ = 0;
  RecomputeSelectedModelState();
  if (!ec) {
    model_download_status_text_ = PL_TEXT("Model files deleted.");
  } else {
    model_download_status_text_ =
        PL_TEXT("Model delete failed: %1", QString::fromStdString(ec.message()));
  }
  emit StateChanged();
}

}  // namespace alcedo::ui

#undef PL_TEXT
