//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_package_install.hpp"

#include <QFile>
#include <QString>
#include <algorithm>
#include <atomic>
#include <map>
#include <system_error>
#include <utility>

#include "app/package_archive.hpp"
#include "utils/lut/lut_metadata.hpp"

namespace alcedo {
namespace {

namespace fs                                      = std::filesystem;

/// Upper bound of `package-inventory.json`; it is not part of `unpacked_bytes`.
constexpr std::uint64_t kMaxPackageInventoryBytes = 16ULL * 1024ULL * 1024ULL;
/// Upper bound of auxiliary files (licenses, attribution) per package.
constexpr std::uint64_t kMaxAuxiliaryFiles        = 256;

auto                    ToQString(const fs::path& path) -> QString {
  const std::u8string text = path.u8string();
  return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                                              static_cast<qsizetype>(text.size()));
}

auto AsciiLower(std::string text) -> std::string {
  for (char& value : text) {
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
  }
  return text;
}

auto PackageDirectory(std::string_view package_id) -> std::string {
  return std::string(kLutPackagesDirectoryName) + "/" + std::string(package_id);
}

auto ContentParent(std::string_view package_id) -> std::string {
  return PackageDirectory(package_id) + "/" + std::string(kLutPackageContentDirectoryName);
}

auto WriteNewFile(const fs::path& path, std::string_view bytes) -> std::string {
  QFile output(ToQString(path));
  if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      output.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
          static_cast<qint64>(bytes.size()) ||
      !output.flush()) {
    return "cannot write " + LutPathToUtf8(path) + ": " + output.errorString().toStdString();
  }
  return {};
}

/// Compare the archive inventory with the signed descriptor and the extracted files.
auto VerifyExtractedContent(const LutPackageInventory& inventory, const LutPackageReceipt& expected,
                            const std::map<std::string, PackageArchiveFile>& files) -> std::string {
  if (inventory.package_id != expected.package_id) {
    return "the archive inventory names another package";
  }
  if (inventory.revision != expected.revision || inventory.file_count != expected.file_count ||
      inventory.inventory_sha256 != expected.inventory_sha256 ||
      inventory.unpacked_bytes != expected.unpacked_bytes) {
    return "the archive inventory differs from the signed package descriptor";
  }
  std::vector<PackageInventoryFile> listed;
  listed.reserve(inventory.luts.size() + inventory.auxiliary_files.size());
  for (const LutInventoryRecord& record : inventory.luts) {
    listed.push_back({record.relative_path, record.size, record.sha256});
  }
  for (const LutAuxiliaryFileRecord& record : inventory.auxiliary_files) {
    listed.push_back({record.relative_path, record.size, record.sha256});
  }
  return VerifyExtractedPackageFiles(listed, files, kLutPackageInventoryFileName);
}

/// Pick a new, unused content directory name for @p inventory_sha256.
auto NewContentDirectory(const fs::path& root, const LutPackageReceipt& expected) -> std::string {
  const std::string parent = ContentParent(expected.package_id);
  std::string       name   = parent + "/" + expected.inventory_sha256;
  std::error_code   error;
  for (int suffix = 2; fs::exists(root / LutPathFromUtf8(name), error); ++suffix) {
    name = parent + "/" + expected.inventory_sha256 + "-" + std::to_string(suffix);
  }
  return name;
}

/// Move a user-declared file out of retired content into `user/<package>/`.
auto RelocateUserFile(const fs::path& root, std::string_view package_id, const fs::path& source,
                      const std::string& path_in_content, std::string* relocated) -> std::string {
  const std::string base = std::string(kLutUserImportDirectoryName) + "/" +
                           std::string(package_id) + "/" + path_in_content;
  std::string     target = base;
  std::error_code error;
  for (int suffix = 2; fs::exists(root / LutPathFromUtf8(target), error); ++suffix) {
    const fs::path base_path = LutPathFromUtf8(base);
    target = LutPathToUtf8(base_path.parent_path() / (base_path.stem().u8string() + u8"-" +
                                                      fs::path(std::to_string(suffix)).u8string() +
                                                      base_path.extension().u8string()));
  }
  const fs::path destination = root / LutPathFromUtf8(target);
  fs::create_directories(destination.parent_path(), error);
  if (!error) fs::rename(source, destination, error);
  if (error) return "cannot move user LUT " + LutPathToUtf8(source) + ": " + error.message();
  *relocated = target;
  return {};
}

