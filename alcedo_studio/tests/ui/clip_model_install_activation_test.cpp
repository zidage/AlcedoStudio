//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file clip_model_install_activation_test.cpp
/// @brief CLIP model install per profile and activation per project (AI Models plan, AM3).
///
/// Install, cancel, and remove run ModelDownloadController through the real DownloadService
/// with an aria2 stand-in that answers its JSON-RPC calls. Activation runs
/// SemanticGenerationController of ApplicationModuleHost against an open project.

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSettings>
#include <QSignalSpy>
#include <QVariantMap>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <json.hpp>
#include <memory>
#include <string>
#include <vector>

#include "app/download_service.hpp"
#include "app/model_asset_catalog.hpp"
#include "app/model_download_service.hpp"
#include "app/project_service.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/semantic/semantic_label_config.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/model_download_controller.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"

#ifndef ALCEDO_RPC_FAKE_ARIA2_PATH
#define ALCEDO_RPC_FAKE_ARIA2_PATH ""
#endif

namespace alcedo::ui::test {
namespace {

namespace fs             = std::filesystem;

constexpr auto kEndpoint = "https://models.example";

auto           WaitUntil(const std::function<bool()>& condition, int timeout_ms = 20000) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    ProcessEvents(20);
  }
  return true;
}

auto RowOf(const ModelDownloadController& controller, const QString& profile_id) -> QVariantMap {
  for (const auto& row : controller.Models()) {
    const auto map = row.toMap();
    if (map.value(QStringLiteral("profileId")).toString() == profile_id) {
      return map;
    }
  }
  return {};
}

void WriteFile(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << bytes;
}

/// Restores the environment variables and settings keys that a test changes. QSettings reads
/// and writes nothing without an organization name; the test process uses its own.
class ScopedTestEnvironment final {
 public:
  ScopedTestEnvironment() {
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ClipModelInstallActivationTest"));
    for (const char* name :
         {"ALCEDO_ARIA2C_BINARY", "ALCEDO_FAKE_ARIA2_SOURCE_DIR", "ALCEDO_FAKE_ARIA2_HOLD"}) {
      saved_env_.push_back({name, qgetenv(name)});
    }
  }
  ~ScopedTestEnvironment() {
    for (const auto& [name, value] : saved_env_) {
      if (value.isNull()) {
        qunsetenv(name);
      } else {
        qputenv(name, value);
      }
    }
    QSettings settings;
    settings.remove(QLatin1String(kSemanticModelDirectoryKey));
    settings.remove(QLatin1String(kSemanticModelEndpointPresetKey));
    settings.remove(QLatin1String(kSemanticCustomModelEndpointKey));
  }

 private:
  std::vector<std::pair<const char*, QByteArray>> saved_env_;
};

// ── Install, cancel, remove per profile ────────────────────────────────────

/// Two small test profiles served by the aria2 stand-in from a source directory laid out as
/// <source>/<repo>/resolve/<revision>/<remote path>.
class ClipModelDownloadControllerTest : public ApplicationModuleHostTestFixture {
 protected:
  void SetUp() override {
    ApplicationModuleHostTestFixture::SetUp();
    source_dir_ = temp_dir_ / "source";
    model_dir_  = temp_dir_ / "models";
    qputenv("ALCEDO_ARIA2C_BINARY", QByteArrayLiteral(ALCEDO_RPC_FAKE_ARIA2_PATH));
    qputenv("ALCEDO_FAKE_ARIA2_SOURCE_DIR", PathToQString(source_dir_).toUtf8());
    qunsetenv("ALCEDO_FAKE_ARIA2_HOLD");
    QSettings settings;
    settings.setValue(QLatin1String(kSemanticModelDirectoryKey), PathToQString(model_dir_));
    settings.setValue(QLatin1String(kSemanticModelEndpointPresetKey), QStringLiteral("custom"));
    settings.setValue(QLatin1String(kSemanticCustomModelEndpointKey),
                      QString::fromLatin1(kEndpoint));

    profiles_.push_back(MakeProfile("test-clip-a", "test/clip-a"));
    profiles_.push_back(MakeProfile("test-clip-b", "test/clip-b"));
    downloads_  = std::make_unique<DownloadService>();
    service_    = std::make_unique<ModelDownloadService>(*downloads_, profiles_);
    tasks_      = std::make_unique<BackgroundTaskController>();
    controller_ = std::make_unique<ModelDownloadController>(*service_, tasks_.get());
  }

