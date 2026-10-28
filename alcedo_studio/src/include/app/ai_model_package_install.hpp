//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo {

/// Package ID of the AI Mask model set in the AI model feed.
inline constexpr std::string_view kAiMaskModelPackageId            = "alcedo-ai-mask";
/// AI Mask model set versions that this build can run (plan decision P1). The installation
/// directory `alcedo-ai-mask-v<version>` is what the sidecar reads.
inline constexpr std::uint64_t    kAiMaskModelSetMinimumVersion    = 1;
inline constexpr std::uint64_t    kAiMaskModelSetMaximumVersion    = 1;

/// Files at the root of an installed AI model package.
inline constexpr std::string_view kAiModelPackageReceiptFileName   = "installed.json";
inline constexpr std::string_view kAiModelPackageInventoryFileName = "package-inventory.json";
inline constexpr std::string_view kAiModelSetFileName              = "model_set.json";

/// `installed.json`: the commit record of one installed package. It is written into the
/// verified content before the content is moved into place.
struct AiModelPackageReceipt {
  std::string   package_id;
  std::uint64_t model_set_version = 0;
  std::string   revision;
  std::string   platform;
  std::uint64_t file_count     = 0;
  std::uint64_t unpacked_bytes = 0;
  /// Lowercase hexadecimal SHA-256 of the exact `package-inventory.json` bytes.
  std::string   inventory_sha256;
  std::string   artifact_url;
  std::uint64_t artifact_size = 0;
  std::string   artifact_sha256;
  std::uint64_t feed_sequence = 0;
};

/// `<package id>-v<version>`, the installation directory name of one model set version.
[[nodiscard]] auto AiModelPackageDirectoryName(std::string_view package_id,
                                               std::uint64_t    model_set_version) -> std::string;

/// Serialized `installed.json` of @p receipt, and its parser (nullopt when invalid).
[[nodiscard]] auto SerializeAiModelPackageReceipt(const AiModelPackageReceipt& receipt)
    -> std::string;
[[nodiscard]] auto ParseAiModelPackageReceipt(std::string_view bytes)
    -> std::optional<AiModelPackageReceipt>;

/// One installed model set version found in the model directory.
struct AiModelSetInstallation {
  std::filesystem::path                directory;
  std::uint64_t                        model_set_version = 0;
  /// Present when `installed.json` is valid and names this package and version.
  std::optional<AiModelPackageReceipt> receipt;
  /// Empty when the installation is complete: the receipt, `model_set.json`, and the
  /// inventory agree, and every listed file exists with its listed size. Otherwise the
  /// reason (Repair). No file is hashed.
  std::string                          problem;

  [[nodiscard]] auto                   Complete() const -> bool { return problem.empty(); }
};

/// Installed versions of one package in the model directory, relative to the versions that
/// this build supports. Read from `installed.json`, `model_set.json`, and
/// `package-inventory.json` only.
struct AiModelInstallation {
  /// The highest installed version in the supported range, complete or not.
  std::optional<AiModelSetInstallation> supported;
  /// The highest installed version below the supported range (Update required).
  std::optional<std::uint64_t>          below_minimum_version;
  /// The highest installed version above the supported range (ignored by this build).
  std::optional<std::uint64_t>          above_maximum_version;

  /// True when a complete installation in the supported range exists.
  [[nodiscard]] auto                    Usable() const -> bool {
    return supported.has_value() && supported->Complete();
  }
};

/// Read every `<package id>-v<N>` directory of @p model_directory. Blocks on file metadata
/// reads only; safe on the GUI thread.
[[nodiscard]] auto ReadAiModelInstallation(const std::filesystem::path& model_directory,
                                           std::string_view             package_id,
                                           std::uint64_t                minimum_version,
                                           std::uint64_t maximum_version) -> AiModelInstallation;

/// The AI Mask model set of @p model_directory for this build's supported range. AM5 reads
/// this before the AI Mask tool loads models.
[[nodiscard]] auto ReadAiMaskModelInstallation(const std::filesystem::path& model_directory)
    -> AiModelInstallation;

/// Stage of an installation that runs on the worker thread.
enum class AiModelPackageInstallStage { kVerifying, kInstalling };

/// A downloaded package archive and the signed descriptor fields it must match.
struct AiModelPackageInstallRequest {
  std::filesystem::path    model_directory;
  std::filesystem::path    archive_path;
  /// Fields of the signed feed entry and artifact; written as `installed.json` on success.
  AiModelPackageReceipt    expected;
  /// Package-relative license files that the feed entry names; each must be in the archive.
  std::vector<std::string> license_files;
};

struct AiModelPackageInstallOutcome {
  std::string error;
  bool        canceled  = false;
  /// True once the verified content is in place (the commit point).
  bool        committed = false;
};

/// `.<package id>-v<version>.download`, the staging directory of one version: it holds the
/// downloaded archive and the extracted content before it is moved into place.
[[nodiscard]] auto AiModelPackageStagingDirectory(const std::filesystem::path& model_directory,
                                                  std::string_view             package_id,
                                                  std::uint64_t                model_set_version)
    -> std::filesystem::path;

/// Complete or undo file operations of an install or removal that the process did not
/// finish. Before the commit point the previous installation is restored; uncommitted
/// staging content is removed (the downloaded archive stays for a retry); a previous
/// installation that a committed install replaced is removed. Blocks; returns the problems.
auto RecoverAiModelPackageDirectory(const std::filesystem::path& model_directory,
                                    std::string_view package_id, std::uint64_t model_set_version)
    -> std::vector<std::string>;

/// Verify, extract, and activate one downloaded package archive.
///
/// Order: recovery of an earlier interrupted operation -> archive size and SHA-256 ->
/// extraction into `<staging>/content` with the safe-entry rules of ExtractPackageArchive ->
/// the inventory digest, the inventory against the signed fields and the extracted files,
/// `model_set.json`, and the license files -> `installed.json` -> move the installed
/// directory aside and the new content into `<package id>-v<version>` (the commit point) ->
/// remove the old directory and the staging directory. Before the commit point every failure
/// or cancellation removes the new content and keeps the installed set. A failure of the
/// archive hash removes the archive; a cancellation keeps it. @p on_stage runs on the calling
/// thread. Blocks; must not run on the GUI thread.
[[nodiscard]] auto InstallAiModelPackageArchive(
    const AiModelPackageInstallRequest& request, const std::atomic<bool>& stop,
    const std::function<void(AiModelPackageInstallStage)>& on_stage)
    -> AiModelPackageInstallOutcome;

/// Remove the installed version @p model_set_version of @p package_id and its staging
/// directory. The installation directory is first renamed, so a failure leaves either the
/// complete installation or no installation. Returns an empty text on success.
/// Blocks; must not run on the GUI thread.
[[nodiscard]] auto RemoveAiModelPackage(const std::filesystem::path& model_directory,
                                        std::string_view             package_id,
                                        std::uint64_t model_set_version) -> std::string;

}  // namespace alcedo
