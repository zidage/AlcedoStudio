//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/camera_profile_library_service.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <system_error>

#include "image/dcp_test_file_writer.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;
using test::MinimalDcpProfile;

class CameraProfileLibraryServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device device;
    temp_ = fs::temp_directory_path() /
            ("alcedo_camera_profile_service_test_" + std::to_string(device()) + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    folder_ = temp_ / "Camera";
    fs::create_directories(folder_);
    settings_file_ = QString::fromStdWString((temp_ / "settings.ini").wstring());
  }
  void TearDown() override {
    std::error_code error;
    fs::remove_all(temp_, error);
  }

  void Write(const fs::path& relative, const std::string& camera, const std::string& name) {
    const auto path = folder_ / relative;
    fs::create_directories(path.parent_path());
    MinimalDcpProfile(camera, name).Write(path);
  }

  auto MakeService(DcpProfileLibrary& library) -> std::unique_ptr<CameraProfileLibraryService> {
    CameraProfileLibraryServiceOptions options;
    options.library       = &library;
    options.settings_file = settings_file_;
    return std::make_unique<CameraProfileLibraryService>(std::move(options));
  }

  auto FolderUrl(const fs::path& folder) const -> QUrl {
    return QUrl::fromLocalFile(QString::fromStdWString(folder.wstring()));
  }

  static auto NativePath(const fs::path& folder) -> QString {
    return QDir::toNativeSeparators(QString::fromStdWString(folder.generic_wstring()));
  }

  fs::path temp_;
  fs::path folder_;
  QString  settings_file_;
};

TEST_F(CameraProfileLibraryServiceTest, RootFolderPersistsInSettingsAndStartsOneScan) {
  Write("Canon EOS R6/s.dcp", "Canon EOS R6", "Camera Standard");
  Write("Canon EOS R6/n.dcp", "Canon EOS R6", "Camera Neutral");
  Write("Sony ILCE-7M4/s.dcp", "Sony ILCE-7M4", "Camera Standard");

  DcpProfileLibrary library;
  {
    auto       service = MakeService(library);
    QSignalSpy finished(service.get(), &CameraProfileLibraryService::ScanFinished);
    service->Start();
    // No folder is set: the start reads the setting and starts no scan.
    QTest::qWait(50);
    EXPECT_EQ(finished.count(), 0);
    EXPECT_FALSE(service->scanning());
    EXPECT_TRUE(service->root_path().isEmpty());
    EXPECT_EQ(service->profile_count(), 0);

    service->setRootFolder(FolderUrl(folder_));
    EXPECT_TRUE(service->scanning());
    ASSERT_TRUE(finished.wait(10000));
    QTest::qWait(50);
    EXPECT_EQ(finished.count(), 1);
    EXPECT_FALSE(service->scanning());
    EXPECT_EQ(service->root_path(), NativePath(folder_));
    EXPECT_EQ(service->profile_count(), 3);
    EXPECT_EQ(service->camera_count(), 2);
    EXPECT_EQ(service->rejected_count(), 0);
    EXPECT_TRUE(service->error_text().isEmpty());
    EXPECT_EQ(library.Match("Canon", "EOS R6").size(), 2u);
  }

  const QSettings settings(settings_file_, QSettings::IniFormat);
  EXPECT_EQ(
      settings.value(QString::fromLatin1(CameraProfileLibraryService::kRootSettingsKey)).toString(),
      NativePath(folder_));

  // A new service reads the stored folder and scans it once.
  DcpProfileLibrary second_library;
  auto              restarted = MakeService(second_library);
  QSignalSpy        finished(restarted.get(), &CameraProfileLibraryService::ScanFinished);
  restarted->Start();
  EXPECT_EQ(restarted->root_path(), NativePath(folder_));
  ASSERT_TRUE(finished.wait(10000));
  QTest::qWait(50);
  EXPECT_EQ(finished.count(), 1);
  EXPECT_EQ(restarted->profile_count(), 3);
  EXPECT_EQ(restarted->camera_count(), 2);
}

