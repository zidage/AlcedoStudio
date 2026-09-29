//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_package_install.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <QCryptographicHash>
#include <QFile>
#include <QString>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <system_error>
#include <utility>

#include "utils/lut/lut_metadata.hpp"

namespace alcedo {
namespace {

namespace fs                                      = std::filesystem;

/// Upper bound of `package-inventory.json`; it is not part of `unpacked_bytes`.
constexpr std::uint64_t kMaxPackageInventoryBytes = 16ULL * 1024ULL * 1024ULL;
/// Upper bound of auxiliary files (licenses, attribution) per package.
constexpr std::uint64_t kMaxAuxiliaryFiles        = 256;
constexpr std::size_t   kCopyBlockBytes           = 256 * 1024;

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

auto ToHex(const QByteArray& bytes) -> std::string { return bytes.toHex().toStdString(); }

auto PackageDirectory(std::string_view package_id) -> std::string {
  return std::string(kLutPackagesDirectoryName) + "/" + std::string(package_id);
}

auto ContentParent(std::string_view package_id) -> std::string {
  return PackageDirectory(package_id) + "/" + std::string(kLutPackageContentDirectoryName);
}

struct ArchiveDeleter {
  void operator()(archive* handle) const { archive_read_free(handle); }
};

auto OpenArchive(const fs::path& path, std::string* error)
    -> std::unique_ptr<archive, ArchiveDeleter> {
  std::unique_ptr<archive, ArchiveDeleter> reader(archive_read_new());
  if (!reader) {
    *error = "the archive reader cannot be created";
    return nullptr;
  }
  // Official packages are 7z archives (LZMA2); no other format is accepted.
  archive_read_support_format_7zip(reader.get());
#ifdef _WIN32
  const int opened = archive_read_open_filename_w(reader.get(), path.wstring().c_str(), 64 * 1024);
#else
  const int opened = archive_read_open_filename(reader.get(), path.c_str(), 64 * 1024);
#endif
  if (opened != ARCHIVE_OK) {
    const char* message = archive_error_string(reader.get());
    *error = "the package archive cannot be opened: " + std::string(message ? message : "");
    return nullptr;
  }
  return reader;
}

auto ArchiveError(archive* reader, std::string_view prefix) -> std::string {
  const char* message = archive_error_string(reader);
  return std::string(prefix) + ": " + (message != nullptr ? message : "unknown archive error");
}

auto EntryName(archive_entry* entry) -> std::string {
  const char* name = archive_entry_pathname_utf8(entry);
  if (name == nullptr) name = archive_entry_pathname(entry);
  std::string text = name != nullptr ? name : "";
  while (!text.empty() && text.back() == '/') text.pop_back();
  return text;
}

struct ExtractedFile {
  std::uint64_t size = 0;
  std::string   sha256;
};

/// Stream one regular entry into a new file and hash it. The file must not exist.
auto WriteEntry(archive* reader, const fs::path& target, std::uint64_t* budget_left,
                std::stop_token stop, ExtractedFile* output, bool* canceled) -> std::string {
  std::error_code error;
  fs::create_directories(target.parent_path(), error);
  if (error) return "cannot create " + LutPathToUtf8(target.parent_path()) + ": " + error.message();
  QFile file(ToQString(target));
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    return "cannot create " + LutPathToUtf8(target) + ": " + file.errorString().toStdString();
  }
  QCryptographicHash                hash(QCryptographicHash::Sha256);
  std::array<char, kCopyBlockBytes> buffer{};
  while (true) {
    if (stop.stop_requested()) {
      *canceled = true;
      return "the installation was canceled";
    }
    const la_ssize_t read = archive_read_data(reader, buffer.data(), buffer.size());
    if (read == 0) break;
    if (read < 0) return ArchiveError(reader, "the package archive cannot be read");
    const auto count = static_cast<std::uint64_t>(read);
    if (count > *budget_left) return "the package archive holds more bytes than its descriptor";
    *budget_left -= count;
    if (file.write(buffer.data(), read) != read) {
      return "cannot write " + LutPathToUtf8(target) + ": " + file.errorString().toStdString();
    }
    hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(read)));
    output->size += count;
  }
  if (!file.flush()) return "cannot write " + LutPathToUtf8(target);
  file.close();
  output->sha256 = ToHex(hash.result());
  return {};
}

