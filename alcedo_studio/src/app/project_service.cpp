//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/project_service.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QSettings>
#include <QThread>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <json.hpp>
#include <optional>
#include <random>
#include <stdexcept>

#include "app/model_asset_catalog.hpp"
#include "app/project_package_backend.hpp"
#include "app/project_package_service.hpp"
#include "utils/diagnostics/app_logging.hpp"
#include "utils/string/convert.hpp"
#include "uuid.h"

namespace alcedo {
namespace {

using namespace std::chrono_literals;

constexpr auto kSemanticModelDirectoryKey     = "semantic/modelDirectory";
constexpr auto kSemanticEndpointPresetKey     = "semantic/modelEndpointPreset";
constexpr auto kSemanticCustomEndpointKey     = "semantic/customModelEndpoint";
constexpr auto kSemanticRuntimeStartupTimeout = 60s;
constexpr auto kJinaClipProfileId             = "jina-clip-v2-int8-multilingual";
constexpr auto kSiglip2ProfileId              = "siglip2-b32-256-multilingual";

auto           DefaultSemanticModelDirectory() -> QString {
  return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("model"));
}

auto QStringToPath(const QString& value) -> std::filesystem::path {
#ifdef _WIN32
  return std::filesystem::path(value.toStdWString());
#else
  return std::filesystem::path(value.toStdString());
#endif
}

auto NormalizedEndpointPreset(QString preset) -> QString {
  preset = preset.trimmed().toLower();
  if (preset == QLatin1String("huggingface") || preset == QLatin1String("sufy") ||
      preset == QLatin1String("custom")) {
    return preset;
  }
  return QStringLiteral("mirror");
}

auto EffectiveSemanticModelEndpoint() -> QString {
  QSettings     settings;
  const QString preset = NormalizedEndpointPreset(
      settings.value(QLatin1String(kSemanticEndpointPresetKey), QStringLiteral("mirror"))
          .toString());
  const QString custom_endpoint =
      settings.value(QLatin1String(kSemanticCustomEndpointKey), QString{}).toString().trimmed();
  if (preset == QLatin1String("huggingface")) {
    return QStringLiteral("https://huggingface.co");
  }
  if (preset == QLatin1String("sufy")) {
    return QStringLiteral("https://hf-cdn.sufy.com");
  }
  if (preset == QLatin1String("custom") && !custom_endpoint.isEmpty()) {
    return custom_endpoint;
  }
  return QStringLiteral("https://hf-mirror.com");
}

auto SemanticModelKeyFromInfo(const SemanticRuntimeModelInfo& info) -> std::string {
  if (info.revision.empty()) {
    return info.model_id;
  }
  return info.model_id + "@" + info.revision;
}

auto ProfileIdForModel(const SemanticModelRecord& model) -> std::string {
  for (const auto& profile : SemanticModelProfiles()) {
    if (model.profile_id_ == profile.profile_id || model.model_id_ == profile.model_id) {
      return profile.profile_id;
    }
  }
  if (!model.profile_id_.empty()) {
    return model.profile_id_;
  }
  return {};
}

auto EmbeddingTimeoutForModel(const SemanticModelRecord& model) -> std::chrono::milliseconds {
  const auto profile_id = ProfileIdForModel(model);
  if (profile_id == kJinaClipProfileId) {
    return 120s;
  }
  if (profile_id == kSiglip2ProfileId) {
    return 60s;
  }
  return 30s;
}

auto RuntimeOptionsForSemanticModel(const SemanticModelRecord& model) -> SemanticRuntimeOptions {
  QSettings     settings;
  const QString base_dir =
      settings.value(QLatin1String(kSemanticModelDirectoryKey), DefaultSemanticModelDirectory())
          .toString();
  const auto    profile_id = ProfileIdForModel(model);
  const QString root =
      profile_id.empty() ? base_dir : QDir(base_dir).filePath(QString::fromStdString(profile_id));

  SemanticRuntimeOptions options;
  options.model_root         = QStringToPath(root);
  options.model_id           = model.model_id_;
  options.revision           = model.revision_;
  options.hf_endpoint        = EffectiveSemanticModelEndpoint().toStdString();
  options.allow_download     = false;
  options.require_model_info = true;
  options.startup_timeout    = kSemanticRuntimeStartupTimeout;
  return options;
}

auto ValidateSearchEmbedding(const SemanticEmbeddingResult& result, const std::string& request_id,
                             int expected_dimension) -> std::optional<std::string> {
  if (!result.ok) {
    return result.error.empty() ? "text embedding request failed" : result.error;
  }
  if (result.request_id != request_id) {
    return "text embedding response request id mismatch";
  }
  if (result.embedding.size() != static_cast<size_t>(expected_dimension)) {
    return "text embedding dimension mismatch";
  }
  if (result.dimension != 0 && result.dimension != static_cast<uint32_t>(expected_dimension)) {
    return "text embedding response dimension mismatch";
  }
  double norm_sq = 0.0;
  for (const auto value : result.embedding) {
    if (!std::isfinite(value)) {
      return "text embedding contains NaN or infinity";
    }
    norm_sq += static_cast<double>(value) * static_cast<double>(value);
  }
  if (norm_sq <= 0.0) {
    return "text embedding norm is zero";
  }
  return std::nullopt;
}

auto MakeSemanticSearchRequestId(sl_element_id_t folder_id, const std::wstring& query)
    -> std::string {
  const auto hash = std::hash<std::wstring>{}(query);
  return "semantic-search-" + std::to_string(folder_id) + "-" + std::to_string(hash);
}

class SemanticRuntimeSearchSession final {
 public:
  explicit SemanticRuntimeSearchSession(std::shared_ptr<SemanticRuntimeService> runtime)
      : runtime_(std::move(runtime)) {}
  ~SemanticRuntimeSearchSession() {
    if (runtime_) {
      runtime_->Stop();
    }
  }

