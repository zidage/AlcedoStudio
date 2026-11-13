//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// Temporary LUT library roots and in-memory preferences shared by the LutLibraryService tests.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "app/lut_library_service.hpp"

namespace alcedo::test {

namespace fs  = std::filesystem;
using Service = LutLibraryService;

inline constexpr std::string_view kOfficialComment =
    R"(# ALCEDO_LUT {"schema":1,"id":"spectral_film_lut:kodak_vision3_250d_5207","origin":"alcedo","category":"film_simulation","source":{"id":"spectral_film_lut","name":"Spectral Film LUT"},"film":{"id":"kodak_vision3_250d_5207","name":"Vision3 250D 5207","brand":"Kodak"},"input_space":"ACEScc","output_space":"ACEScc"})";

inline constexpr std::string_view kNumericTable =
    "LUT_3D_SIZE 2\n"
    "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";

inline auto UserCube(std::string_view title) -> std::string {
  return "TITLE \"" + std::string(title) + "\"\n" + std::string(kNumericTable);
}

inline auto OfficialCube() -> std::string {
  return std::string(kOfficialComment) + "\n" + std::string(kNumericTable);
}

inline auto U8(std::u8string_view text) -> std::string {
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

inline auto ReadBytes(const fs::path& path) -> std::string {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline void WriteBytes(const fs::path& path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Preferences held in memory and shared between service instances, so a test
/// can model an application restart. `fail_save` rejects the root commit.
struct PreferenceStore {
  std::optional<fs::path> root;
  QStringList             legacy_favorites;
  bool                    fail_save = false;
};

class MemoryPreferences final : public LutLibraryPreferences {
 public:
  explicit MemoryPreferences(std::shared_ptr<PreferenceStore> store) : store_(std::move(store)) {}
  [[nodiscard]] auto LoadRoot() const -> std::optional<fs::path> override { return store_->root; }
  [[nodiscard]] auto SaveRoot(const fs::path& root) -> bool override {
    if (store_->fail_save) return false;
    store_->root = root;
    return true;
  }
  [[nodiscard]] auto LoadLegacyFavoritePaths() const -> QStringList override {
    return store_->legacy_favorites;
  }
  void SaveLegacyFavoritePaths(const QStringList& paths) override {
    store_->legacy_favorites = paths;
  }

 private:
  std::shared_ptr<PreferenceStore> store_;
};

/// Temporary library roots under the test working directory.
class LutLibraryRootFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device device;
    base_ = fs::current_path() / "lut_library_service_test" / std::to_string(device());
    fs::create_directories(base_);
    base_         = fs::weakly_canonical(base_);
    library_root_ = base_ / "library";
    preferences_  = std::make_shared<PreferenceStore>();
  }
  void TearDown() override {
    std::error_code error;
    fs::remove_all(base_, error);
  }

  auto MakeService(LutLibraryFileOperations io = LutLibraryFileOperations::Default())
      -> std::unique_ptr<Service> {
    LutLibraryServiceOptions options;
    options.preferences     = std::make_unique<MemoryPreferences>(preferences_);
    options.default_root    = library_root_;
    options.file_operations = std::move(io);
    options.open_url        = [this](const QUrl& url) {
      opened_urls_.push_back(url);
      return open_result_;
    };
    options.scan_worker_count = 2;
    return std::make_unique<Service>(std::move(options));
  }

  static auto WaitUntilIdle(const Service& service) -> bool {
    QElapsedTimer timer;
    timer.start();
    while (service.busy() && timer.elapsed() < 20000) QTest::qWait(5);
    return !service.busy();
  }

  auto StartService(LutLibraryFileOperations io = LutLibraryFileOperations::Default())
      -> std::unique_ptr<Service> {
    auto service = MakeService(std::move(io));
    service->Start();
    EXPECT_TRUE(WaitUntilIdle(*service));
    return service;
  }

  static auto EntryPaths(const Service& service) -> std::vector<std::string> {
    std::vector<std::string> paths;
    service.ForEachEntry(
        [&](const LutLibraryEntry& entry) { paths.push_back(entry.relative_path); });
    return paths;
  }

  static auto FindEntry(const Service& service, std::string_view path)
      -> std::optional<LutLibraryEntry> {
    std::optional<LutLibraryEntry> found;
    service.ReadEntry(path, [&](const LutLibraryEntry& entry) { found = entry; });
    return found;
  }

  fs::path                         base_;
  fs::path                         library_root_;
  std::shared_ptr<PreferenceStore> preferences_;
  std::vector<QUrl>                opened_urls_;
  bool                             open_result_ = true;
};

}  // namespace alcedo::test
