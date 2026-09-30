//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_package_service.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <json.hpp>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "utils/lut/lut_inventory_digest.hpp"
#include "utils/lut/lut_metadata.hpp"

extern "C" {
#include <ed25519.h>
}

#ifndef ALCEDO_FAKE_ARIA2_PATH
#define ALCEDO_FAKE_ARIA2_PATH ""
#endif
#ifndef ALCEDO_LUT_METADATA_FIXTURE_DIR
#define ALCEDO_LUT_METADATA_FIXTURE_DIR ""
#endif

namespace alcedo::test {
namespace {

namespace fs                            = std::filesystem;
using Json                              = nlohmann::json;

constexpr std::string_view kSpectral    = "spectral_film_lut";
constexpr std::string_view kSpektrafilm = "spektrafilm_lut";
const QUrl kFeedUrl(QStringLiteral("https://static.aoraw.org/luts/v1/manifest.json"));

// ── File and archive fixtures ───────────────────────────────────────────────

auto       ReadBytes(const fs::path& path) -> std::string {
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

auto OfficialId(std::string_view package_id, std::string_view film_id) -> std::string {
  return std::string(package_id) + ":" + std::string(film_id);
}

/// An official LUT whose numeric rows depend on @p shade, so revisions differ in bytes.
auto OfficialCube(std::string_view package_id, std::string_view film_id, int shade,
                  std::string_view origin = "alcedo") -> std::string {
  const Json  metadata = {{"schema", 1},
                          {"id", OfficialId(package_id, film_id)},
                          {"origin", origin},
                          {"category", "film_simulation"},
                          {"source", {{"id", package_id}, {"name", "Fixture Source"}}},
                          {"film", {{"id", film_id}, {"name", "Fixture Film"}, {"brand", "Kodak"}}},
                          {"input_space", "ACEScc"},
                          {"output_space", "ACEScc"}};
  std::string text     = "# ALCEDO_LUT " + metadata.dump() + "\nLUT_3D_SIZE 2\n";
  for (int index = 0; index < 8; ++index) {
    text += std::to_string((index + shade) % 3) + " 0.5 " + std::to_string(index % 2) + "\n";
  }
  return text;
}

struct ArchiveItem {
  enum class Kind { kFile, kDirectory, kSymlink };
  std::string path;
  std::string bytes;
  Kind        kind = Kind::kFile;
  std::string link_target;
};

/// Write a 7z (LZMA2) archive with libarchive; entries are written as given, so
/// a test can place unsafe paths and links that the installer must reject.
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
    switch (item.kind) {
      case ArchiveItem::Kind::kFile:
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, static_cast<la_int64_t>(item.bytes.size()));
        break;
      case ArchiveItem::Kind::kDirectory:
        archive_entry_set_filetype(entry, AE_IFDIR);
        archive_entry_set_perm(entry, 0755);
        break;
      case ArchiveItem::Kind::kSymlink:
        archive_entry_set_filetype(entry, AE_IFLNK);
        archive_entry_set_perm(entry, 0777);
        archive_entry_set_symlink_utf8(entry, item.link_target.c_str());
        break;
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

/// One package revision: archive bytes on disk and its signed-feed descriptor.
struct BuiltPackage {
  std::string                        package_id;
  std::string                        revision;
  fs::path                           archive;
  Json                               descriptor;
  /// Package-relative path -> bytes of every LUT.
  std::map<std::string, std::string> luts;
};

auto PackageFiles(std::string_view package_id, int shade) -> std::map<std::string, std::string> {
  return {
      {"kodak_vision3_250d_5207.cube", OfficialCube(package_id, "kodak_vision3_250d_5207", shade)},
      {"stocks/kodak_vision3_500t_5219.cube",
       OfficialCube(package_id, "kodak_vision3_500t_5219", shade + 1)}};
}

auto InventoryJson(std::string_view package_id, std::string_view revision,
                   const std::map<std::string, std::string>& luts,
                   const std::map<std::string, std::string>& auxiliary) -> Json {
  std::vector<LutInventoryRecord> records;
  Json                            lut_list = Json::array();
  Json                            aux_list = Json::array();
  std::uint64_t                   bytes    = 0;
  for (const auto& [path, contents] : luts) {
    const std::string film = fs::path(path).stem().string();
    records.push_back({OfficialId(package_id, film), path, contents.size(), Sha256Hex(contents)});
    lut_list.push_back({{"id", records.back().id},
                        {"path", path},
                        {"size", contents.size()},
                        {"sha256", records.back().sha256}});
    bytes += contents.size();
  }
  for (const auto& [path, contents] : auxiliary) {
    aux_list.push_back(
        {{"path", path}, {"size", contents.size()}, {"sha256", Sha256Hex(contents)}});
    bytes += contents.size();
  }
  return {{"schema", 1},
          {"kind", "alcedo-lut-package-inventory"},
          {"package_id", package_id},
          {"revision", revision},
          {"file_count", luts.size()},
          {"inventory_sha256", ComputeLutInventoryDigest(records).sha256},
          {"unpacked_bytes", bytes},
          {"luts", lut_list},
          {"auxiliary_files", aux_list}};
}

/// Archive entries of a complete, valid package: LUTs, a license, and the inventory.
auto PackageItems(std::string_view package_id, std::string_view revision, int shade,
                  Json* inventory) -> std::vector<ArchiveItem> {
  const std::map<std::string, std::string> luts      = PackageFiles(package_id, shade);
  const std::map<std::string, std::string> auxiliary = {{"LICENSE.txt", "Fixture license.\n"}};
  *inventory = InventoryJson(package_id, revision, luts, auxiliary);
  std::vector<ArchiveItem> items;
  for (const auto& [path, bytes] : luts) items.push_back({path, bytes});
  for (const auto& [path, bytes] : auxiliary) items.push_back({path, bytes});
  items.push_back({"package-inventory.json", inventory->dump(1)});
  return items;
}

auto BuildPackage(const fs::path& directory, std::string_view package_id, std::string_view revision,
                  int shade) -> BuiltPackage {
  BuiltPackage package{std::string(package_id), std::string(revision)};
  package.luts = PackageFiles(package_id, shade);
  Json                           inventory;
  const std::vector<ArchiveItem> items = PackageItems(package_id, revision, shade, &inventory);
  package.archive = directory / (std::string(package_id) + "-" + std::string(revision) + ".7z");
  Write7z(package.archive, items);

  const std::string archive_bytes = ReadBytes(package.archive);
  package.descriptor              = {
      {"id", package_id},
      {"revision", revision},
      {"file_count", inventory["file_count"]},
      {"inventory_sha256", inventory["inventory_sha256"]},
      {"unpacked_bytes", inventory["unpacked_bytes"]},
      {"artifact",
                    {{"url", "https://static.aoraw.org/luts/v1/packages/" + std::string(package_id) + "/" +
                                 std::string(revision) + "/" + std::string(package_id) + ".7z"},
                     {"size", archive_bytes.size()},
                     {"sha256", Sha256Hex(archive_bytes)}}}};
  return package;
}

struct SignedFeed {
  QByteArray json;
  QByteArray signature;
};

void KeyPair(QByteArray* public_key, std::array<unsigned char, 64>* private_key) {
  unsigned char seed[32] = {};
  for (std::size_t index = 0; index < sizeof(seed); ++index) {
    seed[index] = static_cast<unsigned char>(index * 3 + 1);
  }
  unsigned char key[32] = {};
  ed25519_create_keypair(key, private_key->data(), seed);
  *public_key = QByteArray(reinterpret_cast<const char*>(key), sizeof(key));
}

auto SignFeed(const std::vector<const BuiltPackage*>& packages, std::uint64_t sequence)
    -> SignedFeed {
  Json list = Json::array();
  for (const BuiltPackage* package : packages) list.push_back(package->descriptor);
  const QByteArray              json = QByteArray::fromStdString(Json{
                   {"schema", 1},
                   {"kind", "alcedo-lut-packages"},
                   {"sequence", sequence},
                   {"packages", list}}.dump());
  QByteArray                    public_key;
  std::array<unsigned char, 64> private_key{};
  KeyPair(&public_key, &private_key);
  unsigned char signature[64] = {};
  ed25519_sign(signature, reinterpret_cast<const unsigned char*>(json.constData()),
               static_cast<std::size_t>(json.size()),
               reinterpret_cast<const unsigned char*>(public_key.constData()), private_key.data());
  return {json, QByteArray(reinterpret_cast<const char*>(signature), 64).toBase64()};
}

// ── Transport doubles ───────────────────────────────────────────────────────

/// Serves the current signed feed and counts every request.
struct FeedServer {
  SignedFeed feed;
  int        requests = 0;
};

/// Copies a local archive (selected by URL) to the requested destination.
/// With `hold` set, a transfer stays running until Cancel().
class LocalArchiveDownloader final : public LutArchiveDownloader {
 public:
  struct State {
    std::map<std::string, fs::path> sources;
    int                             starts = 0;
    bool                            hold   = false;
    bool                            busy   = false;
    FinishedCallback                pending;
  };
  explicit LocalArchiveDownloader(std::shared_ptr<State> state) : state_(std::move(state)) {}

  auto Start(const DownloadRequest& request, ProgressCallback on_progress,
             FinishedCallback on_finished) -> bool override {
    if (state_->busy) return false;
    ++state_->starts;
    const DownloadItem item   = request.items.front();
    const auto         source = state_->sources.find(item.url.toString().toStdString());
    if (state_->hold) {
      state_->busy    = true;
      state_->pending = std::move(on_finished);
      return true;
    }
    QTimer::singleShot(
        0, [state       = state_, item,
            source_path = source != state_->sources.end() ? source->second : fs::path{},
            on_progress, on_finished] {
          std::error_code error;
          const fs::path  destination = LutPathFromUtf8(item.destination.toStdString());
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
    state_->busy              = false;
    QTimer::singleShot(0, [finished] { finished(false, true, QStringLiteral("canceled")); });
  }

 private:
  std::shared_ptr<State> state_;
};

class MemoryLibraryPreferences final : public LutLibraryPreferences {
 public:
  explicit MemoryLibraryPreferences(fs::path root) : root_(std::move(root)) {}
  [[nodiscard]] auto LoadRoot() const -> std::optional<fs::path> override { return root_; }
  [[nodiscard]] auto SaveRoot(const fs::path& root) -> bool override {
    root_ = root;
    return true;
  }
  [[nodiscard]] auto LoadLegacyFavoritePaths() const -> QStringList override { return {}; }
  void               SaveLegacyFavoritePaths(const QStringList&) override {}

 private:
  fs::path root_;
};

class MemoryPackagePreferences final : public LutPackagePreferences {
 public:
  [[nodiscard]] auto LoadTrustedSequence() const -> quint64 override { return sequence_; }
  void               SaveTrustedSequence(quint64 sequence) override { sequence_ = sequence; }

 private:
  quint64 sequence_ = 1;
};

// ── Fixture ─────────────────────────────────────────────────────────────────

class LutPackageServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device device;
    base_ = fs::current_path() / "lut_package_service_test" / std::to_string(device());
    fs::create_directories(base_);
    base_      = fs::weakly_canonical(base_);
    root_      = base_ / "library";
    archives_  = base_ / "archives";
    downloads_ = std::make_shared<LocalArchiveDownloader::State>();
    feed_      = std::make_shared<FeedServer>();
    fs::create_directories(root_);
  }
  void TearDown() override {
    packages_.reset();
    if (library_) library_->Shutdown();
    library_.reset();
    std::error_code error;
    fs::remove_all(base_, error);
  }

  static auto WaitUntil(const std::function<bool()>& condition) -> bool {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 20000) QTest::qWait(5);
    return condition();
  }

  void StartLibrary(LutLibraryFileOperations io = LutLibraryFileOperations::Default()) {
    packages_.reset();
    if (library_) library_->Shutdown();
    LutLibraryServiceOptions options;
    options.preferences       = std::make_unique<MemoryLibraryPreferences>(root_);
    options.default_root      = root_;
    options.file_operations   = std::move(io);
    options.open_url          = [](const QUrl&) { return true; };
    options.scan_worker_count = 2;
    library_                  = std::make_unique<LutLibraryService>(std::move(options));
    library_->Start();
    ASSERT_TRUE(WaitUntil([&] { return !library_->busy(); }));
  }

  auto MakeOptions(std::unique_ptr<LutArchiveDownloader> downloader = nullptr)
      -> LutPackageServiceOptions {
    LutPackageServiceOptions options;
    options.feed_url = kFeedUrl;
    std::array<unsigned char, 64> unused{};
    KeyPair(&options.public_key, &unused);
    options.fetch = [feed = feed_](const QUrl&                              url, qint64,
                                   std::function<void(QByteArray, QString)> done) {
      ++feed->requests;
      const bool signature = url.path().endsWith(QStringLiteral(".sig"));
      QTimer::singleShot(0, [feed, signature, done] {
        done(signature ? feed->feed.signature : feed->feed.json, {});
      });
    };
    options.downloader =
        downloader ? std::move(downloader) : std::make_unique<LocalArchiveDownloader>(downloads_);
    options.preferences = std::make_unique<MemoryPackagePreferences>();
    return options;
  }

  void StartPackages() {
    packages_ = std::make_unique<LutPackageService>(MakeOptions(), *library_);
  }

  /// Build a package revision, register its archive with the downloader, and return it.
  auto Publish(std::string_view package_id, std::string_view revision, int shade) -> BuiltPackage {
    BuiltPackage package = BuildPackage(archives_, package_id, revision, shade);
    downloads_->sources[package.descriptor["artifact"]["url"].get<std::string>()] = package.archive;
    return package;
  }

  void ServeFeed(const std::vector<const BuiltPackage*>& packages) {
    feed_->feed = SignFeed(packages, ++sequence_);
  }

  void Check() {
    ASSERT_TRUE(packages_->CheckPackages());
    ASSERT_TRUE(WaitUntil([&] { return !packages_->checking() && !library_->busy(); }));
  }

  auto Install(std::string_view package_id) -> bool {
    const QString id =
        QString::fromUtf8(package_id.data(), static_cast<qsizetype>(package_id.size()));
    if (!packages_->InstallPackage(id)) return false;
    return WaitUntil([&] {
      const LutPackageService::PackageState* state = packages_->Package(id);
      return state != nullptr && state->status != LutPackageStatus::kDownloading &&
             state->status != LutPackageStatus::kVerifying &&
             state->status != LutPackageStatus::kInstalling && !library_->busy();
    });
  }

  auto Status(std::string_view package_id) -> LutPackageStatus {
    return packages_
        ->Package(QString::fromUtf8(package_id.data(), static_cast<qsizetype>(package_id.size())))
        ->status;
  }

  auto Receipt(std::string_view package_id) -> std::optional<LutPackageReceipt> {
    for (const LutPackageReceipt& receipt : library_->PackageReceipts()) {
      if (receipt.package_id == package_id) return receipt;
    }
    return std::nullopt;
  }

  /// Relative path inside the package -> bytes of every installed entry of @p package_id.
  auto InstalledFiles(std::string_view package_id) -> std::map<std::string, std::string> {
    std::map<std::string, std::string>     files;
    const std::optional<LutPackageReceipt> receipt = Receipt(package_id);
    if (!receipt) return files;
    library_->ForEachPackageEntry(package_id, [&](const LutLibraryEntry& entry) {
      files[entry.relative_path.substr(receipt->content_directory.size() + 1)] =
          ReadBytes(root_ / LutPathFromUtf8(entry.relative_path));
    });
    return files;
  }

  auto ContentDirectories(std::string_view package_id) -> std::vector<std::string> {
    std::vector<std::string> names;
    const fs::path           parent = root_ / "packages" / std::string(package_id) / "content";
    std::error_code          error;
    for (fs::directory_iterator item(parent, error), end; !error && item != end;
         item.increment(error)) {
      names.push_back(LutPathToUtf8(item->path().lexically_relative(root_)));
    }
    return names;
  }

  /// Install @p package_id revision @p shade through the whole service chain.
  void InstallRevision(const BuiltPackage& package) {
    ServeFeed({&package});
    Check();
    ASSERT_TRUE(Install(package.package_id));
    ASSERT_EQ(Status(package.package_id), LutPackageStatus::kCurrent)
        << packages_->Package(QString::fromStdString(package.package_id))->error.toStdString();
  }

  fs::path                                       base_;
  fs::path                                       root_;
  fs::path                                       archives_;
  std::shared_ptr<LocalArchiveDownloader::State> downloads_;
  std::shared_ptr<FeedServer>                    feed_;
  std::uint64_t                                  sequence_ = 20260929000000ULL;
  std::unique_ptr<LutLibraryService>             library_;
  std::unique_ptr<LutPackageService>             packages_;
};

// ── Checking ────────────────────────────────────────────────────────────────

TEST_F(LutPackageServiceTest, StartupDoesNotRequestLutFeed) {
  StartLibrary();
  StartPackages();
  QTest::qWait(50);
  EXPECT_EQ(feed_->requests, 0);
  EXPECT_EQ(downloads_->starts, 0);
  EXPECT_TRUE(packages_->enabled());
  EXPECT_FALSE(packages_->checked());
  EXPECT_FALSE(packages_->checking());
  EXPECT_TRUE(packages_->Packages().empty());
}

TEST_F(LutPackageServiceTest, SettingsCheckDoesNotStartDownload) {
  StartLibrary();
  StartPackages();
  const BuiltPackage spectral    = Publish(kSpectral, "r1", 0);
  const BuiltPackage spektrafilm = Publish(kSpektrafilm, "r1", 1);
  ServeFeed({&spectral, &spektrafilm});
  Check();
  EXPECT_EQ(feed_->requests, 2);  // Manifest and detached signature only.
  EXPECT_EQ(downloads_->starts, 0);
  ASSERT_EQ(packages_->Packages().size(), 2u);
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kNotInstalled);
  EXPECT_EQ(Status(kSpektrafilm), LutPackageStatus::kNotInstalled);
  EXPECT_FALSE(fs::exists(root_ / "packages"));
  EXPECT_TRUE(library_->PackageReceipts().empty());
}

TEST_F(LutPackageServiceTest, TamperedFeedSignatureKeepsPreviousStatusAndReportsError) {
  StartLibrary();
  StartPackages();
  const BuiltPackage spectral = Publish(kSpectral, "r1", 0);
  ServeFeed({&spectral});
  feed_->feed.json.replace("\"r1\"", "\"r9\"");
  ASSERT_TRUE(packages_->CheckPackages());
  ASSERT_TRUE(WaitUntil([&] { return !packages_->checking(); }));
  EXPECT_FALSE(packages_->checked());
  EXPECT_FALSE(packages_->last_error().isEmpty());
  EXPECT_TRUE(packages_->Packages().empty());
  EXPECT_EQ(downloads_->starts, 0);
}

TEST_F(LutPackageServiceTest, UnchangedRemoteComparisonReadsInventoryWithoutHashing) {
  StartLibrary();
  StartPackages();
  const BuiltPackage spectral = Publish(kSpectral, "r1", 0);
  InstallRevision(spectral);
  const int                              starts  = downloads_->starts;

  // Change bytes on disk without a refresh: the comparison reads only the
  // published inventory, so it neither downloads nor rehashes files.
  const std::optional<LutPackageReceipt> receipt = Receipt(kSpectral);
  ASSERT_TRUE(receipt);
  const fs::path edited =
      root_ / LutPathFromUtf8(receipt->content_directory) / "kodak_vision3_250d_5207.cube";
  WriteBytes(edited, OfficialCube(kSpectral, "kodak_vision3_250d_5207", 7));
  Check();
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kCurrent);
  EXPECT_EQ(downloads_->starts, starts);
  EXPECT_TRUE(
      packages_->Package(QString::fromLatin1(kSpectral))->comparison.local_verification_complete);