void RetireContentDirectory(const fs::path& root, std::string_view package_id,
                            const fs::path& directory, LutPackageContentRetirement* result) {
  std::error_code error;
  if (fs::is_symlink(directory, error) || !fs::is_directory(directory, error)) {
    // A link or stray file is removed itself; a link target is never entered.
    fs::remove(directory, error);
    if (error) {
      result->problems.push_back("cannot remove " + LutPathToUtf8(directory) + ": " +
                                 error.message());
    }
    return;
  }
  // Preserve explicitly reclassified user files before removing the directory.
  fs::recursive_directory_iterator iterator(directory, error), end;
  std::vector<fs::path>            user_files;
  for (; !error && iterator != end; iterator.increment(error)) {
    std::error_code status_error;
    if (iterator->is_symlink(status_error)) {
      iterator.disable_recursion_pending();
      continue;
    }
    if (!iterator->is_regular_file(status_error)) continue;
    if (AsciiLower(LutPathToUtf8(iterator->path().extension())) != ".cube") continue;
    const LutHeaderReadResult header = ReadLutHeaderFile(iterator->path());
    if (header.error == LutHeaderError::kNone && header.header.Origin() == LutOrigin::kUser &&
        header.header.metadata.has_value()) {
      user_files.push_back(iterator->path());
    }
  }
  if (error) {
    result->problems.push_back("cannot read " + LutPathToUtf8(directory) + ": " + error.message());
    return;
  }
  for (const fs::path& file : user_files) {
    std::string relocated;
    std::string problem = RelocateUserFile(
        root, package_id, file, LutPathToUtf8(file.lexically_relative(directory)), &relocated);
    if (!problem.empty()) {
      // Keep the whole directory; removing it would lose the user's bytes.
      result->problems.push_back(std::move(problem));
      return;
    }
    result->relocated_user_paths.push_back(std::move(relocated));
  }
  fs::remove_all(directory, error);
  if (error) {
    result->problems.push_back("cannot remove " + LutPathToUtf8(directory) + ": " +
                               error.message());
  }
}

}  // namespace

auto ExtractLutPackageArchive(const fs::path& archive_path, const fs::path& destination,
                              const LutPackageReceipt& expected, const std::atomic<bool>& stop)
    -> LutPackageExtraction {
  LutPackageExtraction           result;
  const PackageArchiveLimits     limits{expected.unpacked_bytes + kMaxPackageInventoryBytes,
                                    expected.file_count + kMaxAuxiliaryFiles + 1};
  const PackageArchiveExtraction extraction =
      ExtractPackageArchive(archive_path, destination, limits, stop);
  if (!extraction.error.empty()) {
    result.error    = extraction.error;
    result.canceled = extraction.canceled;
    return result;
  }

  std::string inventory_bytes;
  if (!extraction.files.contains(std::string(kLutPackageInventoryFileName)) ||
      !ReadSmallPackageFile(destination / LutPathFromUtf8(kLutPackageInventoryFileName),
                            kMaxPackageInventoryBytes, &inventory_bytes)) {
    result.error = "the package archive has no readable package-inventory.json";
    return result;
  }
  LutPackageInventoryParseResult inventory = ParseLutPackageInventory(inventory_bytes);
  if (!inventory) {
    result.error = "the package inventory is invalid: " + inventory.error;
    return result;
  }
  if (std::string mismatch =
          VerifyExtractedContent(*inventory.inventory, expected, extraction.files);
      !mismatch.empty()) {
    result.error = std::move(mismatch);
    return result;
  }
  result.inventory = std::move(inventory.inventory);
  return result;
}

