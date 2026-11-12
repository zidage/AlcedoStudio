// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/dcp_profile_library.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "dcp_test_file_writer.hpp"
#include "image/dcp_file_reader.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;
using test::DcpTestFileWriter;
using test::MinimalDcpProfile;

class DcpProfileLibraryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device device;
    root_ = fs::temp_directory_path() /
            ("alcedo_dcp_library_test_" + std::to_string(device()) + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root_);
  }
  void TearDown() override {
    std::error_code error;
    fs::remove_all(root_, error);
  }

  /// Writes a minimal profile at @p relative below the test folder.
  auto Write(const fs::path& relative, const std::string& camera, const std::string& name,
             double matrix_scale = 1.0) -> fs::path {
    const auto path = root_ / relative;
    fs::create_directories(path.parent_path());
    MinimalDcpProfile(camera, name, matrix_scale).Write(path);
    return path;
  }

  /// Library whose loader counts its calls in reads_.
  auto CountingLibrary(std::size_t capacity = DcpProfileLibrary::kLoadedProfileCapacity)
      -> DcpProfileLibrary {
    return DcpProfileLibrary(
        [this](const fs::path& path) {
          ++reads_;
          return ReadDcpCameraProfile(path);
        },
        capacity);
  }

  fs::path         root_;
  std::atomic<int> reads_{0};
};

TEST_F(DcpProfileLibraryTest, ScanIndexesNestedFoldersAndCountsCameras) {
  Write("Canon EOS R6/Canon EOS R6 Camera Standard.dcp", "Canon EOS R6", "Camera Standard");
  Write("Canon EOS R6/Canon EOS R6 Camera Neutral.dcp", "Canon EOS R6", "Camera Neutral");
  Write("Canon EOS R6/Canon EOS R6 Camera Portrait.DCP", "Canon EOS R6", "Camera Portrait");
  Write("Nikon/Z/Nikon Z 6_2 Camera Standard.dcp", "Nikon Z 6_2", "Camera Standard");
  Write("Nikon/Z/Nikon Z 6_2 Camera Flat.dcp", "Nikon Z 6_2", "Camera Flat");
  Write("Sony ILCE-7M4/a/b/Sony ILCE-7M4 Camera Standard.dcp", "Sony ILCE-7M4", "Camera Standard");
  Write("Sony ILCE-7M4/Sony ILCE-7M4 Camera Vivid.dcp", "Sony ILCE-7M4", "Camera Vivid");
  // Files without the .dcp extension are not profiles.
  Write("Sony ILCE-7M4/readme.txt", "Sony ILCE-7M4", "Camera Text");
  Write("Sony ILCE-7M4/Sony ILCE-7M4 Camera Old.dcp.bak", "Sony ILCE-7M4", "Camera Old");

  DcpProfileLibrary library;
  const auto        result = library.Scan(root_);

  EXPECT_TRUE(result.error.empty()) << result.error;
  EXPECT_EQ(result.root, root_);
  EXPECT_EQ(result.profile_count, 7u);
  EXPECT_EQ(result.camera_count, 3u);
  EXPECT_EQ(result.rejected_count, 0u);
  EXPECT_EQ(library.ProfileCount(), 7u);
  EXPECT_EQ(library.CameraCount(), 3u);
  EXPECT_EQ(library.Root(), root_);
  ASSERT_NE(library.Find("Canon EOS R6", "Camera Portrait"), nullptr);
  ASSERT_NE(library.Find("Sony ILCE-7M4", "Camera Standard"), nullptr);
}

TEST_F(DcpProfileLibraryTest, ScanSkipsAndCountsUnreadableFiles) {
  Write("A/a1.dcp", "Alcedo A", "Camera Standard");
  Write("A/a2.dcp", "Alcedo A", "Camera Vivid");
  Write("B/b1.dcp", "Alcedo B", "Camera Standard");
  // A truncated file: the header and half of the entry table.
  auto bytes = MinimalDcpProfile("Alcedo B", "Camera Vivid").Bytes();
  bytes.resize(16);
  DcpTestFileWriter::WriteBytes(root_ / "B" / "b2.dcp", bytes);

  DcpProfileLibrary library;
  const auto        result = library.Scan(root_);

  EXPECT_TRUE(result.error.empty()) << result.error;
  EXPECT_EQ(result.rejected_count, 1u);
  EXPECT_EQ(result.profile_count, 3u);
  EXPECT_EQ(result.camera_count, 2u);
  EXPECT_NE(library.Find("Alcedo B", "Camera Standard"), nullptr);
  EXPECT_EQ(library.Find("Alcedo B", "Camera Vivid"), nullptr);
}