  // A user refresh hashes the edited file and the same comparison asks for repair.
  ASSERT_EQ(library_->RefreshInventory(), LutLibraryService::Status::kOk);
  ASSERT_TRUE(WaitUntil([&] { return !library_->busy(); }));
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kRepairRequired);
  EXPECT_EQ(downloads_->starts, starts);
}

// ── Installation ────────────────────────────────────────────────────────────

TEST_F(LutPackageServiceTest, UpdateOnePackageKeepsOtherPackageAndUserFiles) {
  WriteBytes(root_ / "mine" / "look.cube",
             "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n"
             "0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
  WriteBytes(root_ / "notes.txt", "user notes");
  StartLibrary();
  StartPackages();
  const BuiltPackage spectral_r1 = Publish(kSpectral, "r1", 0);
  const BuiltPackage spektrafilm = Publish(kSpektrafilm, "r1", 1);
  ServeFeed({&spectral_r1, &spektrafilm});
  Check();
  ASSERT_TRUE(Install(kSpectral));
  ASSERT_TRUE(Install(kSpektrafilm));
  ASSERT_EQ(Status(kSpectral), LutPackageStatus::kCurrent);
  ASSERT_EQ(Status(kSpektrafilm), LutPackageStatus::kCurrent);
  EXPECT_EQ(InstalledFiles(kSpectral), spectral_r1.luts);
  const std::optional<LutPackageReceipt> other_before = Receipt(kSpektrafilm);
  const auto                             other_files  = InstalledFiles(kSpektrafilm);
  const std::string                      user_bytes   = ReadBytes(root_ / "mine" / "look.cube");

  const BuiltPackage                     spectral_r2  = Publish(kSpectral, "r2", 5);
  ServeFeed({&spectral_r2, &spektrafilm});
  Check();
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kUpdateAvailable);
  EXPECT_EQ(Status(kSpektrafilm), LutPackageStatus::kCurrent);
  ASSERT_TRUE(Install(kSpectral));

  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kCurrent);
  EXPECT_EQ(Status(kSpektrafilm), LutPackageStatus::kCurrent);
  EXPECT_EQ(InstalledFiles(kSpectral), spectral_r2.luts);
  EXPECT_EQ(Receipt(kSpectral)->revision, "r2");
  EXPECT_EQ(ContentDirectories(kSpectral).size(), 1u);  // r1 content retired.
  // The other package and user files are unchanged.
  EXPECT_EQ(Receipt(kSpektrafilm)->content_directory, other_before->content_directory);
  EXPECT_EQ(InstalledFiles(kSpektrafilm), other_files);
  EXPECT_EQ(ReadBytes(root_ / "mine" / "look.cube"), user_bytes);
  EXPECT_EQ(ReadBytes(root_ / "notes.txt"), "user notes");
  EXPECT_TRUE(library_->ReadEntry("mine/look.cube", [](const LutLibraryEntry& entry) {
    EXPECT_TRUE(entry.managed_package_id.empty());
  }));
  // Stable identities survive: the same official IDs are listed for the package.
  std::vector<std::string> ids;
  library_->ForEachPackageEntry(
      kSpectral, [&](const LutLibraryEntry& entry) { ids.push_back(entry.header.metadata->id); });
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids, (std::vector<std::string>{OfficialId(kSpectral, "kodak_vision3_250d_5207"),
                                           OfficialId(kSpectral, "kodak_vision3_500t_5219")}));
  EXPECT_TRUE(fs::is_empty(root_ / ".downloads"));  // Verified archives are removed.
}

