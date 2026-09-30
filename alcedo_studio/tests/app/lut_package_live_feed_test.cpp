//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Live checks of the published LUT package feed with the production components: the HTTPS feed
// fetch, the build's Ed25519 update key, DownloadService with aria2c, the bundled libarchive, and a
// temporary library root (docs/roadmap/alcedo_studio/ui/lut_library_and_package_management_plan.md
// L6B step 6).
//
// The tests need network access and run only when ALCEDO_LUT_LIVE_FEED_URL names the deployed
// feed (for example https://static.aoraw.org/luts/v1/manifest.json) and ALCEDO_ARIA2C_BINARY names
// aria2c. Otherwise every test is skipped. They never write outside their temporary directory.
// A Windows debug test runtime holds copies of the Qt DLLs without Qt's plugin folder; set
// QT_PLUGIN_PATH to the Qt `plugins` directory so HTTPS finds a TLS backend (installed
// applications ship `plugins/tls` through Qt deployment).

#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>

extern "C" {
#include <ed25519.h>
}

#include "app/download_service.hpp"
#include "app/lut_library_service.hpp"
#include "app/lut_package_service.hpp"

namespace alcedo::test {
namespace {

namespace fs = std::filesystem;

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
  explicit MemoryPackagePreferences(quint64 sequence) : sequence_(sequence) {}
  [[nodiscard]] auto LoadTrustedSequence() const -> quint64 override { return sequence_; }
  void               SaveTrustedSequence(quint64 sequence) override { sequence_ = sequence; }

