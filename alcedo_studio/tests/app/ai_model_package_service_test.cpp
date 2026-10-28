//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_model_package_service.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QVariantMap>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <json.hpp>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "app/package_archive.hpp"
#include "ui/alcedo_main/album_backend/ai_model_download_task_publisher.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

extern "C" {
#include <ed25519.h>
}

#ifndef ALCEDO_FAKE_ARIA2_PATH
#define ALCEDO_FAKE_ARIA2_PATH ""
#endif

namespace alcedo::test {
namespace {

namespace fs                         = std::filesystem;
using Json                           = nlohmann::json;

constexpr std::string_view kPlatform = "windows-x86_64";
const QUrl    kFeedUrl(QStringLiteral("https://static.aoraw.org/ai-models/v1/manifest.json"));
const QString kId = QStringLiteral("alcedo-ai-mask");

// ── File and archive fixtures ───────────────────────────────────────────────

auto          ReadBytes(const fs::path& path) -> std::string {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteBytes(const fs::path& path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

auto Sha256Hex(std::string_view bytes) -> std::string {
  return QCryptographicHash::hash(
             QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
             QCryptographicHash::Sha256)
      .toHex()
      .toStdString();
}

struct ArchiveItem {
  enum class Kind { kFile, kDirectory };
  std::string path;
  std::string bytes;
  Kind        kind = Kind::kFile;
};

/// Write a 7z (LZMA2) archive with libarchive; entries are written as given, so a test can
/// place unsafe paths that the installer must reject.
void Write7z(const fs::path& path, const std::vector<ArchiveItem>& items) {
  fs::create_directories(path.parent_path());
  archive* writer = archive_write_new();
  ASSERT_EQ(archive_write_set_format_7zip(writer), ARCHIVE_OK);
  ASSERT_EQ(archive_write_set_format_option(writer, "7zip", "compression", "lzma2"), ARCHIVE_OK);
#ifdef _WIN32
  ASSERT_EQ(archive_write_open_filename_w(writer, path.wstring().c_str()), ARCHIVE_OK);
#else
  ASSERT_EQ(archive_write_open_filename(writer, path.c_str()), ARCHIVE_OK);
#endif
  for (const ArchiveItem& item : items) {
    archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname_utf8(entry, item.path.c_str());
    if (item.kind == ArchiveItem::Kind::kDirectory) {
      archive_entry_set_filetype(entry, AE_IFDIR);
      archive_entry_set_perm(entry, 0755);
    } else {
      archive_entry_set_filetype(entry, AE_IFREG);
      archive_entry_set_perm(entry, 0644);
      archive_entry_set_size(entry, static_cast<la_int64_t>(item.bytes.size()));
    }
    ASSERT_EQ(archive_write_header(writer, entry), ARCHIVE_OK) << archive_error_string(writer);
    if (item.kind == ArchiveItem::Kind::kFile && !item.bytes.empty()) {
      archive_write_data(writer, item.bytes.data(), item.bytes.size());
    }
    archive_entry_free(entry);
  }
  ASSERT_EQ(archive_write_close(writer), ARCHIVE_OK);
  archive_write_free(writer);
}

/// One package version and revision: archive bytes on disk and its signed feed entry.
struct BuiltPackage {
  std::uint64_t                      version = 0;
  std::string                        revision;
  fs::path                           archive;
  Json                               entry;
  /// Package-relative path -> bytes of every file that the installation holds.
  std::map<std::string, std::string> files;
};

/// The files of an AI Mask package; @p shade changes the model bytes between revisions.
auto PackageFiles(std::uint64_t version, std::string_view revision, int shade)
    -> std::map<std::string, std::string> {
  const auto model = [shade](std::string_view name, std::size_t size) {
    std::string bytes(size, static_cast<char>('a' + shade));
    bytes.replace(0, name.size(), name);
    return bytes;
  };
  return {{"point_encoder.onnx", model("encoder", 4096)},
          {"point_decoder.onnx", model("decoder", 2048)},
          {"s3od.mlmodelc/model.mil", model("mil", 512)},
          {"s3od.mlmodelc/weights/weight.bin", model("weights", 1024)},
          {"people_sky_head.json", R"({"thresholds":[0.5]})"},
          {"licenses/Apache-2.0.txt", "Apache License 2.0\n"},
          {"licenses/GPL-3.0.txt", "GNU General Public License 3.0\n"},
          {"NOTICE.txt", "Model sources and training data.\n"},
          {"model_set.json", Json{{"schema", 1},
                                  {"kind", "alcedo-ai-model-set"},
                                  {"id", kId.toStdString()},
                                  {"model_set_version", version},
                                  {"revision", revision},
                                  {"platform", kPlatform}}
                                 .dump(1)}};
}

auto InventoryBytes(std::uint64_t version, std::string_view revision,
                    const std::map<std::string, std::string>& files) -> std::string {
  Json list = Json::array();
  for (const auto& [path, bytes] : files) {
    list.push_back({{"path", path}, {"size", bytes.size()}, {"sha256", Sha256Hex(bytes)}});
  }
  return Json{{"schema", 1},
              {"kind", "alcedo-ai-model-package-inventory"},
              {"id", kId.toStdString()},
              {"model_set_version", version},
              {"revision", revision},
              {"platform", kPlatform},
              {"files", list}}
      .dump(1);
}

/// @p omitted_path, when set, is listed by the inventory but left out of the archive.
auto BuildPackage(const fs::path& directory, std::uint64_t version, std::string_view revision,
                  int shade, const std::vector<ArchiveItem>& extra_items = {},
                  std::string_view omitted_path = {}) -> BuiltPackage {
  BuiltPackage package{version, std::string(revision)};
  package.files                      = PackageFiles(version, revision, shade);
  const std::string        inventory = InventoryBytes(version, revision, package.files);
  std::vector<ArchiveItem> items     = {{"s3od.mlmodelc", {}, ArchiveItem::Kind::kDirectory}};
  for (const auto& [path, bytes] : package.files) {
    if (path != omitted_path) items.push_back({path, bytes});
  }
  items.push_back({"package-inventory.json", inventory});
  for (const ArchiveItem& item : extra_items) items.push_back(item);
  const std::string name = "alcedo-ai-mask-v" + std::to_string(version) + "-" +
                           std::string(revision) + "-" + std::to_string(shade);
  package.archive = directory / (name + ".7z");
  Write7z(package.archive, items);

  std::uint64_t unpacked = 0;
  for (const auto& [path, bytes] : package.files) unpacked += bytes.size();
  const std::string archive_bytes = ReadBytes(package.archive);
  package.entry                   = {
      {"id", kId.toStdString()},
      {"name", "AI Mask"},
      {"model_set_version", version},
      {"revision", revision},
      {"licenses", Json::array({{{"name", "Apache-2.0"}, {"file", "licenses/Apache-2.0.txt"}},
                                                  {{"name", "GPL-3.0"}, {"file", "licenses/GPL-3.0.txt"}}})},
      {"artifacts",
                         {{kPlatform,
                           {{"url", "https://static.aoraw.org/ai-models/v1/packages/alcedo-ai-mask/v" +
                                        std::to_string(version) + "/" + std::string(revision) + "/" + name + ".7z"},
                            {"size", archive_bytes.size()},
                            {"sha256", Sha256Hex(archive_bytes)},
                            {"file_count", package.files.size()},
                            {"unpacked_bytes", unpacked},
                            {"inventory_sha256", Sha256Hex(inventory)}}}}}};
  return package;
}

struct SignedFeed {
  QByteArray json;
  QByteArray signature;
};

void KeyPair(QByteArray* public_key, std::array<unsigned char, 64>* private_key) {
  unsigned char seed[32] = {};
  for (std::size_t index = 0; index < sizeof(seed); ++index) {
    seed[index] = static_cast<unsigned char>(index * 5 + 3);
  }
  unsigned char key[32] = {};
  ed25519_create_keypair(key, private_key->data(), seed);
  *public_key = QByteArray(reinterpret_cast<const char*>(key), sizeof(key));
}

auto SignFeed(const Json& entries, std::uint64_t sequence) -> SignedFeed {
  const QByteArray              json = QByteArray::fromStdString(Json{
                   {"schema", 1},
                   {"kind", "alcedo-ai-model-packages"},
                   {"sequence", sequence},
                   {"packages", entries}}.dump());
  QByteArray                    public_key;
  std::array<unsigned char, 64> private_key{};
  KeyPair(&public_key, &private_key);
  unsigned char signature[64] = {};
  ed25519_sign(signature, reinterpret_cast<const unsigned char*>(json.constData()),
               static_cast<std::size_t>(json.size()),
               reinterpret_cast<const unsigned char*>(public_key.constData()), private_key.data());
  return {json, QByteArray(reinterpret_cast<const char*>(signature), 64).toBase64()};
}

// ── Transport and preference doubles ────────────────────────────────────────

/// Serves the current signed feed and counts every request.
struct FeedServer {
  SignedFeed feed;
  int        requests = 0;
};

/// Copies a local archive (selected by URL) to the requested destination. With `hold` set, a
/// transfer stays running until Cancel().
class LocalArchiveDownloader final : public PackageArchiveDownloader {
 public:
  struct State {
    std::map<std::string, fs::path> sources;
    int                             starts = 0;
    bool                            hold   = false;
    FinishedCallback                pending;
  };
  explicit LocalArchiveDownloader(std::shared_ptr<State> state) : state_(std::move(state)) {}

  auto Start(const DownloadRequest& request, ProgressCallback on_progress,
             FinishedCallback on_finished) -> bool override {
    if (state_->pending) return false;
    ++state_->starts;
    const DownloadItem item   = request.items.front();
    const auto         source = state_->sources.find(item.url.toString().toStdString());
    if (state_->hold) {
      state_->pending = std::move(on_finished);
      return true;
    }
    QTimer::singleShot(
        0, [item, source_path = source != state_->sources.end() ? source->second : fs::path{},
            on_progress, on_finished] {
          std::error_code error;
          const fs::path  destination = PackagePathFromUtf8(item.destination.toStdString());
          fs::create_directories(destination.parent_path(), error);
          fs::copy_file(source_path, destination, fs::copy_options::overwrite_existing, error);
          if (on_progress) on_progress(item.expected_size, item.expected_size);
          on_finished(!error, false, error ? QString::fromStdString(error.message()) : QString());
        });
    return true;
  }
  void Cancel(const QString&) override {
    if (!state_->pending) return;
    FinishedCallback finished = std::move(state_->pending);
    state_->pending           = nullptr;
    QTimer::singleShot(0, [finished] { finished(false, true, QStringLiteral("canceled")); });
  }

 private:
  std::shared_ptr<State> state_;
};

struct PreferenceValues {
  quint64 sequence           = 1;
  quint64 statement_accepted = 0;
};

class MemoryPreferences final : public AiModelPackagePreferences {
 public:
  explicit MemoryPreferences(std::shared_ptr<PreferenceValues> values)
      : values_(std::move(values)) {}
  [[nodiscard]] auto LoadTrustedSequence() const -> quint64 override { return values_->sequence; }
  void SaveTrustedSequence(quint64 sequence) override { values_->sequence = sequence; }
  [[nodiscard]] auto LoadAcceptedStatementVersion() const -> quint64 override {
    return values_->statement_accepted;
  }
  void SaveAcceptedStatementVersion(quint64 version) override {
    values_->statement_accepted = version;
  }

 private:
  std::shared_ptr<PreferenceValues> values_;
};

class Aria2Environment final {
 public:
  Aria2Environment() : previous_(qgetenv("ALCEDO_ARIA2C_BINARY")) {
    qputenv("ALCEDO_ARIA2C_BINARY", QByteArrayLiteral(ALCEDO_FAKE_ARIA2_PATH));
  }
  ~Aria2Environment() {
    if (previous_.isNull()) {
      qunsetenv("ALCEDO_ARIA2C_BINARY");
    } else {
      qputenv("ALCEDO_ARIA2C_BINARY", previous_);
    }
  }

 private:
  QByteArray previous_;
};

auto FindTask(const ui::BackgroundTaskController& tasks, const QString& task_id) -> QVariantMap {
  for (const QVariant& task : tasks.Tasks()) {
    if (task.toMap().value(QStringLiteral("id")).toString() == task_id) return task.toMap();
  }
  return {};
}

// ── Fixture ─────────────────────────────────────────────────────────────────

class AiModelPackageServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device device;
    base_ = fs::current_path() / "ai_model_package_service_test" / std::to_string(device());
    fs::create_directories(base_);
    base_                            = fs::weakly_canonical(base_);
    models_                          = base_ / "models";
    archives_                        = base_ / "archives";
    downloads_                       = std::make_shared<LocalArchiveDownloader::State>();
    feed_                            = std::make_shared<FeedServer>();
    preferences_                     = std::make_shared<PreferenceValues>();
    preferences_->statement_accepted = kAiModelDownloadStatementVersion;
    fs::create_directories(models_);
  }
  void TearDown() override {
    service_.reset();
    std::error_code error;
    fs::remove_all(base_, error);
  }

  static auto WaitUntil(const std::function<bool()>& condition) -> bool {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 20000) QTest::qWait(5);
    return condition();
  }