TEST_F(LutPackageServiceTest, ArchiveHashMismatchKeepsInstalledPackage) {
  StartLibrary();
  StartPackages();
  const BuiltPackage r1 = Publish(kSpectral, "r1", 0);
  InstallRevision(r1);
  const std::optional<LutPackageReceipt> before = Receipt(kSpectral);

  // The descriptor is signed for r2 bytes, but the transfer delivers other bytes.
  const BuiltPackage                     r2     = Publish(kSpectral, "r2", 4);
  const BuiltPackage                     wrong  = BuildPackage(base_ / "wrong", kSpectral, "r2", 6);
  downloads_->sources[r2.descriptor["artifact"]["url"].get<std::string>()] = wrong.archive;
  ServeFeed({&r2});
  Check();
  ASSERT_TRUE(Install(kSpectral));

  const LutPackageService::PackageState* state = packages_->Package(QString::fromLatin1(kSpectral));
  EXPECT_EQ(state->status, LutPackageStatus::kError);
  EXPECT_NE(state->error.indexOf(QStringLiteral("SHA-256")), -1) << state->error.toStdString();
  EXPECT_EQ(Receipt(kSpectral)->content_directory, before->content_directory);
  EXPECT_EQ(Receipt(kSpectral)->revision, "r1");
  EXPECT_EQ(ContentDirectories(kSpectral), (std::vector<std::string>{before->content_directory}));
  EXPECT_EQ(InstalledFiles(kSpectral), r1.luts);
  EXPECT_TRUE(fs::is_empty(root_ / ".downloads"));  // The rejected archive is removed.
}

