//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Preset file reader and writer: the safety rules of edit_presets_plan.md section 3 (PS1).

#include "app/preset_file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "app/adjustment_transfer_package_builder.hpp"
#include "app/document_transfer.hpp"
#include "app/document_transfer_planner.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/test_temp_directory.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

auto CanonicalText(const AdjustmentTransferPackage& package) -> std::string {
  return ExportDocumentTransfer(package).dump();
}

auto CanonicalJson(const AdjustmentTransferPackage& package) -> nlohmann::json {
  return ExportDocumentTransfer(package);
}

/// @p json with the fingerprint of its content, as a canonical writer would make it.
auto WithMatchingFingerprint(nlohmann::json json) -> std::string {
  json.erase("fingerprint");
  const std::string body = json.dump();
  json["fingerprint"]    = Hash128::Compute(body.data(), body.size()).ToString();
  return json.dump();
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

auto FirstLmtParams(nlohmann::json& json) -> nlohmann::json& {
  const std::string lmt_type{type_ids::Lmt().Text()};
  for (auto& grade : json.at("color_grades")) {
    for (auto& adjustment : grade.at("adjustments")) {
      if (adjustment.at("type") == lmt_type) {
        return adjustment.at("params");
      }
    }
  }
  throw std::runtime_error("the package has no LMT adjustment");
}

/// Canonical text of the LUT package of the test support with @p params as the LMT parameters.
auto TextWithLmtParams(const nlohmann::json& params) -> std::string {
  auto json            = CanonicalJson(test::MakeLutTransferPackage("C:/luts/look.cube"));
  FirstLmtParams(json) = params;
  return WithMatchingFingerprint(std::move(json));
}

TEST(PresetFileTest, WrittenPresetReadsBackWithEqualFingerprint) {
  test::TestTempDirectory directory;
  const auto              package = test::MakeExposureTransferPackage(1.25);
  const fs::path          path    = directory / "Warm.alcpreset";

  const auto              written = WritePresetFile(package, path);
  ASSERT_TRUE(written.Ok()) << written.message;

  const auto read = ReadPresetFile(path);
  ASSERT_TRUE(read.Ok()) << read.message;
  ASSERT_TRUE(read.package.has_value());
  EXPECT_EQ(read.package->fingerprint_, package.fingerprint_);
  EXPECT_EQ(read.canonical_text, CanonicalText(package));
  // No temporary file stays in the folder.
  EXPECT_EQ(std::distance(fs::directory_iterator(directory.Path()), fs::directory_iterator{}), 1);
}

TEST(PresetFileTest, WriteRefusesToReplaceAnExistingFile) {
  test::TestTempDirectory directory;
  const fs::path          path = directory / "Warm.alcpreset";
  WriteBytes(path, "keep");

  const auto written = WritePresetFile(test::MakeExposureTransferPackage(1.0), path);

  EXPECT_EQ(written.rule, PresetFileRule::kTargetExists);
  std::ifstream     stream(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(stream)), {});
  EXPECT_EQ(bytes, "keep");
}

TEST(PresetFileTest, PresetAboveSizeLimitIsRefusedBeforeParse) {
  test::TestTempDirectory directory;
  const fs::path          path = directory / "Large.alcpreset";
  // Text that the parser would refuse as invalid JSON, so the rule shows that it did not run.
  WriteBytes(path, std::string(static_cast<std::size_t>(kPresetFileMaxBytes) + 1, '{'));

  const auto read = ReadPresetFile(path);

  EXPECT_EQ(read.rule, PresetFileRule::kFileTooLarge) << read.message;
  EXPECT_FALSE(read.package.has_value());
}