auto InstallLutPackageArchive(const fs::path& root, const LutPackageInstallRequest& request,
                              const LutPackageInstallSteps& steps, const std::atomic<bool>& stop,
                              const std::function<void(LutPackageInstallStage)>& on_stage)
    -> LutPackageInstallOutcome {
  LutPackageInstallOutcome outcome;
  const LutPackageReceipt& expected = request.expected;
  const auto               stage    = [&](LutPackageInstallStage value) {
    if (on_stage) on_stage(value);
  };
  if (!IsLutPackageId(expected.package_id) || expected.inventory_sha256.size() != 64 ||
      expected.artifact_sha256.size() != 64) {
    outcome.error = "the package descriptor is not valid";
    return outcome;
  }

  // 1. The exact archive bytes must match the signed size and SHA-256.
  stage(LutPackageInstallStage::kVerifying);
  if (std::string mismatch = VerifyPackageArchiveFile(request.archive_path, expected.artifact_size,
                                                      expected.artifact_sha256);
      !mismatch.empty()) {
    outcome.error = std::move(mismatch);
    return outcome;
  }
  if (stop.load()) {
    outcome.canceled = true;
    outcome.error    = "the installation was canceled";
    return outcome;
  }

  // 2. Remove content left by an interrupted installation before adding more.
  {
    std::unique_lock<std::shared_mutex> removal;
    if (steps.lock_content_for_removal) removal = steps.lock_content_for_removal();
    LutPackageContentRetirement earlier = RetireInactiveLutPackageContent(root);
    outcome.relocated_user_paths        = std::move(earlier.relocated_user_paths);
  }

  // 3. Extract and verify into a new, inactive content directory.
  stage(LutPackageInstallStage::kInstalling);
  const std::string content_directory = NewContentDirectory(root, expected);
  const fs::path    content_path      = root / LutPathFromUtf8(content_directory);
  const auto        abandon           = [&](std::string error, bool canceled) {
    std::error_code ignored;
    fs::remove_all(content_path, ignored);
    outcome.error    = std::move(error);
    outcome.canceled = canceled;
    return outcome;
  };
  LutPackageExtraction extraction =
      ExtractLutPackageArchive(request.archive_path, content_path, expected, stop);
  if (!extraction.error.empty()) return abandon(extraction.error, extraction.canceled);

  // 4. Keep the signed feed bytes with the content for offline repair.
  for (const auto& [name, bytes] :
       {std::pair{kLutPackageFeedEvidenceFileName, std::string_view(request.feed_manifest)},
        std::pair{kLutPackageFeedSignatureFileName, std::string_view(request.feed_signature)}}) {
    if (std::string error = WriteNewFile(content_path / LutPathFromUtf8(name), bytes);
        !error.empty()) {
      return abandon(std::move(error), false);
    }
  }
  // Last cancellation point. Activation completes as one operation once started.
  if (stop.load()) return abandon("the installation was canceled", true);

  // 5. Replacing the receipt is the persistent commit point.
  LutPackageReceipt receipt = expected;
  receipt.content_directory = content_directory;
  if (std::string error = steps.write_receipt(root, receipt); !error.empty()) {
    return abandon("the package receipt cannot be saved: " + error, false);
  }
  outcome.committed         = true;
  outcome.content_directory = content_directory;
  return outcome;
}

auto RetireInactiveLutPackageContent(const fs::path& root) -> LutPackageContentRetirement {
  LutPackageContentRetirement    result;
  std::vector<LutPackageReceipt> receipts;
  std::vector<LutScanDiagnostic> diagnostics;
  ReadLutPackageReceipts(root, &receipts, &diagnostics);

  const fs::path  packages = root / LutPathFromUtf8(kLutPackagesDirectoryName);
  std::error_code error;
  if (!fs::is_directory(packages, error)) return result;
  for (fs::directory_iterator package(packages, error), end; !error && package != end;
       package.increment(error)) {
    std::error_code status_error;
    if (!package->is_directory(status_error) || package->is_symlink(status_error)) continue;
    const std::string package_id = LutPathToUtf8(package->path().filename());
    if (!IsLutPackageId(package_id)) continue;
    const std::string receipt_path =
        PackageDirectory(package_id) + "/" + std::string(kLutPackageReceiptFileName);
    // An unreadable receipt gives no basis to decide which content is active.
    if (std::any_of(diagnostics.begin(), diagnostics.end(), [&](const LutScanDiagnostic& item) {
          return item.relative_path == receipt_path;
        })) {
      continue;
    }
    const auto     receipt = std::find_if(receipts.begin(), receipts.end(), [&](const auto& item) {
      return item.package_id == package_id;
    });
    const fs::path content_parent = root / LutPathFromUtf8(ContentParent(package_id));
    if (!fs::is_directory(content_parent, status_error)) continue;
    std::vector<fs::path> retired;
    for (fs::directory_iterator content(content_parent, status_error), content_end;
         !status_error && content != content_end; content.increment(status_error)) {
      const std::string relative = LutPathToUtf8(content->path().lexically_relative(root));
      if (receipt != receipts.end() && relative == receipt->content_directory) continue;
      retired.push_back(content->path());
    }
    for (const fs::path& directory : retired) {
      RetireContentDirectory(root, package_id, directory, &result);
    }
    // A removed package leaves no receipt: drop its emptied folders. fs::remove keeps a
    // directory that still holds files.
    if (receipt == receipts.end()) {
      std::error_code ignored;
      fs::remove(content_parent, ignored);
      fs::remove(package->path(), ignored);
    }
  }
  return result;
}

}  // namespace alcedo
