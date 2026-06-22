//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace alcedo {

// Mirrors the model asset catalog in rust/puerh_mind/src/service/model_assets.rs.
// The C++ application layer (ModelDownloadService) uses this to know which
// files to fetch from Hugging Face, verify them, and write the resolved
// manifest. Keep the data here in sync with the Rust catalog.

constexpr uint32_t    kSemanticRequiredEmbeddingDimension = 512;
constexpr uint32_t    kSemanticSiglip2EmbeddingDimension  = 768;
constexpr const char* kSemanticResolvedManifestFile       = "alcedo_model_manifest.json";

enum class ModelAssetRole : uint8_t {
  kTextModel,
  kVisionModel,
  kCoreMlTextModel,
  kCoreMlVisionModel,
  kMultimodalModel,
  kOnnxConfig,
  kModelConfig,
  kCoreMlManifest,
  kPreprocessConfig,
  kTokenizer,
  kTokenizerArchive,
  kTokenizerConfig,
  kVocab,
  kSpecialTokens,
};

auto ToString(ModelAssetRole role) -> const char*;

enum class ModelLanguage : uint8_t { kEn, kZh, kMultilingual };

auto ToString(ModelLanguage language) -> const char*;

struct ModelAssetSpec {
  ModelAssetRole role;
  const char*    repo_id;
  const char*    revision;
  const char*    remote_path;
  const char*    local_path;
  uint64_t       size_bytes = 0;
  const char*    sha256;  // nullptr when no checksum is pinned.
};

struct ModelProfileSpec {
  const char*                 profile_id;
  const char*                 display_name;
  const char*                 model_id;
  const char*                 revision;
  const char*                 engine_profile_id;
  ModelLanguage               language;
  uint32_t                    embedding_dimension        = 0;
  uint32_t                    native_embedding_dimension = 0;
  uint32_t                    image_size                 = 0;
  const char*                 embedding_transform;
  std::vector<ModelAssetSpec> assets;
};

// Returns the canonical semantic model profiles. Backed by a function-local
// static so the returned reference is stable and initialization order is
// well-defined.
auto SemanticModelProfiles() -> const std::vector<ModelProfileSpec>&;

// Looks up a profile by profile_id or model_id. Returns nullptr if unknown.
auto FindSemanticProfile(const std::string& profile_or_model_id) -> const ModelProfileSpec*;

auto ProfileTotalBytes(const ModelProfileSpec& profile) -> uint64_t;

// Sibling staging directory used while a profile download is in flight:
// "<parent>/.<root-name>.download".
auto StagingRoot(const std::filesystem::path& root) -> std::filesystem::path;

// Builds the Hugging Face resolve URL for an asset:
// "{hf_endpoint}/{repo_id}/resolve/{revision}/{remote_path}".
auto BuildAssetUrl(const std::string& hf_endpoint, const ModelAssetSpec& asset) -> std::string;

// Lowercase hex SHA-256 of a file, or an empty string on failure.
auto Sha256File(const std::filesystem::path& path) -> std::string;

// Verifies a downloaded asset: file size must match and, when a sha256 is
// pinned, the digest must match (case-insensitive). Returns nullopt on success
// or a human-readable error message.
auto ValidateAssetFile(const ModelAssetSpec& asset, const std::filesystem::path& local_path)
    -> std::optional<std::string>;

// Writes "<root>/alcedo_model_manifest.json" describing the resolved profile.
// Returns nullopt on success or an error message.
auto WriteResolvedManifest(const ModelProfileSpec& profile, const std::filesystem::path& root)
    -> std::optional<std::string>;

// Atomically promotes a staged profile directory into its final location:
// creates the parent, removes an existing root, and renames staging -> root.
// Returns nullopt on success or an error message.
auto PromoteStagingRoot(const std::filesystem::path& staging, const std::filesystem::path& root)
    -> std::optional<std::string>;

}  // namespace alcedo