  auto MakeOptions(std::unique_ptr<PackageArchiveDownloader> downloader = nullptr)
      -> AiModelPackageServiceOptions {
    AiModelPackageServiceOptions options;
    options.feed_url = kFeedUrl;
    std::array<unsigned char, 64> unused{};
    KeyPair(&options.public_key, &unused);
    options.platform        = QString::fromLatin1(kPlatform.data(), kPlatform.size());
    options.model_directory = [models = models_] { return models; };
    options.fetch           = [feed = feed_](const QUrl&                              url, qint64,
                                   std::function<void(QByteArray, QString)> done) {
      ++feed->requests;
      const bool signature = url.path().endsWith(QStringLiteral(".sig"));
      QTimer::singleShot(0, [feed, signature, done] {
        done(signature ? feed->feed.signature : feed->feed.json, {});
      });
    };
    options.downloader =
        downloader ? std::move(downloader) : std::make_unique<LocalArchiveDownloader>(downloads_);
    options.preferences = std::make_unique<MemoryPreferences>(preferences_);
    return options;
  }

  void StartService(std::unique_ptr<PackageArchiveDownloader> downloader = nullptr) {
    service_ = std::make_unique<AiModelPackageService>(MakeOptions(std::move(downloader)));
  }

  /// Build a package, register its archive with the downloader, and return it.
  auto Publish(std::uint64_t version, std::string_view revision, int shade,
               const std::vector<ArchiveItem>& extra_items = {}) -> BuiltPackage {
    BuiltPackage package = BuildPackage(archives_, version, revision, shade, extra_items);
    downloads_->sources[ArtifactUrl(package)] = package.archive;
    return package;
  }