  SemanticRuntimeSearchSession(const SemanticRuntimeSearchSession&)            = delete;
  SemanticRuntimeSearchSession& operator=(const SemanticRuntimeSearchSession&) = delete;

 private:
  std::shared_ptr<SemanticRuntimeService> runtime_;
};

class ProjectSemanticSearchProvider final : public SemanticSearchProvider {
 public:
  ProjectSemanticSearchProvider(
      std::shared_ptr<StorageService>                          storage_service,
      std::function<std::shared_ptr<SemanticRuntimeService>()> runtime_factory)
      : storage_service_(std::move(storage_service)),
        runtime_factory_(std::move(runtime_factory)) {}

  [[nodiscard]] auto Search(sl_element_id_t folder_id, const std::wstring& query, size_t offset,
                            size_t limit) const -> std::vector<FuzzySearchMatch> override {
    diag::TraceScope trace(diag::semanticLog(), QStringLiteral("semantic.search.provider"),
                           QStringLiteral("folder_id=%1 query_chars=%2 offset=%3 limit=%4")
                               .arg(static_cast<qulonglong>(folder_id))
                               .arg(static_cast<qulonglong>(query.size()))
                               .arg(static_cast<qulonglong>(offset))
                               .arg(static_cast<qulonglong>(limit)));
    if (query.empty() || limit == 0) {
      return {};
    }
    if (!storage_service_) {
      throw std::runtime_error("Semantic storage service is unavailable.");
    }

    auto&       semantic = storage_service_->GetSemanticStorageController();
    std::string error;
    const auto  active_model = semantic.ActiveModel(&error);
    if (!active_model.has_value()) {
      throw std::runtime_error(error.empty() ? "No active semantic model is registered." : error);
    }

    auto runtime = runtime_factory_ ? runtime_factory_() : nullptr;
    if (!runtime) {
      throw std::runtime_error("Semantic runtime service is unavailable.");
    }

    const auto runtime_options = RuntimeOptionsForSemanticModel(*active_model);
    auto       runtime_status  = runtime->Status();
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
        throw std::runtime_error(runtime_status.message.empty()
                                     ? "Semantic runtime failed to start."
                                     : runtime_status.message);
      }
      runtime_status = runtime->Status();
    }
    if (runtime_status.state != SemanticRuntimeState::kReady ||
        !runtime_status.model_info.has_value()) {
      throw std::runtime_error(runtime_status.message.empty()
                                   ? "Semantic runtime did not report model information."
                                   : runtime_status.message);
    }

