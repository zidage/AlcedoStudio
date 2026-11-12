// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/dcp_file_reader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "dcp_test_file_writer.hpp"
#include "image/dcp_camera_profile.hpp"
#include "image/dng_tone_curve.hpp"

namespace alcedo {
namespace {

using test::DcpTestFileWriter;

namespace tag {
constexpr std::uint16_t kUniqueCameraModel           = 50708;
constexpr std::uint16_t kColorMatrix1                = 50721;
constexpr std::uint16_t kColorMatrix2                = 50722;
constexpr std::uint16_t kReductionMatrix1            = 50725;
constexpr std::uint16_t kCalibrationIlluminant1      = 50778;
constexpr std::uint16_t kCalibrationIlluminant2      = 50779;
constexpr std::uint16_t kProfileCalibrationSignature = 50932;
constexpr std::uint16_t kProfileName                 = 50936;
constexpr std::uint16_t kProfileHueSatMapDims        = 50937;
constexpr std::uint16_t kProfileHueSatMapData1       = 50938;
constexpr std::uint16_t kProfileHueSatMapData2       = 50939;
constexpr std::uint16_t kProfileToneCurve            = 50940;
constexpr std::uint16_t kForwardMatrix1              = 50964;
constexpr std::uint16_t kForwardMatrix2              = 50965;
constexpr std::uint16_t kProfileLookTableDims        = 50981;
constexpr std::uint16_t kProfileLookTableData        = 50982;
constexpr std::uint16_t kProfileLookTableEncoding    = 51108;
constexpr std::uint16_t kBaselineExposureOffset      = 51109;
constexpr std::uint16_t kCalibrationIlluminant3      = 52529;
constexpr std::uint16_t kColorMatrix3                = 52531;
constexpr std::uint16_t kRgbTables                   = 52543;
}  // namespace tag

const std::vector<double> kColorMatrix1{0.6722, -0.0635, -0.0963, -0.4287, 1.246,
                                        0.2028, -0.0908, 0.2162,  0.5668};
const std::vector<double> kColorMatrix2{0.6875, -0.097,  -0.0932, -0.4691, 1.2459,
                                        0.2501, -0.0874, 0.1953,  0.5809};
const std::vector<double> kForwardMatrix1{0.7978, 0.1352, 0.0313, 0.288, 0.7119,
                                          0.0001, 0.0,    0.0,    0.8251};
const std::vector<double> kForwardMatrix2{0.7978, 0.1352, 0.0313,  0.288, 0.7119,
                                          0.0001, 0.0,    -0.0001, 0.8253};

constexpr std::uint32_t   kHue        = 3;
constexpr std::uint32_t   kSaturation = 2;
constexpr std::uint32_t   kValue      = 2;

/// Value written for a rational with denominator 10000, read back the way the reader reads it.
auto                      RationalValue(double value) -> double {
  return static_cast<double>(std::lround(value * 10000.0)) / 10000.0;
}

/// Table in DNG order with value scale 1 on each zero-saturation row.
auto TableEntries(bool omit_zero_saturation, float hue_offset) -> std::vector<float> {
  std::vector<float> entries;
  for (std::uint32_t slice = 0; slice < kHue * kValue; ++slice) {
    for (std::uint32_t saturation = omit_zero_saturation ? 1 : 0; saturation < kSaturation;
         ++saturation) {
      entries.push_back(hue_offset + static_cast<float>(slice) + 0.5f * saturation);
      entries.push_back(1.0f + 0.01f * static_cast<float>(slice));
      entries.push_back(saturation == 0 ? 1.0f : 0.9f);
    }
  }
  return entries;
}

/// Dual-illuminant profile with forward matrices, two hue/saturation maps, and a look table.
auto StandardProfile() -> DcpTestFileWriter {
  DcpTestFileWriter writer;
  writer.Ascii(tag::kUniqueCameraModel, "Alcedo Test Camera")
      .Ascii(tag::kProfileName, "Camera Standard")
      .SRationals(tag::kColorMatrix1, kColorMatrix1)
      .SRationals(tag::kColorMatrix2, kColorMatrix2)
      .Shorts(tag::kCalibrationIlluminant1, {17})
      .Shorts(tag::kCalibrationIlluminant2, {21})
      .SRationals(tag::kForwardMatrix1, kForwardMatrix1)
      .SRationals(tag::kForwardMatrix2, kForwardMatrix2)
      .Ascii(tag::kProfileCalibrationSignature, "com.adobe")
      .Longs(tag::kProfileHueSatMapDims, {kHue, kSaturation, kValue})
      .Floats(tag::kProfileHueSatMapData1, TableEntries(false, 0.0f))
      .Floats(tag::kProfileHueSatMapData2, TableEntries(false, 10.0f))
      .Longs(tag::kProfileLookTableDims, {kHue, kSaturation, kValue})
      .Floats(tag::kProfileLookTableData, TableEntries(false, -5.0f))
      .Longs(tag::kProfileLookTableEncoding, {1})
      .SRationalPairs(tag::kBaselineExposureOffset, {{-1, 4}});
  return writer;
}

class DcpFileReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("alcedo-dcp-reader-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  auto Write(const DcpTestFileWriter& writer, const std::string& name,
             DcpTestFileWriter::ByteOrder  order = DcpTestFileWriter::ByteOrder::kLittleEndian,
             DcpTestFileWriter::EntryOrder entry_order = DcpTestFileWriter::EntryOrder::kAscending)
      -> std::filesystem::path {
    const auto path = dir_ / name;
    writer.Write(path, order, entry_order);
    return path;
  }