  static auto ArtifactUrl(const BuiltPackage& package) -> std::string {
    return package.entry["artifacts"][std::string(kPlatform)]["url"].get<std::string>();
  }

  void ServeFeed(const std::vector<const BuiltPackage*>& packages) {
    Json entries = Json::array();
    for (const BuiltPackage* package : packages) entries.push_back(package->entry);
    feed_->feed = SignFeed(entries, ++sequence_);
  }

  void Check() {
    ASSERT_TRUE(service_->CheckPackages());
    ASSERT_TRUE(WaitUntil([&] { return !service_->checking(); }));
  }

  /// Start the install of the offered entry and wait for its end. True when it committed.
  auto Install() -> bool {
    QSignalSpy finished(service_.get(), &AiModelPackageService::InstallFinished);
    if (!service_->InstallPackage(kId)) return false;
    if (!WaitUntil([&] { return finished.count() == 1; })) return false;
    return finished.at(0).at(1).toBool();
  }

  void InstallRevision(const BuiltPackage& package) {
    ServeFeed({&package});
    Check();
    ASSERT_TRUE(Install()) << State().error.toStdString();
    ASSERT_EQ(State().status, AiModelPackageStatus::kCurrent) << State().error.toStdString();
  }

  auto State() const -> const AiModelPackageService::PackageState& {
    return *service_->Package(kId);
  }

  auto Row() const -> QVariantMap { return service_->packages().front().toMap(); }

  auto InstalledDirectory(std::uint64_t version) const -> fs::path {
    return models_ / ("alcedo-ai-mask-v" + std::to_string(version));
  }

  /// Package-relative path -> bytes of every installed file except `installed.json` and
  /// `package-inventory.json`.
  auto InstalledFiles(std::uint64_t version) const -> std::map<std::string, std::string> {
    std::map<std::string, std::string> files;
    const fs::path                     root = InstalledDirectory(version);
    std::error_code                    error;
    for (fs::recursive_directory_iterator item(root, error), end; !error && item != end;
         item.increment(error)) {
      if (!item->is_regular_file()) continue;
      const std::string path = PackagePathToUtf8(item->path().lexically_relative(root));
      if (path == "installed.json" || path == "package-inventory.json") continue;
      files[path] = ReadBytes(item->path());
    }
    return files;
  }

  auto Receipt(std::uint64_t version) const -> std::optional<AiModelPackageReceipt> {
    return ParseAiModelPackageReceipt(ReadBytes(InstalledDirectory(version) / "installed.json"));
  }

