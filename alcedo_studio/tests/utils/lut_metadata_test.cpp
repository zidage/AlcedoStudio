//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/lut/lut_metadata.hpp"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <json.hpp>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "utils/lut/lut_inventory_digest.hpp"
#include "utils/lut/lut_library_scan.hpp"

namespace alcedo::test {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kOfficialFilmWithPrint =
    R"(# ALCEDO_LUT {"schema":1,"id":"spectral_film_lut:kodak_vision3_250d_5207:kodak_vision_2383","origin":"alcedo","category":"film_simulation","source":{"id":"spectral_film_lut","name":"Spectral Film LUT"},"film":{"id":"kodak_vision3_250d_5207","name":"Vision3 250D 5207","brand":"Kodak"},"print":{"id":"kodak_vision_2383","name":"Vision 2383","brand":"Kodak","kind":"film"},"input_space":"ACEScc","output_space":"ACEScc"})";

constexpr std::string_view kUserFilmWithPaperPrint =
    R"(# ALCEDO_LUT {"schema":1,"id":"my_lab:portra_400:crystal_archive","origin":"user","category":"film_simulation","source":{"id":"my_lab","name":"My Lab"},"film":{"id":"kodak_portra_400","name":"Portra 400","brand":"Kodak"},"print":{"id":"fujifilm_crystal_archive","name":"Crystal Archive","brand":"Fujifilm","kind":"paper"},"input_space":"ACEScc","output_space":"ACEScc","aliases":["portra"],"future_field":{"ignored":true}})";

constexpr std::string_view kNumericTable =
    "LUT_3D_SIZE 2\n"
    "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";

auto CubeWith(std::string_view comment_line) -> std::string {
  return std::string(comment_line) + "\n# Attribution: generator\nTITLE \"t\"\n" +
         std::string(kNumericTable);
}

auto Sha256Hex(std::string_view bytes) -> std::string {
  return QCryptographicHash::hash(
             QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
             QCryptographicHash::Sha256)
      .toHex()
      .toStdString();
}

class TemporaryLutRoot {
 public:
  TemporaryLutRoot() {
    std::random_device device;
    path_ = fs::temp_directory_path() / ("alcedo-lut-metadata-test-" + std::to_string(device()));
    fs::create_directories(path_);
  }
  ~TemporaryLutRoot() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  TemporaryLutRoot(const TemporaryLutRoot&)             = delete;
  TemporaryLutRoot&  operator=(const TemporaryLutRoot&) = delete;

  [[nodiscard]] auto Path() const -> const fs::path& { return path_; }