    const auto& info = *runtime_status.model_info;
    if (SemanticModelKeyFromInfo(info) != active_model->model_key_) {
      throw std::runtime_error("Semantic runtime model does not match the active project model.");
    }
    if (static_cast<int>(info.embedding_dimension) != active_model->embedding_dim_) {
      throw std::runtime_error("Semantic runtime embedding dimension does not match storage.");
    }
    if (static_cast<int>(info.image_size) != active_model->image_size_) {
      throw std::runtime_error("Semantic runtime image size does not match storage.");
    }

    const SemanticRuntimeSearchSession session(runtime);
    const auto                         request_id = MakeSemanticSearchRequestId(folder_id, query);
    const auto                         text       = conv::ToBytes(query);
    qCInfo(diag::semanticLog).noquote()
        << QStringLiteral("semantic.search.embedding.request request_id=%1 model_key=%2")
               .arg(QString::fromStdString(request_id),
                    QString::fromStdString(active_model->model_key_));
    const auto                         embedding =
        runtime->EmbedText(request_id, text, EmbeddingTimeoutForModel(*active_model));
    if (const auto validation =
            ValidateSearchEmbedding(embedding, request_id, active_model->embedding_dim_);
        validation.has_value()) {
      throw std::runtime_error(*validation);
    }

    const auto ranked = semantic.SearchImageEmbeddings(folder_id, active_model->model_key_,
                                                       embedding.embedding, offset, limit, &error);
    if (!error.empty()) {
      throw std::runtime_error(error);
    }

    std::vector<FuzzySearchMatch> out;
    out.reserve(ranked.size());
    for (const auto& row : ranked) {
      out.push_back(FuzzySearchMatch{
          .file_id_ = row.file_id_, .image_id_ = row.image_id_, .file_name_ = row.file_name_});
    }
    qCInfo(diag::semanticLog).noquote()
        << QStringLiteral("semantic.search.provider.result request_id=%1 count=%2")
               .arg(QString::fromStdString(request_id))
               .arg(static_cast<qulonglong>(out.size()));
    return out;
  }

 private:
  std::shared_ptr<StorageService>                          storage_service_;
  std::function<std::shared_ptr<SemanticRuntimeService>()> runtime_factory_;
};

auto ParseSemVer(std::string_view version, std::array<int, 3>* out) -> bool {
  std::array<int, 3> parts{};
  size_t             begin = 0;
  for (size_t index = 0; index < parts.size(); ++index) {
    const size_t end = version.find('.', begin);
    const auto   token =
        version.substr(begin, end == std::string_view::npos ? version.size() - begin : end - begin);
    if (token.empty()) {
      return false;
    }
    int value = 0;
    for (const char ch : token) {
      if (ch < '0' || ch > '9') {
        return false;
      }
      value = value * 10 + (ch - '0');
    }
    parts[index] = value;
    if (index + 1 < parts.size()) {
      if (end == std::string_view::npos) {
        return false;
      }
      begin = end + 1;
    } else if (end != std::string_view::npos) {
      return false;
    }
  }
  *out = parts;
  return true;
}