TEST_F(LutPackageServiceTest, ArchiveTraversalAndLinksAreRejected) {
  const BuiltPackage valid = BuildPackage(archives_, kSpectral, "r1", 0);
  LutPackageReceipt  expected;
  expected.package_id       = std::string(kSpectral);
  expected.revision         = "r1";
  expected.file_count       = valid.descriptor["file_count"].get<std::uint64_t>();
  expected.inventory_sha256 = valid.descriptor["inventory_sha256"].get<std::string>();
  expected.unpacked_bytes   = valid.descriptor["unpacked_bytes"].get<std::uint64_t>();

  // Each case is the complete valid package plus one bad entry, so the rejection
  // reason is the bad entry itself.
  const std::string lut     = valid.luts.begin()->second;
  struct BadEntry {
    std::string label;
    ArchiveItem item;
    std::string expected_error;
  };
  const std::vector<BadEntry> cases = {
      {"parent traversal", {"../escape.cube", lut}, "unsafe path"},
      {"nested traversal", {"stocks/../../escape.cube", lut}, "unsafe path"},
      {"absolute path", {"/escape.cube", lut}, "unsafe path"},
      {"drive path", {"C:/escape.cube", lut}, "unsafe path"},
      {"backslash path", {"stocks\\..\\..\\escape.cube", lut}, "unsafe path"},
      {"symbolic link",
       {"link.cube", "", ArchiveItem::Kind::kSymlink, "../escape.cube"},
       "link or special file"},
      {"case-colliding duplicate", {"KODAK_VISION3_250D_5207.cube", lut}, "duplicate destination"},
      {"unlisted file", {"extra.cube", lut}, "unlisted files"},
  };
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const BadEntry&          bad = cases[index];
    Json                     unused;
    std::vector<ArchiveItem> items = PackageItems(kSpectral, "r1", 0, &unused);
    items.push_back(bad.item);
    const fs::path archive     = base_ / "bad" / (std::to_string(index) + ".7z");
    const fs::path destination = base_ / "extract" / archive.stem();
    Write7z(archive, items);
    const LutPackageExtraction result =
        ExtractLutPackageArchive(archive, destination, expected, std::stop_token{});
    EXPECT_NE(result.error.find(bad.expected_error), std::string::npos)
        << bad.label << ": " << result.error;
    EXPECT_FALSE(result.inventory.has_value()) << bad.label;
    EXPECT_FALSE(fs::exists(base_ / "extract" / "escape.cube")) << bad.label;
    EXPECT_FALSE(fs::exists(base_ / "escape.cube")) << bad.label;
  }

  // Extraction never writes into an existing directory.
  fs::create_directories(base_ / "existing");
  const LutPackageExtraction existing =
      ExtractLutPackageArchive(valid.archive, base_ / "existing", expected, std::stop_token{});
  EXPECT_FALSE(existing.error.empty());
  EXPECT_TRUE(fs::is_empty(base_ / "existing"));

  // The same archive extracts completely into a new directory.
  const LutPackageExtraction accepted =
      ExtractLutPackageArchive(valid.archive, base_ / "accepted", expected, std::stop_token{});
  ASSERT_TRUE(accepted.error.empty()) << accepted.error;
  ASSERT_TRUE(accepted.inventory.has_value());
  for (const auto& [path, bytes] : valid.luts) {
    EXPECT_EQ(ReadBytes(base_ / "accepted" / LutPathFromUtf8(path)), bytes) << path;
  }
}