  void               Write(const std::u8string& relative, std::string_view bytes) const {
    const fs::path target = path_ / fs::path(relative);
    fs::create_directories(target.parent_path());
    std::ofstream output(target, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

 private:
  fs::path path_;
};

auto FindEntry(const LutLibraryInventory& inventory, std::string_view path)
    -> const LutLibraryEntry* {
  const auto found =
      std::find_if(inventory.entries.begin(), inventory.entries.end(),
                   [&](const LutLibraryEntry& entry) { return entry.relative_path == path; });
  return found == inventory.entries.end() ? nullptr : &*found;
}

TEST(LutMetadataTest, MetadataClassifiesUserFilmWithPrint) {
  const LutHeaderReadResult result = ReadLutHeader(CubeWith(kUserFilmWithPaperPrint));
  ASSERT_TRUE(result.Ok()) << result.message;
  ASSERT_TRUE(result.header.metadata.has_value());
  const LutMetadata& metadata = *result.header.metadata;
  EXPECT_EQ(metadata.origin, LutOrigin::kUser);
  EXPECT_EQ(metadata.category, LutCategory::kFilmSimulation);
  ASSERT_TRUE(metadata.source && metadata.film && metadata.print);
  EXPECT_EQ(metadata.source->id, "my_lab");
  EXPECT_EQ(metadata.film->brand, "Kodak");
  EXPECT_EQ(metadata.print->kind, LutPrintKind::kPaper);
  EXPECT_EQ(metadata.aliases, std::vector<std::string>{"portra"});
  EXPECT_EQ(result.header.lut_3d_size, 2);
  EXPECT_EQ(result.header.title, "t");
  // A third-party user LUT keeps its file name as the display name.
  EXPECT_EQ(LutDisplayName(result.header, "my portra"), "my portra");
  EXPECT_EQ(LutPrintOptionName(result.header), "");
}

// Plan 1.4 (L6A): an official film is titled by brand and stock; the print is a separate option.
TEST(LutMetadataTest, OfficialFilmTitleCombinesBrandAndStockWithoutPrint) {
  const LutHeaderReadResult result = ReadLutHeader(CubeWith(kOfficialFilmWithPrint));
  ASSERT_TRUE(result.Ok()) << result.message;
  EXPECT_EQ(result.header.Origin(), LutOrigin::kAlcedo);
  EXPECT_EQ(LutDisplayName(result.header, "kodak_vision3_250d_5207__kodak_vision_2383"),
            "Kodak Vision3 250D 5207");
  EXPECT_EQ(LutPrintOptionName(result.header), "Vision 2383");
  EXPECT_EQ(CanonicalLutFileStem(*result.header.metadata),
            "kodak_vision3_250d_5207__kodak_vision_2383");

  // A film name that already starts with its brand word is not prefixed again; a longer
  // first word that merely begins with the brand text still is.
  LutHeader prefixed            = result.header;
  prefixed.metadata->film->name = "kodak Gold 200";
  EXPECT_EQ(LutDisplayName(prefixed, "stem"), "kodak Gold 200");
  prefixed.metadata->film->name = "Kodakchrome 64";
  EXPECT_EQ(LutDisplayName(prefixed, "stem"), "Kodak Kodakchrome 64");
  // Without a brand, the film name alone is the title.
  prefixed.metadata->film->brand.clear();
  EXPECT_EQ(LutDisplayName(prefixed, "stem"), "Kodakchrome 64");
}

TEST(LutMetadataTest, SerializedMetadataParsesToTheSameFields) {
  const LutHeaderReadResult original = ReadLutHeader(CubeWith(kOfficialFilmWithPrint));
  ASSERT_TRUE(original.Ok());
  const std::string         json   = SerializeLutMetadataJson(*original.header.metadata);
  const LutHeaderReadResult reread = ReadLutHeader(CubeWith("# ALCEDO_LUT " + json));
  ASSERT_TRUE(reread.Ok()) << reread.message;
  EXPECT_EQ(SerializeLutMetadataJson(*reread.header.metadata), json);
}

TEST(LutMetadataTest, UnannotatedCubeIsGeneralUserLut) {
  const LutHeaderReadResult result =
      ReadLutHeader("\xEF\xBB\xBF# plain comment\r\nTITLE \"x\"\r\nLUT_3D_SIZE 2\r\n0 0 0\r\n");
  ASSERT_TRUE(result.Ok()) << result.message;
  EXPECT_FALSE(result.header.metadata.has_value());
  EXPECT_EQ(result.header.Origin(), LutOrigin::kUnannotated);
  EXPECT_TRUE(result.header.SupportsGradeApplication());
}

TEST(LutMetadataTest, OneDimensionalCubeIsVisibleButNotApplicable) {
  const LutHeaderReadResult result = ReadLutHeader("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n");
  ASSERT_TRUE(result.Ok()) << result.message;
  EXPECT_EQ(result.header.lut_1d_size, 2);
  EXPECT_FALSE(result.header.SupportsGradeApplication());
}

TEST(LutMetadataTest, MetadataRejectsDuplicateAndOversizedComments) {
  const std::string duplicate =
      std::string(kOfficialFilmWithPrint) + "\n" + CubeWith(kUserFilmWithPaperPrint);
  EXPECT_EQ(ReadLutHeader(duplicate).error, LutHeaderError::kDuplicateMetadata);

  std::string       oversized_description(kLutMetadataLineLimitBytes, 'a');
  const std::string oversized =
      R"(# ALCEDO_LUT {"schema":1,"id":"x","origin":"user","category":"general","input_space":"a","output_space":"a","description":")" +
      oversized_description + "\"}";
  EXPECT_EQ(ReadLutHeader(CubeWith(oversized)).error, LutHeaderError::kMetadataLineTooLarge);

  const std::string huge_header(kLutHeaderLimitBytes + 16, '#');
  EXPECT_EQ(ReadLutHeader(huge_header + "\n" + std::string(kNumericTable)).error,
            LutHeaderError::kHeaderTooLarge);
}

TEST(LutMetadataTest, MalformedMetadataIsAnErrorNotAGeneralLut) {
  struct Case {
    std::string    line;
    LutHeaderError expected;
  };
  const std::vector<Case> cases = {
      {"# ALCEDO_LUT {not json", LutHeaderError::kInvalidMetadataJson},
      {R"(# ALCEDO_LUT {"schema":2,"id":"x"})", LutHeaderError::kUnsupportedSchema},
      {R"(# ALCEDO_LUT {"schema":1,"id":"Bad ID","origin":"user","category":"general","input_space":"a","output_space":"a"})",
       LutHeaderError::kInvalidMetadataField},
      {R"(# ALCEDO_LUT {"schema":1,"id":"x","origin":"vendor","category":"general","input_space":"a","output_space":"a"})",
       LutHeaderError::kInvalidMetadataField},
      {R"(# ALCEDO_LUT {"schema":1,"id":"x","origin":"user","category":"film_simulation","source":{"id":"s","name":"S"},"input_space":"a","output_space":"a"})",
       LutHeaderError::kInvalidMetadataField},
      {R"(# ALCEDO_LUT {"schema":1,"id":"x","origin":"user","category":"general","film":{"id":"f","name":"F","brand":"B"},"input_space":"a","output_space":"a"})",
       LutHeaderError::kInvalidMetadataField},
      {R"(# ALCEDO_LUT {"schema":1,"id":"x","origin":"user","category":"film_simulation","source":{"id":"s","name":"S"},"film":{"id":"f","name":"F","brand":"B"},"print":{"id":"p","name":"P","brand":"B","kind":"slide"},"input_space":"a","output_space":"a"})",
       LutHeaderError::kInvalidMetadataField},
  };
  for (const Case& item : cases) {
    const LutHeaderReadResult result = ReadLutHeader(CubeWith(item.line));
    EXPECT_EQ(result.error, item.expected) << item.line;
    EXPECT_FALSE(result.message.empty()) << item.line;
    EXPECT_FALSE(result.header.metadata.has_value()) << item.line;
  }
}

TEST(LutMetadataTest, CubeWithoutNumericRowsIsRejected) {
  EXPECT_EQ(ReadLutHeader("TITLE \"x\"\nLUT_3D_SIZE 2\n").error, LutHeaderError::kNoNumericTable);
  EXPECT_EQ(ReadLutHeader("LUT_3D_SIZE many\n0 0 0\n").error, LutHeaderError::kInvalidDirective);
}

TEST(LutMetadataTest, PythonAndCppInventoryDigestsMatch) {
  std::ifstream input(fs::path(ALCEDO_LUT_METADATA_FIXTURE_DIR) / "inventory_digest_records.json",
                      std::ios::binary);
  ASSERT_TRUE(input.is_open());
  const nlohmann::json            fixture = nlohmann::json::parse(input);
  std::vector<LutInventoryRecord> records;
  for (const auto& item : fixture.at("records")) {
    records.push_back({item.at("id").get<std::string>(), item.at("path").get<std::string>(),
                       item.at("size").get<std::uint64_t>(), item.at("sha256").get<std::string>()});
  }
  const std::string expected           = fixture.at("expected_inventory_sha256").get<std::string>();
  const LutInventoryDigestResult first = ComputeLutInventoryDigest(records);
  ASSERT_TRUE(first.Ok()) << first.error;
  EXPECT_EQ(first.sha256, expected);

  std::reverse(records.begin(), records.end());
  EXPECT_EQ(ComputeLutInventoryDigest(records).sha256, expected);
  std::rotate(records.begin(), records.begin() + 1, records.end());
  EXPECT_EQ(ComputeLutInventoryDigest(records).sha256, expected);
}

TEST(LutMetadataTest, PythonPackageInventoryParsesInCpp) {
  // Written by scripts/luts/lut_inventory.py; test_lut_packages.py regenerates and compares it.
  std::ifstream input(
      fs::path(ALCEDO_LUT_METADATA_FIXTURE_DIR) / "package_inventory_from_python.json",
      std::ios::binary);
  ASSERT_TRUE(input.is_open());
  const std::string                    bytes((std::istreambuf_iterator<char>(input)),
                                             std::istreambuf_iterator<char>());
  const LutPackageInventoryParseResult result = ParseLutPackageInventory(bytes);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.inventory->package_id, "spectral_film_lut");
  EXPECT_EQ(result.inventory->file_count, 3u);
  EXPECT_EQ(result.inventory->inventory_sha256,
            "a55a78b24371b41a1d20c29439b8e6b8be9aef7ef760e2dd8aa48182f9c38eb3");
  EXPECT_EQ(result.inventory->auxiliary_files.size(), 1u);
}

TEST(LutMetadataTest, InventoryDigestRejectsDuplicateIdsAndCollidingPaths) {
  const std::string hash(64, 'a');
  EXPECT_FALSE(
      ComputeLutInventoryDigest({{"a", "a.cube", 1, hash}, {"a", "b.cube", 1, hash}}).Ok());
  EXPECT_FALSE(
      ComputeLutInventoryDigest({{"a", "A.cube", 1, hash}, {"b", "a.cube", 1, hash}}).Ok());
  EXPECT_FALSE(ComputeLutInventoryDigest({{"a", "../a.cube", 1, hash}}).Ok());
  EXPECT_FALSE(ComputeLutInventoryDigest({{"a", "/a.cube", 1, hash}}).Ok());
  EXPECT_FALSE(ComputeLutInventoryDigest({{"a", "dir\\a.cube", 1, hash}}).Ok());
  EXPECT_FALSE(ComputeLutInventoryDigest({{"a\nb", "a.cube", 1, hash}}).Ok());
  EXPECT_FALSE(ComputeLutInventoryDigest({{"a", "a.cube", 1, std::string(64, 'A')}}).Ok());
}

TEST(LutMetadataTest, PackageInventoryCrossChecksCountBytesAndDigest) {
  const std::string              lut_hash = Sha256Hex("lut");
  const LutInventoryDigestResult digest =
      ComputeLutInventoryDigest({{"src:film", "film.cube", 3, lut_hash}});
  ASSERT_TRUE(digest.Ok());
  nlohmann::json document = {
      {"schema", 1},
      {"kind", "alcedo-lut-package-inventory"},
      {"package_id", "src"},
      {"revision", "r1"},
      {"file_count", 1},
      {"inventory_sha256", digest.sha256},
      {"unpacked_bytes", 10},
      {"luts", {{{"id", "src:film"}, {"path", "film.cube"}, {"size", 3}, {"sha256", lut_hash}}}},
      {"auxiliary_files",
       {{{"path", "LICENSE.txt"}, {"size", 7}, {"sha256", Sha256Hex("license")}}}}};
  const LutPackageInventoryParseResult valid = ParseLutPackageInventory(document.dump());
  ASSERT_TRUE(valid) << valid.error;
  EXPECT_EQ(valid.inventory->file_count, 1u);
  EXPECT_EQ(valid.inventory->auxiliary_files.size(), 1u);

  nlohmann::json wrong_count = document;
  wrong_count["file_count"]  = 2;
  EXPECT_FALSE(ParseLutPackageInventory(wrong_count.dump()));
  nlohmann::json wrong_bytes    = document;
  wrong_bytes["unpacked_bytes"] = 3;
  EXPECT_FALSE(ParseLutPackageInventory(wrong_bytes.dump()));
  nlohmann::json wrong_digest      = document;
  wrong_digest["inventory_sha256"] = std::string(64, '0');
  EXPECT_FALSE(ParseLutPackageInventory(wrong_digest.dump()));
}

TEST(LutMetadataTest, ScanHashesOnlyOfficialLutsAndKeepsEqualNamesSeparate) {
  TemporaryLutRoot  root;
  const std::string official = CubeWith(kOfficialFilmWithPrint);
  root.Write(u8"packages/kodak_vision3_250d_5207__kodak_vision_2383.cube", official);
  root.Write(u8"胶片/my look.CUBE", CubeWith(kUserFilmWithPaperPrint));
  root.Write(u8"a/same.cube", std::string(kNumericTable));
  root.Write(u8"b/same.cube", std::string(kNumericTable));
  root.Write(u8"broken.cube", CubeWith("# ALCEDO_LUT {broken"));
  root.Write(u8"notes.txt", "not a lut");
  root.Write(u8".downloads/partial.cube", std::string(kNumericTable));

  const LutLibraryInventory inventory = ScanLutLibrary(root.Path());
  ASSERT_EQ(inventory.entries.size(), 5u);
  EXPECT_TRUE(inventory.Complete());

  const LutLibraryEntry* official_entry =
      FindEntry(inventory, "packages/kodak_vision3_250d_5207__kodak_vision_2383.cube");
  ASSERT_NE(official_entry, nullptr);
  EXPECT_TRUE(official_entry->IsOfficial());
  EXPECT_EQ(official_entry->sha256, Sha256Hex(official));
  EXPECT_EQ(official_entry->size, official.size());
  EXPECT_EQ(official_entry->DisplayName(), "Kodak Vision3 250D 5207");
  EXPECT_EQ(official_entry->PrintOptionName(), "Vision 2383");

  const LutLibraryEntry* user_entry = FindEntry(inventory, "\xE8\x83\xB6\xE7\x89\x87/my look.CUBE");
  ASSERT_NE(user_entry, nullptr);
  EXPECT_EQ(user_entry->name, "my look");
  EXPECT_TRUE(user_entry->sha256.empty());
  EXPECT_EQ(user_entry->DisplayName(), "my look");

  const LutLibraryEntry* first  = FindEntry(inventory, "a/same.cube");
  const LutLibraryEntry* second = FindEntry(inventory, "b/same.cube");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->name, second->name);
  EXPECT_TRUE(first->sha256.empty());