  void TearDown() override {
    controller_.reset();
    tasks_.reset();
    service_.reset();
    downloads_.reset();
    ApplicationModuleHostTestFixture::TearDown();
  }

  /// A profile with two files whose sizes and SHA-256 the download checks.
  auto MakeProfile(const char* profile_id, const char* repo_id) -> ModelProfileSpec {
    ModelProfileSpec profile{};
    profile.profile_id                 = profile_id;
    profile.display_name               = profile_id;
    profile.model_id                   = repo_id;
    profile.revision                   = "rev1";
    profile.engine_profile_id          = "test-engine";
    profile.inference_backend          = ModelInferenceBackend::kOnnxRuntime;
    profile.language                   = ModelLanguage::kEn;
    profile.embedding_dimension        = 512;
    profile.native_embedding_dimension = 512;
    profile.image_size                 = 256;
    profile.embedding_transform        = "l2_normalize";
    profile.license_name               = "Apache-2.0";
    profile.license_url                = "https://www.apache.org/licenses/LICENSE-2.0";
    for (const char* remote_path : {"onnx/model.onnx", "tokenizer.json"}) {
      const std::string bytes = std::string(profile_id) + ":" + remote_path + ":payload";
      WriteFile(source_dir_ / repo_id / "resolve" / "rev1" / remote_path, bytes);
      hashes_.push_back(
          QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256)
              .toHex()
              .toStdString());
      profile.assets.push_back(ModelAssetSpec{ModelAssetRole::kModelConfig, repo_id, "rev1",
                                              remote_path, remote_path, bytes.size(),
                                              hashes_.back().c_str()});
    }
    return profile;
  }

  auto Status(const char* profile_id) -> QString {
    return RowOf(*controller_, QString::fromLatin1(profile_id))
        .value(QStringLiteral("status"))
        .toString();
  }

  ScopedTestEnvironment                     environment_;
  fs::path                                  source_dir_;
  fs::path                                  model_dir_;
  std::deque<std::string>                   hashes_;
  std::vector<ModelProfileSpec>             profiles_;
  std::unique_ptr<DownloadService>          downloads_;
  std::unique_ptr<ModelDownloadService>     service_;
  std::unique_ptr<BackgroundTaskController> tasks_;
  std::unique_ptr<ModelDownloadController>  controller_;
};