TEST_F(DcpProfileLibraryTest, DuplicateIdentityKeepsFirstSortedPath) {
  const auto first  = Write("a/Alcedo X Camera Standard.dcp", "Alcedo X", "Camera Standard", 1.0);
  const auto second = Write("b/Alcedo X Camera Standard.dcp", "Alcedo X", "Camera Standard", 0.5);
  // The same identity with another spelling of the camera model is a duplicate too.
  Write("c/alcedo-x.dcp", "ALCEDO X", "Camera Standard", 0.25);

  DcpProfileLibrary library;
  const auto        result = library.Scan(root_);
  EXPECT_EQ(result.profile_count, 1u);
  EXPECT_EQ(result.camera_count, 1u);
  EXPECT_EQ(result.rejected_count, 0u);

  const auto found = library.Find("Alcedo X", "Camera Standard");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->fingerprint, ReadDcpCameraProfile(first)->fingerprint);
  EXPECT_NE(found->fingerprint, ReadDcpCameraProfile(second)->fingerprint);
}

TEST(DcpCameraMatchTest, MatchUsesMakeAliasAndModel) {
  struct Row {
    const char* make;
    const char* model;
    const char* unique_camera_model;
  };
  const Row rows[] = {
      // Exif make and model.
      {"SONY", "ILCE-7M4", "Sony ILCE-7M4"},
      {"NIKON CORPORATION", "NIKON Z 6_2", "Nikon Z 6_2"},
      {"Canon", "Canon EOS R6", "Canon EOS R6"},
      {"Panasonic", "DC-S5", "Panasonic DC-S5"},
      {"OLYMPUS IMAGING CORP.", "E-M1", "Olympus E-M1"},
      {"OLYMPUS CORPORATION", "E-M1MarkIII", "Olympus E-M1 Mark III"},
      {"OM Digital Solutions", "OM-1", "OM Digital Solutions OM-1"},
      {"RICOH IMAGING COMPANY, LTD.", "PENTAX K-1", "Pentax K-1"},
      {"RICOH IMAGING COMPANY, LTD.", "GR III", "Ricoh GR III"},
      {"PENTAX", "PENTAX K-3 Mark III", "Pentax K-3 Mark III"},
      {"LEICA CAMERA AG", "D-LUX 7", "Leica D-Lux 7"},
      {"SIGMA", "fp L", "SIGMA fp L"},
      {"FUJIFILM", "X-T4", "Fujifilm X-T4"},
      {"Phase One", "IQ4 150MP", "Phase One IQ4 150MP"},
      {"KONICA MINOLTA", "DYNAX 7D", "Konica Minolta DYNAX 7D"},
      {"EASTMAN KODAK COMPANY", "DCS Pro 14N", "Kodak DCS Pro 14N"},
      // LibRaw make and model (make already normalized, model without the make).
      {"Nikon", "Z 6_2", "Nikon Z 6_2"},
      {"Sony", "ILCE-7M4", "Sony ILCE-7M4"},
      {"Canon", "EOS R6", "Canon EOS R6"},
      {"Olympus", "OM-1", "OM Digital Solutions OM-1"},
      {"OM Digital Solutions", "E-M10 Mark IV", "Olympus E-M10 Mark IV"},
  };
  for (const auto& row : rows) {
    EXPECT_TRUE(DcpCameraModelMatches(row.make, row.model, row.unique_camera_model))
        << row.make << " / " << row.model << " -> " << row.unique_camera_model;
  }
}