TEST(PresetFileTest, PresetDeeperThanDepthLimitIsRefused) {
  const std::string too_deep = std::string(17, '[') + std::string(17, ']');
  EXPECT_EQ(ParsePresetText(too_deep).rule, PresetFileRule::kTooDeep);

  // Sixteen levels pass the depth rule and fail as a package instead.
  const std::string at_limit = std::string(16, '[') + std::string(16, ']');
  EXPECT_EQ(ParsePresetText(at_limit).rule, PresetFileRule::kInvalidPackage);

  // A depth far above the limit stops at the first value deeper than the limit.
  const std::string very_deep = std::string(100000, '[') + std::string(100000, ']');
  EXPECT_EQ(ParsePresetText(very_deep).rule, PresetFileRule::kTooDeep);
}

TEST(PresetFileTest, InvalidUtf8TextIsRefused) {
  std::string text = CanonicalText(test::MakeExposureTransferPackage(1.0));
  text.insert(text.find("display_name") + 15, "\xC0\xAF");

  EXPECT_EQ(ParsePresetText(text).rule, PresetFileRule::kInvalidText);
}

TEST(PresetFileTest, PresetWithoutFingerprintIsRefused) {
  auto json = CanonicalJson(test::MakeExposureTransferPackage(1.0));
  json.erase("fingerprint");

  EXPECT_EQ(ParsePresetText(json.dump()).rule, PresetFileRule::kMissingFingerprint);
}

TEST(PresetFileTest, NonCanonicalPresetIsRefused) {
  const auto package  = test::MakeExposureTransferPackage(1.0);

  const auto indented = ParsePresetText(CanonicalJson(package).dump(2));
  EXPECT_EQ(indented.rule, PresetFileRule::kNotCanonical) << indented.message;

  // A changed value with the old fingerprint.
  std::string changed = CanonicalText(package);
  changed.replace(changed.find("\"deletion_protected\":true"), 25, "\"deletion_protected\":false");
  const auto stale = ParsePresetText(changed);
  EXPECT_EQ(stale.rule, PresetFileRule::kNotCanonical) << stale.message;

  EXPECT_TRUE(ParsePresetText(CanonicalText(package)).Ok());
}

TEST(PresetFileTest, PresetWithUnknownFieldIsRefused) {
  auto json             = CanonicalJson(test::MakeExposureTransferPackage(1.0));
  json["remote_script"] = "run";
  const auto top_level  = ParsePresetText(WithMatchingFingerprint(json));
  EXPECT_EQ(top_level.rule, PresetFileRule::kInvalidPackage);
  EXPECT_NE(top_level.message.find("remote_script"), std::string::npos) << top_level.message;

  json.erase("remote_script");
  json["color_grades"][0]["shader"] = "code";
  const auto nested                 = ParsePresetText(WithMatchingFingerprint(json));
  EXPECT_EQ(nested.rule, PresetFileRule::kInvalidPackage);
  EXPECT_NE(nested.message.find("shader"), std::string::npos) << nested.message;
}

TEST(PresetFileTest, PresetWithUnknownAdjustmentTypeIsRefused) {
  auto json = CanonicalJson(test::MakeExposureTransferPackage(1.0));
  ASSERT_FALSE(json["color_grades"][0]["adjustments"].empty());
  json["color_grades"][0]["adjustments"][0]["type"] = "alcedo.adjustment.native_plugin";

  const auto read = ParsePresetText(WithMatchingFingerprint(json));

  EXPECT_EQ(read.rule, PresetFileRule::kInvalidPackage);
  EXPECT_NE(read.message.find("alcedo.adjustment.native_plugin"), std::string::npos)
      << read.message;
}

TEST(PresetFileTest, PresetWithAbsoluteLutPathIsRefused) {
  const auto read =
      ParsePresetText(CanonicalText(test::MakeLutTransferPackage("C:/luts/look.cube")));

  EXPECT_EQ(read.rule, PresetFileRule::kFileReference);
  EXPECT_NE(read.message.find("LUT"), std::string::npos) << read.message;
  EXPECT_FALSE(CheckPresetReferences(test::MakeLutTransferPackage("/home/user/look.cube")).Ok());
}