  fs::path                                       base_;
  fs::path                                       models_;
  fs::path                                       archives_;
  std::shared_ptr<LocalArchiveDownloader::State> downloads_;
  std::shared_ptr<FeedServer>                    feed_;
  std::shared_ptr<PreferenceValues>              preferences_;
  std::uint64_t                                  sequence_ = 20261027000000ULL;
  std::unique_ptr<AiModelPackageService>         service_;
};

// ── Feed check ──────────────────────────────────────────────────────────────

TEST_F(AiModelPackageServiceTest, StartupReadsLocalInstallationWithoutRequestingFeed) {
  StartService();
  QTest::qWait(50);
  EXPECT_EQ(feed_->requests, 0);
  EXPECT_EQ(downloads_->starts, 0);
  EXPECT_TRUE(service_->enabled());
  EXPECT_FALSE(service_->checked());
  EXPECT_FALSE(service_->checking());
  ASSERT_EQ(service_->packages().size(), 1);
  const QVariantMap row = Row();
  EXPECT_EQ(row.value(QStringLiteral("id")).toString(), kId);
  EXPECT_EQ(row.value(QStringLiteral("status")).toString(), QStringLiteral("notInstalled"));
  EXPECT_EQ(row.value(QStringLiteral("action")).toString(), QString());
  EXPECT_FALSE(row.value(QStringLiteral("available")).toBool());
  EXPECT_EQ(row.value(QStringLiteral("minimumSupportedVersion")).toULongLong(),
            kAiMaskModelSetMinimumVersion);
  EXPECT_EQ(row.value(QStringLiteral("maximumSupportedVersion")).toULongLong(),
            kAiMaskModelSetMaximumVersion);
}

TEST_F(AiModelPackageServiceTest, CheckRequestsManifestAndSignatureOnceWithoutDownloading) {
  StartService();
  const BuiltPackage v1 = Publish(1, "r1", 0);
  ServeFeed({&v1});
  ASSERT_TRUE(service_->CheckPackages());
  EXPECT_FALSE(service_->CheckPackages());  // A running check is not started twice.
  ASSERT_TRUE(WaitUntil([&] { return !service_->checking(); }));
  EXPECT_EQ(feed_->requests, 2);  // Manifest and detached signature only.
  EXPECT_EQ(downloads_->starts, 0);
  EXPECT_TRUE(service_->checked());
  EXPECT_EQ(preferences_->sequence, sequence_);
  const QVariantMap row = Row();
  EXPECT_EQ(row.value(QStringLiteral("status")).toString(), QStringLiteral("notInstalled"));
  EXPECT_EQ(row.value(QStringLiteral("action")).toString(), QStringLiteral("install"));
  EXPECT_TRUE(row.value(QStringLiteral("available")).toBool());
  EXPECT_EQ(row.value(QStringLiteral("revision")).toString(), QStringLiteral("r1"));
  EXPECT_EQ(row.value(QStringLiteral("licenses")).toList().size(), 2);
  EXPECT_FALSE(fs::exists(InstalledDirectory(1)));
}

TEST_F(AiModelPackageServiceTest, TamperedOrOlderFeedKeepsStateAndReportsError) {
  StartService();
  const BuiltPackage v1 = Publish(1, "r1", 0);
  ServeFeed({&v1});
  feed_->feed.json.replace("\"r1\"", "\"r9\"");
  Check();
  EXPECT_FALSE(service_->checked());
  EXPECT_FALSE(service_->last_error().isEmpty());

  ServeFeed({&v1});
  Check();
  ASSERT_TRUE(service_->checked());
  // A feed older than the trusted sequence is rejected after a newer one was accepted.
  preferences_->sequence = sequence_ + 5;
  Check();
  EXPECT_NE(service_->last_error().indexOf(QStringLiteral("sequence")), -1)
      << service_->last_error().toStdString();
  EXPECT_EQ(downloads_->starts, 0);
}

// ── Installation and status ─────────────────────────────────────────────────

TEST_F(AiModelPackageServiceTest, InstallsHighestSupportedVersionIntoVersionDirectory) {
  StartService();
  const BuiltPackage v1 = Publish(1, "r1", 0);
  const BuiltPackage v2 = Publish(2, "r1", 1);  // Above this build's maximum.
  ServeFeed({&v1, &v2});
  Check();
  EXPECT_EQ(Row().value(QStringLiteral("modelSetVersion")).toULongLong(), 1u);
  ASSERT_TRUE(Install()) << State().error.toStdString();

  EXPECT_EQ(State().status, AiModelPackageStatus::kCurrent);
  EXPECT_EQ(InstalledFiles(1), v1.files);
  EXPECT_FALSE(fs::exists(InstalledDirectory(2)));
  const std::optional<AiModelPackageReceipt> receipt = Receipt(1);
  ASSERT_TRUE(receipt);
  EXPECT_EQ(receipt->revision, "r1");
  EXPECT_EQ(receipt->model_set_version, 1u);
  EXPECT_EQ(receipt->artifact_sha256,
            v1.entry["artifacts"][std::string(kPlatform)]["sha256"].get<std::string>());
  EXPECT_FALSE(fs::exists(AiModelPackageStagingDirectory(models_, "alcedo-ai-mask", 1)));

  const AiModelInstallation installation = ReadAiMaskModelInstallation(models_);
  ASSERT_TRUE(installation.Usable());
  EXPECT_EQ(installation.supported->model_set_version, 1u);
  EXPECT_EQ(installation.supported->receipt->revision, "r1");
  const QVariantMap row = Row();
  EXPECT_EQ(row.value(QStringLiteral("installedModelSetVersion")).toULongLong(), 1u);
  EXPECT_EQ(row.value(QStringLiteral("installedRevision")).toString(), QStringLiteral("r1"));
  EXPECT_TRUE(row.value(QStringLiteral("removable")).toBool());
  EXPECT_EQ(row.value(QStringLiteral("action")).toString(), QString());
}

// scripts/ai_models/tests/test_ai_model_packages.py writes this feed and its two archives with
// the publishing code (py7zr, LZMA2) and the key of KeyPair().
TEST_F(AiModelPackageServiceTest, PythonPreparedPackageInstallsOnEachPlatform) {
  const fs::path fixture = PackagePathFromUtf8(ALCEDO_AI_MODEL_PACKAGE_FIXTURE_DIR);
  feed_->feed            = {QByteArray::fromStdString(ReadBytes(fixture / "manifest.json")),
                            QByteArray::fromStdString(ReadBytes(fixture / "manifest.json.sig"))};
  const Json manifest    = Json::parse(ReadBytes(fixture / "manifest.json"));
  for (const auto& [platform, artifact] : manifest["packages"][0]["artifacts"].items()) {
    SCOPED_TRACE(platform);
    const std::string url    = artifact["url"].get<std::string>();
    downloads_->sources[url] = fixture / PackagePathFromUtf8(url.substr(url.rfind('/') + 1));
    models_                  = base_ / ("models-" + platform);
    fs::create_directories(models_);
    AiModelPackageServiceOptions options = MakeOptions();
    options.platform                     = QString::fromStdString(platform);
    service_ = std::make_unique<AiModelPackageService>(std::move(options));

    Check();
    ASSERT_TRUE(service_->checked()) << service_->last_error().toStdString();
    EXPECT_EQ(Row().value(QStringLiteral("action")).toString(), QStringLiteral("install"));
    EXPECT_EQ(Row().value(QStringLiteral("licenses")).toList().size(), 4);
    ASSERT_TRUE(Install()) << State().error.toStdString();
    EXPECT_EQ(State().status, AiModelPackageStatus::kCurrent) << State().error.toStdString();

    const AiModelInstallation installation = ReadAiMaskModelInstallation(models_);
    ASSERT_TRUE(installation.Usable());
    EXPECT_EQ(installation.supported->receipt->platform, platform);
    EXPECT_EQ(installation.supported->receipt->revision, "r1");
    const std::map<std::string, std::string> files = InstalledFiles(1);
    // Every listed file; installed.json and package-inventory.json are not listed.
    EXPECT_EQ(files.size(), artifact["file_count"].get<std::size_t>());
    for (const char* path :
         {"model_set.json", "NOTICE.txt", "people_sky_head.json", "licenses/SAM2-Apache-2.0.txt",
          "licenses/S3OD-MIT.txt", "licenses/DINOv3-License.md", "licenses/GPL-3.0.txt"}) {
      EXPECT_TRUE(files.contains(path)) << path;
    }
    const Json model_set = Json::parse(files.at("model_set.json"));
    EXPECT_EQ(model_set["platform"], platform);
    if (platform == "macos-arm64") {
      // Core ML model directories keep their nested files.
      EXPECT_TRUE(files.contains("s3od.mlmodelc/weights/weight.bin"));
      EXPECT_TRUE(files.contains("point_encoder.mlmodelc/analytics/coremldata.bin"));
    } else {
      EXPECT_TRUE(files.contains("subject_masks.onnx"));
    }
    service_.reset();
  }
}

TEST_F(AiModelPackageServiceTest, InstallReportsDownloadVerifyAndInstallStages) {
  StartService();
  const BuiltPackage v1 = Publish(1, "r1", 0);
  ServeFeed({&v1});
  Check();
  std::vector<AiModelPackageStatus> stages;
  QObject::connect(service_.get(), &AiModelPackageService::PackageChanged, service_.get(), [&] {
    const AiModelPackageStatus status = State().status;
    if (stages.empty() || stages.back() != status) stages.push_back(status);
  });
  ASSERT_TRUE(Install());
  ASSERT_GE(stages.size(), 3u);
  EXPECT_EQ(stages[0], AiModelPackageStatus::kDownloading);
  EXPECT_EQ(stages[1], AiModelPackageStatus::kVerifying);
  EXPECT_EQ(stages[2], AiModelPackageStatus::kInstalling);
  EXPECT_EQ(stages.back(), AiModelPackageStatus::kCurrent);
}

TEST_F(AiModelPackageServiceTest, NewerRevisionIsUpdateAvailableAndUpdateReplacesInstalledSet) {
  StartService();
  const BuiltPackage r1 = Publish(1, "r1", 0);
  InstallRevision(r1);
  const BuiltPackage r2 = Publish(1, "r2", 3);
  ServeFeed({&r2});
  Check();
  EXPECT_EQ(State().status, AiModelPackageStatus::kUpdateAvailable);
  EXPECT_EQ(Row().value(QStringLiteral("action")).toString(), QStringLiteral("update"));

  ASSERT_TRUE(Install()) << State().error.toStdString();
  EXPECT_EQ(State().status, AiModelPackageStatus::kCurrent);
  EXPECT_EQ(InstalledFiles(1), r2.files);
  EXPECT_EQ(Receipt(1)->revision, "r2");
  EXPECT_FALSE(fs::exists(models_ / ".alcedo-ai-mask-v1.previous"));
  EXPECT_FALSE(fs::exists(AiModelPackageStagingDirectory(models_, "alcedo-ai-mask", 1)));
}

TEST_F(AiModelPackageServiceTest, FeedWithOnlyHigherVersionsNeedsNewerApplication) {
  StartService();
  const BuiltPackage v2 = Publish(2, "r1", 0);
  ServeFeed({&v2});
  Check();
  EXPECT_EQ(State().status, AiModelPackageStatus::kNeedsNewerApplication);
  const QVariantMap row = Row();
  EXPECT_EQ(row.value(QStringLiteral("status")).toString(),
            QStringLiteral("needsNewerApplication"));
  EXPECT_EQ(row.value(QStringLiteral("action")).toString(), QString());
  EXPECT_FALSE(row.value(QStringLiteral("available")).toBool());
  EXPECT_FALSE(service_->InstallPackage(kId));
  EXPECT_EQ(downloads_->starts, 0);
  // AM5: the AI Mask tool shows the same state.
  EXPECT_EQ(service_->ReadAiMaskModelSet().availability,
            AiMaskModelSetAvailability::kNeedsNewerApplication);
}

// ── AI Mask tool readiness (AM5) ────────────────────────────────────────────

TEST_F(AiModelPackageServiceTest, AiMaskToolReadsTheInstalledSetAgainAndItsDirectory) {
  StartService();
  AiMaskModelSetReadiness readiness = service_->ReadAiMaskModelSet();
  EXPECT_EQ(readiness.availability, AiMaskModelSetAvailability::kNotInstalled);
  EXPECT_TRUE(readiness.directory.empty());
  EXPECT_EQ(feed_->requests, 0) << "the readiness read makes no network request";

  const BuiltPackage v1 = Publish(1, "r1", 0);
  InstallRevision(v1);
  readiness = service_->ReadAiMaskModelSet();
  EXPECT_EQ(readiness.availability, AiMaskModelSetAvailability::kReady);
  EXPECT_EQ(readiness.directory, InstalledDirectory(1));

  // A file changed on disk after the install: the next read finds it without a refresh call.
  WriteBytes(InstalledDirectory(1) / "point_encoder.onnx", "short");
  readiness = service_->ReadAiMaskModelSet();
  EXPECT_EQ(readiness.availability, AiMaskModelSetAvailability::kRepairRequired);
  EXPECT_TRUE(readiness.directory.empty());

  // The set removed outside the application: not installed.
  fs::remove_all(InstalledDirectory(1));
  EXPECT_EQ(service_->ReadAiMaskModelSet().availability, AiMaskModelSetAvailability::kNotInstalled);
}

TEST_F(AiModelPackageServiceTest, AiMaskToolReadinessFollowsTheSupportedRange) {
  StartService();
  const BuiltPackage v1 = Publish(1, "r1", 0);
  InstallRevision(v1);

  // A build that supports versions 2-3 finds only version 1: Update required.
  EXPECT_EQ(AiMaskModelSetReadinessFor(ReadAiModelInstallation(models_, "alcedo-ai-mask", 2, 3),
                                       AiModelPackageStatus::kUpdateRequired)
                .availability,
            AiMaskModelSetAvailability::kUpdateRequired);
  // A build that supports only an older range finds only version 1: Needs a newer Alcedo
  // Studio, also before a feed check.
  const AiModelInstallation above = ReadAiModelInstallation(models_, "alcedo-ai-mask", 0, 0);
  ASSERT_TRUE(above.above_maximum_version.has_value());
  EXPECT_EQ(AiMaskModelSetReadinessFor(above, AiModelPackageStatus::kNotInstalled).availability,
            AiMaskModelSetAvailability::kNeedsNewerApplication);
  // Nothing installed: the feed comparison decides between the two states.
  EXPECT_EQ(AiMaskModelSetReadinessFor(AiModelInstallation{}, AiModelPackageStatus::kNotInstalled)
                .availability,
            AiMaskModelSetAvailability::kNotInstalled);
  EXPECT_EQ(AiMaskModelSetReadinessFor(AiModelInstallation{},
                                       AiModelPackageStatus::kNeedsNewerApplication)
                .availability,
            AiMaskModelSetAvailability::kNeedsNewerApplication);
  // A usable supported set is ready whatever the feed lists (an update is optional).
  const AiMaskModelSetReadiness ready = AiMaskModelSetReadinessFor(
      ReadAiMaskModelInstallation(models_), AiModelPackageStatus::kUpdateAvailable);
  EXPECT_EQ(ready.availability, AiMaskModelSetAvailability::kReady);
  EXPECT_EQ(ready.directory, InstalledDirectory(1));
}

TEST_F(AiModelPackageServiceTest, InstalledVersionBelowMinimumIsUpdateRequired) {
  StartService();
  const BuiltPackage v1 = Publish(1, "r1", 0);
  InstallRevision(v1);

  // A build that supports versions 2-3 finds only version 1 installed.
  const AiModelInstallation installation = ReadAiModelInstallation(models_, "alcedo-ai-mask", 2, 3);
  EXPECT_FALSE(installation.supported.has_value());
  ASSERT_TRUE(installation.below_minimum_version.has_value());
  EXPECT_EQ(*installation.below_minimum_version, 1u);
  const QString                  platform = QString::fromLatin1(kPlatform.data(), kPlatform.size());
  const AiModelPackageComparison unchecked =
      CompareAiModelPackage(installation, nullptr, kId, 2, 3, platform);
  EXPECT_EQ(unchecked.status, AiModelPackageStatus::kUpdateRequired);
  EXPECT_EQ(AiModelPackageActionFor(unchecked.status, false), AiModelPackageAction::kNone);

  const BuiltPackage v2 = BuildPackage(archives_, 2, "r1", 1);
  ServeFeed({&v2});
  QByteArray                    public_key;
  std::array<unsigned char, 64> unused{};
  KeyPair(&public_key, &unused);
  const AiModelPackageManifestResult feed = VerifyAiModelPackageManifest(
      feed_->feed.json, feed_->feed.signature, public_key, kFeedUrl, 1);
  ASSERT_TRUE(feed) << feed.error.toStdString();
  const AiModelPackageComparison offered =
      CompareAiModelPackage(installation, &*feed.manifest, kId, 2, 3, platform);
  EXPECT_EQ(offered.status, AiModelPackageStatus::kUpdateRequired);
  ASSERT_NE(offered.offer, nullptr);
  EXPECT_EQ(offered.offer->model_set_version, 2u);
  EXPECT_EQ(AiModelPackageActionFor(offered.status, true), AiModelPackageAction::kUpdate);
}

TEST_F(AiModelPackageServiceTest, MissingOrResizedFileIsRepairWithoutHashing) {
  StartService();
  const BuiltPackage v1 = Publish(1, "r1", 0);
  InstallRevision(v1);
  const fs::path encoder = InstalledDirectory(1) / "point_encoder.onnx";

  // Same size, other bytes: the comparison reads sizes only, so the set stays current.
  WriteBytes(encoder, std::string(v1.files.at("point_encoder.onnx").size(), 'z'));
  service_->RefreshInstallations();
  EXPECT_EQ(State().status, AiModelPackageStatus::kCurrent);

  WriteBytes(encoder, "short");
  service_->RefreshInstallations();
  EXPECT_EQ(State().status, AiModelPackageStatus::kRepairRequired);
  EXPECT_EQ(Row().value(QStringLiteral("action")).toString(), QStringLiteral("repair"));
  EXPECT_FALSE(ReadAiMaskModelInstallation(models_).Usable());

  ASSERT_TRUE(Install()) << State().error.toStdString();
  EXPECT_EQ(State().status, AiModelPackageStatus::kCurrent);
  EXPECT_EQ(InstalledFiles(1), v1.files);
}

// ── Failures keep the installed set ─────────────────────────────────────────

TEST_F(AiModelPackageServiceTest, ArchiveHashMismatchKeepsInstalledSet) {
  StartService();
  const BuiltPackage r1 = Publish(1, "r1", 0);
  InstallRevision(r1);

  // The entry is signed for r2 bytes, but the transfer delivers other bytes.
  const BuiltPackage r2                = Publish(1, "r2", 4);
  const BuiltPackage wrong             = BuildPackage(base_ / "wrong", 1, "r2", 6);
  downloads_->sources[ArtifactUrl(r2)] = wrong.archive;
  ServeFeed({&r2});
  Check();
  EXPECT_FALSE(Install());

  EXPECT_EQ(State().status, AiModelPackageStatus::kError);
  EXPECT_NE(State().error.indexOf(QStringLiteral("SHA-256")), -1) << State().error.toStdString();
  EXPECT_EQ(Row().value(QStringLiteral("action")).toString(), QStringLiteral("retry"));
  EXPECT_EQ(InstalledFiles(1), r1.files);
  EXPECT_EQ(Receipt(1)->revision, "r1");
  // The rejected archive is removed; no staging content remains.
  const fs::path staging = AiModelPackageStagingDirectory(models_, "alcedo-ai-mask", 1);
  EXPECT_TRUE(!fs::exists(staging) || fs::is_empty(staging));
}

TEST_F(AiModelPackageServiceTest, PathTraversalEntryKeepsInstalledSet) {
  StartService();
  const BuiltPackage r1 = Publish(1, "r1", 0);
  InstallRevision(r1);

  const BuiltPackage r2 = Publish(1, "r2", 2, {{"../escape.onnx", "escape"}});
  ServeFeed({&r2});
  Check();
  EXPECT_FALSE(Install());

  EXPECT_EQ(State().status, AiModelPackageStatus::kError);
  EXPECT_NE(State().error.indexOf(QStringLiteral("unsafe path")), -1)
      << State().error.toStdString();
  EXPECT_EQ(InstalledFiles(1), r1.files);
  EXPECT_EQ(Receipt(1)->revision, "r1");
  EXPECT_FALSE(fs::exists(models_ / "escape.onnx"));
  EXPECT_FALSE(
      fs::exists(AiModelPackageStagingDirectory(models_, "alcedo-ai-mask", 1) / "escape.onnx"));
  EXPECT_FALSE(
      fs::exists(AiModelPackageStagingDirectory(models_, "alcedo-ai-mask", 1) / "content"));
}

TEST_F(AiModelPackageServiceTest, ExtraUnlistedOrMissingFileIsRejected) {
  struct BadArchive {
    std::string              label;
    std::vector<ArchiveItem> extra;
    std::string              omitted;
    std::string              expected_error;
  };
  // An extra entry exceeds the signed file count while streaming; a listed file replaced by
  // an unlisted one reaches the inventory comparison.
  const std::vector<BadArchive> cases = {
      {"extra file", {{"extra.onnx", "extra"}}, {}, "more files than its descriptor"},
      {"installed.json in archive", {{"installed.json", "{}"}}, {}, "more files than its"},
      {"listed file replaced",
       {{"extra.txt", "extra"}},
       "NOTICE.txt",
       "does not contain NOTICE.txt"},
  };
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const BadArchive&  bad      = cases[index];
    const BuiltPackage package  = BuildPackage(base_ / "bad" / std::to_string(index), 1, "r2",
                                               static_cast<int>(index) + 3, bad.extra, bad.omitted);
    const Json&        artifact = package.entry["artifacts"][std::string(kPlatform)];
    AiModelPackageInstallRequest request;
    request.model_directory = models_;
    request.archive_path    = package.archive;
    request.expected        = {"alcedo-ai-mask",
                               1,
                               "r2",
                               std::string(kPlatform),
                               artifact["file_count"].get<std::uint64_t>(),
                               artifact["unpacked_bytes"].get<std::uint64_t>(),
                               artifact["inventory_sha256"].get<std::string>(),
                               artifact["url"].get<std::string>(),
                               artifact["size"].get<std::uint64_t>(),
                               artifact["sha256"].get<std::string>(),
                               1};
    const AiModelPackageInstallOutcome outcome =
        InstallAiModelPackageArchive(request, std::atomic<bool>{false}, {});
    EXPECT_FALSE(outcome.committed) << bad.label;
    EXPECT_NE(outcome.error.find(bad.expected_error), std::string::npos)
        << bad.label << ": " << outcome.error;
    EXPECT_FALSE(fs::exists(InstalledDirectory(1))) << bad.label;
  }
}