TEST(DcpCameraMatchTest, MatchRejectsNearNames) {
  EXPECT_FALSE(DcpCameraModelMatches("Canon", "Canon EOS R6", "Canon EOS R6 Mark II"));
  EXPECT_FALSE(DcpCameraModelMatches("Canon", "Canon EOS R6", "Canon EOS R"));
  EXPECT_FALSE(DcpCameraModelMatches("Canon", "EOS R6", "Canon EOS R6 Mark II"));
  EXPECT_FALSE(DcpCameraModelMatches("Sony", "ILCE-7M4", "Sony ILCE-7M3"));
  EXPECT_FALSE(DcpCameraModelMatches("Nikon", "Z 6", "Nikon Z 6_2"));
  // Another make with the same model name does not match.
  EXPECT_FALSE(DcpCameraModelMatches("Sony", "GR III", "Ricoh GR III"));
  EXPECT_FALSE(DcpCameraModelMatches("Canon", "", "Canon EOS R6"));
  EXPECT_FALSE(DcpCameraModelMatches("Canon", "EOS R6", ""));
}

TEST_F(DcpProfileLibraryTest, MatchListsTheProfilesOfOneCameraByName) {
  Write("Canon EOS R6/v.dcp", "Canon EOS R6", "Camera Vivid");
  Write("Canon EOS R6/s.dcp", "Canon EOS R6", "Camera Standard");
  Write("Canon EOS R6/n.dcp", "Canon EOS R6", "Camera Neutral");
  Write("Canon EOS R6 Mark II/s.dcp", "Canon EOS R6 Mark II", "Camera Standard");
  Write("Canon EOS R/s.dcp", "Canon EOS R", "Camera Standard");

  DcpProfileLibrary library;
  library.Scan(root_);
  const auto matches = library.Match("Canon", "EOS R6");

  ASSERT_EQ(matches.size(), 3u);
  EXPECT_EQ(matches[0].profile_name, "Camera Neutral");
  EXPECT_EQ(matches[1].profile_name, "Camera Standard");
  EXPECT_EQ(matches[2].profile_name, "Camera Vivid");
  for (const auto& match : matches) EXPECT_EQ(match.unique_camera_model, "Canon EOS R6");
  EXPECT_TRUE(library.Match("Canon", "EOS R5").empty());
}

TEST_F(DcpProfileLibraryTest, FindLoadsOnceAndReloadsAfterFileChange) {
  const auto path    = Write("x.dcp", "Alcedo X", "Camera Standard");
  auto       library = CountingLibrary();
  library.Scan(root_);

  const auto first  = library.Find("Alcedo X", "Camera Standard");
  const auto second = library.Find("alcedo-x", "Camera Standard");
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_EQ(reads_.load(), 1);
  EXPECT_EQ(library.ProfileReadCount(), 1u);

  fs::last_write_time(path, fs::last_write_time(path) + std::chrono::hours(1));
  const auto third = library.Find("Alcedo X", "Camera Standard");
  ASSERT_NE(third, nullptr);
  EXPECT_EQ(reads_.load(), 2);
  EXPECT_EQ(third->fingerprint, first->fingerprint);
  library.Find("Alcedo X", "Camera Standard");
  EXPECT_EQ(reads_.load(), 2);
}

TEST_F(DcpProfileLibraryTest, FindEvictsTheLeastRecentlyUsedProfile) {
  Write("a.dcp", "Alcedo A", "Camera Standard");
  Write("b.dcp", "Alcedo B", "Camera Standard");
  Write("c.dcp", "Alcedo C", "Camera Standard");
  auto library = CountingLibrary(2);
  library.Scan(root_);

  library.Find("Alcedo A", "Camera Standard");
  library.Find("Alcedo B", "Camera Standard");
  library.Find("Alcedo A", "Camera Standard");  // A is now the most recent.
  library.Find("Alcedo C", "Camera Standard");  // Evicts B.
  EXPECT_EQ(reads_.load(), 3);
  library.Find("Alcedo A", "Camera Standard");
  EXPECT_EQ(reads_.load(), 3);
  library.Find("Alcedo B", "Camera Standard");
  EXPECT_EQ(reads_.load(), 4);
}