auto IsSupportedProjectVersion(std::string_view version) -> bool {
  std::array<int, 3> parsed{};
  std::array<int, 3> min_supported{};
  std::array<int, 3> max_supported{};
  return ParseSemVer(version, &parsed) &&
         ParseSemVer(project_pack::kMinSupportedProjectFileVersion, &min_supported) &&
         ParseSemVer(project_pack::kMaxSupportedProjectFileVersion, &max_supported) &&
         parsed >= min_supported && parsed <= max_supported;
}

auto GenerateProjectUUID() -> std::string {
  std::random_device random_device;
  std::seed_seq      seed{random_device(), random_device(), random_device(), random_device()};
  std::mt19937       generator(seed);
  uuids::uuid_random_generator uuid_gen(generator);
  return uuids::to_string(uuid_gen());
}

auto MakeSemanticRuntimeService() -> std::shared_ptr<SemanticRuntimeService> {
  return std::shared_ptr<SemanticRuntimeService>(
      new SemanticRuntimeService(), [](SemanticRuntimeService* runtime) {
        if (runtime == nullptr) {
          return;
        }
        runtime->StopForProjectClose();
        if (QThread::currentThread() == runtime->thread()) {
          delete runtime;
          return;
        }
        QMetaObject::invokeMethod(
            runtime, [runtime]() { delete runtime; }, Qt::BlockingQueuedConnection);
      });
}

// Collects lightweight diagnostic summary of the project database:
// per-table row counts and min/max primary key ranges. This is NOT
// a strong integrity check — mismatches produce a warning, not a
// load failure. Use data_fingerprint (L3) for semantic verification.
auto ComputeProjectDataSummary(StorageService& storage_service) -> nlohmann::json {
  auto guard       = storage_service.GetDBController().GetConnectionGuard();

  auto query_int64 = [&](const std::string& sql) -> std::optional<int64_t> {
    duckdb_result result;
    if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
      duckdb_destroy_result(&result);
      return std::nullopt;
    }
    std::optional<int64_t> value;
    if (duckdb_row_count(&result) > 0 && duckdb_column_count(&result) > 0) {
      if (!duckdb_value_is_null(&result, 0, 0)) {
        value = duckdb_value_int64(&result, 0, 0);
      }
    }
    duckdb_destroy_result(&result);
    return value;
  };

  struct TableInfo {
    const char* name;
    const char* pk_column;  // nullptr if no meaningful single numeric PK
  };
  static constexpr TableInfo kTables[] = {
      {"Sleeve", "id"},
      {"Image", "id"},
      {"SleeveRoot", "id"},
      {"Element", "id"},
      {"FolderContent", nullptr},
      {"FileImage", "file_id"},
      {"ComboFolder", "combo_id"},
      {"Filter", "combo_id"},
      {"EditHistory", "file_id"},
      {"Version", "hash"},
      {"PipelineParam", "file_id"},
      {"SemanticModel", nullptr},
      {"SemanticImageEmbedding", "file_id"},
      {"SemanticImageEmbedding768", "file_id"},
      {"SemanticImageLabel", "file_id"},
      {"SemanticLabelQuery", nullptr},
      {"SemanticLabelPrototype", nullptr},
      {"SemanticLabelPrototype768", nullptr},
  };

  nlohmann::json summary;
  summary["version"]    = 1;
  nlohmann::json tables = nlohmann::json::object();

  for (const auto& table : kTables) {
    nlohmann::json entry;

    auto           count = query_int64(std::string("SELECT COUNT(*) FROM \"") + table.name + "\"");
    if (!count.has_value()) continue;
    entry["rows"] = *count;

    if (table.pk_column != nullptr) {
      const std::string pk_str(table.pk_column);
      auto              min_val =
          query_int64(std::string("SELECT MIN(\"") + pk_str + "\") FROM \"" + table.name + "\"");
      auto max_val =
          query_int64(std::string("SELECT MAX(\"") + pk_str + "\") FROM \"" + table.name + "\"");
      if (min_val.has_value() && max_val.has_value()) {
        entry["min_id"] = *min_val;
        entry["max_id"] = *max_val;
      }
    }

    tables[table.name] = entry;
  }

  summary["tables"] = tables;
  return summary;
}

}  // namespace