TEST_F(LutPackageServiceTest, PublishingToolArchiveExtractsWithBundledLibarchive) {
  // Written by scripts/luts/prepare_lut_packages.py (py7zr, LZMA2 preset 9) from two
  // official LUTs (one nested) and LICENSE.txt; the JSON is its feed descriptor.
  const fs::path fixtures(ALCEDO_LUT_METADATA_FIXTURE_DIR);
  const Json descriptor = Json::parse(ReadBytes(fixtures / "spectral_film_lut_py7zr_package.json"));
  LutPackageReceipt expected;
  expected.package_id       = descriptor["id"].get<std::string>();
  expected.revision         = descriptor["revision"].get<std::string>();
  expected.file_count       = descriptor["file_count"].get<std::uint64_t>();
  expected.inventory_sha256 = descriptor["inventory_sha256"].get<std::string>();
  expected.unpacked_bytes   = descriptor["unpacked_bytes"].get<std::uint64_t>();

  const fs::path    archive = fixtures / "spectral_film_lut_py7zr_package.7z";
  const std::string bytes   = ReadBytes(archive);
  EXPECT_EQ(Sha256Hex(bytes), descriptor["artifact"]["sha256"].get<std::string>());
  const LutPackageExtraction result =
      ExtractLutPackageArchive(archive, base_ / "py7zr", expected, std::stop_token{});
  ASSERT_TRUE(result.error.empty()) << result.error;
  ASSERT_TRUE(result.inventory.has_value());
  EXPECT_EQ(result.inventory->luts.size(), 2u);
  EXPECT_TRUE(fs::is_regular_file(base_ / "py7zr" / "stocks" / "kodak_vision3_500t_5219.cube"));
  EXPECT_TRUE(fs::is_regular_file(base_ / "py7zr" / "LICENSE.txt"));
  const LutHeaderReadResult header =
      ReadLutHeaderFile(base_ / "py7zr" / "kodak_vision3_250d_5207.cube");
  ASSERT_EQ(header.error, LutHeaderError::kNone);
  EXPECT_EQ(header.header.Origin(), LutOrigin::kAlcedo);
}