TEST_F(ClipModelDownloadControllerTest, EachProfileInstallsIntoItsOwnDirectory) {
  ASSERT_EQ(controller_->Models().size(), 2);
  EXPECT_EQ(Status("test-clip-a"), QStringLiteral("notInstalled"));
  EXPECT_EQ(Status("test-clip-b"), QStringLiteral("notInstalled"));

  ASSERT_TRUE(controller_->InstallModel(QStringLiteral("test-clip-a")));
  EXPECT_EQ(Status("test-clip-a"), QStringLiteral("downloading"));
  EXPECT_EQ(controller_->DownloadingProfileId(), QStringLiteral("test-clip-a"));
  ASSERT_TRUE(WaitUntil([&] { return !controller_->ModelDownloadRunning(); }));

  const auto row_a = RowOf(*controller_, QStringLiteral("test-clip-a"));
  EXPECT_EQ(row_a.value(QStringLiteral("status")).toString(), QStringLiteral("installed"))
      << row_a.value(QStringLiteral("error")).toString().toStdString();
  EXPECT_TRUE(fs::exists(model_dir_ / "test-clip-a" / "onnx" / "model.onnx"));
  EXPECT_TRUE(fs::exists(model_dir_ / "test-clip-a" / kSemanticResolvedManifestFile));
  EXPECT_FALSE(fs::exists(model_dir_ / ".test-clip-a.download"));
  EXPECT_TRUE(controller_->IsModelInstalled(QStringLiteral("test-clip-a")));
  EXPECT_EQ(Status("test-clip-b"), QStringLiteral("notInstalled"));
  EXPECT_FALSE(fs::exists(model_dir_ / "test-clip-b"));

  ASSERT_TRUE(controller_->InstallModel(QStringLiteral("test-clip-b")));
  ASSERT_TRUE(WaitUntil([&] { return !controller_->ModelDownloadRunning(); }));
  EXPECT_EQ(Status("test-clip-a"), QStringLiteral("installed"));
  EXPECT_EQ(Status("test-clip-b"), QStringLiteral("installed"));
  QString    error;
  const auto manifest = controller_->LoadResolvedManifest(QStringLiteral("test-clip-b"), &error);
  ASSERT_TRUE(manifest.has_value()) << error.toStdString();
  EXPECT_EQ(manifest->model_id, "test/clip-b");
}

TEST_F(ClipModelDownloadControllerTest, CancelKeepsProfileNotInstalledAndOneDownloadRunsAtATime) {
  qputenv("ALCEDO_FAKE_ARIA2_HOLD", QByteArrayLiteral("1"));
  ASSERT_TRUE(controller_->InstallModel(QStringLiteral("test-clip-a")));
  ASSERT_EQ(tasks_->Tasks().size(), 1);
  EXPECT_EQ(tasks_->Tasks().front().toMap().value(QStringLiteral("kind")).toString(),
            QStringLiteral("modelDownload"));

  // One download at a time: the second profile reports it on its own row.
  EXPECT_FALSE(controller_->InstallModel(QStringLiteral("test-clip-b")));
  const auto row_b = RowOf(*controller_, QStringLiteral("test-clip-b"));
  EXPECT_EQ(row_b.value(QStringLiteral("error")).toString(),
            QStringLiteral("Another download is running."));
  EXPECT_EQ(row_b.value(QStringLiteral("status")).toString(), QStringLiteral("notInstalled"));
  // A profile cannot be removed while it downloads.
  EXPECT_FALSE(controller_->RemoveModel(QStringLiteral("test-clip-a")));
  // Only the downloading profile cancels.
  EXPECT_FALSE(controller_->CancelInstall(QStringLiteral("test-clip-b")));

  ProcessEvents(300);
  ASSERT_TRUE(controller_->CancelInstall(QStringLiteral("test-clip-a")));
  EXPECT_EQ(Status("test-clip-a"), QStringLiteral("canceling"));
  ASSERT_TRUE(WaitUntil([&] { return !controller_->ModelDownloadRunning(); }));

  const auto row_a = RowOf(*controller_, QStringLiteral("test-clip-a"));
  EXPECT_EQ(row_a.value(QStringLiteral("status")).toString(), QStringLiteral("notInstalled"));
  EXPECT_EQ(row_a.value(QStringLiteral("message")).toString(),
            QStringLiteral("Model download canceled."));
  EXPECT_TRUE(row_a.value(QStringLiteral("error")).toString().isEmpty());
  EXPECT_FALSE(fs::exists(model_dir_ / "test-clip-a"));
  EXPECT_EQ(tasks_->Tasks().front().toMap().value(QStringLiteral("state")).toString(),
            QStringLiteral("canceled"));

  // After the cancel the next profile can download.
  qunsetenv("ALCEDO_FAKE_ARIA2_HOLD");
  ASSERT_TRUE(controller_->InstallModel(QStringLiteral("test-clip-b")));
  ASSERT_TRUE(WaitUntil([&] { return !controller_->ModelDownloadRunning(); }));
  EXPECT_EQ(Status("test-clip-b"), QStringLiteral("installed"));
}