auto ReadSmallFile(const fs::path& path, std::uint64_t limit, std::string* bytes) -> bool {
  QFile input(ToQString(path));
  if (!input.open(QIODevice::ReadOnly) || static_cast<std::uint64_t>(input.size()) > limit) {
    return false;
  }
  const QByteArray data = input.readAll();
  bytes->assign(data.constData(), static_cast<std::size_t>(data.size()));
  return true;
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
                            const std::map<std::string, ExtractedFile>& files) -> std::string {
  if (inventory.package_id != expected.package_id) {
    return "the archive inventory names another package";
  }
  if (inventory.revision != expected.revision || inventory.file_count != expected.file_count ||
      inventory.inventory_sha256 != expected.inventory_sha256 ||
      inventory.unpacked_bytes != expected.unpacked_bytes) {
    return "the archive inventory differs from the signed package descriptor";
  }
  std::size_t listed = 0;
  const auto  check  = [&](const std::string& path, std::uint64_t size,
                         const std::string& sha256) -> std::string {
    ++listed;
    const auto found = files.find(path);
    if (found == files.end()) return "the package archive does not contain " + path;
    if (found->second.size != size || found->second.sha256 != sha256) {
      return "an extracted file differs from the package inventory: " + path;
    }
    return {};
  };
  for (const LutInventoryRecord& record : inventory.luts) {
    if (std::string error = check(record.relative_path, record.size, record.sha256);
        !error.empty()) {
      return error;
    }
  }
  for (const LutAuxiliaryFileRecord& record : inventory.auxiliary_files) {
    if (std::string error = check(record.relative_path, record.size, record.sha256);
        !error.empty()) {
      return error;
    }
  }
  // The inventory file itself is the one extracted file that is not listed.
  if (files.size() != listed + 1) return "the package archive contains unlisted files";
  return {};
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
                              const LutPackageReceipt& expected, std::stop_token stop)
    -> LutPackageExtraction {
  LutPackageExtraction result;
  std::error_code      error;
  if (fs::exists(destination, error) || !fs::create_directories(destination, error)) {
    result.error = "the package content directory cannot be created: " + LutPathToUtf8(destination);
    return result;
  }
  std::unique_ptr<archive, ArchiveDeleter> reader = OpenArchive(archive_path, &result.error);
  if (!reader) return result;

  std::map<std::string, ExtractedFile> files;
  std::set<std::string>                folded_names;
  std::uint64_t       budget_left = expected.unpacked_bytes + kMaxPackageInventoryBytes;
  const std::uint64_t max_files   = expected.file_count + kMaxAuxiliaryFiles + 1;
  archive_entry*      entry       = nullptr;
  while (true) {
    if (stop.stop_requested()) {
      result.canceled = true;
      result.error    = "the installation was canceled";
      return result;
    }
    const int status = archive_read_next_header(reader.get(), &entry);
    if (status == ARCHIVE_EOF) break;
    if (status != ARCHIVE_OK) {
      result.error = ArchiveError(reader.get(), "the package archive cannot be read");
      return result;
    }
    const std::string name = EntryName(entry);
    if (!IsSafeLutRelativePath(name)) {
      result.error = "the package archive contains an unsafe path: " + name;
      return result;
    }
    const auto type = archive_entry_filetype(entry);
    if (archive_entry_hardlink(entry) != nullptr || archive_entry_symlink(entry) != nullptr ||
        (type != AE_IFREG && type != AE_IFDIR)) {
      result.error = "the package archive contains a link or special file: " + name;
      return result;
    }
    if (type == AE_IFDIR) continue;  // Directories are created for the files they hold.
    if (!folded_names.insert(AsciiLower(name)).second) {
      result.error = "the package archive contains a duplicate destination: " + name;
      return result;
    }
    if (folded_names.size() > max_files) {
      result.error = "the package archive holds more files than its descriptor";
      return result;
    }
    ExtractedFile extracted;
    std::string   write_error = WriteEntry(reader.get(), destination / LutPathFromUtf8(name),
                                           &budget_left, stop, &extracted, &result.canceled);
    if (!write_error.empty()) {
      result.error = std::move(write_error);
      return result;
    }
    files.emplace(name, std::move(extracted));
  }

  std::string inventory_bytes;
  if (!files.contains(std::string(kLutPackageInventoryFileName)) ||
      !ReadSmallFile(destination / LutPathFromUtf8(kLutPackageInventoryFileName),
                     kMaxPackageInventoryBytes, &inventory_bytes)) {
    result.error = "the package archive has no readable package-inventory.json";
    return result;
  }
  LutPackageInventoryParseResult inventory = ParseLutPackageInventory(inventory_bytes);
  if (!inventory) {
    result.error = "the package inventory is invalid: " + inventory.error;
    return result;
  }
  if (std::string mismatch = VerifyExtractedContent(*inventory.inventory, expected, files);
      !mismatch.empty()) {
    result.error = std::move(mismatch);
    return result;
  }
  result.inventory = std::move(inventory.inventory);
  return result;
}

auto InstallLutPackageArchive(const fs::path& root, const LutPackageInstallRequest& request,
                              const LutPackageInstallSteps& steps, std::stop_token stop,
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
  const std::optional<LutFileHash> archive_hash = HashLutFile(request.archive_path);
  if (!archive_hash) {
    outcome.error = "the downloaded package archive cannot be read";
    return outcome;
  }
  if (archive_hash->size != expected.artifact_size ||
      archive_hash->sha256 != expected.artifact_sha256) {
    outcome.error = "the downloaded package archive does not match its signed size and SHA-256";
    return outcome;
  }
  if (stop.stop_requested()) {
    outcome.canceled = true;
    outcome.error    = "the installation was canceled";
    return outcome;
  }

  // 2. Remove content left by an interrupted installation before adding more.
  LutPackageContentRetirement earlier = RetireInactiveLutPackageContent(root);
  outcome.relocated_user_paths        = std::move(earlier.relocated_user_paths);

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
  if (stop.stop_requested()) return abandon("the installation was canceled", true);

  // 5. Replacing the receipt is the persistent commit point.
  LutPackageReceipt receipt = expected;
  receipt.content_directory = content_directory;
  if (std::string error = steps.write_receipt(root, receipt); !error.empty()) {
    return abandon("the package receipt cannot be saved: " + error, false);
  }
  outcome.committed                   = true;
  outcome.content_directory           = content_directory;

  // 6. Retire the previous content (and relocate explicitly reclassified user files).
  LutPackageContentRetirement retired = RetireInactiveLutPackageContent(root);
  outcome.relocated_user_paths.insert(outcome.relocated_user_paths.end(),
                                      retired.relocated_user_paths.begin(),
                                      retired.relocated_user_paths.end());
  outcome.retirement_problems = std::move(retired.problems);
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
  }
  return result;
}

}  // namespace alcedo