TEST_F(LutPackageServiceTest, InterruptedActivationRecoversCommittedReceipt) {
  StartLibrary();
  StartPackages();
  const BuiltPackage r1 = Publish(kSpectral, "r1", 0);
  InstallRevision(r1);
  const std::string        r1_content     = Receipt(kSpectral)->content_directory;

  // Before the commit point: the receipt write fails, so r1 stays active and the
  // new content directory is removed.
  bool                     fail_receipt   = true;
  bool                     fail_inventory = false;
  LutLibraryFileOperations io             = LutLibraryFileOperations::Default();
  io.write_package_receipt = [&](const fs::path& root, const LutPackageReceipt& receipt) {
    return fail_receipt ? std::string("injected receipt failure")
                        : WriteLutPackageReceiptFile(root, receipt);
  };
  io.write_inventory = [&](const fs::path& root, const LutLibraryInventory& inventory) {
    return fail_inventory ? std::string("injected inventory failure")
                          : WriteLutLibraryInventoryFile(root, inventory);
  };
  StartLibrary(io);
  StartPackages();
  const BuiltPackage r2 = Publish(kSpectral, "r2", 3);
  ServeFeed({&r2});
  Check();
  ASSERT_TRUE(Install(kSpectral));
  EXPECT_EQ(library_->LastResult().status, LutLibraryService::Status::kIoError);
  EXPECT_TRUE(library_->LastResult().committed_package_id.empty());
  EXPECT_EQ(Receipt(kSpectral)->content_directory, r1_content);
  EXPECT_EQ(ContentDirectories(kSpectral), (std::vector<std::string>{r1_content}));
  EXPECT_EQ(InstalledFiles(kSpectral), r1.luts);

  // A process stop between extraction and commit leaves an inactive directory;
  // the next start retires it and keeps r1 active.
  WriteBytes(root_ / "packages" / std::string(kSpectral) / "content" / "stopped" / "x.cube",
             OfficialCube(kSpectral, "kodak_vision3_250d_5207", 9));
  StartLibrary(io);
  EXPECT_EQ(ContentDirectories(kSpectral), (std::vector<std::string>{r1_content}));
  EXPECT_EQ(InstalledFiles(kSpectral), r1.luts);

  // After the commit point: the inventory write fails. The installation reports
  // that the package is installed and a refresh is required.
  fail_receipt   = false;
  fail_inventory = true;
  StartPackages();
  ServeFeed({&r2});
  Check();
  ASSERT_TRUE(Install(kSpectral));
  EXPECT_EQ(library_->LastResult().status, LutLibraryService::Status::kPersistenceError);
  EXPECT_EQ(library_->LastResult().committed_package_id, std::string(kSpectral));
  EXPECT_NE(library_->LastResult().message.find("inventory refresh is required"),
            std::string::npos);
  std::vector<LutPackageReceipt> on_disk;
  std::vector<LutScanDiagnostic> diagnostics;
  ReadLutPackageReceipts(root_, &on_disk, &diagnostics);
  ASSERT_EQ(on_disk.size(), 1u);
  EXPECT_EQ(on_disk.front().revision, "r2");
  EXPECT_NE(on_disk.front().content_directory, r1_content);

  // Restart: the persisted inventory disagrees with the committed receipt and is rebuilt.
  fail_inventory = false;
  StartLibrary(io);
  StartPackages();
  EXPECT_EQ(Receipt(kSpectral)->revision, "r2");
  EXPECT_EQ(InstalledFiles(kSpectral), r2.luts);
  Check();
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kCurrent);
}

TEST_F(LutPackageServiceTest, CancelBeforeActivationKeepsInstalledPackage) {
  StartLibrary();
  StartPackages();
  const BuiltPackage r1 = Publish(kSpectral, "r1", 0);
  InstallRevision(r1);
  const std::optional<LutPackageReceipt> before = Receipt(kSpectral);

  // Cancel during the download.
  const BuiltPackage                     r2     = Publish(kSpectral, "r2", 2);
  ServeFeed({&r2});
  Check();
  downloads_->hold = true;
  ASSERT_TRUE(packages_->InstallPackage(QString::fromLatin1(kSpectral)));
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kDownloading);
  EXPECT_TRUE(packages_->CancelInstall(QString::fromLatin1(kSpectral)));
  ASSERT_TRUE(WaitUntil([&] { return Status(kSpectral) != LutPackageStatus::kDownloading; }));
  EXPECT_NE(
      packages_->Package(QString::fromLatin1(kSpectral))->error.indexOf(QStringLiteral("canceled")),
      -1);
  EXPECT_EQ(Receipt(kSpectral)->content_directory, before->content_directory);
  EXPECT_EQ(Receipt(kSpectral)->revision, "r1");

  // Cancel during verification and extraction (the stop is observed before the commit).
  std::stop_source stop;
  stop.request_stop();
  LutPackageInstallRequest request;
  request.archive_path              = r2.archive;
  request.expected.package_id       = std::string(kSpectral);
  request.expected.revision         = "r2";
  request.expected.file_count       = r2.descriptor["file_count"].get<std::uint64_t>();
  request.expected.inventory_sha256 = r2.descriptor["inventory_sha256"].get<std::string>();
  request.expected.unpacked_bytes   = r2.descriptor["unpacked_bytes"].get<std::uint64_t>();
  request.expected.artifact_size    = r2.descriptor["artifact"]["size"].get<std::uint64_t>();
  request.expected.artifact_sha256  = r2.descriptor["artifact"]["sha256"].get<std::string>();
  const LutPackageInstallOutcome outcome =
      InstallLutPackageArchive(root_, request, LutPackageInstallSteps{}, stop.get_token(), {});
  EXPECT_TRUE(outcome.canceled);
  EXPECT_FALSE(outcome.committed);
  std::vector<LutPackageReceipt> on_disk;
  std::vector<LutScanDiagnostic> diagnostics;
  ReadLutPackageReceipts(root_, &on_disk, &diagnostics);
  ASSERT_EQ(on_disk.size(), 1u);
  EXPECT_EQ(on_disk.front().content_directory, before->content_directory);
  EXPECT_EQ(ContentDirectories(kSpectral), (std::vector<std::string>{before->content_directory}));

  // A retry after cancellation installs normally.
  downloads_->hold = false;
  ASSERT_TRUE(Install(kSpectral));
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kCurrent);
  EXPECT_EQ(InstalledFiles(kSpectral), r2.luts);
}

// ── Settings package rows ───────────────────────────────────────────────────