TEST_F(ClipModelDownloadControllerTest, RemoveDeletesOnlyThatProfileAndReportsIt) {
  for (const char* id : {"test-clip-a", "test-clip-b"}) {
    ASSERT_TRUE(controller_->InstallModel(QString::fromLatin1(id)));
    ASSERT_TRUE(WaitUntil([&] { return !controller_->ModelDownloadRunning(); }));
    ASSERT_EQ(Status(id), QStringLiteral("installed"));
  }
  QSignalSpy removed(controller_.get(), &ModelDownloadController::ModelRemoved);

  ASSERT_TRUE(controller_->RemoveModel(QStringLiteral("test-clip-a")));
  ASSERT_EQ(removed.count(), 1);
  EXPECT_EQ(removed.at(0).at(0).toString(), QStringLiteral("test-clip-a"));
  EXPECT_EQ(Status("test-clip-a"), QStringLiteral("notInstalled"));
  EXPECT_FALSE(fs::exists(model_dir_ / "test-clip-a"));
  EXPECT_EQ(Status("test-clip-b"), QStringLiteral("installed"));
  EXPECT_TRUE(fs::exists(model_dir_ / "test-clip-b" / "tokenizer.json"));
}

TEST_F(ClipModelDownloadControllerTest, RowsShowLicensesOfTheModelCards) {
  ModelDownloadService    catalog_service(*downloads_);
  ModelDownloadController catalog_controller(catalog_service, tasks_.get());
  const auto              rows = catalog_controller.Models();
  ASSERT_EQ(rows.size(), static_cast<qsizetype>(SemanticModelProfiles().size()));
  for (const auto& value : rows) {
    const auto row = value.toMap();
    EXPECT_FALSE(row.value(QStringLiteral("licenseName")).toString().isEmpty());
    EXPECT_TRUE(row.value(QStringLiteral("licenseUrl")).toString().startsWith("https://"));
  }
#ifndef __APPLE__
  EXPECT_EQ(RowOf(catalog_controller, QStringLiteral("mobileclip2-s2-en"))
                .value(QStringLiteral("licenseName"))
                .toString(),
            QStringLiteral("apple-amlr"));
  EXPECT_EQ(RowOf(catalog_controller, QStringLiteral("jina-clip-v2-int8-multilingual"))
                .value(QStringLiteral("licenseName"))
                .toString(),
            QStringLiteral("cc-by-nc-4.0"));
#endif
}

// ── Activation per project ─────────────────────────────────────────────────

#ifndef __APPLE__
constexpr auto kProfileId = "mobileclip2-s2-en";
#else
constexpr auto kProfileId = "siglip2-base-256-coreml-macos";
#endif

/// ApplicationModuleHost with a model directory in the test folder. An "installed" profile is a
/// tree of files with the catalog sizes and a resolved manifest (activation checks sizes only).
class ClipModelActivationTest : public ApplicationModuleHostTestFixture {
 protected:
  void SetUp() override {
    ApplicationModuleHostTestFixture::SetUp();
    model_dir_ = temp_dir_ / "models";
    QSettings{}.setValue(QLatin1String(kSemanticModelDirectoryKey), PathToQString(model_dir_));
    host_ = std::make_unique<ApplicationModuleHost>();
  }

  void TearDown() override {
    host_.reset();
    ApplicationModuleHostTestFixture::TearDown();
  }

  auto Downloads() -> ModelDownloadController& { return *host_->model_download(); }
  auto Semantic() -> SemanticGenerationController& { return *host_->semantic_generation(); }

