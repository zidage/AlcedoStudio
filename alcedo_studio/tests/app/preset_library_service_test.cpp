//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Preset folder owner: scan, package cache, and file operations (edit_presets_plan.md, PS1).

#include "app/preset_library_service.hpp"

#include <gtest/gtest.h>

#include <QTest>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/document_transfer.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "json.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/test_temp_directory.hpp"

namespace alcedo {
namespace {

namespace fs  = std::filesystem;
using Service = PresetLibraryService;

/// Preferences in memory. @p saved_root is shared with the test, so it can read the stored value.
class MemoryPresetPreferences final : public PresetLibraryPreferences {
 public:
  explicit MemoryPresetPreferences(std::shared_ptr<std::optional<fs::path>> saved_root)
      : saved_root_(std::move(saved_root)) {}
  [[nodiscard]] auto LoadRoot() const -> std::optional<fs::path> override { return *saved_root_; }
  [[nodiscard]] auto SaveRoot(const fs::path& root) -> bool override {
    *saved_root_ = root;
    return true;
  }

 private:
  std::shared_ptr<std::optional<fs::path>> saved_root_;
};

void WriteBytes(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

auto ReadBytes(const fs::path& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

auto PresetText(double exposure_ev) -> std::string {
  return ExportDocumentTransfer(test::MakeExposureTransferPackage(exposure_ev)).dump();
}

auto EntryIds(const Service& service) -> std::vector<std::string> {
  std::vector<std::string> ids;
  for (const auto& entry : service.Entries()) {
    ids.push_back(entry.id);
  }
  return ids;
}

auto WaitUntilIdle(Service& service) -> bool {
  return QTest::qWaitFor([&service] { return !service.busy(); }, 20000);
}

class PresetLibraryServiceTest : public ::testing::Test {
 protected:
  PresetLibraryServiceTest() : saved_root_(std::make_shared<std::optional<fs::path>>()) {
    fs::create_directories(root_);
    *saved_root_ = root_;
  }

  auto MakeService(PresetLibraryServiceOptions options = {}) -> std::unique_ptr<Service> {
    options.preferences = std::make_unique<MemoryPresetPreferences>(saved_root_);
    if (!options.move_to_trash) {
      options.move_to_trash = [this](const fs::path& path, std::string*) {
        trashed_.push_back(path);
        std::error_code error;
        return fs::remove(path, error);
      };
    }
    return std::make_unique<Service>(std::move(options));
  }

  auto StartService(PresetLibraryServiceOptions options = {}) -> std::unique_ptr<Service> {
    auto service = MakeService(std::move(options));
    service->Start();
    EXPECT_TRUE(WaitUntilIdle(*service));
    return service;
  }

  test::TestTempDirectory                  directory_;
  fs::path                                 root_ = directory_ / "presets";
  std::shared_ptr<std::optional<fs::path>> saved_root_;
  std::vector<fs::path>                    trashed_;
};

TEST_F(PresetLibraryServiceTest, ScanListsGroupsAndMarksBadFilesNotAvailable) {
  WriteBytes(root_ / "Warm.alcpreset", PresetText(0.5));
  WriteBytes(root_ / "Film" / "Kodak.ALCPRESET", PresetText(1.0));
  WriteBytes(root_ / "Film" / "Deep" / "Hidden.alcpreset", PresetText(1.5));
  WriteBytes(root_ / "Broken.alcpreset", "{\"schema\":1}");
  WriteBytes(root_ / "notes.txt", "not a preset");

  const auto service = StartService();

  EXPECT_EQ(EntryIds(*service), (std::vector<std::string>{
                                    "Broken.alcpreset", "Film/Kodak.ALCPRESET", "Warm.alcpreset"}));
  EXPECT_TRUE(service->InventoryError().empty());
  const PresetEntry* kodak = service->FindEntry("Film/Kodak.ALCPRESET");
  ASSERT_NE(kodak, nullptr);
  EXPECT_EQ(kodak->group, "Film");
  EXPECT_EQ(kodak->name, "Kodak");
  EXPECT_TRUE(kodak->available);
  EXPECT_EQ(kodak->fingerprint, test::MakeExposureTransferPackage(1.0).fingerprint_);
  const PresetEntry* broken = service->FindEntry("Broken.alcpreset");
  ASSERT_NE(broken, nullptr);
  EXPECT_FALSE(broken->available);
  EXPECT_EQ(broken->rule, PresetFileRule::kMissingFingerprint);
  EXPECT_FALSE(broken->reason.empty());
  EXPECT_FALSE(service->PackageFor("Broken.alcpreset").Ok());
}

TEST_F(PresetLibraryServiceTest, PackageForReadsFileOnceUntilFileChanges) {
  WriteBytes(root_ / "Warm.alcpreset", PresetText(0.5));
  const auto service = StartService();

  const auto first   = service->PackageFor("Warm.alcpreset");
  const auto second  = service->PackageFor("Warm.alcpreset");
  ASSERT_TRUE(first.Ok()) << first.message;
  ASSERT_TRUE(second.Ok());
  EXPECT_EQ(first.package.get(), second.package.get());
  EXPECT_EQ(service->PackageFileReadCount(), 1u);

  // Another person replaces the file with the same size; the write time differs.
  const fs::path path = root_ / "Warm.alcpreset";
  WriteBytes(path, PresetText(0.7));
  fs::last_write_time(path, fs::last_write_time(path) + std::chrono::hours(1));
  const auto third = service->PackageFor("Warm.alcpreset");
  ASSERT_TRUE(third.Ok()) << third.message;
  EXPECT_EQ(service->PackageFileReadCount(), 2u);
  EXPECT_EQ(third.package->fingerprint_, test::MakeExposureTransferPackage(0.7).fingerprint_);
}

TEST_F(PresetLibraryServiceTest, PackageForRefusesIdThatTheInventoryDoesNotList) {
  WriteBytes(directory_ / "Outside.alcpreset", PresetText(0.5));
  const auto service = StartService();

  EXPECT_FALSE(service->PackageFor("../Outside.alcpreset").Ok());
  EXPECT_EQ(service->PackageFileReadCount(), 0u);
}

TEST_F(PresetLibraryServiceTest, PackageCacheKeepsMostRecentlyUsedPackages) {
  for (std::size_t index = 0; index <= kPresetPackageCacheCapacity; ++index) {
    WriteBytes(root_ / ("P" + std::to_string(index) + ".alcpreset"),
               PresetText(0.01 * static_cast<double>(index)));
  }
  const auto service = StartService();
  for (std::size_t index = 0; index <= kPresetPackageCacheCapacity; ++index) {
    ASSERT_TRUE(service->PackageFor("P" + std::to_string(index) + ".alcpreset").Ok());
  }
  const std::size_t reads = service->PackageFileReadCount();

  // The newest package is cached; the oldest left the cache when the limit was passed.
  ASSERT_TRUE(
      service->PackageFor("P" + std::to_string(kPresetPackageCacheCapacity) + ".alcpreset").Ok());
  EXPECT_EQ(service->PackageFileReadCount(), reads);
  ASSERT_TRUE(service->PackageFor("P0.alcpreset").Ok());
  EXPECT_EQ(service->PackageFileReadCount(), reads + 1);
}

TEST_F(PresetLibraryServiceTest, AddCopiesValidFileAndRefusesBadFile) {
  const fs::path good = directory_ / "incoming" / "Portrait.alcpreset";
  const fs::path bad  = directory_ / "incoming" / "Attack.alcpreset";
  WriteBytes(good, PresetText(0.3));
  auto attack = ExportDocumentTransfer(test::MakeLutTransferPackage(R"(\\host\share\x.cube)"));
  WriteBytes(bad, attack.dump());
  const auto service = StartService();

  service->Add({good, bad});
  ASSERT_TRUE(WaitUntilIdle(*service));

  EXPECT_EQ(service->LastResult().status, Service::Status::kRefused);
  EXPECT_NE(service->LastResult().message.find("Attack"), std::string::npos)
      << service->LastResult().message;
  EXPECT_EQ(service->LastResult().affected_ids, (std::vector<std::string>{"Portrait.alcpreset"}));
  EXPECT_EQ(EntryIds(*service), (std::vector<std::string>{"Portrait.alcpreset"}));
  EXPECT_EQ(ReadBytes(root_ / "Portrait.alcpreset"), ReadBytes(good));
  EXPECT_FALSE(fs::exists(root_ / "Attack.alcpreset"));
}

TEST_F(PresetLibraryServiceTest, SaveNeverOverwrites) {
  const auto service = StartService();
  ASSERT_EQ(service->Save(test::MakeExposureTransferPackage(0.5), "Warm"), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  const std::string first = ReadBytes(root_ / "Warm.alcpreset");

  ASSERT_EQ(service->Save(test::MakeExposureTransferPackage(0.9), "Warm"), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));

  EXPECT_EQ(service->LastResult().status, Service::Status::kOk);
  EXPECT_EQ(service->LastResult().affected_ids, (std::vector<std::string>{"Warm 2.alcpreset"}));
  EXPECT_EQ(EntryIds(*service), (std::vector<std::string>{"Warm 2.alcpreset", "Warm.alcpreset"}));
  EXPECT_EQ(ReadBytes(root_ / "Warm.alcpreset"), first);
  EXPECT_EQ(service->FindEntry("Warm 2.alcpreset")->fingerprint,
            test::MakeExposureTransferPackage(0.9).fingerprint_);
}

TEST_F(PresetLibraryServiceTest, SaveRefusesInvalidName) {
  const auto service = StartService();

  EXPECT_EQ(service->Save(test::MakeExposureTransferPackage(0.5), "a/b"),
            Service::Status::kInvalidRequest);
  EXPECT_FALSE(service->busy());
  EXPECT_TRUE(service->Entries().empty());
}

TEST_F(PresetLibraryServiceTest, SaveWritesLibraryReferenceForLutLibraryFile) {
  const fs::path              lut_file = directory_ / "luts" / "user" / "look.cube";
  PresetLibraryServiceOptions options;
  options.lut_reference_for_path = [lut_file](const fs::path& path) -> std::optional<LutReference> {
    if (path == lut_file) {
      return LibraryLutReference{"user/look.cube"};
    }
    return std::nullopt;
  };
  const auto service = StartService(std::move(options));

  ASSERT_EQ(service->Save(test::MakeLutTransferPackage(lut_file.generic_string()), "Look"),
            Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));

  ASSERT_EQ(service->LastResult().status, Service::Status::kOk) << service->LastResult().message;
  const auto package = service->PackageFor("Look.alcpreset");
  ASSERT_TRUE(package.Ok()) << package.message;
  bool found = false;
  for (const auto& grade : package.package->color_grades_) {
    for (const auto& adjustment : grade.adjustments) {
      if (adjustment.type == type_ids::Lmt()) {
        found = true;
        EXPECT_EQ(adjustment.params.at("cube_path"), "");
        EXPECT_EQ(adjustment.params.at("reference"),
                  (nlohmann::json{{"kind", "library"}, {"path", "user/look.cube"}}));
      }
    }
  }
  EXPECT_TRUE(found);
  // The preset text holds no part of the absolute path.
  EXPECT_EQ(ReadBytes(root_ / "Look.alcpreset").find(directory_.Path().filename().string()),
            std::string::npos);
}

TEST_F(PresetLibraryServiceTest, SaveRefusesLutOutsideLibrary) {
  PresetLibraryServiceOptions options;
  options.lut_reference_for_path = [](const fs::path&) -> std::optional<LutReference> {
    return std::nullopt;
  };
  const auto service = StartService(std::move(options));

  EXPECT_EQ(service->Save(test::MakeLutTransferPackage("D:/elsewhere/look.cube"), "Look"),
            Service::Status::kRefused);
  EXPECT_NE(service->LastResult().message.find("look.cube"), std::string::npos)
      << service->LastResult().message;
  EXPECT_FALSE(service->busy());
  EXPECT_FALSE(fs::exists(root_ / "Look.alcpreset"));
}

TEST_F(PresetLibraryServiceTest, RenameAndDeleteUpdateInventory) {
  WriteBytes(root_ / "Film" / "Warm.alcpreset", PresetText(0.5));
  WriteBytes(root_ / "Film" / "Taken.alcpreset", PresetText(0.6));
  const auto service = StartService();

  ASSERT_EQ(service->Rename("Film/Warm.alcpreset", "Blue Hour"), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->LastResult().affected_ids,
            (std::vector<std::string>{"Film/Blue Hour.alcpreset"}));
  EXPECT_EQ(EntryIds(*service),
            (std::vector<std::string>{"Film/Blue Hour.alcpreset", "Film/Taken.alcpreset"}));

  // A rename to an existing name replaces no file.
  const std::string taken = ReadBytes(root_ / "Film" / "Taken.alcpreset");
  ASSERT_EQ(service->Rename("Film/Blue Hour.alcpreset", "Taken"), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->LastResult().status, Service::Status::kConflict);
  EXPECT_EQ(ReadBytes(root_ / "Film" / "Taken.alcpreset"), taken);
  EXPECT_TRUE(fs::exists(root_ / "Film" / "Blue Hour.alcpreset"));

  ASSERT_TRUE(service->PackageFor("Film/Taken.alcpreset").Ok());
  ASSERT_EQ(service->Delete("Film/Taken.alcpreset"), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->LastResult().status, Service::Status::kOk);
  EXPECT_EQ(trashed_, (std::vector<fs::path>{root_ / "Film" / "Taken.alcpreset"}));
  EXPECT_EQ(EntryIds(*service), (std::vector<std::string>{"Film/Blue Hour.alcpreset"}));
  EXPECT_FALSE(service->PackageFor("Film/Taken.alcpreset").Ok());
}

TEST_F(PresetLibraryServiceTest, OperationsRunInRequestOrder) {
  const auto     service = StartService();

  // A save queued after a folder change writes into the new folder.
  const fs::path other   = directory_ / "other";
  fs::create_directories(other);
  service->UseRoot(other);
  ASSERT_EQ(service->Save(test::MakeExposureTransferPackage(0.5), "Warm"), Service::Status::kOk);
  EXPECT_TRUE(service->busy());
  ASSERT_TRUE(WaitUntilIdle(*service));

  EXPECT_TRUE(fs::exists(other / "Warm.alcpreset"));
  EXPECT_FALSE(fs::exists(root_ / "Warm.alcpreset"));
  EXPECT_EQ(EntryIds(*service), (std::vector<std::string>{"Warm.alcpreset"}));
}

TEST_F(PresetLibraryServiceTest, UseRootScansNewFolderAndStoresPreference) {
  WriteBytes(root_ / "Old.alcpreset", PresetText(0.2));
  const fs::path other = directory_ / "other";
  WriteBytes(other / "New.alcpreset", PresetText(0.4));
  const auto service = StartService();
  ASSERT_TRUE(service->PackageFor("Old.alcpreset").Ok());

  service->useRoot(QString::fromStdString(other.generic_string()));
  ASSERT_TRUE(WaitUntilIdle(*service));

  EXPECT_EQ(service->LastResult().status, Service::Status::kOk);
  EXPECT_EQ(service->Root(), fs::weakly_canonical(other));
  EXPECT_EQ(*saved_root_, fs::weakly_canonical(other));
  EXPECT_EQ(EntryIds(*service), (std::vector<std::string>{"New.alcpreset"}));
  // The old folder keeps its files; nothing is moved.
  EXPECT_TRUE(fs::exists(root_ / "Old.alcpreset"));
}

TEST_F(PresetLibraryServiceTest, UseRootOfMissingFolderKeepsCurrentFolder) {
  WriteBytes(root_ / "Old.alcpreset", PresetText(0.2));
  const auto     service = StartService();
  const fs::path before  = service->Root();

  service->UseRoot(directory_ / "absent");
  ASSERT_TRUE(WaitUntilIdle(*service));

  EXPECT_EQ(service->LastResult().status, Service::Status::kRootUnavailable);
  EXPECT_EQ(service->Root(), before);
  EXPECT_EQ(*saved_root_, root_);
  EXPECT_EQ(EntryIds(*service), (std::vector<std::string>{"Old.alcpreset"}));
}

TEST_F(PresetLibraryServiceTest, ScanStopsAtFileLimitWithError) {
  // The limit counts files; the content does not matter here.
  const std::string text = "{}";
  for (std::size_t index = 0; index <= kPresetLibraryMaxFiles; ++index) {
    WriteBytes(root_ / ("P" + std::to_string(index) + ".alcpreset"), text);
  }

  const auto inventory = ScanPresetLibraryRoot(root_);

  EXPECT_EQ(inventory.entries.size(), kPresetLibraryMaxFiles);
  EXPECT_NE(inventory.error.find(std::to_string(kPresetLibraryMaxFiles)), std::string::npos);
}

}  // namespace
}  // namespace alcedo