TEST_F(AiModelPackageServiceTest, CanceledDownloadKeepsInstalledSet) {
  StartService();
  const BuiltPackage r1 = Publish(1, "r1", 0);
  InstallRevision(r1);
  const BuiltPackage r2 = Publish(1, "r2", 5);
  ServeFeed({&r2});
  Check();

  downloads_->hold = true;
  QSignalSpy finished(service_.get(), &AiModelPackageService::InstallFinished);
  ASSERT_TRUE(service_->InstallPackage(kId));
  EXPECT_EQ(State().status, AiModelPackageStatus::kDownloading);
  EXPECT_TRUE(Row().value(QStringLiteral("cancelable")).toBool());
  EXPECT_FALSE(service_->CheckPackages());  // No check while an action runs.
  ASSERT_TRUE(service_->CancelInstall(kId));
  ASSERT_TRUE(WaitUntil([&] { return finished.count() == 1; }));
  EXPECT_FALSE(finished.at(0).at(1).toBool());
  EXPECT_TRUE(finished.at(0).at(2).toBool());
  EXPECT_NE(State().error.indexOf(QStringLiteral("canceled")), -1) << State().error.toStdString();
  EXPECT_EQ(InstalledFiles(1), r1.files);
  EXPECT_EQ(Receipt(1)->revision, "r1");
}

