//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "app/lut_library_inventory.hpp"
#include "utils/lut/lut_inventory_digest.hpp"

namespace alcedo {

/// Name of the directory under `packages/<id>/` that holds content directories.
inline constexpr std::string_view kLutPackageContentDirectoryName  = "content";
/// Files written beside the extracted content: the exact signed feed bytes and
/// their detached signature, kept for offline repair of the receipt.
inline constexpr std::string_view kLutPackageFeedEvidenceFileName  = "feed-manifest.json";
inline constexpr std::string_view kLutPackageFeedSignatureFileName = "feed-manifest.json.sig";
/// `package-inventory.json` inside every package archive.
inline constexpr std::string_view kLutPackageInventoryFileName     = "package-inventory.json";

/// Stage of an installation that runs on the library worker thread.
enum class LutPackageInstallStage { kVerifying, kInstalling };

/// A downloaded package archive and the verified feed descriptor it must match.
///
/// `expected` carries the descriptor fields of the signed feed (package ID,
/// revision, count, inventory digest, unpacked size, artifact size and hash, and
/// feed sequence); its `content_directory` is ignored and chosen by the
/// installation. The request is a value handed to the library worker; it is not
/// a copy of library state.
struct LutPackageInstallRequest {
  std::filesystem::path archive_path;
  LutPackageReceipt     expected;
  /// Exact signed feed bytes and signature text, stored with the content.
  std::string           feed_manifest;
  std::string           feed_signature;
};

/// Result of InstallLutPackageArchive.
struct LutPackageInstallOutcome {
  std::string              error;
  bool                     canceled  = false;
  /// True once the new receipt replaced the previous one (the commit point).
  bool                     committed = false;
  /// Root-relative path of the new active content directory.
  std::string              content_directory;
  /// Root-relative paths of user-declared files moved out of content left by an earlier
  /// interrupted installation.
  std::vector<std::string> relocated_user_paths;
};

/// Steps with real file-system effects that a test can replace.
struct LutPackageInstallSteps {
  std::function<std::string(const std::filesystem::path& root, const LutPackageReceipt&)>
      write_receipt = WriteLutPackageReceiptFile;
  /// Lock held while inactive package content is removed, so no LUT resource read sees
  /// a file disappear. The library passes LutLibraryPublication::LockContentForRemoval;
  /// empty means no concurrent reader exists.
  std::function<std::unique_lock<std::shared_mutex>()> lock_content_for_removal;
};

/// Result of reading and verifying an archive's entries against its descriptor.
struct LutPackageExtraction {
  std::string                        error;
  bool                               canceled = false;
  /// The archive's `package-inventory.json`, checked against the descriptor.
  std::optional<LutPackageInventory> inventory;
};

/// Extract the 7z archive at @p archive_path into the new directory
/// @p destination and verify it against @p expected.
///
/// Accepts only regular files and directories with safe relative paths; rejects
/// links, devices, absolute paths, `..`, duplicate or case-colliding
/// destinations, and entries beyond the signed byte and file limits while
/// streaming. Afterwards the archive's inventory must match the descriptor and
/// list exactly the extracted files with matching sizes and SHA-256 values.
/// @p destination must not exist; it is created. On failure the caller removes it.
/// Blocks; must not run on the GUI thread. Stops early when @p stop is requested.
[[nodiscard]] auto ExtractLutPackageArchive(const std::filesystem::path& archive_path,
                                            const std::filesystem::path& destination,
                                            const LutPackageReceipt&     expected,
                                            const std::atomic<bool>& stop) -> LutPackageExtraction;

/// Verify, extract, and activate one downloaded package in the library at @p root.
///
/// Order: archive size and SHA-256 check -> retire content left by an earlier
/// interrupted installation (under steps.lock_content_for_removal) -> extract into a
/// new `packages/<id>/content/<name>` directory -> verify -> store the feed evidence ->
/// replace the receipt (the commit point). Before the commit point every failure or
/// cancellation removes the new directory and keeps the previous receipt and content
/// active. The previous content stays in place after the commit: renders may still
/// resolve to it until the caller publishes the new inventory, so the caller retires it
/// afterwards with RetireInactiveLutPackageContent under its content lock.
/// The other packages and loose user files are never touched. @p on_stage runs on
/// the calling thread. Blocks; must not run on the GUI thread.
[[nodiscard]] auto InstallLutPackageArchive(
    const std::filesystem::path& root, const LutPackageInstallRequest& request,
    const LutPackageInstallSteps& steps, const std::atomic<bool>& stop,
    const std::function<void(LutPackageInstallStage)>& on_stage) -> LutPackageInstallOutcome;

struct LutPackageContentRetirement {
  std::vector<std::string> relocated_user_paths;
  std::vector<std::string> problems;
};

/// Remove every content directory of every package that its receipt does not
/// name as active, including directories left by an interrupted installation.
///
/// A `.cube` file in a retired directory that explicitly declares `origin: user`
/// is moved to `user/<package-id>/` first (keeping its relative path, with a
/// numeric suffix on a name conflict) so that its bytes survive as a user entry.
/// Files that still declare `origin: alcedo` are removed with their directory;
/// no user copy is made. A package whose receipt cannot be read is left
/// unchanged. Blocks; must not run on the GUI thread.
[[nodiscard]] auto RetireInactiveLutPackageContent(const std::filesystem::path& root)
    -> LutPackageContentRetirement;

}  // namespace alcedo