 private:
  quint64 sequence_;
};

/// A valid Ed25519 public key that did not sign the feed.
auto ForeignPublicKey() -> QByteArray {
  unsigned char seed[32] = {};
  for (std::size_t index = 0; index < sizeof(seed); ++index) {
    seed[index] = static_cast<unsigned char>(index * 11 + 3);
  }
  unsigned char public_key[32]  = {};
  unsigned char private_key[64] = {};
  ed25519_create_keypair(public_key, private_key, seed);
  return QByteArray(reinterpret_cast<const char*>(public_key), sizeof(public_key));
}

auto ReadBytes(const fs::path& path) -> std::string {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class LutPackageLiveFeedTest : public ::testing::Test {
 protected:
  void SetUp() override {
    feed_url_ = QUrl(qEnvironmentVariable("ALCEDO_LUT_LIVE_FEED_URL"));
    if (!feed_url_.isValid() || feed_url_.isEmpty() ||
        qEnvironmentVariable("ALCEDO_ARIA2C_BINARY").isEmpty()) {
      GTEST_SKIP() << "Set ALCEDO_LUT_LIVE_FEED_URL and ALCEDO_ARIA2C_BINARY to run live checks.";
    }
    // A short root like the default `~/.alcedo/luts`: package content paths add about 160
    // characters, and on Windows the LUT readers do not open paths longer than 260 characters.
    std::random_device device;
    base_ = LutPathFromUtf8(QDir::tempPath().toStdString()) /
            ("alcedo-lut-live-" + std::to_string(device()));
    fs::create_directories(base_ / "library");
    base_ = fs::weakly_canonical(base_);
    root_ = base_ / "library";
  }

  void TearDown() override {
    packages_.reset();
    if (library_) library_->Shutdown();
    library_.reset();
    transfers_.reset();
    std::error_code error;
    if (!base_.empty()) fs::remove_all(base_, error);
  }

  static auto WaitUntil(const std::function<bool()>& condition, int timeout_ms = 600000) -> bool {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout_ms) QTest::qWait(20);
    return condition();
  }

  void Start(quint64 trusted_sequence = 1, QByteArray public_key = {}) {
    LutLibraryServiceOptions library_options;
    library_options.preferences  = std::make_unique<MemoryLibraryPreferences>(root_);
    library_options.default_root = root_;
    library_options.open_url     = [](const QUrl&) { return true; };
    library_                     = std::make_unique<LutLibraryService>(std::move(library_options));
    library_->Start();
    ASSERT_TRUE(WaitUntil([&] { return !library_->busy(); }));

    transfers_                       = std::make_unique<DownloadService>();
    // The production options carry the build's update key; only the feed URL comes from the
    // environment, so a build without a configured feed can still check the deployed one.
    LutPackageServiceOptions options = LutPackageServiceOptions::FromBuildConfiguration();
    options.feed_url                 = feed_url_;
    if (!public_key.isEmpty()) options.public_key = std::move(public_key);
    options.downloader  = std::make_unique<DownloadServiceLutArchiveDownloader>(*transfers_);
    options.preferences = std::make_unique<MemoryPackagePreferences>(trusted_sequence);
    packages_           = std::make_unique<LutPackageService>(std::move(options), *library_);
    ASSERT_TRUE(packages_->enabled()) << "The build has no update public key.";
  }

  void Check() {
    ASSERT_TRUE(packages_->CheckPackages());
    ASSERT_TRUE(WaitUntil([&] { return !packages_->checking() && !library_->busy(); }, 60000));
  }

  auto State(const QString& id) -> const LutPackageService::PackageState* {
    return packages_->Package(id);
  }

  auto Settled(const QString& id) -> bool {
    const LutPackageService::PackageState* state = State(id);
    return state != nullptr && state->status != LutPackageStatus::kDownloading &&
           state->status != LutPackageStatus::kVerifying &&
           state->status != LutPackageStatus::kInstalling && !library_->busy();
  }

  auto Install(const QString& id) -> bool {
    if (!packages_->InstallPackage(id)) return false;
    return WaitUntil([&] { return Settled(id); });
  }

  auto EntryCountOf(const QString& id) -> std::size_t {
    std::size_t count = 0;
    library_->ForEachPackageEntry(id.toStdString(), [&](const LutLibraryEntry& entry) {
      EXPECT_TRUE(entry.IsOfficial()) << entry.relative_path;
      EXPECT_EQ(entry.managed_package_id, id.toStdString());
      ++count;
    });
    return count;
  }

  QUrl                               feed_url_;
  fs::path                           base_;
  fs::path                           root_;
  std::unique_ptr<LutLibraryService> library_;
  std::unique_ptr<DownloadService>   transfers_;
  std::unique_ptr<LutPackageService> packages_;
};

TEST_F(LutPackageLiveFeedTest, LiveFeedInstallsEachPackageIndependentlyAndRechecksCurrent) {
  Start();
  Check();
  ASSERT_TRUE(packages_->last_error().isEmpty()) << packages_->last_error().toStdString();
  ASSERT_GE(packages_->Packages().size(), 2u);
  for (const LutPackageService::PackageState& package : packages_->Packages()) {
    EXPECT_EQ(package.status, LutPackageStatus::kNotInstalled)
        << package.descriptor.id.toStdString();
    EXPECT_FALSE(package.descriptor.name.isEmpty());
  }
  EXPECT_TRUE(library_->PackageReceipts().empty());

  std::size_t installed_entries = 0;
  for (std::size_t index = 0; index < packages_->Packages().size(); ++index) {
    const QString id = packages_->Packages()[index].descriptor.id;
    ASSERT_TRUE(Install(id)) << id.toStdString();
    ASSERT_EQ(State(id)->status, LutPackageStatus::kCurrent)
        << id.toStdString() << ": " << State(id)->error.toStdString();
    EXPECT_EQ(EntryCountOf(id), State(id)->descriptor.file_count);
    installed_entries += State(id)->descriptor.file_count;
    // Packages after this one are still not installed.
    for (std::size_t later = index + 1; later < packages_->Packages().size(); ++later) {
      EXPECT_EQ(packages_->Packages()[later].status, LutPackageStatus::kNotInstalled);
    }
  }
  EXPECT_EQ(library_->EntryCount(), installed_entries);
  EXPECT_TRUE(library_->inventory_complete());
  // Downloaded archives are removed after activation.
  std::error_code error;
  EXPECT_TRUE(!fs::exists(root_ / ".downloads", error) ||
              fs::is_empty(root_ / ".downloads", error));

  // A later check compares the same feed without downloading anything.
  Check();
  for (const LutPackageService::PackageState& package : packages_->Packages()) {
    EXPECT_EQ(package.status, LutPackageStatus::kCurrent) << package.descriptor.id.toStdString();
  }
}

TEST_F(LutPackageLiveFeedTest, LiveDownloadCancelKeepsLibraryAndRetryInstalls) {
  Start();
  Check();
  ASSERT_FALSE(packages_->Packages().empty()) << packages_->last_error().toStdString();
  // The largest archive gives the cancellation the widest window.
  const auto largest = std::max_element(
      packages_->Packages().begin(), packages_->Packages().end(), [](const auto& a, const auto& b) {
        return a.descriptor.artifact.size < b.descriptor.artifact.size;
      });
  const QString id = largest->descriptor.id;

  ASSERT_TRUE(packages_->InstallPackage(id));
  EXPECT_EQ(State(id)->status, LutPackageStatus::kDownloading);
  ASSERT_TRUE(packages_->CancelInstall(id));
  ASSERT_TRUE(WaitUntil([&] { return Settled(id); }, 60000));
  EXPECT_EQ(State(id)->status, LutPackageStatus::kError);
  EXPECT_FALSE(State(id)->error.isEmpty());
  EXPECT_TRUE(library_->PackageReceipts().empty());
  EXPECT_EQ(library_->EntryCount(), 0u);

  ASSERT_TRUE(Install(id));
  EXPECT_EQ(State(id)->status, LutPackageStatus::kCurrent) << State(id)->error.toStdString();
  EXPECT_EQ(EntryCountOf(id), State(id)->descriptor.file_count);
}

TEST_F(LutPackageLiveFeedTest, LiveRepairReplacesAnEditedOfficialFile) {
  Start();
  Check();
  ASSERT_FALSE(packages_->Packages().empty()) << packages_->last_error().toStdString();
  const QString id = packages_->Packages().front().descriptor.id;
  ASSERT_TRUE(Install(id));
  ASSERT_EQ(State(id)->status, LutPackageStatus::kCurrent) << State(id)->error.toStdString();

  // Edit the numeric rows of one installed file; its official metadata line stays.
  std::string relative;
  library_->ForEachPackageEntry(id.toStdString(), [&](const LutLibraryEntry& entry) {
    if (relative.empty()) relative = entry.relative_path;
  });
  ASSERT_FALSE(relative.empty());
  const fs::path    file     = root_ / LutPathFromUtf8(relative);
  const std::string original = ReadBytes(file);
  {
    std::ofstream output(file, std::ios::binary | std::ios::app);
    output << "0 0 0\n";
  }
  ASSERT_EQ(library_->RefreshInventory(), LutLibraryService::Status::kOk);
  ASSERT_TRUE(WaitUntil([&] { return !library_->busy(); }));
  ASSERT_EQ(State(id)->status, LutPackageStatus::kRepairRequired);
  EXPECT_EQ(LutPackageActionFor(State(id)->status), LutPackageAction::kRepair);

  ASSERT_TRUE(Install(id));
  EXPECT_EQ(State(id)->status, LutPackageStatus::kCurrent) << State(id)->error.toStdString();
  std::string repaired_relative;
  library_->ForEachPackageEntry(id.toStdString(), [&](const LutLibraryEntry& entry) {
    if (fs::path(entry.relative_path).filename() == fs::path(relative).filename()) {
      repaired_relative = entry.relative_path;
    }
  });
  ASSERT_FALSE(repaired_relative.empty());
  EXPECT_EQ(ReadBytes(root_ / LutPathFromUtf8(repaired_relative)), original);
  // No user copy of the edited file was kept.
  EXPECT_FALSE(fs::exists(root_ / "user"));
}

TEST_F(LutPackageLiveFeedTest, LiveFeedRejectsForeignKeyAndOlderSequenceWithoutChanges) {
  // A key that did not sign the feed.
  Start(1, ForeignPublicKey());
  Check();
  EXPECT_EQ(packages_->last_error(), QStringLiteral("The LUT package signature is not valid."));
  EXPECT_TRUE(packages_->Packages().empty());
  EXPECT_FALSE(packages_->checked());
  packages_.reset();
  library_->Shutdown();
  library_.reset();
  transfers_.reset();

  // A trusted sequence newer than the published feed rejects it as a rollback.
  Start(99999999999999ULL);
  Check();
  EXPECT_EQ(packages_->last_error(),
            QStringLiteral("The LUT package manifest sequence is older than a trusted manifest."));
  EXPECT_TRUE(packages_->Packages().empty());
  EXPECT_TRUE(library_->PackageReceipts().empty());
}

}  // namespace
}  // namespace alcedo::test
