//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "app/album_browse_service.hpp"
#include "app/model_asset_catalog.hpp"
#include "storage/controller/semantic/semantic_label_config.hpp"
#include "ui/alcedo_main/album_backend/album_backend.hpp"

namespace alcedo::ui {

using namespace std::chrono_literals;

#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {

constexpr auto   kSemanticGenerationImportPreferenceKey = "semantic/importGenerationPreference";
constexpr auto   kSemanticModelProfileKey               = "semantic/modelProfileId";
constexpr auto   kSemanticModelDirectoryKey             = "semantic/modelDirectory";
constexpr auto   kSemanticEndpointPresetKey             = "semantic/modelEndpointPreset";
constexpr auto   kSemanticCustomEndpointKey             = "semantic/customModelEndpoint";
constexpr auto   kSemanticPreferenceAsk                 = "ask";
constexpr auto   kSemanticPreferenceAlways              = "always";
constexpr auto   kSemanticPreferenceNever               = "never";
constexpr auto   kSemanticResolvedManifestFile          = "alcedo_model_manifest.json";
constexpr auto   kSemanticRuntimeStartupTimeout         = 60s;
constexpr auto   kJinaClipProfileId                     = "jina-clip-v2-int8-multilingual";
constexpr auto   kSiglip2ProfileId                      = "siglip2-b32-256-multilingual";
constexpr size_t kMobileClipBatchSize                   = 64;
constexpr size_t kJinaClipBatchSize                     = 4;
constexpr size_t kSiglip2BatchSize                      = 8;

auto             SemanticModelKeyFromInfo(const SemanticRuntimeModelInfo& info) -> std::string {
  if (info.revision.empty()) {
    return info.model_id;
  }
  return info.model_id + "@" + info.revision;
}

auto NormalizedSemanticPreference(QString preference) -> QString {
  preference = preference.trimmed().toLower();
  if (preference == QLatin1String(kSemanticPreferenceAlways) ||
      preference == QLatin1String(kSemanticPreferenceNever)) {
    return preference;
  }
  return QString::fromLatin1(kSemanticPreferenceAsk);
}

auto ClampToInt(size_t value) -> int {
  return static_cast<int>(
      std::min<size_t>(value, static_cast<size_t>(std::numeric_limits<int>::max())));
}

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

auto FindProfileByModel(const std::string& profile_id, const std::string& model_id)
    -> const ModelProfileSpec* {
  for (const auto& profile : SemanticModelProfiles()) {
    if (profile_id == profile.profile_id || model_id == profile.model_id) {
      return &profile;
    }
  }
  return nullptr;
}

auto CurrentUiSemanticLabelLanguage() -> SemanticLabelLanguage {
  QString code =
      QSettings{}.value(QStringLiteral("ui/language"), QStringLiteral("system")).toString();
  if (code.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0) {
    code = QLocale::system().bcp47Name();
  }
  return code.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive)
             ? SemanticLabelLanguage::kChinese
             : SemanticLabelLanguage::kEnglish;
}

auto ModelLabelLanguage(const SemanticRuntimeModelInfo& info) -> SemanticLabelLanguage {
  return SemanticLabelLanguageForModel(info.profile_id.empty() ? info.model_id : info.profile_id,
                                       info.language);
}

auto ModelLabelLanguage(const SemanticResolvedModelManifest& manifest) -> SemanticLabelLanguage {
  return SemanticLabelLanguageForModel(
      manifest.profile_id.empty() ? manifest.model_id : manifest.profile_id, manifest.language);
}

auto EmbeddingBatchSizeForProfile(const SemanticRuntimeModelInfo& info) -> size_t {
  const auto profile_id = info.profile_id.empty() ? info.model_id : info.profile_id;
  if (profile_id == kJinaClipProfileId) {
    return kJinaClipBatchSize;
  }
  if (profile_id == kSiglip2ProfileId) {
    return kSiglip2BatchSize;
  }
  return kMobileClipBatchSize;
}

auto LabelPrototypeBatchSizeForProfile(const SemanticRuntimeModelInfo& info) -> size_t {
  return EmbeddingBatchSizeForProfile(info);
}

auto LabelPrototypeBatchSizeForProfile(const SemanticResolvedModelManifest& manifest) -> size_t {
  const SemanticRuntimeModelInfo info{.profile_id = manifest.profile_id,
                                      .model_id   = manifest.model_id};
  return LabelPrototypeBatchSizeForProfile(info);
}

auto EmbeddingTimeoutForProfile(const SemanticRuntimeModelInfo& info) -> std::chrono::milliseconds {
  const auto profile_id = info.profile_id.empty() ? info.model_id : info.profile_id;
  if (profile_id == kJinaClipProfileId) {
    return 120s;
  }
  if (profile_id == kSiglip2ProfileId) {
    return 60s;
  }
  return 30s;
}