TEST_F(AiModelPackageServiceTest, CanceledInstallKeepsInstalledSetAndDownloadedArchive) {
  const BuiltPackage           r1       = BuildPackage(archives_, 1, "r1", 0);
  const Json&                  artifact = r1.entry["artifacts"][std::string(kPlatform)];
  AiModelPackageInstallRequest request;
  request.model_directory = models_;
  request.archive_path =
      AiModelPackageStagingDirectory(models_, "alcedo-ai-mask", 1) / "download.7z";
  fs::create_directories(request.archive_path.parent_path());
  fs::copy_file(r1.archive, request.archive_path);
  request.expected = {"alcedo-ai-mask",
                      1,
                      "r1",
                      std::string(kPlatform),
                      artifact["file_count"].get<std::uint64_t>(),
                      artifact["unpacked_bytes"].get<std::uint64_t>(),
                      artifact["inventory_sha256"].get<std::string>(),
                      artifact["url"].get<std::string>(),
                      artifact["size"].get<std::uint64_t>(),
                      artifact["sha256"].get<std::string>(),
                      1};
  // An installed set that the canceled install must leave as it is.
  WriteBytes(InstalledDirectory(1) / "point_encoder.onnx", "installed encoder");

  std::atomic<bool>                  stop{false};
  const AiModelPackageInstallOutcome outcome =
      InstallAiModelPackageArchive(request, stop, [&stop](AiModelPackageInstallStage stage) {
        if (stage == AiModelPackageInstallStage::kInstalling) stop.store(true);
      });
  EXPECT_TRUE(outcome.canceled);
  EXPECT_FALSE(outcome.committed);
  EXPECT_EQ(ReadBytes(InstalledDirectory(1) / "point_encoder.onnx"), "installed encoder");
  EXPECT_TRUE(fs::exists(request.archive_path));  // Kept for a retry.
  EXPECT_FALSE(fs::exists(request.archive_path.parent_path() / "content"));
}