  const LutLibraryEntry* broken = FindEntry(inventory, "broken.cube");
  ASSERT_NE(broken, nullptr);
  EXPECT_EQ(broken->header_error, LutHeaderError::kInvalidMetadataJson);
  EXPECT_TRUE(broken->sha256.empty());
  EXPECT_EQ(FindEntry(inventory, ".downloads/partial.cube"), nullptr);
}

TEST(LutMetadataTest, ParallelScanMatchesSingleWorkerScan) {
  TemporaryLutRoot root;
  for (int index = 0; index < 96; ++index) {
    const std::string   name = "dir" + std::to_string(index % 7) + "/lut" + std::to_string(index);
    const std::u8string relative(reinterpret_cast<const char8_t*>(name.data()), name.size());
    root.Write(relative + u8".cube", index % 3 == 0
                                         ? CubeWith(kOfficialFilmWithPrint) + std::to_string(index)
                                         : std::string(kNumericTable));
  }
  LutLibraryScanOptions serial;
  serial.worker_count = 1;
  LutLibraryScanOptions parallel;
  parallel.worker_count          = 8;
  const LutLibraryInventory one  = ScanLutLibrary(root.Path(), serial);
  const LutLibraryInventory many = ScanLutLibrary(root.Path(), parallel);
  ASSERT_EQ(one.entries.size(), 96u);
  EXPECT_EQ(SerializeLutLibraryInventory(one), SerializeLutLibraryInventory(many));
  EXPECT_EQ(std::count_if(many.entries.begin(), many.entries.end(),
                          [](const LutLibraryEntry& entry) { return !entry.sha256.empty(); }),
            32);
  EXPECT_TRUE(std::is_sorted(many.entries.begin(), many.entries.end(),
                             [](const LutLibraryEntry& left, const LutLibraryEntry& right) {
                               return left.relative_path < right.relative_path;
                             }));
}

