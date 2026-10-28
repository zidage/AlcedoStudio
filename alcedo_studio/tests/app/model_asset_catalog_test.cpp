//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/model_asset_catalog.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <sstream>
#include <string>

#ifndef ALCEDO_CLIP_MODEL_CATALOG_FIXTURE
#define ALCEDO_CLIP_MODEL_CATALOG_FIXTURE ""
#endif

namespace alcedo::test {
namespace {

using Json = nlohmann::json;

auto ReadCatalogFile() -> Json {
  std::ifstream     in(std::filesystem::path(ALCEDO_CLIP_MODEL_CATALOG_FIXTURE), std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return Json::parse(text.str());
}

// The Rust catalog test (model_assets.rs, profile_table_equals_the_shared_clip_catalog_file)
// compares the Rust profile table with the same file, so the C++ and Rust catalogs agree,
// including the license fields of each profile.
TEST(ModelAssetCatalogTest, ProfileTableEqualsTheSharedClipCatalogFile) {
  const Json  expected = ReadCatalogFile();
  const auto& profiles = AllSemanticModelProfiles();
  ASSERT_TRUE(expected.is_array());
  ASSERT_EQ(expected.size(), profiles.size());
  for (size_t i = 0; i < profiles.size(); ++i) {
    const auto& profile = profiles[i];
    const Json& row     = expected[i];
    SCOPED_TRACE(profile.profile_id);
    EXPECT_EQ(row["profile_id"], profile.profile_id);
    EXPECT_EQ(row["display_name"], profile.display_name);
    EXPECT_EQ(row["model_id"], profile.model_id);
    EXPECT_EQ(row["revision"], profile.revision);
    EXPECT_EQ(row["engine_profile_id"], profile.engine_profile_id);
    EXPECT_EQ(row["inference_backend"], ToString(profile.inference_backend));
    EXPECT_EQ(row["language"], ToString(profile.language));
    EXPECT_EQ(row["embedding_dimension"], profile.embedding_dimension);
    EXPECT_EQ(row["native_embedding_dimension"], profile.native_embedding_dimension);
    EXPECT_EQ(row["image_size"], profile.image_size);
    EXPECT_EQ(row["embedding_transform"], profile.embedding_transform);
    EXPECT_EQ(row["license_name"], profile.license_name);
    EXPECT_EQ(row["license_url"], profile.license_url);
    ASSERT_EQ(row["assets"].size(), profile.assets.size());
    for (size_t a = 0; a < profile.assets.size(); ++a) {
      const auto& asset = profile.assets[a];
      const Json& item  = row["assets"][a];
      SCOPED_TRACE(asset.remote_path);
      EXPECT_EQ(item["role"], ToString(asset.role));
      EXPECT_EQ(item["repo_id"], asset.repo_id);
      EXPECT_EQ(item["revision"], asset.revision);
      EXPECT_EQ(item["remote_path"], asset.remote_path);
      EXPECT_EQ(item["local_path"], asset.local_path);
      EXPECT_EQ(item["size_bytes"], asset.size_bytes);
      if (asset.sha256 == nullptr) {
        EXPECT_TRUE(item["sha256"].is_null());
      } else {
        EXPECT_EQ(item["sha256"], asset.sha256);
      }
    }
  }
}

TEST(ModelAssetCatalogTest, ProfilesNameTheLicensesOfTheModelCards) {
  const auto license_of = [](const std::string& profile_id) -> std::string {
    for (const auto& profile : AllSemanticModelProfiles()) {
      if (profile_id == profile.profile_id) {
        return profile.license_name;
      }
    }
    return {};
  };
  EXPECT_EQ(license_of("mobileclip2-s2-en"), "apple-amlr");
  EXPECT_EQ(license_of("jina-clip-v2-int8-multilingual"), "cc-by-nc-4.0");
  EXPECT_EQ(license_of("siglip2-b32-256-multilingual"), "Apache-2.0");
  EXPECT_EQ(license_of("siglip2-base-256-coreml-macos"), "Apache-2.0");
}

TEST(ModelAssetCatalogTest, SourcePresetsMapToTheirEndpoints) {
  EXPECT_EQ(ModelEndpointForPreset(QStringLiteral("mirror"), {}),
            QStringLiteral("https://hf-mirror.com"));
  EXPECT_EQ(ModelEndpointForPreset(QStringLiteral("huggingface"), {}),
            QStringLiteral("https://huggingface.co"));
  EXPECT_EQ(ModelEndpointForPreset(QStringLiteral("sufy"), {}),
            QStringLiteral("https://hf-cdn.sufy.com"));
  EXPECT_EQ(ModelEndpointForPreset(QStringLiteral("custom"), QStringLiteral(" https://a.test ")),
            QStringLiteral("https://a.test"));
  EXPECT_EQ(ModelEndpointForPreset(QStringLiteral("custom"), {}),
            QStringLiteral("https://hf-mirror.com"));
  EXPECT_EQ(NormalizedModelEndpointPreset(QStringLiteral("unknown")), QStringLiteral("mirror"));
}

}  // namespace
}  // namespace alcedo::test