ProjectService::ProjectService(const std::filesystem::path& db_path,
                               const std::filesystem::path& meta_path, ProjectOpenMode open_mode)
    : db_path_(db_path), meta_path_(meta_path) {
  const auto create_new_project = [this]() {
    storage_service_ = std::make_shared<StorageService>(db_path_);
    RecreateSleeveService(0);
    pool_service_   = std::make_shared<ImagePoolService>(storage_service_, 0);
    filter_service_ = std::make_shared<SleeveFilterService>(storage_service_);
    RegisterSemanticSearchProvider();
    browse_service_  = std::make_shared<AlbumBrowseService>(sleeve_service_, filter_service_);
    package_service_ = std::make_shared<ProjectPackageService>();

    project_uuid_    = GenerateProjectUUID();
  };

  switch (open_mode) {
    case ProjectOpenMode::kLoadExisting:
      LoadProject(meta_path);
      return;
    case ProjectOpenMode::kCreateNew:
      create_new_project();
      return;
    case ProjectOpenMode::kLoadOrCreate:
      break;
  }

  std::error_code ec;
  const bool      meta_exists = std::filesystem::exists(meta_path, ec);
  if (ec) {
    throw std::runtime_error("Failed to inspect project metadata path");
  }
  if (meta_exists) {
    LoadProject(meta_path);
    return;
  }
  create_new_project();
}

ProjectService::~ProjectService() {
  package_service_.reset();
  std::shared_ptr<SemanticRuntimeService> runtime;
  {
    std::lock_guard lock(semantic_runtime_mutex_);
    runtime = std::move(semantic_runtime_service_);
  }
  if (runtime) {
    runtime->StopForProjectClose();
    runtime.reset();
  }
  browse_service_.reset();
  filter_service_.reset();
  pool_service_.reset();
  sleeve_service_.reset();
  storage_service_.reset();
}

void ProjectService::SaveProject(const std::filesystem::path& meta_path) {
  if (!sleeve_service_) {
    throw std::runtime_error("SleeveService is not initialized");
  }

  meta_path_ = meta_path;

  nlohmann::json metadata;
  metadata["db_path"]              = conv::ToBytes(db_path_.wstring());
  metadata["meta_path"]            = conv::ToBytes(meta_path_.wstring());
  metadata["project_uuid"]         = project_uuid_;
  metadata["project_file_version"] = std::string(project_pack::kProjectFileVersion);
  metadata["project_file_min_supported_version"] =
      std::string(project_pack::kMinSupportedProjectFileVersion);
  metadata["project_file_max_supported_version"] =
      std::string(project_pack::kMaxSupportedProjectFileVersion);
  metadata["start_id"]            = sleeve_service_->GetCurrentID();
  metadata["image_pool_start_id"] = pool_service_->GetCurrentID();
  metadata["data_summary"]        = ComputeProjectDataSummary(*storage_service_);

  std::ofstream file(meta_path_);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open meta file for writing");
  }
  file << metadata.dump(4);
  file.close();
}