TEST(LutPackageActionTest, EachStatusOffersOneSettingsAction) {
  EXPECT_EQ(LutPackageActionFor(LutPackageStatus::kNotInstalled), LutPackageAction::kInstall);
  EXPECT_EQ(LutPackageActionFor(LutPackageStatus::kUpdateAvailable), LutPackageAction::kUpdate);
  EXPECT_EQ(LutPackageActionFor(LutPackageStatus::kRepairRequired), LutPackageAction::kRepair);
  EXPECT_EQ(LutPackageActionFor(LutPackageStatus::kError), LutPackageAction::kRetry);
  for (const LutPackageStatus status :
       {LutPackageStatus::kCurrent, LutPackageStatus::kChecking, LutPackageStatus::kDownloading,
        LutPackageStatus::kVerifying, LutPackageStatus::kInstalling}) {
    EXPECT_EQ(LutPackageActionFor(status), LutPackageAction::kNone);
  }
}

auto RowFor(const LutPackageService& service, std::string_view package_id) -> QVariantMap {
  for (const QVariant& row : service.packages()) {
    const QVariantMap map = row.toMap();
    if (map.value(QStringLiteral("id")).toString().toStdString() == package_id) return map;
  }
  return {};
}

TEST_F(LutPackageServiceTest, SettingsRowsFollowDownloadCancelAndRetry) {
  StartLibrary();
  StartPackages();
  const BuiltPackage spectral    = Publish(kSpectral, "r1", 0);
  const BuiltPackage spektrafilm = Publish(kSpektrafilm, "r1", 1);
  ServeFeed({&spectral, &spektrafilm});
  Check();

  QVariantMap row = RowFor(*packages_, kSpectral);
  EXPECT_EQ(row.value(QStringLiteral("name")).toString(), QString::fromLatin1(kSpectral));
  EXPECT_EQ(row.value(QStringLiteral("action")).toString(), QStringLiteral("install"));
  EXPECT_FALSE(row.value(QStringLiteral("busy")).toBool());
  EXPECT_FALSE(row.value(QStringLiteral("cancelable")).toBool());
  EXPECT_EQ(row.value(QStringLiteral("installedFileCount")).toULongLong(), 0u);

  // A held transfer: only the chosen package is busy and cancelable.
  downloads_->hold = true;
  ASSERT_TRUE(packages_->InstallPackage(QString::fromLatin1(kSpectral)));
  row = RowFor(*packages_, kSpectral);
  EXPECT_EQ(row.value(QStringLiteral("status")).toString(), QStringLiteral("downloading"));
  EXPECT_TRUE(row.value(QStringLiteral("action")).toString().isEmpty());
  EXPECT_TRUE(row.value(QStringLiteral("busy")).toBool());
  EXPECT_TRUE(row.value(QStringLiteral("cancelable")).toBool());
  const QVariantMap other = RowFor(*packages_, kSpektrafilm);
  EXPECT_EQ(other.value(QStringLiteral("action")).toString(), QStringLiteral("install"));
  EXPECT_FALSE(other.value(QStringLiteral("busy")).toBool());
  EXPECT_FALSE(other.value(QStringLiteral("cancelable")).toBool());
  // A second package action is refused while the first runs; the first keeps running.
  EXPECT_FALSE(packages_->InstallPackage(QString::fromLatin1(kSpektrafilm)));
  EXPECT_EQ(downloads_->starts, 1);

  ASSERT_TRUE(packages_->CancelInstall(QString::fromLatin1(kSpectral)));
  ASSERT_TRUE(WaitUntil([&] { return Status(kSpectral) == LutPackageStatus::kError; }));
  row = RowFor(*packages_, kSpectral);
  EXPECT_EQ(row.value(QStringLiteral("action")).toString(), QStringLiteral("retry"));
  EXPECT_FALSE(row.value(QStringLiteral("busy")).toBool());
  EXPECT_FALSE(row.value(QStringLiteral("error")).toString().isEmpty());
  EXPECT_TRUE(library_->PackageReceipts().empty());

  // Retry runs the same installation and ends Current with no action.
  downloads_->hold = false;
  ASSERT_TRUE(Install(kSpectral));
  row = RowFor(*packages_, kSpectral);
  EXPECT_EQ(row.value(QStringLiteral("status")).toString(), QStringLiteral("current"));
  EXPECT_TRUE(row.value(QStringLiteral("action")).toString().isEmpty());
  EXPECT_TRUE(row.value(QStringLiteral("error")).toString().isEmpty());
  EXPECT_EQ(row.value(QStringLiteral("installedRevision")).toString(), QStringLiteral("r1"));
  EXPECT_EQ(row.value(QStringLiteral("installedFileCount")).toULongLong(), spectral.luts.size());
}

TEST_F(LutPackageServiceTest, FeedPackageNameIsShownInsteadOfTheId) {
  StartLibrary();
  StartPackages();
  BuiltPackage spectral          = Publish(kSpectral, "r1", 0);
  spectral.descriptor["name"]    = "Spectral Film LUT";
  const BuiltPackage spektrafilm = Publish(kSpektrafilm, "r1", 1);
  ServeFeed({&spectral, &spektrafilm});
  Check();
  EXPECT_EQ(RowFor(*packages_, kSpectral).value(QStringLiteral("name")).toString(),
            QStringLiteral("Spectral Film LUT"));
  EXPECT_EQ(RowFor(*packages_, kSpektrafilm).value(QStringLiteral("name")).toString(),
            QString::fromLatin1(kSpektrafilm));
}

// ── Replacement policy (plan section 3) ─────────────────────────────────────

TEST_F(LutPackageServiceTest, OfficialHashMismatchDoesNotChangeOwnership) {
  StartLibrary();
  StartPackages();
  const BuiltPackage r1 = Publish(kSpectral, "r1", 0);
  InstallRevision(r1);
  const std::string path = Receipt(kSpectral)->content_directory + "/kodak_vision3_250d_5207.cube";
  std::string       before_sha;
  library_->ReadEntry(path, [&](const LutLibraryEntry& entry) { before_sha = entry.sha256; });

  WriteBytes(root_ / LutPathFromUtf8(path), OfficialCube(kSpectral, "kodak_vision3_250d_5207", 8));
  ASSERT_EQ(library_->RefreshInventory(), LutLibraryService::Status::kOk);
  ASSERT_TRUE(WaitUntil([&] { return !library_->busy(); }));

  ASSERT_TRUE(library_->ReadEntry(path, [&](const LutLibraryEntry& entry) {
    EXPECT_TRUE(entry.IsOfficial());
    EXPECT_EQ(entry.managed_package_id, std::string(kSpectral));
    EXPECT_NE(entry.sha256, before_sha);
    EXPECT_EQ(entry.header.metadata->id, OfficialId(kSpectral, "kodak_vision3_250d_5207"));
  }));
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kRepairRequired);
}

