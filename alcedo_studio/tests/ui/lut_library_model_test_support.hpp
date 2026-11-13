//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// Temporary LUT libraries with classified CUBE files for the LUT browser model and target tests.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTest>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "app/lut_library_inventory.hpp"
#include "app/lut_library_service.hpp"
#include "ui/alcedo_main/album_backend/lut_library_model.hpp"

namespace alcedo::ui::test {

inline constexpr const char* kLutNumericTable =
    "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";

/// One print declared by film simulation metadata.
struct PrintFields {
  std::string id;
  std::string name;
  std::string brand;
  std::string kind;  // "film" or "paper"
};

/// `# ALCEDO_LUT` JSON of a user-declared film simulation.
inline auto FilmMetadata(const std::string& id, const std::string& source_id,
                         const std::string& source_name, const std::string& film_id,
                         const std::string& film_name, const std::string& brand,
                         const std::optional<PrintFields>& print = std::nullopt) -> std::string {
  std::string json = R"({"schema":1,"id":")" + id +
                     R"(","origin":"user","category":"film_simulation","source":{"id":")" +
                     source_id + R"(","name":")" + source_name + R"("},"film":{"id":")" + film_id +
                     R"(","name":")" + film_name + R"(","brand":")" + brand + R"("})";
  if (print) {
    json += R"(,"print":{"id":")" + print->id + R"(","name":")" + print->name +
            R"(","brand":")" + print->brand + R"(","kind":")" + print->kind + R"("})";
  }
  return json + R"(,"input_space":"ACEScc","output_space":"ACEScc"})";
}

/// `# ALCEDO_LUT` JSON of a user-declared general LUT.
inline auto GeneralMetadata(const std::string& id, const std::string& source_id = {},
                            const std::string& aliases_json = {}) -> std::string {
  std::string json = R"({"schema":1,"id":")" + id + R"(","origin":"user","category":"general")";
  if (!source_id.empty()) {
    json += R"(,"source":{"id":")" + source_id + R"(","name":")" + source_id + R"("})";
  }
  if (!aliases_json.empty()) json += R"(,"aliases":)" + aliases_json;
  return json + R"(,"input_space":"ACEScc","output_space":"ACEScc"})";
}

/// A 3D CUBE file with an optional metadata comment.
inline auto CubeWithMetadata(const std::string& metadata_json) -> std::string {
  std::string text;
  if (!metadata_json.empty()) text = "# ALCEDO_LUT " + metadata_json + "\n";
  return text + kLutNumericTable;
}

class FixedLutRootPreferences final : public LutLibraryPreferences {
 public:
  [[nodiscard]] auto LoadRoot() const -> std::optional<std::filesystem::path> override {
    return std::nullopt;
  }
  [[nodiscard]] auto SaveRoot(const std::filesystem::path&) -> bool override { return true; }
  [[nodiscard]] auto LoadLegacyFavoritePaths() const -> QStringList override { return {}; }
  void               SaveLegacyFavoritePaths(const QStringList&) override {}
};

/// A started LutLibraryService over a temporary root under the test working directory.
class TemporaryLutLibrary {
 public:
  /// @p files are root-relative paths and their file contents. @p prepare_root runs after the
  /// files are written and before the service starts (for example, to write package receipts).
  explicit TemporaryLutLibrary(
      const std::vector<std::pair<std::string, std::string>>&  files,
      const LutLibraryInventory*                               persisted_inventory = nullptr,
      const std::function<void(const std::filesystem::path&)>& prepare_root        = {}) {
    std::random_device device;
    root_ = std::filesystem::current_path() / "lut_library_model_test" / std::to_string(device());
    std::filesystem::create_directories(root_);
    root_ = std::filesystem::weakly_canonical(root_);
    for (const auto& [relative, contents] : files) Write(relative, contents);
    if (prepare_root) prepare_root(root_);
    if (persisted_inventory != nullptr) {
      EXPECT_TRUE(WriteLutLibraryInventoryFile(root_, *persisted_inventory).empty());
    }
    LutLibraryServiceOptions options;
    options.preferences       = std::make_unique<FixedLutRootPreferences>();
    options.default_root      = root_;
    options.open_url          = [](const QUrl&) { return true; };
    options.scan_worker_count = 2;
    service_                  = std::make_unique<LutLibraryService>(std::move(options));
    service_->Start();
    EXPECT_TRUE(WaitUntilIdle());
  }
  ~TemporaryLutLibrary() {
    service_.reset();
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }
  TemporaryLutLibrary(const TemporaryLutLibrary&)            = delete;
  TemporaryLutLibrary& operator=(const TemporaryLutLibrary&) = delete;

  [[nodiscard]] auto Service() -> LutLibraryService* { return service_.get(); }
  [[nodiscard]] auto Root() const -> const std::filesystem::path& { return root_; }

  auto               WaitUntilIdle() -> bool {
    QElapsedTimer timer;
    timer.start();
    while (service_->busy() && timer.elapsed() < 20000) QTest::qWait(5);
    return !service_->busy();
  }

  void Write(const std::string& relative, const std::string& contents) const {
    const std::filesystem::path path = root_ / LutPathFromUtf8(relative);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
  }

 private:
  std::filesystem::path              root_;
  std::unique_ptr<LutLibraryService> service_;
};

/// Entry IDs of the model rows in row order.
inline auto RowEntryIds(const LutLibraryModel& model) -> QStringList {
  QStringList ids;
  for (int row = 0; row < model.rowCount(); ++row) ids.push_back(model.entryIdAt(row));
  return ids;
}

/// Entry IDs of the model rows, sorted, for set comparisons.
inline auto SortedRowEntryIds(const LutLibraryModel& model) -> QStringList {
  QStringList ids = RowEntryIds(model);
  ids.sort();
  return ids;
}

}  // namespace alcedo::ui::test