// ── Download statement ──────────────────────────────────────────────────────

TEST_F(AiModelPackageServiceTest, InstallBeforeStatementAcceptedStartsNothing) {
  preferences_->statement_accepted = 0;
  StartService();
  const BuiltPackage v1 = Publish(1, "r1", 0);
  ServeFeed({&v1});
  Check();
  EXPECT_FALSE(service_->download_statement_accepted());
  QSignalSpy required(service_.get(), &AiModelPackageService::DownloadStatementRequired);
  EXPECT_FALSE(service_->InstallPackage(kId));
  EXPECT_EQ(required.count(), 1);
  EXPECT_EQ(downloads_->starts, 0);
  EXPECT_EQ(State().status, AiModelPackageStatus::kNotInstalled);
  EXPECT_FALSE(fs::exists(AiModelPackageStagingDirectory(models_, "alcedo-ai-mask", 1)));

  service_->AcceptDownloadStatement();
  EXPECT_EQ(preferences_->statement_accepted, kAiModelDownloadStatementVersion);
  EXPECT_TRUE(service_->download_statement_accepted());
  ASSERT_TRUE(Install()) << State().error.toStdString();
  EXPECT_EQ(downloads_->starts, 1);
}

// ── Removal and recovery ────────────────────────────────────────────────────