  void InstallStubProfile(const char* profile_id) {
    const auto* profile = FindSemanticProfile(profile_id);
    ASSERT_NE(profile, nullptr);
    const auto root = model_dir_ / profile_id;
    for (const auto& asset : profile->assets) {
      const auto path = root / asset.local_path;
      fs::create_directories(path.parent_path());
      std::ofstream out(path, std::ios::binary);
      out.seekp(static_cast<std::streamoff>(asset.size_bytes) - 1);
      out.put('\0');
    }
    ASSERT_FALSE(WriteResolvedManifest(*profile, root).has_value());
    Downloads().RefreshModels();
    ASSERT_TRUE(Downloads().IsModelInstalled(QString::fromLatin1(profile_id)));
  }

  auto ModelKey(const char* profile_id) -> std::string {
    const auto* profile = FindSemanticProfile(profile_id);
    return std::string(profile->model_id) + "@" + profile->revision;
  }

  /// The project registered @p profile_id before and cached its label prototypes, so
  /// activation needs no sidecar.
  void CacheLabelPrototypes(const char* profile_id) {
    const auto* profile = FindSemanticProfile(profile_id);
    auto        project = host_->project()->handler().project();
    auto&       storage = *project->GetStorage();
    const auto  language =
        SemanticLabelLanguageForModel(profile->profile_id, ToString(profile->language));
    const std::string hash      = SemanticPromptConfigHashForLanguage(language);
    const std::string model_key = ModelKey(profile_id);
    std::string       error;
    ASSERT_TRUE(storage.GetSemanticModelRegistry().UpsertModel(
        SemanticModelRecord{.model_key_          = model_key,
                            .model_id_           = profile->model_id,
                            .revision_           = profile->revision,
                            .embedding_dim_      = static_cast<int>(profile->embedding_dimension),
                            .image_size_         = static_cast<int>(profile->image_size),
                            .profile_id_         = profile->profile_id,
                            .prompt_config_hash_ = hash,
                            .active_             = false},
        &error))
        << error;
    std::vector<SemanticLabelPrototypeRecord> prototypes;
    for (const auto& query : storage.GetSemanticLabelStore().ListLabelQueries(hash, &error)) {
      std::vector<float> embedding(profile->embedding_dimension, 0.0F);
      embedding[prototypes.size() % embedding.size()] = 1.0F;
      prototypes.push_back(SemanticLabelPrototypeRecord{model_key, query.label_, hash, embedding});
    }
    ASSERT_FALSE(prototypes.empty());
    ASSERT_TRUE(storage.GetSemanticLabelStore().UpsertLabelPrototypes(prototypes, &error)) << error;
  }

  auto RowError(const char* profile_id) -> QString {
    return RowOf(Downloads(), QString::fromLatin1(profile_id))
        .value(QStringLiteral("error"))
        .toString();
  }

  auto ActiveKey() -> std::string {
    return host_->project()
        ->handler()
        .project()
        ->GetStorage()
        ->GetSemanticModelRegistry()
        .ActiveModelKey();
  }

  ScopedTestEnvironment                  environment_;
  fs::path                               model_dir_;
  std::unique_ptr<ApplicationModuleHost> host_;
};

TEST_F(ClipModelActivationTest, ActivationWithoutOpenProjectFailsWithReason) {
  InstallStubProfile(kProfileId);
  EXPECT_FALSE(Semantic().ActivateModel(QString::fromLatin1(kProfileId)));
  EXPECT_EQ(RowError(kProfileId),
            QStringLiteral("Open a project before activating a semantic model."));
  EXPECT_FALSE(Semantic().ModelActivationRunning());
}

TEST_F(ClipModelActivationTest, ActivationOfModelThatIsNotInstalledFailsWithReason) {
  ASSERT_TRUE(CreateTestProject(*host_));
  EXPECT_FALSE(Semantic().ActivateModel(QString::fromLatin1(kProfileId)));
  EXPECT_TRUE(RowError(kProfileId).startsWith(QStringLiteral("Cannot activate model: ")))
      << RowError(kProfileId).toStdString();
  EXPECT_TRUE(ActiveKey().empty());
}