TEST(PresetFileTest, PresetWithUncLutPathIsRefused) {
  const auto read =
      ParsePresetText(TextWithLmtParams({{"cube_path", R"(\\attacker.example\share\look.cube)"}}));

  EXPECT_EQ(read.rule, PresetFileRule::kFileReference) << read.message;
}

TEST(PresetFileTest, PresetWithParentPathLutReferenceIsRefused) {
  for (const std::string path : {"../outside.cube", "user/../../outside.cube", "/etc/look.cube",
                                 "C:/luts/look.cube", R"(user\..\look.cube)"}) {
    const auto read = ParsePresetText(TextWithLmtParams(
        {{"cube_path", ""}, {"reference", {{"kind", "library"}, {"path", path}}}}));
    EXPECT_EQ(read.rule, PresetFileRule::kFileReference) << path << ": " << read.message;
  }
}

TEST(PresetFileTest, PresetWithLibraryOrOfficialLutReferenceIsAccepted) {
  const auto library = ParsePresetText(TextWithLmtParams(
      {{"cube_path", ""}, {"reference", {{"kind", "library"}, {"path", "user/look.cube"}}}}));
  EXPECT_TRUE(library.Ok()) << library.message;

  const auto official = ParsePresetText(TextWithLmtParams(
      {{"cube_path", ""},
       {"reference",
        {{"kind", "official"}, {"package_id", "spectral_film_lut"}, {"lut_id", "kodak-5207"}}}}));
  EXPECT_TRUE(official.Ok()) << official.message;
}

TEST(PresetFileTest, PresetWithLensDatabasePathIsRefused) {
  auto json = CanonicalJson(test::MakeDefaultDocumentTransferPackage());
  ASSERT_TRUE(json["develop"].contains("lens_calib"));
  json["develop"]["lens_calib"]["lens_profile_db_path"] = R"(\\attacker.example\share\lens.xml)";

  const auto read = ParsePresetText(WithMatchingFingerprint(json));

  EXPECT_EQ(read.rule, PresetFileRule::kFileReference) << read.message;
}

TEST(PresetFileTest, PresetWithNonFiniteNumberIsRefused) {
  // JSON text has no form for a non-finite number: the parser refuses an overflowing number and a
  // NaN word before a value is built.
  const std::string text = CanonicalText(test::MakeExposureTransferPackage(1.0));
  for (const std::string value : {"1e999", "-1e999", "NaN", "Infinity"}) {
    std::string changed = text;
    changed.replace(changed.find("\"deletion_protected\":true"), 25,
                    "\"deletion_protected\":" + value);
    const auto read = ParsePresetText(changed);
    EXPECT_EQ(read.rule, PresetFileRule::kInvalidJson) << value << ": " << read.message;
  }
}

TEST(PresetFileTest, PresetWithForeignLensDatabasePathIsRefusedAndBuiltInPathIsAccepted) {
  auto json = CanonicalJson(test::MakeDefaultDocumentTransferPackage());
  ASSERT_EQ(json["develop"]["lens_calib"]["lens_profile_db_path"],
            DevelopPayload{}.lens_profile_db_path);
  EXPECT_TRUE(ParsePresetText(json.dump()).Ok());

  json["develop"]["lens_calib"]["lens_profile_db_path"] = "C:/other/lens_calib";
  EXPECT_EQ(ParsePresetText(WithMatchingFingerprint(json)).rule, PresetFileRule::kFileReference);
}