TEST_F(AiModelPackageServiceTest, RemoveDeletesInstalledSetWithoutFeedCheck) {
  StartService();
  const BuiltPackage v1 = Publish(1, "r1", 0);
  InstallRevision(v1);

  // A new service has not checked the feed; the installed set is still removable.
  service_.reset();
  StartService();
  EXPECT_EQ(State().status, AiModelPackageStatus::kCurrent);
  ASSERT_TRUE(service_->RemovePackage(kId));
  EXPECT_EQ(State().status, AiModelPackageStatus::kRemoving);
  ASSERT_TRUE(WaitUntil([&] { return State().status != AiModelPackageStatus::kRemoving; }));
  EXPECT_EQ(State().status, AiModelPackageStatus::kNotInstalled) << State().error.toStdString();
  EXPECT_FALSE(fs::exists(InstalledDirectory(1)));
  EXPECT_FALSE(fs::exists(models_ / ".alcedo-ai-mask-v1.removing"));
  EXPECT_FALSE(Row().value(QStringLiteral("removable")).toBool());
}

TEST_F(AiModelPackageServiceTest, InterruptedReplacementRestoresInstalledSetOnCheck) {
  StartService();
  const BuiltPackage r1 = Publish(1, "r1", 0);
  InstallRevision(r1);

  // The process stopped after it moved the installed set aside and before the new content
  // was moved into place.
  fs::rename(InstalledDirectory(1), models_ / ".alcedo-ai-mask-v1.previous");
  const fs::path content = AiModelPackageStagingDirectory(models_, "alcedo-ai-mask", 1) / "content";
  WriteBytes(content / "point_encoder.onnx", "uncommitted");
  service_->RefreshInstallations();
  EXPECT_EQ(State().status, AiModelPackageStatus::kNotInstalled);

  Check();
  EXPECT_EQ(State().status, AiModelPackageStatus::kCurrent);
  EXPECT_EQ(InstalledFiles(1), r1.files);
  EXPECT_FALSE(fs::exists(models_ / ".alcedo-ai-mask-v1.previous"));
  EXPECT_FALSE(fs::exists(content));
}

// ── Background task and shared transfer admission ───────────────────────────

TEST_F(AiModelPackageServiceTest, DownloadIsCancelableModelDownloadBackgroundTask) {
  StartService();
  ui::BackgroundTaskController     tasks;
  ui::AiModelDownloadTaskPublisher publisher(*service_, tasks);
  const BuiltPackage               r1 = Publish(1, "r1", 0);
  ServeFeed({&r1});
  Check();

  downloads_->hold = true;
  ASSERT_TRUE(service_->InstallPackage(kId));
  const QString task_id = publisher.TaskId(kId);
  ASSERT_FALSE(task_id.isEmpty());
  const QVariantMap task = FindTask(tasks, task_id);
  EXPECT_EQ(task.value(QStringLiteral("kind")).toString(), QStringLiteral("modelDownload"));
  EXPECT_EQ(task.value(QStringLiteral("state")).toString(), QStringLiteral("running"));
  EXPECT_TRUE(task.value(QStringLiteral("cancelable")).toBool());
  EXPECT_EQ(task.value(QStringLiteral("locks")).toList().size(), 1);

  ASSERT_TRUE(tasks.CancelTask(task_id));
  ASSERT_TRUE(WaitUntil([&] { return publisher.TaskId(kId).isEmpty(); }));
  EXPECT_EQ(FindTask(tasks, task_id).value(QStringLiteral("state")).toString(),
            QStringLiteral("canceled"));
  EXPECT_FALSE(fs::exists(InstalledDirectory(1)));

  downloads_->hold = false;
  QSignalSpy finished(service_.get(), &AiModelPackageService::InstallFinished);
  ASSERT_TRUE(service_->InstallPackage(kId));
  const QString second_task_id = publisher.TaskId(kId);
  ASSERT_FALSE(second_task_id.isEmpty());
  ASSERT_TRUE(WaitUntil([&] { return finished.count() == 1; }));
  EXPECT_EQ(FindTask(tasks, second_task_id).value(QStringLiteral("state")).toString(),
            QStringLiteral("succeeded"));
}

TEST_F(AiModelPackageServiceTest, ApplicationAndAiModelDownloadsShareAdmission) {
  Aria2Environment environment;
  {
    DownloadService transfers;
    StartService(std::make_unique<DownloadServicePackageArchiveDownloader>(transfers));
    const BuiltPackage v1 = Publish(1, "r1", 0);
    ServeFeed({&v1});
    Check();

    // An application update transfer holds the shared admission.
    DownloadRequest update;
    update.id = QStringLiteral("application-update");
    update.items.push_back({QUrl(QStringLiteral("https://example.invalid/update.exe")),
                            QString::fromStdString(PackagePathToUtf8(base_ / "update.exe")),
                            1,
                            {}});
    ASSERT_TRUE(transfers.Start(update));
    QSignalSpy finished(&transfers, &DownloadService::Finished);

    EXPECT_FALSE(service_->InstallPackage(kId));
    EXPECT_EQ(State().status, AiModelPackageStatus::kError);
    EXPECT_NE(State().error.indexOf(QStringLiteral("Another download")), -1);
    EXPECT_EQ(transfers.ActiveRequestId(), QStringLiteral("application-update"));
    EXPECT_FALSE(service_->CancelInstall(kId));
    EXPECT_FALSE(fs::exists(InstalledDirectory(1)));

    // While the AI model download holds the admission, an application download waits.
    transfers.Cancel(QStringLiteral("application-update"));
    ASSERT_TRUE(finished.wait(15000));
    ASSERT_TRUE(service_->InstallPackage(kId));
    EXPECT_FALSE(transfers.Start(update));
    ASSERT_TRUE(service_->CancelInstall(kId));
    ASSERT_TRUE(WaitUntil([&] { return !transfers.IsRunning(); }));
    service_.reset();
  }
}

}  // namespace
}  // namespace alcedo::test