TEST_F(ClipModelActivationTest, ActivationOfInstalledModelSetsProjectActiveKey) {
  ASSERT_TRUE(CreateTestProject(*host_));
  InstallStubProfile(kProfileId);
  CacheLabelPrototypes(kProfileId);

  ASSERT_TRUE(Semantic().ActivateModel(QString::fromLatin1(kProfileId)));
  EXPECT_EQ(Semantic().ActivatingModelProfileId(), QString::fromLatin1(kProfileId));
  // A second request while the first runs fails with the reason.
  EXPECT_FALSE(Semantic().ActivateModel(QString::fromLatin1(kProfileId)));
  EXPECT_EQ(RowError(kProfileId), QStringLiteral("Another model activation is running."));

  ASSERT_TRUE(WaitUntil([&] { return !Semantic().ModelActivationRunning(); }));
  EXPECT_TRUE(RowError(kProfileId).isEmpty()) << RowError(kProfileId).toStdString();
  EXPECT_EQ(ActiveKey(), ModelKey(kProfileId));
  EXPECT_EQ(Semantic().ActiveModelProfileId(), QString::fromLatin1(kProfileId));
  EXPECT_TRUE(Semantic().ActivatingModelProfileId().isEmpty());

  // The active model of the project is the sidecar identity.
  const auto options = Semantic().RuntimeOptionsForCurrentSidecarSnapshot(false);
  EXPECT_EQ(options.model_id, FindSemanticProfile(kProfileId)->model_id);
  EXPECT_EQ(options.revision, FindSemanticProfile(kProfileId)->revision);
  EXPECT_EQ(options.model_root, Downloads().ModelRootForProfile(QString::fromLatin1(kProfileId)));

  // An active model is not activated again.
  EXPECT_FALSE(Semantic().ActivateModel(QString::fromLatin1(kProfileId)));
  EXPECT_EQ(RowError(kProfileId), QStringLiteral("This model is already active for this project."));
}

TEST_F(ClipModelActivationTest, WithoutActiveModelSidecarStartsWithoutModelId) {
  ASSERT_TRUE(CreateTestProject(*host_));
  InstallStubProfile(kProfileId);  // Installed is not active.
  const auto options = Semantic().RuntimeOptionsForCurrentSidecarSnapshot(true);
  EXPECT_TRUE(options.model_id.empty());
  EXPECT_TRUE(options.revision.empty());
  EXPECT_FALSE(options.require_model_info);
  EXPECT_EQ(options.model_root, fs::path(Downloads().ModelDownloadDirectory().toStdU16String()));
}

TEST_F(ClipModelActivationTest, RemovingActiveModelLeavesProjectWithoutActiveModel) {
  ASSERT_TRUE(CreateTestProject(*host_));
  InstallStubProfile(kProfileId);
  CacheLabelPrototypes(kProfileId);
  ASSERT_TRUE(Semantic().ActivateModel(QString::fromLatin1(kProfileId)));
  ASSERT_TRUE(WaitUntil([&] { return !Semantic().ModelActivationRunning(); }));
  ASSERT_EQ(ActiveKey(), ModelKey(kProfileId));

  ASSERT_TRUE(Downloads().RemoveModel(QString::fromLatin1(kProfileId)));
  EXPECT_TRUE(ActiveKey().empty());
  EXPECT_TRUE(Semantic().ActiveModelProfileId().isEmpty());
  EXPECT_FALSE(Downloads().IsModelInstalled(QString::fromLatin1(kProfileId)));
  EXPECT_TRUE(Semantic().RuntimeOptionsForCurrentSidecarSnapshot(false).model_id.empty());
  // The rows stay until the save purge drops the data of the removed model.
  EXPECT_TRUE(
      host_->project()->handler().project()->GetStorage()->GetSemanticModelRegistry().HasModel(
          ModelKey(kProfileId)));
}

}  // namespace
}  // namespace alcedo::ui::test