// A finite number above the float range is refused by the model loaders instead of a conversion
// with undefined behavior (#1097).
TEST(PresetFileTest, PresetWithNumberAboveFloatRangeIsRefused) {
  auto  json        = CanonicalJson(test::MakeExposureTransferPackage(1.0));
  auto& adjustments = json["color_grades"][0]["adjustments"];
  bool  found       = false;
  for (auto& adjustment : adjustments) {
    if (adjustment["params"].contains("exposure_ev")) {
      adjustment["params"]["exposure_ev"] = 1e39;
      found                               = true;
    }
  }
  ASSERT_TRUE(found);
  const auto exposure = ParsePresetText(WithMatchingFingerprint(json));
  EXPECT_EQ(exposure.rule, PresetFileRule::kInvalidPackage) << exposure.message;

  auto mix                      = CanonicalJson(test::MakeExposureTransferPackage(1.0));
  mix["color_grades"][0]["mix"] = 1e39;
  EXPECT_EQ(ParsePresetText(WithMatchingFingerprint(mix)).rule, PresetFileRule::kInvalidPackage);
}

TEST(PresetFileTest, PresetAboveGradeCountLimitIsRefused) {
  auto       json      = CanonicalJson(test::MakeExposureTransferPackage(1.0));
  const auto grade     = json["color_grades"][0];
  json["color_grades"] = nlohmann::json::array();
  for (std::size_t index = 0; index <= kPresetMaxColorGrades; ++index) {
    json["color_grades"].push_back(grade);
  }

  EXPECT_EQ(ParsePresetText(WithMatchingFingerprint(json)).rule, PresetFileRule::kCountLimit);
}

TEST(PresetFileTest, PresetWithLongStringIsRefused) {
  auto json                               = CanonicalJson(test::MakeExposureTransferPackage(1.0));
  json["color_grades"][0]["display_name"] = std::string(kPresetMaxStringBytes + 1, 'a');
  EXPECT_EQ(ParsePresetText(WithMatchingFingerprint(json)).rule, PresetFileRule::kStringTooLong);

  json["color_grades"][0]["display_name"]                              = "Grade";
  json["color_grades"][0][std::string(kPresetMaxStringBytes + 1, 'k')] = 1;
  EXPECT_EQ(ParsePresetText(WithMatchingFingerprint(json)).rule, PresetFileRule::kStringTooLong);
}

TEST(PresetFileTest, SymbolicLinkPresetIsRefused) {
  test::TestTempDirectory directory;
  const fs::path          target = directory / "Real.alcpreset";
  ASSERT_TRUE(WritePresetFile(test::MakeExposureTransferPackage(1.0), target).Ok());
  const fs::path  link = directory / "Link.alcpreset";
  std::error_code error;
  fs::create_symlink(target, link, error);
  if (error) {
    GTEST_SKIP() << "This account cannot create a symbolic link: " << error.message();
  }

  EXPECT_EQ(ReadPresetFile(link).rule, PresetFileRule::kNotRegularFile);
  EXPECT_TRUE(ReadPresetFile(target).Ok());
}

#ifdef _WIN32
TEST(PresetFileTest, JunctionPresetIsRefused) {
  // A junction needs no privilege, so this checks the link rule where symbolic links cannot be
  // made. The reader opens the junction itself and never its target folder.
  test::TestTempDirectory directory;
  fs::create_directories(directory / "target");
  const fs::path    link = directory / "Junction.alcpreset";
  const std::string command =
      "mklink /J \"" + link.string() + "\" \"" + (directory / "target").string() + "\" >NUL";
  ASSERT_EQ(std::system(command.c_str()), 0);
  ASSERT_TRUE(fs::exists(link));

  EXPECT_EQ(ReadPresetFile(link).rule, PresetFileRule::kNotRegularFile);
}
#endif

TEST(PresetFileTest, FolderIsNotARegularPresetFile) {
  test::TestTempDirectory directory;
  fs::create_directories(directory / "Folder.alcpreset");

  EXPECT_EQ(ReadPresetFile(directory / "Folder.alcpreset").rule, PresetFileRule::kNotRegularFile);
  EXPECT_EQ(ReadPresetFile(directory / "Absent.alcpreset").rule, PresetFileRule::kReadFailed);
}

