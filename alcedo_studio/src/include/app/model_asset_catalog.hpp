//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>
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

enum class ModelInferenceBackend : uint8_t { kOnnxRuntime, kNativeCoreMl };

auto ToString(ModelInferenceBackend backend) -> const char*;

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
  ModelInferenceBackend       inference_backend;
  ModelLanguage               language;
  uint32_t                    embedding_dimension        = 0;
  uint32_t                    native_embedding_dimension = 0;
  uint32_t                    image_size                 = 0;
  const char*                 embedding_transform;
  // License shown on the model card (AI Models plan, P13) and the web page of its text.
  const char*                 license_name;
  const char*                 license_url;
  std::vector<ModelAssetSpec> assets;
};

// Every profile of the catalog, on all platforms. The catalog test compares it with the Rust
// catalog through tests/resources/clip_model_catalog/clip_model_profiles.json.
auto AllSemanticModelProfiles() -> const std::vector<ModelProfileSpec>&;

// Returns the canonical semantic model profiles of this platform. Backed by a function-local
// static so the returned reference is stable and initialization order is
// well-defined.
auto SemanticModelProfiles() -> const std::vector<ModelProfileSpec>&;

// Looks up a profile by profile_id or model_id. Returns nullptr if unknown.
auto FindSemanticProfile(const std::string& profile_or_model_id) -> const ModelProfileSpec*;

// Whether a profile can run in this build's native inference stack. ONNX Runtime
// profiles are cross-platform; native CoreML profiles are macOS-only.
auto IsModelProfileSupportedOnCurrentPlatform(const ModelProfileSpec& profile) -> bool;

auto ProfileTotalBytes(const ModelProfileSpec& profile) -> uint64_t;

// ── CLIP model download settings (QSettings) ──
// ModelDownloadController and ProjectService read these through the functions below, so the
// source presets have one definition.
inline constexpr const char* kSemanticModelDirectoryKey      = "semantic/modelDirectory";
inline constexpr const char* kSemanticModelEndpointPresetKey = "semantic/modelEndpointPreset";
inline constexpr const char* kSemanticCustomModelEndpointKey = "semantic/customModelEndpoint";

// <application directory>/model, the model directory when no directory is set.
auto                         DefaultSemanticModelDirectory() -> QString;
// The configured model directory, or the default.
auto                         ConfiguredSemanticModelDirectory() -> QString;
// "mirror" (default), "huggingface", "sufy", or "custom".
auto                         NormalizedModelEndpointPreset(QString preset) -> QString;
// The Hugging Face endpoint of a source preset; "custom" uses @p custom_endpoint when it is
// not empty, else the mirror.
auto ModelEndpointForPreset(const QString& preset, const QString& custom_endpoint) -> QString;
auto ConfiguredModelEndpointPreset() -> QString;
auto ConfiguredCustomModelEndpoint() -> QString;
// The endpoint of the configured source preset.
auto EffectiveSemanticModelEndpoint() -> QString;

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