  static auto PathText(const std::filesystem::path& path) -> std::string {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
  }

  /// Message of the error that @p read throws; fails the test when it does not throw.
  template <typename Read>
  static auto ErrorMessage(Read read) -> std::string {
    try {
      read();
    } catch (const std::runtime_error& error) {
      return error.what();
    }
    ADD_FAILURE() << "read did not throw";
    return {};
  }

  /// Asserts that the full read fails and that the message names the file and @p detail.
  void ExpectReadFails(const std::filesystem::path& path, const std::string& detail) {
    const auto message = ErrorMessage([&] { (void)ReadDcpCameraProfile(path); });
    EXPECT_NE(message.find(PathText(path)), std::string::npos) << message;
    EXPECT_NE(message.find(detail), std::string::npos) << message;
  }

  std::filesystem::path dir_;
};

TEST_F(DcpFileReaderTest, ReadsMatricesIlluminantsAndLookTableFromLittleEndianFile) {
  const auto profile = ReadDcpCameraProfile(Write(StandardProfile(), "standard.dcp"));

  EXPECT_EQ(profile->unique_camera_model, "Alcedo Test Camera");
  EXPECT_EQ(profile->profile_name, "Camera Standard");
  EXPECT_EQ(profile->profile_calibration_signature, "com.adobe");
  for (std::size_t i = 0; i < 9; ++i) {
    EXPECT_DOUBLE_EQ(profile->color_matrix_1[i], RationalValue(kColorMatrix1[i])) << i;
    EXPECT_DOUBLE_EQ(profile->color_matrix_2[i], RationalValue(kColorMatrix2[i])) << i;
    EXPECT_DOUBLE_EQ(profile->forward_matrix_1[i], RationalValue(kForwardMatrix1[i])) << i;
    EXPECT_DOUBLE_EQ(profile->forward_matrix_2[i], RationalValue(kForwardMatrix2[i])) << i;
  }
  EXPECT_TRUE(profile->forward_matrices_valid);
  EXPECT_DOUBLE_EQ(profile->color_matrix_1_cct, 2856.0);
  EXPECT_DOUBLE_EQ(profile->color_matrix_2_cct, 6504.0);

  ASSERT_NE(profile->tables, nullptr);
  const auto& tables = *profile->tables;
  EXPECT_EQ(tables.name, "Camera Standard");
  EXPECT_EQ(tables.camera_calibration_1, kDngIdentityMatrix);
  EXPECT_EQ(tables.camera_calibration_2, kDngIdentityMatrix);
  EXPECT_EQ(tables.analog_balance, (std::array<double, 3>{1, 1, 1}));
  EXPECT_DOUBLE_EQ(tables.baseline_exposure, 0.0);
  EXPECT_DOUBLE_EQ(tables.baseline_exposure_offset, -0.25);
  EXPECT_EQ(tables.look_table.divisions, (std::array<std::uint32_t, 3>{kHue, kSaturation, kValue}));
  EXPECT_EQ(tables.look_table.encoding, 1u);
  EXPECT_EQ(tables.look_table.entries, TableEntries(false, -5.0f));
  EXPECT_EQ(tables.hue_sat_map_1.entries, TableEntries(false, 0.0f));
  EXPECT_EQ(tables.hue_sat_map_2.entries, TableEntries(false, 10.0f));
  EXPECT_EQ(tables.hue_sat_map_1.encoding, 0u);
  EXPECT_NE(profile->fingerprint, 0u);
}

TEST_F(DcpFileReaderTest, ReadsTheSameProfileFromBigEndianFile) {
  const auto little = ReadDcpCameraProfile(Write(StandardProfile(), "little.dcp"));
  const auto big    = ReadDcpCameraProfile(
      Write(StandardProfile(), "big.dcp", DcpTestFileWriter::ByteOrder::kBigEndian));
  EXPECT_EQ(big->fingerprint, little->fingerprint);
  EXPECT_EQ(big->tables->fingerprint, little->tables->fingerprint);
  EXPECT_EQ(big->color_matrix_1, little->color_matrix_1);
}

TEST_F(DcpFileReaderTest, AcceptsTiffMagicFortyTwo) {
  const auto dcp_magic  = ReadDcpCameraProfile(Write(StandardProfile(), "dcp_magic.dcp"));
  const auto tiff_magic = ReadDcpCameraProfile(Write(StandardProfile().Magic(42), "tiff.dcp"));
  EXPECT_EQ(tiff_magic->fingerprint, dcp_magic->fingerprint);
}

TEST_F(DcpFileReaderTest, SingleColorMatrixFillsBothIlluminantSlots) {
  auto writer = StandardProfile();
  writer.Remove(tag::kColorMatrix2)
      .Remove(tag::kForwardMatrix2)
      .Remove(tag::kProfileHueSatMapData2);
  const auto profile = ReadDcpCameraProfile(Write(writer, "single.dcp"));
  EXPECT_EQ(profile->color_matrix_2, profile->color_matrix_1);
  EXPECT_DOUBLE_EQ(profile->color_matrix_1_cct, 2856.0);
  EXPECT_DOUBLE_EQ(profile->color_matrix_2_cct, profile->color_matrix_1_cct);
  EXPECT_TRUE(profile->forward_matrices_valid);
  EXPECT_EQ(profile->forward_matrix_2, profile->forward_matrix_1);
  EXPECT_TRUE(profile->tables->hue_sat_map_2.entries.empty());
}

TEST_F(DcpFileReaderTest, ProfileWithoutForwardMatricesMarksThemInvalid) {
  auto writer = StandardProfile();
  writer.Remove(tag::kForwardMatrix1).Remove(tag::kForwardMatrix2);
  const auto profile = ReadDcpCameraProfile(Write(writer, "no_forward.dcp"));
  EXPECT_FALSE(profile->forward_matrices_valid);
  EXPECT_EQ(profile->forward_matrix_1, kDngIdentityMatrix);
}

TEST_F(DcpFileReaderTest, LookTableWithoutZeroSaturationRowsGetsNeutralRows) {
  auto writer = StandardProfile();
  writer.Floats(tag::kProfileLookTableData, TableEntries(true, -5.0f));
  const auto  profile = ReadDcpCameraProfile(Write(writer, "omitted_rows.dcp"));
  const auto& entries = profile->tables->look_table.entries;
  ASSERT_EQ(entries.size(), static_cast<std::size_t>(kHue) * kSaturation * kValue * 3);
  const auto written = TableEntries(true, -5.0f);
  for (std::size_t slice = 0; slice < kHue * kValue; ++slice) {
    const std::size_t offset = slice * kSaturation * 3;
    EXPECT_FLOAT_EQ(entries[offset + 0], entries[offset + 3]) << slice;
    EXPECT_FLOAT_EQ(entries[offset + 1], entries[offset + 4]) << slice;
    EXPECT_FLOAT_EQ(entries[offset + 2], 1.0f) << slice;
    // The stored row keeps the written values.
    EXPECT_FLOAT_EQ(entries[offset + 3], written[slice * 3 + 0]) << slice;
    EXPECT_FLOAT_EQ(entries[offset + 5], written[slice * 3 + 2]) << slice;
  }
}

TEST_F(DcpFileReaderTest, IdentityReadDoesNotReadTablePayloads) {
  auto writer = StandardProfile();
  writer.PayloadOffsetOverride(tag::kProfileLookTableData, 0x7FFFFF00u);
  const auto path     = Write(writer, "bad_table_offset.dcp");
  const auto identity = ReadDcpProfileIdentity(path);
  EXPECT_EQ(identity.unique_camera_model, "Alcedo Test Camera");
  EXPECT_EQ(identity.profile_name, "Camera Standard");
  ExpectReadFails(path, "tag 50982 data is outside the file");
}

TEST_F(DcpFileReaderTest, ProfileToneCurveIsReadAndKeepsTheProfileFingerprint) {
  auto with_curve = StandardProfile();
  with_curve.Floats(tag::kProfileToneCurve, {0.0f, 0.0f, 0.25f, 0.4f, 0.5f, 0.6f, 1.0f, 1.0f});
  auto other_curve = StandardProfile();
  other_curve.Floats(tag::kProfileToneCurve, {0.0f, 0.0f, 0.5f, 0.7f, 1.0f, 1.0f});
  const auto plain = ReadDcpCameraProfile(Write(StandardProfile(), "plain.dcp"));
  const auto curve = ReadDcpCameraProfile(Write(with_curve, "curve.dcp"));
  const auto other = ReadDcpCameraProfile(Write(other_curve, "other.dcp"));

  EXPECT_EQ(plain->profile_tone_curve, nullptr);
  ASSERT_NE(curve->profile_tone_curve, nullptr);
  EXPECT_EQ(
      curve->profile_tone_curve->points,
      (std::vector<DngToneCurvePoint>{{0.0f, 0.0f}, {0.25f, 0.4f}, {0.5f, 0.6f}, {1.0f, 1.0f}}));
  EXPECT_FLOAT_EQ(curve->profile_tone_curve->table[2048], 0.6f);
  // The curve stays outside the profile fingerprint, so a stored reference keeps its value.
  EXPECT_EQ(curve->fingerprint, plain->fingerprint);
  EXPECT_EQ(other->fingerprint, plain->fingerprint);
  EXPECT_NE(curve->profile_tone_curve->fingerprint, other->profile_tone_curve->fingerprint);
}

TEST_F(DcpFileReaderTest, ProfileToneCurveReadsTheSameFromBigEndianFile) {
  auto writer = StandardProfile();
  writer.Floats(tag::kProfileToneCurve, {0.0f, 0.0f, 0.18f, 0.43f, 1.0f, 1.0f});
  const auto little = ReadDcpCameraProfile(Write(writer, "little.dcp"));
  const auto big =
      ReadDcpCameraProfile(Write(writer, "big.dcp", DcpTestFileWriter::ByteOrder::kBigEndian));
  ASSERT_NE(big->profile_tone_curve, nullptr);
  EXPECT_EQ(big->profile_tone_curve->points, little->profile_tone_curve->points);
  EXPECT_EQ(big->profile_tone_curve->fingerprint, little->profile_tone_curve->fingerprint);
}

TEST_F(DcpFileReaderTest, ProfileToneCurveWithAnOddValueCountFails) {
  auto writer = StandardProfile();
  writer.Floats(tag::kProfileToneCurve, {0.0f, 0.0f, 0.5f, 0.6f, 1.0f});
  ExpectReadFails(Write(writer, "odd.dcp"), "tag 50940 (ProfileToneCurve) has 5 values");
}

TEST_F(DcpFileReaderTest, ProfileToneCurveWithOnePointFails) {
  auto writer = StandardProfile();
  writer.Floats(tag::kProfileToneCurve, {0.0f, 0.0f});
  ExpectReadFails(Write(writer, "one_point.dcp"), "tag 50940 (ProfileToneCurve): ");
}

TEST_F(DcpFileReaderTest, ProfileToneCurveWithXThatDoesNotRiseFails) {
  auto writer = StandardProfile();
  writer.Floats(tag::kProfileToneCurve, {0.0f, 0.0f, 0.5f, 0.6f, 0.5f, 0.7f, 1.0f, 1.0f});
  ExpectReadFails(Write(writer, "falling_x.dcp"),
                  "tag 50940 (ProfileToneCurve): ProfileToneCurve point 2: x does not rise");
}

TEST_F(DcpFileReaderTest, ProfileToneCurveWithValueOutsideTheUnitRangeFails) {
  auto above = StandardProfile();
  above.Floats(tag::kProfileToneCurve, {0.0f, 0.0f, 0.5f, 1.2f, 1.0f, 1.0f});
  ExpectReadFails(Write(above, "above.dcp"), "point 1: y is outside [0, 1]");
  auto below = StandardProfile();
  below.Floats(tag::kProfileToneCurve, {-0.1f, 0.0f, 1.0f, 1.0f});
  ExpectReadFails(Write(below, "below.dcp"), "point 0: x is outside [0, 1]");
}

TEST_F(DcpFileReaderTest, ProfileToneCurveWithNonFiniteValueFails) {
  auto writer = StandardProfile();
  writer.Floats(tag::kProfileToneCurve, {0.0f, 0.0f, 0.5f, std::nanf(""), 1.0f, 1.0f});
  ExpectReadFails(Write(writer, "nan.dcp"), "point 1: a value is not finite");
}

TEST_F(DcpFileReaderTest, ProfileToneCurveWithWrongTypeFails) {
  auto writer = StandardProfile();
  writer.Longs(tag::kProfileToneCurve, {0, 0, 1, 1});
  ExpectReadFails(Write(writer, "long_curve.dcp"), "tag 50940 has TIFF type 4, expected FLOAT");
}

TEST_F(DcpFileReaderTest, MissingProfileNameFails) {
  auto writer = StandardProfile();
  writer.Remove(tag::kProfileName);
  const auto path = Write(writer, "no_name.dcp");
  ExpectReadFails(path, "tag 50936");
  const auto message = ErrorMessage([&] { (void)ReadDcpProfileIdentity(path); });
  EXPECT_NE(message.find("tag 50936 (ProfileName) is missing"), std::string::npos) << message;
}

TEST_F(DcpFileReaderTest, MissingColorMatrix1Fails) {
  auto writer = StandardProfile();
  writer.Remove(tag::kColorMatrix1);
  ExpectReadFails(Write(writer, "no_matrix.dcp"), "tag 50721");
}

TEST_F(DcpFileReaderTest, ThirdIlluminantFails) {
  auto writer = StandardProfile();
  writer.Shorts(tag::kCalibrationIlluminant3, {23}).SRationals(tag::kColorMatrix3, kColorMatrix1);
  ExpectReadFails(Write(writer, "third.dcp"), "tag 52529 (CalibrationIlluminant3)");
  auto matrix_only = StandardProfile();
  matrix_only.SRationals(tag::kColorMatrix3, kColorMatrix1);
  ExpectReadFails(Write(matrix_only, "third_matrix.dcp"), "tag 52531 (ColorMatrix3)");
}

TEST_F(DcpFileReaderTest, RgbTablesFail) {
  auto writer = StandardProfile();
  writer.Floats(tag::kRgbTables, {0.0f, 1.0f});
  ExpectReadFails(Write(writer, "rgb_tables.dcp"), "tag 52543 (RGBTables)");
}

TEST_F(DcpFileReaderTest, ReductionMatrixFails) {
  auto writer = StandardProfile();
  writer.SRationals(tag::kReductionMatrix1, kColorMatrix1);
  ExpectReadFails(Write(writer, "reduction.dcp"), "tag 50725 (ReductionMatrix1)");
}

TEST_F(DcpFileReaderTest, FourChannelColorMatrixFails) {
  auto                writer       = StandardProfile();
  std::vector<double> four_channel = kColorMatrix1;
  four_channel.insert(four_channel.end(), {0.1, 0.2, 0.3});
  writer.SRationals(tag::kColorMatrix1, four_channel);
  ExpectReadFails(Write(writer, "four_channel.dcp"), "tag 50721 has 12 values, expected 9");
}

TEST_F(DcpFileReaderTest, ZeroDenominatorFails) {
  auto writer = StandardProfile();
  writer.SRationalPairs(tag::kBaselineExposureOffset, {{1, 0}});
  ExpectReadFails(Write(writer, "zero_denominator.dcp"), "tag 51109 has a zero denominator");
}

TEST_F(DcpFileReaderTest, IlluminantWithoutTemperatureFails) {
  auto writer = StandardProfile();
  writer.Shorts(tag::kCalibrationIlluminant2, {255});
  ExpectReadFails(Write(writer, "other_illuminant.dcp"), "tag 50779: illuminant 255");
}

TEST_F(DcpFileReaderTest, TagWithAnotherTypeFails) {
  auto writer = StandardProfile();
  writer.Floats(tag::kColorMatrix1, {1, 0, 0, 0, 1, 0, 0, 0, 1});
  ExpectReadFails(Write(writer, "float_matrix.dcp"), "tag 50721 has TIFF type 11");
}

TEST_F(DcpFileReaderTest, LookTableWithWrongEntryCountFails) {
  auto writer = StandardProfile();
  writer.Floats(tag::kProfileLookTableData, {0.0f, 1.0f, 1.0f});
  ExpectReadFails(Write(writer, "short_table.dcp"), "tag 50982: DNG profile");
}

TEST_F(DcpFileReaderTest, TruncatedIfdFails) {
  auto bytes = StandardProfile().Bytes();
  bytes.resize(8 + 2 + 5 * 12);  // Header, entry count, and only 5 of the entries.
  const auto path = dir_ / "truncated.dcp";
  DcpTestFileWriter::WriteBytes(path, bytes);
  ExpectReadFails(path, "the IFD is truncated");
}

TEST_F(DcpFileReaderTest, WrongMagicFails) {
  const auto path = Write(StandardProfile().Magic(0x2B), "bigtiff.dcp");
  ExpectReadFails(path, "the magic number 43");
  const auto message = ErrorMessage([&] { (void)ReadDcpProfileIdentity(path); });
  EXPECT_NE(message.find("the magic number 43"), std::string::npos) << message;
}

TEST_F(DcpFileReaderTest, FileAboveSizeLimitFailsBeforeRead) {
  const auto path = Write(StandardProfile(), "large.dcp");
  std::filesystem::resize_file(path, kMaxDcpFileBytes + 1);
  ExpectReadFails(path, "above the limit");
}

TEST_F(DcpFileReaderTest, MissingFileFailsWithThePath) {
  ExpectReadFails(dir_ / "missing.dcp", "cannot read the file size");
}

TEST_F(DcpFileReaderTest, EqualContentGivesEqualFingerprint) {
  const auto ascending  = ReadDcpCameraProfile(Write(StandardProfile(), "ascending.dcp"));
  const auto descending = ReadDcpCameraProfile(Write(StandardProfile(), "descending.dcp",
                                                     DcpTestFileWriter::ByteOrder::kLittleEndian,
                                                     DcpTestFileWriter::EntryOrder::kDescending));
  EXPECT_EQ(descending->fingerprint, ascending->fingerprint);

  auto other_name = StandardProfile();
  other_name.Ascii(tag::kProfileName, "Camera Vivid");
  EXPECT_NE(ReadDcpCameraProfile(Write(other_name, "vivid.dcp"))->fingerprint,
            ascending->fingerprint);
  auto other_table = StandardProfile();
  other_table.Floats(tag::kProfileLookTableData, TableEntries(false, -4.0f));
  EXPECT_NE(ReadDcpCameraProfile(Write(other_table, "other_table.dcp"))->fingerprint,
            ascending->fingerprint);
}

/// Reads every `.dcp` file below the folder in `ALCEDO_DCP_FOLDER` (a user DCP library, never
/// part of the repository). Each file must give its identity and a complete profile, and both
/// reads must agree on the identity.
TEST(DcpFileReaderFolderTest, EveryFileOfTheUserFolderLoadsWithoutRejection) {
  const char* folder = std::getenv("ALCEDO_DCP_FOLDER");
  if (folder == nullptr || *folder == '\0') GTEST_SKIP() << "ALCEDO_DCP_FOLDER is not set";
  std::size_t                         files       = 0;
  std::size_t                         tone_curves = 0;
  std::vector<std::string>            rejected;
  std::set<std::string>               cameras;
  std::chrono::steady_clock::duration identity_time{};
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".dcp") continue;
    ++files;
    try {
      const auto start    = std::chrono::steady_clock::now();
      const auto identity = ReadDcpProfileIdentity(entry.path());
      identity_time += std::chrono::steady_clock::now() - start;
      const auto profile = ReadDcpCameraProfile(entry.path());
      EXPECT_EQ(profile->unique_camera_model, identity.unique_camera_model) << entry.path();
      EXPECT_EQ(profile->profile_name, identity.profile_name) << entry.path();
      cameras.insert(identity.unique_camera_model);
      if (profile->profile_tone_curve) ++tone_curves;
    } catch (const std::runtime_error& error) {
      rejected.emplace_back(error.what());
    }
  }
  std::cout << "DCP files: " << files << ", cameras: " << cameras.size()
            << ", tone curves: " << tone_curves << ", rejected: " << rejected.size()
            << ", identity read time: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(identity_time).count()
            << " ms\n";
  for (const auto& message : rejected) std::cout << "  rejected: " << message << '\n';
  EXPECT_GT(files, 0u);
  EXPECT_TRUE(rejected.empty());
}

}  // namespace
}  // namespace alcedo