TEST(PresetFileTest, PresetNameRulesRefuseFileSystemNames) {
  EXPECT_TRUE(PresetNameError("Warm Film 2").empty());
  EXPECT_TRUE(PresetNameError("暖色 胶片").empty());
  const std::vector<std::string> invalid_names = {
      "",    " Warm", "Warm ",   "Warm.", "a/b",  R"(a\b)",
      "a:b", "a*b",   "a?b",     "a\"b",  "a<b",  "a|b",
      "CON", "con",   "NUL.txt", "COM1",  "lpt9", std::string{'a', '\x01', 'b'}};
  for (const auto& name : invalid_names) {
    EXPECT_FALSE(PresetNameError(name).empty()) << name;
  }
  EXPECT_TRUE(PresetNameError(std::string(kPresetMaxNameLength, 'a')).empty());
  EXPECT_FALSE(PresetNameError(std::string(kPresetMaxNameLength + 1, 'a')).empty());
}

TEST(PresetFileTest, PresetSelectionOmitsGeometryAndLensCorrection) {
  const auto document  = test::DocumentWithExposureEv(0.5);
  const auto selection = SelectPresetItems(document);

  for (const auto& node : selection.nodes) {
    for (const auto& item : node.items) {
      EXPECT_NE(item.kind, AdjustmentTransferItemKind::Geometry);
      EXPECT_NE(item.kind, AdjustmentTransferItemKind::LensCalibration);
    }
  }
  const auto package = AdjustmentTransferPackageBuilder::Build(document, selection);
  EXPECT_FALSE(package.develop_.geometry.has_value());
  EXPECT_FALSE(package.develop_.lens_calib.has_value());
  EXPECT_TRUE(package.develop_.raw_decode.has_value());
  EXPECT_TRUE(package.develop_.color_temp.has_value());
  EXPECT_EQ(package.color_grades_.size(), CaptureDocumentTransfer(document).color_grades_.size());
}

// ── Mutation run (plan 9, PS1) ──────────────────────────────────────────────

/// Valid preset packages that the mutations start from: Develop fields, adjustments, a LUT
/// library reference, and Radial and Linear Masks.
auto MutationSourcePackages() -> std::vector<nlohmann::json> {
  std::vector<nlohmann::json> sources;
  sources.push_back(CanonicalJson(test::MakeDefaultDocumentTransferPackage()));
  sources.push_back(CanonicalJson(test::MakeExposureTransferPackage(2.0)));
  auto with_masks = test::DocumentWithExposureEv(-0.5);
  grade_mask_test::AddRadialMask(with_masks, MaskId{"mask.radial"});
  grade_mask_test::AddLinearGradientMask(with_masks, MaskId{"mask.linear"});
  sources.push_back(CanonicalJson(CaptureDocumentTransfer(with_masks)));
  auto library = nlohmann::json::parse(TextWithLmtParams(
      {{"cube_path", ""}, {"reference", {{"kind", "library"}, {"path", "user/look.cube"}}}}));
  sources.push_back(std::move(library));
  return sources;
}

void CollectPointers(const nlohmann::json& json, const nlohmann::json::json_pointer& pointer,
                     std::vector<nlohmann::json::json_pointer>* pointers) {
  if (pointer.to_string() != "/fingerprint") {
    pointers->push_back(pointer);
  }
  if (json.is_object()) {
    for (const auto& [key, value] : json.items()) {
      CollectPointers(value, pointer / key, pointers);
    }
  } else if (json.is_array()) {
    for (std::size_t index = 0; index < json.size(); ++index) {
      CollectPointers(json[index], pointer / index, pointers);
    }
  }
}