TEST_F(CameraProfileLibraryServiceTest, RescanRefreshesCountsAfterFileRemoval) {
  Write("A/a1.dcp", "Alcedo A", "Camera Standard");
  Write("A/a2.dcp", "Alcedo A", "Camera Vivid");
  Write("B/b1.dcp", "Alcedo B", "Camera Standard");

  DcpProfileLibrary library;
  auto              service = MakeService(library);
  QSignalSpy        finished(service.get(), &CameraProfileLibraryService::ScanFinished);
  service->setRootFolder(FolderUrl(folder_));
  ASSERT_TRUE(finished.wait(10000));
  ASSERT_EQ(service->profile_count(), 3);
  ASSERT_EQ(service->camera_count(), 2);

  fs::remove(folder_ / "B" / "b1.dcp");
  QSignalSpy counts(service.get(), &CameraProfileLibraryService::ScanResultChanged);
  service->rescan();
  EXPECT_TRUE(service->scanning());
  ASSERT_TRUE(finished.wait(10000));
  EXPECT_EQ(counts.count(), 1);
  EXPECT_EQ(service->profile_count(), 2);
  EXPECT_EQ(service->camera_count(), 1);
  EXPECT_EQ(library.Find("Alcedo B", "Camera Standard"), nullptr);
}

TEST_F(CameraProfileLibraryServiceTest, MissingFolderShowsErrorAndKeepsStoredPath) {
  Write("A/a1.dcp", "Alcedo A", "Camera Standard");
  DcpProfileLibrary library;
  auto              service = MakeService(library);
  QSignalSpy        finished(service.get(), &CameraProfileLibraryService::ScanFinished);
  service->setRootFolder(FolderUrl(folder_));
  ASSERT_TRUE(finished.wait(10000));
  ASSERT_EQ(service->profile_count(), 1);

  const auto missing = temp_ / "Removed Drive";
  service->setRootFolder(FolderUrl(missing));
  ASSERT_TRUE(finished.wait(10000));
  EXPECT_FALSE(service->error_text().isEmpty());
  EXPECT_EQ(service->profile_count(), 0);
  EXPECT_EQ(service->camera_count(), 0);
  EXPECT_EQ(service->root_path(), NativePath(missing));

  const QSettings settings(settings_file_, QSettings::IniFormat);
  EXPECT_EQ(
      settings.value(QString::fromLatin1(CameraProfileLibraryService::kRootSettingsKey)).toString(),
      NativePath(missing));

  // The folder returns: a rescan of the stored path reads it.
  fs::create_directories(missing);
  MinimalDcpProfile("Alcedo C", "Camera Standard").Write(missing / "c.dcp");
  service->rescan();
  ASSERT_TRUE(finished.wait(10000));
  EXPECT_TRUE(service->error_text().isEmpty());
  EXPECT_EQ(service->profile_count(), 1);
}

TEST_F(CameraProfileLibraryServiceTest, EmptyFolderClearsSettingAndCounts) {
  Write("A/a1.dcp", "Alcedo A", "Camera Standard");
  DcpProfileLibrary library;
  auto              service = MakeService(library);
  QSignalSpy        finished(service.get(), &CameraProfileLibraryService::ScanFinished);
  service->setRootFolder(FolderUrl(folder_));
  ASSERT_TRUE(finished.wait(10000));
  ASSERT_EQ(service->profile_count(), 1);

  service->setRootFolder(QUrl());
  ASSERT_TRUE(finished.wait(10000));
  EXPECT_TRUE(service->root_path().isEmpty());
  EXPECT_EQ(service->profile_count(), 0);
  EXPECT_TRUE(service->error_text().isEmpty());
  // Rescan does nothing without a folder.
  service->rescan();
  EXPECT_FALSE(service->scanning());
}

}  // namespace
}  // namespace alcedo