TEST_F(DcpProfileLibraryTest, FindReturnsNullForUnknownIdentity) {
  Write("x.dcp", "Alcedo X", "Camera Standard");
  auto library = CountingLibrary();
  library.Scan(root_);

  DcpCameraProfilePtr profile;
  EXPECT_NO_THROW(profile = library.Find("Alcedo X", "Camera Vivid"));
  EXPECT_EQ(profile, nullptr);
  EXPECT_NO_THROW(profile = library.Find("Alcedo Y", "Camera Standard"));
  EXPECT_EQ(profile, nullptr);
  // The profile name is compared exactly.
  EXPECT_NO_THROW(profile = library.Find("Alcedo X", "camera standard"));
  EXPECT_EQ(profile, nullptr);
  EXPECT_EQ(reads_.load(), 0);
}

TEST_F(DcpProfileLibraryTest, FindFailsWhenTheFileNowHoldsAnotherIdentity) {
  const auto path    = Write("x.dcp", "Alcedo X", "Camera Standard");
  auto       library = CountingLibrary();
  library.Scan(root_);
  MinimalDcpProfile("Alcedo X", "Camera Vivid").Write(path);

  try {
    library.Find("Alcedo X", "Camera Standard");
    FAIL() << "Find must fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("Camera Vivid"), std::string::npos) << error.what();
  }
  // No cache entry stays; the next Find reads the file again.
  EXPECT_THROW(library.Find("Alcedo X", "Camera Standard"), std::runtime_error);
  EXPECT_EQ(reads_.load(), 2);
}

TEST_F(DcpProfileLibraryTest, MissingFolderReportsErrorAndClearsIndex) {
  Write("x.dcp", "Alcedo X", "Camera Standard");
  DcpProfileLibrary library;
  ASSERT_EQ(library.Scan(root_).profile_count, 1u);

  const auto missing = root_ / "missing";
  const auto result  = library.Scan(missing);

  EXPECT_FALSE(result.error.empty());
  EXPECT_EQ(result.root, missing);
  EXPECT_EQ(result.profile_count, 0u);
  EXPECT_EQ(result.camera_count, 0u);
  EXPECT_EQ(library.ProfileCount(), 0u);
  EXPECT_EQ(library.CameraCount(), 0u);
  EXPECT_TRUE(library.Root().empty());
  EXPECT_TRUE(library.Match("Alcedo", "X").empty());
  EXPECT_EQ(library.Find("Alcedo X", "Camera Standard"), nullptr);
}

TEST_F(DcpProfileLibraryTest, EmptyFolderPathClearsIndexWithoutError) {
  Write("x.dcp", "Alcedo X", "Camera Standard");
  DcpProfileLibrary library;
  ASSERT_EQ(library.Scan(root_).profile_count, 1u);

  const auto result = library.Scan({});
  EXPECT_TRUE(result.error.empty());
  EXPECT_EQ(result.profile_count, 0u);
  EXPECT_EQ(library.ProfileCount(), 0u);
}

/// Scans the folder in `ALCEDO_DCP_FOLDER` (a user DCP library, never part of the repository)
/// and prints the counts and the scan time. The scan time target (below 2 s) is measured with a
/// Release build of this test.
TEST(DcpProfileLibraryFolderTest, UserFolderScanCountsProfilesAndCamerasWithoutRejection) {
  const char* folder = std::getenv("ALCEDO_DCP_FOLDER");
  if (folder == nullptr || *folder == '\0') GTEST_SKIP() << "ALCEDO_DCP_FOLDER is not set";
  DcpProfileLibrary library;
  const auto        result = library.Scan(folder);
  std::cout << "DCP library scan: " << result.profile_count << " profiles, " << result.camera_count
            << " cameras, " << result.rejected_count << " rejected, " << result.elapsed.count()
            << " ms\n";
  EXPECT_TRUE(result.error.empty()) << result.error;
  EXPECT_GT(result.profile_count, 0u);
  EXPECT_EQ(result.rejected_count, 0u);
}

}  // namespace
}  // namespace alcedo