auto EmbeddingTimeoutForProfile(const SemanticResolvedModelManifest& manifest)
    -> std::chrono::milliseconds {
  const SemanticRuntimeModelInfo info{.profile_id = manifest.profile_id,
                                      .model_id   = manifest.model_id};
  return EmbeddingTimeoutForProfile(info);
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

auto ModelRootForProfile(const QString& profile_id, const QString& base_directory)
    -> std::filesystem::path {
  return QStringToPath(ProfileRootPath(base_directory, profile_id));
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

auto ItemsNeedingSemanticGeneration(const std::vector<SemanticGenerationItem>& items,
                                    SemanticStorageController&                 semantic,
                                    const std::string& model_key, bool force_regenerate)
    -> std::vector<SemanticGenerationItem> {
  if (force_regenerate || model_key.empty()) {
    return items;
  }

  std::vector<SemanticGenerationItem> pending;
  pending.reserve(items.size());
  constexpr bool require_label = true;
  for (const auto& item : items) {
    if (!semantic.HasReadyImageEmbedding(item.element_id, item.image_id, model_key,
                                         require_label)) {
      pending.push_back(item);
    }
  }
  return pending;
}

}  // namespace

namespace detail {

auto LoadLocalResolvedModelManifestForActivation(const QString& profileId,
                                                 const QString& baseDirectory, QString* error)
    -> std::optional<SemanticResolvedModelManifest> {
  return LoadLocalResolvedModelManifestImpl(profileId, baseDirectory, error);
}

}  // namespace detail

class SemanticRuntimeSessionGuard final {
 public:
  explicit SemanticRuntimeSessionGuard(std::shared_ptr<SemanticRuntimeService> runtime)
      : runtime_(std::move(runtime)) {}

  ~SemanticRuntimeSessionGuard() {
    if (runtime_) {
      runtime_->Stop();
    }
  }

  SemanticRuntimeSessionGuard(const SemanticRuntimeSessionGuard&)            = delete;
  SemanticRuntimeSessionGuard& operator=(const SemanticRuntimeSessionGuard&) = delete;

 private:
  std::shared_ptr<SemanticRuntimeService> runtime_;
};

SemanticGenerationController::SemanticGenerationController(AlbumBackend& backend, QObject* parent)
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

bool SemanticGenerationController::PromptVisible() const {
  return prompt_pending_ && !running_ && !backend_.nikon_he_recovery_.is_active();
}

void SemanticGenerationController::StartPendingGeneration(bool forceRegenerate) {
  StartGenerationForItems(pending_items_, forceRegenerate);
}

void SemanticGenerationController::SkipPendingGeneration(bool rememberChoice) {
  if (rememberChoice) {
    SetImportPreference(QString::fromLatin1(kSemanticPreferenceNever));
  }
  ClearPrompt();
  status_text_ = PL_TEXT("Semantic generation skipped.");
  emit StateChanged();
}

QString SemanticGenerationController::ImportPreference() const {
  return NormalizedSemanticPreference(
      QSettings{}
          .value(QLatin1String(kSemanticGenerationImportPreferenceKey),
                 QLatin1String(kSemanticPreferenceAsk))
          .toString());
}

void SemanticGenerationController::SetImportPreference(const QString& preference) {
  QSettings{}.setValue(QLatin1String(kSemanticGenerationImportPreferenceKey),
                       NormalizedSemanticPreference(preference));
  emit StateChanged();
}

QVariantList SemanticGenerationController::ModelProfileOptions() const {
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

QString SemanticGenerationController::SelectedModelSizeLabel() const {
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

void SemanticGenerationController::RecomputeSelectedModelState() {
  const auto  profile_id = SelectedModelProfileId();
  const auto* profile    = FindSemanticProfile(profile_id.toStdString());

  // Installed: local files present and validate against the catalog checksums.
  bool installed = false;
  if (profile != nullptr) {
    const auto root  = ModelRootForProfile(profile_id, ModelDownloadDirectory());
    const auto error = ValidateLocalCatalogModelProfile(*profile, root);
    installed        = !error.has_value();
  }
  selected_model_installed_ = installed;

  // Active: the project's active-model record points at the selected profile,
  // AND the files are still on disk. Deleting the model files deactivates it in
  // the UI even though the storage record lingers, so the card never claims a
  // model is active after it has been removed.
  const QString active_profile = ActiveModelProfileId();
  selected_model_active_ =
      installed && !active_profile.isEmpty()
      && active_profile == NormalizedProfileId(profile_id);
}

void SemanticGenerationController::UpdateDownloadSpeed(
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

  const auto now      = std::chrono::steady_clock::now();
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

QString SemanticGenerationController::SelectedModelProfileId() const {
  const auto& profiles = SemanticModelProfiles();
  return NormalizedProfileId(QSettings{}
                                 .value(QLatin1String(kSemanticModelProfileKey),
                                        QLatin1String(profiles.front().profile_id))
                                 .toString());
}

QString SemanticGenerationController::ActiveModelProfileId() const {
  auto project = backend_.project_handler_.project();
  if (!project) {
    return {};
  }
  std::string error;
  const auto  model =
      project->GetStorageService()->GetSemanticStorageController().ActiveModel(&error);
  if (!model.has_value()) {
    return {};
  }
  return QString::fromStdString(model->profile_id_.empty() ? model->model_id_ : model->profile_id_);
}

QString SemanticGenerationController::ActiveModelDisplayName() const {
  auto project = backend_.project_handler_.project();
  if (!project) {
    return PL_TEXT("No active model").Render();
  }
  std::string error;
  const auto  model =
      project->GetStorageService()->GetSemanticStorageController().ActiveModel(&error);
  if (!model.has_value()) {
    return PL_TEXT("No active model").Render();
  }
  if (const auto* profile = FindProfileByModel(model->profile_id_, model->model_id_)) {
    return QString::fromLatin1(profile->display_name);
  }
  return QString::fromStdString(model->model_id_);
}

QString SemanticGenerationController::ActiveModelKeyQString() const {
  return QString::fromStdString(ActiveModelKey());
}

QString SemanticGenerationController::ModelDownloadDirectory() const {
  const QString stored =
      QSettings{}.value(QLatin1String(kSemanticModelDirectoryKey), QString{}).toString().trimmed();
  return stored.isEmpty() ? DefaultSemanticModelDirectory() : stored;
}

QString SemanticGenerationController::ModelEndpointPreset() const {
  return NormalizedEndpointPreset(
      QSettings{}
          .value(QLatin1String(kSemanticEndpointPresetKey), QStringLiteral("mirror"))
          .toString());
}

QString SemanticGenerationController::CustomModelEndpoint() const {
  return QSettings{}.value(QLatin1String(kSemanticCustomEndpointKey), QString{}).toString();
}

QString SemanticGenerationController::EffectiveModelEndpoint() const {
  return EndpointForPreset(ModelEndpointPreset(), CustomModelEndpoint());
}

void SemanticGenerationController::SetSelectedModelProfileId(const QString& profileId) {
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

void SemanticGenerationController::SetModelDownloadDirectory(const QString& directory) {
  QString value = directory.trimmed();
  if (value.startsWith(QLatin1String("file:"))) {
    value = QUrl(value).toLocalFile();
  }
  QSettings{}.setValue(QLatin1String(kSemanticModelDirectoryKey),
                       value.isEmpty() ? DefaultSemanticModelDirectory() : value);
  model_download_status_text_ = PL_TEXT("Model directory updated.");
  model_download_progress_    = 0;
  emit StateChanged();
}

void SemanticGenerationController::SetModelEndpointPreset(const QString& preset) {
  QSettings{}.setValue(QLatin1String(kSemanticEndpointPresetKey), NormalizedEndpointPreset(preset));
  emit StateChanged();
}

void SemanticGenerationController::SetCustomModelEndpoint(const QString& endpoint) {
  QSettings{}.setValue(QLatin1String(kSemanticCustomEndpointKey), endpoint.trimmed());
  emit StateChanged();
}

void SemanticGenerationController::ResetModelDownloadDirectory() {
  QSettings{}.remove(QLatin1String(kSemanticModelDirectoryKey));
  model_download_status_text_ = PL_TEXT("Model directory reset to the executable folder.");
  model_download_progress_    = 0;
  emit StateChanged();
}

void SemanticGenerationController::RefreshSelectedModelStatus() {
  const auto  profile_id = SelectedModelProfileId();
  const auto* profile    = FindSemanticProfile(profile_id.toStdString());
  RecomputeSelectedModelState();
  if (profile == nullptr) {
    model_download_progress_    = 0;
    model_download_status_text_ = PL_TEXT("Unknown semantic model profile: %1", profile_id);
    emit StateChanged();
    return;
  }

  const auto root  = ModelRootForProfile(profile_id, ModelDownloadDirectory());
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

void SemanticGenerationController::StartSelectedModelDownload() {
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

void SemanticGenerationController::CancelSelectedModelDownload() {
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

void SemanticGenerationController::DeleteSelectedModel() {
  if (model_download_running_) {
    CancelSelectedModelDownload();
  }
  const auto      profile_id = SelectedModelProfileId();
  const auto      root       = ModelRootForProfile(profile_id, ModelDownloadDirectory());
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

void SemanticGenerationController::ActivateSelectedModel() {
  if (running_) {
    model_download_status_text_ =
        PL_TEXT("Finish or cancel semantic generation before activating another model.");
    emit StateChanged();
    return;
  }
  if (model_activation_running_) {
    return;
  }
  const QString profile_id = SelectedModelProfileId();

  QString       manifest_error;
  const auto    manifest = detail::LoadLocalResolvedModelManifestForActivation(
      profile_id, ModelDownloadDirectory(), &manifest_error);
  if (!manifest.has_value()) {
    model_download_status_text_ = manifest_error.isEmpty()
                                      ? PL_TEXT("Install the selected model before activating it.")
                                      : PL_TEXT("Cannot activate model: %1", manifest_error);
    emit StateChanged();
    return;
  }

  auto project = backend_.project_handler_.project();
  if (!project) {
    model_download_status_text_ = PL_TEXT("Open a project before activating a semantic model.");
    emit StateChanged();
    return;
  }
  auto runtime = project->GetSemanticRuntimeService();
  if (!runtime) {
    model_download_status_text_ = PL_TEXT("Semantic runtime service is unavailable.");
    emit StateChanged();
    return;
  }

  const std::string model_key      = manifest->revision.empty()
                                         ? manifest->model_id
                                         : manifest->model_id + "@" + manifest->revision;
  const auto        label_language = ModelLabelLanguage(*manifest);

  model_activation_running_        = true;
  model_download_progress_         = 0;
  model_download_status_text_      = PL_TEXT("Activating model and preparing labels...");
  emit StateChanged();

  auto runtime_options = RuntimeOptionsForProfile(profile_id, true);
  QPointer<SemanticGenerationController> self(this);
  std::thread([self, project = std::move(project), runtime = std::move(runtime),
               manifest = *manifest, model_key, label_language, runtime_options]() mutable {
    bool        ok             = false;
    bool        prototype_warm = false;
    std::string message;

    auto        finish = [&]() {
      if (!self) {
        return;
      }
      QMetaObject::invokeMethod(
          self,
          [self, ok, prototype_warm, model_key, message = std::move(message)]() mutable {
            if (!self) {
              return;
            }
            self->model_activation_running_ = false;
            self->model_download_progress_  = 0;
            if (ok) {
              self->model_key_ = model_key;
              self->model_download_status_text_ =
                  prototype_warm
                             ? PL_TEXT("%1 is active for this project. Label prompts are ready.",
                                       self->ActiveModelDisplayName())
                             : PL_TEXT("%1 is active for this project.", self->ActiveModelDisplayName());
              self->RecomputeSelectedModelState();
              self->RefreshAlbumSummary();
              self->backend_.ReloadCurrentFolder();
            } else {
              const QString detail = QString::fromUtf8(message.c_str());
              self->model_download_status_text_ =
                  detail.isEmpty() ? PL_TEXT("Semantic model activation failed.")
                                          : PL_TEXT("Semantic model activation failed: %1", detail);
            }
            emit self->StateChanged();
          },
          Qt::QueuedConnection);
    };

    auto&       semantic       = project->GetStorageService()->GetSemanticStorageController();
    const auto  prompt_hash    = SemanticPromptConfigHashForLanguage(label_language);
    const bool  already_active = semantic.ActiveModelKey() == model_key;
    std::string error;
    if (!semantic.UpsertModel(
            SemanticModelRecord{.model_key_     = model_key,
                                .model_id_      = manifest.model_id,
                                .revision_      = manifest.revision,
                                .embedding_dim_ = static_cast<int>(manifest.embedding_dimension),
                                .image_size_    = static_cast<int>(manifest.image_size),
                                .engine_id_     = manifest.engine_profile_id,
                                .profile_id_    = manifest.profile_id,
                                .supported_text_languages_json_ =
                                    SemanticSupportedTextLanguagesJson(label_language),
                                .prompt_config_hash_  = prompt_hash,
                                .asset_manifest_json_ = {},
                                .active_              = already_active},
            &error)) {
      message = error;
      finish();
      return;
    }

    const auto query_count     = semantic.CountLabelQueries(prompt_hash);
    const auto prototype_count = semantic.CountLabelPrototypes(model_key, prompt_hash);
    prototype_warm             = query_count > 0 && prototype_count >= query_count;
    if (query_count == 0 || prototype_count < query_count) {
      auto runtime_status = runtime->Status();
      if (runtime_status.state == SemanticRuntimeState::kReady &&
          runtime_status.model_info.has_value() &&
          (runtime_status.model_info->model_id != runtime_options.model_id ||
           runtime_status.model_info->revision != runtime_options.revision ||
           runtime_status.model_info->model_root != runtime_options.model_root.string())) {
        runtime->Stop();
        runtime_status = runtime->Status();
      }
      if (runtime_status.state != SemanticRuntimeState::kReady ||
          !runtime_status.model_info.has_value()) {
        if (!runtime->StartAndWait(runtime_options)) {
          runtime_status = runtime->Status();
          message        = runtime_status.message.empty() ? "semantic runtime failed to start"
                                                          : runtime_status.message;
          finish();
          return;
        }
        runtime_status = runtime->Status();
      }
      if (runtime_status.state != SemanticRuntimeState::kReady ||
          !runtime_status.model_info.has_value()) {
        message = runtime_status.message.empty()
                      ? "semantic runtime did not report model information"
                      : runtime_status.message;
        finish();
        return;
      }

      auto runtime_session = std::make_shared<SemanticRuntimeSessionGuard>(runtime);
      auto embedder        = std::make_shared<SemanticRuntimeImageEmbeddingClient>(runtime);
      SemanticGenerationPersistenceOptions persistence;
      persistence.storage_controller         = &semantic;
      persistence.model_key                  = model_key;
      persistence.prompt_config_hash         = prompt_hash;
      persistence.label_prototype_batch_size = LabelPrototypeBatchSizeForProfile(manifest);
      if (!SemanticGenerationService::EnsureLabelPrototypes(
              persistence, embedder, EmbeddingTimeoutForProfile(manifest), &error)) {
        message = error;
        finish();
        return;
      }
      prototype_warm = true;
    }

    if (!semantic.SetActiveModelKey(model_key, &error)) {
      message = error;
      finish();
      return;
    }

    ok = true;
    finish();
  }).detach();
}

void SemanticGenerationController::CancelGeneration() {
  if (job_) {
    job_->Cancel();
    status_text_ = PL_TEXT("Cancelling semantic generation...");
    emit StateChanged();
  }
}

void SemanticGenerationController::RefreshAlbumSummary() {
  const auto previous_total     = album_total_count_;
  const auto previous_labeled   = album_labeled_count_;
  const auto previous_unlabeled = album_unlabeled_count_;
  const auto previous_summary   = album_summary_text_;

  auto       project            = backend_.project_handler_.project();
  auto       browse             = project ? project->GetAlbumBrowseService() : nullptr;
  if (!project || !browse) {
    album_total_count_     = 0;
    album_labeled_count_   = 0;
    album_unlabeled_count_ = 0;
    album_summary_text_    = PL_TEXT("Open a project before running AI content recognition.");
    if (previous_total != album_total_count_ || previous_labeled != album_labeled_count_ ||
        previous_unlabeled != album_unlabeled_count_ ||
        previous_summary.source_ != album_summary_text_.source_ ||
        previous_summary.args_ != album_summary_text_.args_) {
      emit StateChanged();
    }
    return;
  }

  album_total_count_   = static_cast<int>(std::min<size_t>(
      browse->CountFilesInFolderById(0), static_cast<size_t>(std::numeric_limits<int>::max())));

  const auto model_key = ActiveModelKey();
  if (model_key.empty()) {
    album_labeled_count_ = 0;
  } else {
    album_labeled_count_ = static_cast<int>(std::min<size_t>(
        project->GetStorageService()->GetSemanticStorageController().CountImageLabelsInFolder(
            0, model_key),
        static_cast<size_t>(std::numeric_limits<int>::max())));
  }
  album_unlabeled_count_ = std::max(0, album_total_count_ - album_labeled_count_);
  album_summary_text_    = PL_TEXT("%1 image(s) total. %2 already have labels, %3 need labels.",
                                   album_total_count_, album_labeled_count_, album_unlabeled_count_);
  if (previous_total != album_total_count_ || previous_labeled != album_labeled_count_ ||
      previous_unlabeled != album_unlabeled_count_ ||
      previous_summary.source_ != album_summary_text_.source_ ||
      previous_summary.args_ != album_summary_text_.args_) {
    emit StateChanged();
  }
}

void SemanticGenerationController::StartAlbumGeneration(bool forceRegenerate) {
  auto project = backend_.project_handler_.project();
  auto browse  = project ? project->GetAlbumBrowseService() : nullptr;
  if (!project || !browse) {
    status_text_ = PL_TEXT("Semantic generation is unavailable without an open project.");
    emit StateChanged();
    return;
  }

  std::vector<SemanticGenerationItem> items;
  const auto                          files = browse->ListFilesInFolderById(0);
  items.reserve(files.size());
  for (const auto& file : files) {
    if (file.file_id_ == 0 || file.image_id_ == 0) {
      continue;
    }
    items.push_back(SemanticGenerationItem{file.file_id_, file.image_id_});
  }

  StartGenerationForItems(std::move(items), forceRegenerate);
}

void SemanticGenerationController::QueuePrompt(std::vector<SemanticGenerationItem> items) {
  pending_items_  = std::move(items);
  prompt_pending_ = !pending_items_.empty();
  total_          = static_cast<int>(pending_items_.size());
  embedded_       = 0;
  skipped_        = 0;
  failed_         = 0;
  canceled_       = 0;
  if (prompt_pending_) {
    status_text_ = PL_TEXT("Generate semantic labels for %1 imported image(s)?", total_);
  }
  emit StateChanged();
  ResumeQueuedWorkflow();
}

void SemanticGenerationController::ResumeQueuedWorkflow() {
  if (!prompt_pending_ || running_ || backend_.nikon_he_recovery_.is_active()) {
    emit StateChanged();
    return;
  }

  const QString preference =
      NormalizedSemanticPreference(QSettings{}
                                       .value(QLatin1String(kSemanticGenerationImportPreferenceKey),
                                              QLatin1String(kSemanticPreferenceAsk))
                                       .toString());
  if (preference == QLatin1String(kSemanticPreferenceAlways)) {
    StartPendingGeneration(false);
    return;
  }
  if (preference == QLatin1String(kSemanticPreferenceNever)) {
    ClearPrompt();
    return;
  }
  emit StateChanged();
}

auto SemanticGenerationController::StoredModelKey() const -> std::string {
  auto project = backend_.project_handler_.project();
  if (!project) {
    return {};
  }
  return project->GetStorageService()->GetSemanticStorageController().ActiveModelKey();
}

auto SemanticGenerationController::ActiveModelKey() const -> std::string {
  const auto stored = StoredModelKey();
  if (!stored.empty()) {
    return stored;
  }
  return model_key_;
}

auto SemanticGenerationController::LabelDisplayText(sl_element_id_t elementId) const -> QString {
  const auto model_key = ActiveModelKey();
  if (model_key.empty()) {
    return {};
  }
  auto project = backend_.project_handler_.project();
  if (!project) {
    return {};
  }
  std::string error;
  const auto  label =
      project->GetStorageService()->GetSemanticStorageController().GetImageLabelForFile(
          elementId, model_key, &error);
  if (!label.has_value()) {
    return {};
  }
  const auto display_language = CurrentUiSemanticLabelLanguage();
  const auto display_label    = [&](const std::string& value) {
    return QString::fromUtf8(::alcedo::SemanticLabelDisplayText(value, display_language).c_str());
  };
  // top_scores_json_ is already elbow-truncated at assignment time, so it holds exactly
  // the tags worth showing for this image (a single entry when top-1 dominates, several
  // when the score distribution supports them). Display them as-is rather than
  // re-filtering against a fixed margin here.
  const QJsonDocument doc =
      QJsonDocument::fromJson(QByteArray::fromStdString(label->top_scores_json_));
  if (!doc.isArray()) {
    return display_label(label->label_);
  }
  const auto  array = doc.array();
  QStringList labels;
  for (const auto& value : array) {
    const auto object = value.toObject();
    const auto name   = object.value(QStringLiteral("label")).toString();
    if (name.isEmpty()) {
      continue;
    }
    labels.push_back(display_label(name.toStdString()));
    if (labels.size() >= static_cast<qsizetype>(kMaxSemanticImageLabelCount)) {
      break;
    }
  }
  return labels.isEmpty() ? display_label(label->label_) : labels.join(QStringLiteral(", "));
}

void SemanticGenerationController::StartGenerationForItems(
    std::vector<SemanticGenerationItem> items, bool forceRegenerate) {
  if (running_) {
    return;
  }
  if (items.empty()) {
    prompt_pending_ = false;
    status_text_    = PL_TEXT("No images are waiting for semantic generation.");
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }

  pending_items_  = std::move(items);
  prompt_pending_ = false;
  running_        = true;
  embedded_       = 0;
  skipped_        = 0;
  failed_         = 0;
  canceled_       = 0;
  total_          = 0;
  status_text_    = PL_TEXT("Preparing semantic generation...");
  backend_.SetTaskState(status_text_, 0, true);
  emit                                   StateChanged();

  QPointer<SemanticGenerationController> self(this);
  QTimer::singleShot(160, this, [self, forceRegenerate]() {
    if (self) {
      self->ContinueGenerationForItems(forceRegenerate);
    }
  });
}

void SemanticGenerationController::ContinueGenerationForItems(bool forceRegenerate) {
  if (!running_) {
    return;
  }

  auto project           = backend_.project_handler_.project();
  auto thumbnail_service = backend_.project_handler_.thumbnail_service();
  if (!project || !thumbnail_service) {
    running_ = false;
    pending_items_.clear();
    status_text_ = PL_TEXT("Semantic generation is unavailable without an open project.");
    backend_.SetTaskState(status_text_, 0, false);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }

  auto runtime = project->GetSemanticRuntimeService();
  if (!runtime) {
    running_ = false;
    pending_items_.clear();
    status_text_ = PL_TEXT("Semantic runtime service is unavailable.");
    backend_.SetTaskState(status_text_, 0, false);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }
  QString active_profile_id = ActiveModelProfileId();
  if (active_profile_id.isEmpty()) {
    active_profile_id = SelectedModelProfileId();
  }
  SemanticRuntimeOptions runtime_options = RuntimeOptionsForProfile(active_profile_id, true);
  auto                   runtime_status  = runtime->Status();
  if (runtime_status.state == SemanticRuntimeState::kReady &&
      runtime_status.model_info.has_value() &&
      (runtime_status.model_info->model_id != runtime_options.model_id ||
       runtime_status.model_info->revision != runtime_options.revision ||
       runtime_status.model_info->model_root != runtime_options.model_root.string())) {
    runtime->Stop();
    runtime_status = runtime->Status();
  }
  if (runtime_status.state != SemanticRuntimeState::kReady ||
      !runtime_status.model_info.has_value()) {
    status_text_ = PL_TEXT("Starting semantic runtime...");
    emit StateChanged();

    if (!runtime->StartAndWait(runtime_options)) {
      runtime_status        = runtime->Status();
      const QString message = QString::fromStdString(runtime_status.message);
      running_              = false;
      pending_items_.clear();
      status_text_ = message.isEmpty() ? PL_TEXT("Semantic runtime failed to start.")
                                       : PL_TEXT("Semantic runtime failed to start: %1", message);
      backend_.SetTaskState(status_text_, 0, false);
      RefreshAlbumSummary();
      emit StateChanged();
      return;
    }

    runtime_status = runtime->Status();
    if (runtime_status.state != SemanticRuntimeState::kReady ||
        !runtime_status.model_info.has_value()) {
      const QString message = QString::fromStdString(runtime_status.message);
      running_              = false;
      pending_items_.clear();
      status_text_ = message.isEmpty()
                         ? PL_TEXT("Semantic runtime did not report model information.")
                         : PL_TEXT("Semantic runtime is not ready: %1", message);
      backend_.SetTaskState(status_text_, 0, false);
      RefreshAlbumSummary();
      emit StateChanged();
      return;
    }
  }

  auto              runtime_session = std::make_shared<SemanticRuntimeSessionGuard>(runtime);

  auto&             semantic        = project->GetStorageService()->GetSemanticStorageController();
  const std::string model_key       = SemanticModelKeyFromInfo(*runtime_status.model_info);
  const auto        label_language  = ModelLabelLanguage(*runtime_status.model_info);
  std::string       error;
  if (!semantic.UpsertModel(
          SemanticModelRecord{
              .model_key_     = model_key,
              .model_id_      = runtime_status.model_info->model_id,
              .revision_      = runtime_status.model_info->revision,
              .embedding_dim_ = static_cast<int>(runtime_status.model_info->embedding_dimension),
              .image_size_    = static_cast<int>(runtime_status.model_info->image_size),
              .engine_id_     = runtime_status.model_info->provider,
              .profile_id_    = runtime_status.model_info->profile_id,
              .supported_text_languages_json_ = SemanticSupportedTextLanguagesJson(label_language),
              .prompt_config_hash_            = SemanticPromptConfigHashForLanguage(label_language),
              .active_                        = true},
          &error)) {
    running_ = false;
    pending_items_.clear();
    status_text_ =
        PL_TEXT("Semantic model registration failed: %1", QString::fromUtf8(error.c_str()));
    backend_.SetTaskState(status_text_, 0, false);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }

  pending_items_ =
      ItemsNeedingSemanticGeneration(pending_items_, semantic, model_key, forceRegenerate);
  model_key_ = model_key;
  total_     = ClampToInt(pending_items_.size());
  embedded_  = 0;
  skipped_   = 0;
  failed_    = 0;
  canceled_  = 0;
  if (pending_items_.empty()) {
    running_     = false;
    status_text_ = PL_TEXT("All images already have semantic labels.");
    backend_.SetTaskState(status_text_, 100, false);
    backend_.ScheduleIdleTaskStateReset(1800);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }
  status_text_ = PL_TEXT("Generating semantic labels for %1 image(s)...", total_);
  emit                      StateChanged();

  SemanticGenerationOptions options;
  options.thumbnail_resolution = ThumbnailResolution::k256;
  options.thumbnail_batch_size = 8;
  options.embedding_batch_size = EmbeddingBatchSizeForProfile(*runtime_status.model_info);
  options.embedding_timeout    = EmbeddingTimeoutForProfile(*runtime_status.model_info);
  options.expected_model_info  = runtime_status.model_info;
  options.force_regenerate     = forceRegenerate;
  SemanticGenerationPersistenceOptions persistence;
  persistence.storage_controller = &semantic;
  persistence.model_key          = model_key;
  persistence.prompt_config_hash = SemanticPromptConfigHashForLanguage(label_language);
  persistence.label_prototype_batch_size =
      LabelPrototypeBatchSizeForProfile(*runtime_status.model_info);
  options.persistence = persistence;

  auto thumbnails = std::make_shared<ThumbnailServiceSemanticThumbnailProvider>(thumbnail_service);
  auto embedder   = std::make_shared<SemanticRuntimeImageEmbeddingClient>(runtime);
  auto service    = std::make_shared<SemanticGenerationService>(thumbnails, embedder);
  QPointer<SemanticGenerationController> self(this);
  auto                                   job = service->StartGeneration(
      pending_items_, options,
      [self](const SemanticGenerationProgress& progress) {
        if (!self) {
          return;
        }
        QMetaObject::invokeMethod(
            self,
            [self, progress]() {
              if (self) {
                self->UpdateProgress(progress);
              }
            },
            Qt::QueuedConnection);
      },
      [self](std::vector<SemanticGenerationItemResult> results) {
        if (!self) {
          return;
        }
        QMetaObject::invokeMethod(
            self,
            [self, results = std::move(results)]() mutable {
              if (self) {
                self->Finish(std::move(results));
              }
            },
            Qt::QueuedConnection);
      });

  runtime_session_ = std::move(runtime_session);
  job_             = std::move(job);
}

auto SemanticGenerationController::RuntimeOptionsForProfile(const QString& profileId,
                                                            bool           profileRoot) const
    -> SemanticRuntimeOptions {
  const auto*            profile = FindProfile(profileId);
  SemanticRuntimeOptions options;
  const QString          base_dir = ModelDownloadDirectory();
  const QString          root =
      profileRoot ? ProfileRootPath(base_dir, QString::fromLatin1(profile->profile_id)) : base_dir;
  options.model_root         = QStringToPath(root);
  options.model_id           = profile->model_id;
  options.revision           = profile->revision;
  options.hf_endpoint        = EffectiveModelEndpoint().toStdString();
  options.allow_download     = false;
  options.require_model_info = profileRoot;
  options.startup_timeout    = kSemanticRuntimeStartupTimeout;
  return options;
}

void SemanticGenerationController::UpdateProgress(const SemanticGenerationProgress& progress) {
  total_              = static_cast<int>(progress.total);
  embedded_           = static_cast<int>(progress.embedded);
  skipped_            = static_cast<int>(progress.skipped);
  failed_             = static_cast<int>(progress.failed);
  canceled_           = static_cast<int>(progress.canceled);
  const int completed = embedded_ + skipped_ + failed_ + canceled_;
  status_text_ =
      PL_TEXT("Generating semantic labels... %1/%2 complete", completed, std::max(total_, 1));
  backend_.SetTaskState(status_text_, total_ > 0 ? (completed * 100) / total_ : 0, true);
  RefreshAlbumSummary();
  emit StateChanged();
}

void SemanticGenerationController::Finish(std::vector<SemanticGenerationItemResult> results) {
  running_ = false;
  job_.reset();
  runtime_session_.reset();
  pending_items_.clear();
  prompt_pending_ = false;

  int embedded    = 0;
  int skipped     = 0;
  int failed      = 0;
  int canceled    = 0;
  for (const auto& result : results) {
    switch (result.status) {
      case SemanticGenerationItemStatus::kEmbedded:
        ++embedded;
        break;
      case SemanticGenerationItemStatus::kSkipped:
        ++skipped;
        break;
      case SemanticGenerationItemStatus::kCanceled:
        ++canceled;
        break;
      case SemanticGenerationItemStatus::kError:
        ++failed;
        break;
      default:
        break;
    }
  }
  embedded_    = embedded;
  skipped_     = skipped;
  failed_      = failed;
  canceled_    = canceled;
  total_       = static_cast<int>(results.size());

  status_text_ = PL_TEXT("Semantic generation complete: %1 generated, %2 skipped, %3 failed.",
                         embedded, skipped, failed + canceled);
  backend_.SetTaskState(status_text_, 100, false);
  backend_.ScheduleIdleTaskStateReset(2200);
  RefreshAlbumSummary();
  backend_.ReloadCurrentFolder();
  if (backend_.project_handler_.PersistCurrentProjectState()) {
    QString ignored_error;
    (void)backend_.project_handler_.PackageCurrentProjectFiles(&ignored_error);
  }
  emit StateChanged();
}

void SemanticGenerationController::ClearPrompt() {
  pending_items_.clear();
  prompt_pending_ = false;
  ResetCounters();
}

void SemanticGenerationController::ResetCounters() {
  total_    = 0;
  embedded_ = 0;
  skipped_  = 0;
  failed_   = 0;
  canceled_ = 0;
}

}  // namespace alcedo::ui

#undef PL_TEXT