auto HostileValue(std::mt19937_64& random) -> nlohmann::json {
  static const std::vector<nlohmann::json>   kValues = {0,
                                                        -1,
                                                        1,
                                                        2,
                                                        3,
                                                        7,
                                                        255,
                                                        256,
                                                        65535,
                                                        65536,
                                                        2147483647,
                                                        -2147483648LL,
                                                        4294967295ULL,
                                                        9223372036854775807LL,
                                                        1e30,
                                                        -1e30,
                                                        1e-30,
                                                        0.5,
                                                        -0.5,
                                                        1e9,
                                                        -1e9,
                                                        "",
                                                        "x",
                                                        "../../x",
                                                        true,
                                                        false,
                                                        nullptr,
                                                        nlohmann::json::array(),
                                                        nlohmann::json::object(),
                                                        {1, 2, 3},
                                                        std::string(kPresetMaxStringBytes, 'z')};
  std::uniform_int_distribution<std::size_t> pick(0, kValues.size());
  const std::size_t                          index = pick(random);
  if (index == kValues.size()) {
    std::uniform_real_distribution<double> real(-1e6, 1e6);
    return real(random);
  }
  return kValues[index];
}

/// Mutate one value of @p json: replace a value, remove an object member, or repeat an array
/// element. Keeps the fingerprint of the result valid, so that the package rules run.
auto MutateStructure(nlohmann::json json, std::mt19937_64& random) -> std::string {
  std::vector<nlohmann::json::json_pointer> pointers;
  CollectPointers(json, nlohmann::json::json_pointer{}, &pointers);
  std::uniform_int_distribution<std::size_t> pick(1, pointers.size() - 1);
  const auto&                                pointer = pointers[pick(random)];
  std::uniform_int_distribution<int>         kind(0, 9);
  const int                                  mutation = kind(random);
  auto&                                      parent   = json[pointer.parent_pointer()];
  if (mutation == 0 && parent.is_object()) {
    parent.erase(pointer.back());
  } else if (mutation == 1 && parent.is_array()) {
    parent.push_back(json[pointer]);
  } else {
    json[pointer] = HostileValue(random);
  }
  return WithMatchingFingerprint(std::move(json));
}

/// Mutate the bytes of @p text: change, insert, remove, or repeat a range.
auto MutateBytes(std::string text, std::mt19937_64& random) -> std::string {
  std::uniform_int_distribution<std::size_t> position(0, text.size() - 1);
  std::uniform_int_distribution<int>         kind(0, 3);
  std::uniform_int_distribution<int>         byte(0, 255);
  const std::size_t                          at = position(random);
  switch (kind(random)) {
    case 0:
      text[at] = static_cast<char>(byte(random));
      break;
    case 1:
      text.insert(at, 1, "[{\"]}:,0e-\\"[byte(random) % 11]);
      break;
    case 2:
      text.erase(at, std::min<std::size_t>(text.size() - at, 1 + byte(random) % 32));
      break;
    default:
      text.insert(at, text.substr(at, std::min<std::size_t>(text.size() - at, 64)));
      break;
  }
  return text;
}

TEST(PresetFileTest, MutatedPresetsNeverCrashReaderOrPastePlanner) {
  constexpr int   kCases = 10000;
  std::mt19937_64 random(20261124);
  const auto      sources  = MutationSourcePackages();
  const auto      target   = CreateDefaultPipelineDocument();
  int             accepted = 0;
  int             planned  = 0;
  for (int index = 0; index < kCases; ++index) {
    const auto&       source = sources[static_cast<std::size_t>(index) % sources.size()];
    const std::string text =
        index % 2 == 0 ? MutateStructure(source, random) : MutateBytes(source.dump(), random);
    const auto read = ParsePresetText(text);
    if (!read.Ok()) {
      EXPECT_FALSE(read.message.empty()) << "case " << index;
      continue;
    }
    ++accepted;
    try {
      (void)DocumentTransferPlanner::Plan(*read.package, target);
      ++planned;
    } catch (const std::exception&) {
      // A refusal of the planner is a valid result; a crash is not.
    }
  }
  // The structural mutations keep the fingerprint valid, so a part of them reaches the planner.
  EXPECT_GT(accepted, 0);
  EXPECT_GT(planned, 0);
  RecordProperty("accepted", accepted);
  RecordProperty("planned", planned);
}

}  // namespace
}  // namespace alcedo