void ProjectService::LoadProject(const std::filesystem::path& meta_path) {
  std::shared_ptr<SemanticRuntimeService> previous_runtime;
  {
    std::lock_guard lock(semantic_runtime_mutex_);
    previous_runtime = std::move(semantic_runtime_service_);
  }
  if (previous_runtime) {
    previous_runtime->StopForProjectClose();
    previous_runtime.reset();
  }

  std::ifstream file(meta_path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open meta file for reading");
  }

  nlohmann::json metadata;
  file >> metadata;

  if (!metadata.contains("project_file_version") ||
      !metadata.at("project_file_version").is_string()) {
    throw std::runtime_error("Project metadata version is missing");
  }
  if (!project_pack::ProjectVersionIsSupported(
          metadata.at("project_file_version").get<std::string>())) {
    throw std::runtime_error("Project metadata version is not supported");
  }

  if (metadata.contains("project_uuid") && metadata.at("project_uuid").is_string()) {
    project_uuid_ = metadata.at("project_uuid").get<std::string>();
  } else {
    project_uuid_ = GenerateProjectUUID();
  }

  if (!metadata.contains("db_path")) {
    throw std::runtime_error("Project metadata missing db_path");
  }

  db_path_   = std::filesystem::path(conv::FromBytes(metadata.at("db_path")));
  meta_path_ = meta_path;
  if (metadata.contains("meta_path")) {
    const auto stored_meta_path = std::filesystem::path(conv::FromBytes(metadata.at("meta_path")));
    if (!stored_meta_path.empty()) {
      meta_path_ = stored_meta_path;
    }
  }

  if (db_path_.empty()) {
    throw std::runtime_error("Project metadata db_path is empty");
  }
  if (!std::filesystem::exists(db_path_)) {
    throw std::runtime_error("Project database file does not exist");
  }

  // Parse data_summary for diagnostic purposes — mismatch is a
  // warning, not a load failure. Old projects carry db_checksum_xxh3_64
  // instead; that field is silently ignored here.
  std::optional<nlohmann::json> expected_summary;
  if (metadata.contains("data_summary") && metadata.at("data_summary").is_object()) {
    expected_summary = metadata.at("data_summary");
  }

  sl_element_id_t start_id = 0;
  if (metadata.contains("start_id")) {
    start_id = static_cast<sl_element_id_t>(metadata.at("start_id"));
  }

  sl_element_id_t image_pool_start_id =
      metadata.contains("image_pool_start_id")
          ? static_cast<sl_element_id_t>(metadata.at("image_pool_start_id"))
          : 0;

  storage_service_ = std::make_shared<StorageService>(db_path_);

  if (expected_summary.has_value()) {
    try {
      nlohmann::json actual_summary = ComputeProjectDataSummary(*storage_service_);
      if (actual_summary != *expected_summary) {
        std::cerr << "[Alcedo] Project data summary differs from saved metadata. "
                     "This may indicate data changes since the project was saved.\n";
      }
    } catch (const std::exception& e) {
      std::cerr << "[Alcedo] Unable to compute project data summary for comparison: " << e.what()
                << "\n";
    } catch (...) {
      std::cerr << "[Alcedo] Unable to compute project data summary for comparison.\n";
    }
  }

  RecreateSleeveService(start_id);
  pool_service_   = std::make_shared<ImagePoolService>(storage_service_, image_pool_start_id);
  filter_service_ = std::make_shared<SleeveFilterService>(storage_service_);
  RegisterSemanticSearchProvider();
  browse_service_  = std::make_shared<AlbumBrowseService>(sleeve_service_, filter_service_);
  package_service_ = std::make_shared<ProjectPackageService>();
}

void ProjectService::RecreateSleeveService(sl_element_id_t start_id) {
  if (!storage_service_) {
    throw std::runtime_error("StorageService is not initialized");
  }
  sleeve_service_ = std::make_shared<SleeveServiceImpl>(storage_service_, db_path_, start_id);
}

void ProjectService::RegisterSemanticSearchProvider() {
  if (!filter_service_ || !storage_service_) {
    return;
  }
  filter_service_->SetSemanticSearchProvider(std::make_shared<ProjectSemanticSearchProvider>(
      storage_service_, [this]() { return GetSemanticRuntimeService(); }));
}

auto ProjectService::GetSemanticRuntimeService() const -> std::shared_ptr<SemanticRuntimeService> {
  std::lock_guard lock(semantic_runtime_mutex_);
  if (!semantic_runtime_service_) {
    semantic_runtime_service_ = MakeSemanticRuntimeService();
  }
  return semantic_runtime_service_;
}
};  // namespace alcedo