TEST_F(LutPackageServiceTest, EditedOfficialLutIsReplacedWithoutPromptOrUserCopy) {
  StartLibrary();
  StartPackages();
  const BuiltPackage r1 = Publish(kSpectral, "r1", 0);
  InstallRevision(r1);
  const std::string relative = "kodak_vision3_250d_5207.cube";
  WriteBytes(root_ / LutPathFromUtf8(Receipt(kSpectral)->content_directory) / relative,
             OfficialCube(kSpectral, "kodak_vision3_250d_5207", 8));
  ASSERT_EQ(library_->RefreshInventory(), LutLibraryService::Status::kOk);
  ASSERT_TRUE(WaitUntil([&] { return !library_->busy(); }));
  ASSERT_EQ(Status(kSpectral), LutPackageStatus::kRepairRequired);
  const std::size_t entries_before = library_->EntryCount();

  // Repair: one user action, no confirmation step, no preserved copy.
  ASSERT_TRUE(Install(kSpectral));
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kCurrent);
  EXPECT_EQ(InstalledFiles(kSpectral), r1.luts);
  EXPECT_EQ(library_->EntryCount(), entries_before);
  EXPECT_FALSE(fs::exists(root_ / "user"));
  EXPECT_EQ(ContentDirectories(kSpectral).size(), 1u);
  const std::string path = Receipt(kSpectral)->content_directory + "/" + relative;
  ASSERT_TRUE(library_->ReadEntry(path, [&](const LutLibraryEntry& entry) {
    EXPECT_EQ(entry.header.metadata->id, OfficialId(kSpectral, "kodak_vision3_250d_5207"));
    EXPECT_EQ(entry.managed_package_id, std::string(kSpectral));
  }));
}

TEST_F(LutPackageServiceTest, UserDeclaredVariantSurvivesPackageReplacement) {
  StartLibrary();
  StartPackages();
  const BuiltPackage r1 = Publish(kSpectral, "r1", 0);
  InstallRevision(r1);
  const std::string relative     = "stocks/kodak_vision3_500t_5219.cube";
  const std::string user_variant = OfficialCube(kSpectral, "kodak_vision3_500t_5219", 4, "user");
  WriteBytes(root_ / LutPathFromUtf8(Receipt(kSpectral)->content_directory) / relative,
             user_variant);
  ASSERT_EQ(library_->RefreshInventory(), LutLibraryService::Status::kOk);
  ASSERT_TRUE(WaitUntil([&] { return !library_->busy(); }));
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kRepairRequired);

  ASSERT_TRUE(Install(kSpectral));
  EXPECT_EQ(Status(kSpectral), LutPackageStatus::kCurrent);
  // The user's bytes survive as a loose user entry outside package content.
  const std::string moved = "user/" + std::string(kSpectral) + "/" + relative;
  EXPECT_EQ(ReadBytes(root_ / LutPathFromUtf8(moved)), user_variant);
  ASSERT_TRUE(library_->ReadEntry(moved, [&](const LutLibraryEntry& entry) {
    EXPECT_TRUE(entry.managed_package_id.empty());
    EXPECT_EQ(entry.header.Origin(), LutOrigin::kUser);
  }));
  // The official ID resolves to the newly installed official bytes.
  EXPECT_EQ(InstalledFiles(kSpectral), r1.luts);
  int official = 0;
  library_->ForEachPackageEntry(kSpectral, [&](const LutLibraryEntry& entry) {
    if (entry.header.metadata->id == OfficialId(kSpectral, "kodak_vision3_500t_5219")) {
      ++official;
      EXPECT_TRUE(entry.IsOfficial());
    }
  });
  EXPECT_EQ(official, 1);
}

// ── Shared transfer admission ───────────────────────────────────────────────

TEST_F(LutPackageServiceTest, ApplicationAndLutDownloadsShareAdmission) {
  const QByteArray previous = qgetenv("ALCEDO_ARIA2C_BINARY");
  qputenv("ALCEDO_ARIA2C_BINARY", QByteArrayLiteral(ALCEDO_FAKE_ARIA2_PATH));
  {
    DownloadService transfers;
    StartLibrary();
    packages_ = std::make_unique<LutPackageService>(
        MakeOptions(std::make_unique<DownloadServiceLutArchiveDownloader>(transfers)), *library_);
    const BuiltPackage spectral = Publish(kSpectral, "r1", 0);
    ServeFeed({&spectral});
    Check();

    // An application update transfer holds the shared admission.
    DownloadRequest update;
    update.id = QStringLiteral("application-update");
    update.items.push_back({QUrl(QStringLiteral("https://example.invalid/update.exe")),
                            QString::fromStdString(LutPathToUtf8(base_ / "update.exe")),
                            1,
                            {}});
    ASSERT_TRUE(transfers.Start(update));
    QSignalSpy finished(&transfers, &DownloadService::Finished);

    EXPECT_FALSE(packages_->InstallPackage(QString::fromLatin1(kSpectral)));
    const LutPackageService::PackageState* state =
        packages_->Package(QString::fromLatin1(kSpectral));
    EXPECT_EQ(state->status, LutPackageStatus::kError);
    EXPECT_NE(state->error.indexOf(QStringLiteral("Another download")), -1);
    EXPECT_EQ(transfers.ActiveRequestId(), QStringLiteral("application-update"));
    EXPECT_FALSE(packages_->CancelInstall(QString::fromLatin1(kSpectral)));
    EXPECT_FALSE(fs::exists(root_ / "packages"));

    transfers.Cancel(QStringLiteral("application-update"));
    ASSERT_TRUE(finished.wait(15000));
    packages_.reset();
  }
  if (previous.isNull()) {
    qunsetenv("ALCEDO_ARIA2C_BINARY");
  } else {
    qputenv("ALCEDO_ARIA2C_BINARY", previous);
  }
}

}  // namespace
}  // namespace alcedo::test