TEST(LutMetadataTest, PersistedInventoryReadsBackWithoutRescanning) {
  TemporaryLutRoot root;
  root.Write(u8"official.cube", CubeWith(kOfficialFilmWithPrint));
  root.Write(u8"user.cube", std::string(kNumericTable));
  root.Write(u8"broken.cube", "no table");
  const LutLibraryInventory scanned = ScanLutLibrary(root.Path());
  ASSERT_EQ(WriteLutLibraryInventoryFile(root.Path(), scanned), "");

  // Changing a file after the scan does not change the persisted inventory:
  // reading it back performs no header parsing or hashing.
  root.Write(u8"official.cube", std::string(kNumericTable));
  const LutLibraryInventoryParseResult reread = ReadLutLibraryInventoryFile(root.Path());
  ASSERT_TRUE(reread) << reread.error;
  EXPECT_EQ(SerializeLutLibraryInventory(*reread.inventory), SerializeLutLibraryInventory(scanned));
  const LutLibraryEntry* official = FindEntry(*reread.inventory, "official.cube");
  ASSERT_NE(official, nullptr);
  EXPECT_TRUE(official->IsOfficial());
  EXPECT_EQ(official->PrintOptionName(), "Vision 2383");
}

TEST(LutMetadataTest, DamagedInventoryFileIsRejected) {
  EXPECT_FALSE(ParseLutLibraryInventory("{"));
  const std::string escaping_path =
      R"({"schema":1,"kind":"alcedo-lut-library-inventory","complete":true,"luts":[{"name":"x","path":"../x.cube","size":1,"status":"none"}],"diagnostics":[]})";
  EXPECT_FALSE(ParseLutLibraryInventory(escaping_path));
  const std::string user_with_hash =
      R"({"schema":1,"kind":"alcedo-lut-library-inventory","complete":true,"luts":[{"name":"x","path":"x.cube","size":1,"status":"none","lut_3d_size":2,"lut_1d_size":0,"sha256":"00"}],"diagnostics":[]})";
  EXPECT_FALSE(ParseLutLibraryInventory(user_with_hash));
}

}  // namespace
}  // namespace alcedo::test
