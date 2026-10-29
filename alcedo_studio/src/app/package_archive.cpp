//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/package_archive.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QString>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace alcedo {
namespace {

namespace fs                          = std::filesystem;

constexpr std::size_t kCopyBlockBytes = 256 * 1024;

auto                  ToQString(const fs::path& path) -> QString {
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

auto HasLineBreakOrNul(std::string_view text) -> bool {
  return text.find('\0') != std::string_view::npos || text.find('\n') != std::string_view::npos ||
         text.find('\r') != std::string_view::npos;
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

/// Stream one regular entry into a new file and hash it. The file must not exist.
auto WriteEntry(archive* reader, const fs::path& target, std::uint64_t* budget_left,
                const std::atomic<bool>& stop, PackageArchiveFile* output, bool* canceled)
    -> std::string {
  std::error_code error;
  fs::create_directories(target.parent_path(), error);
  if (error) {
    return "cannot create " + PackagePathToUtf8(target.parent_path()) + ": " + error.message();
  }
  QFile file(ToQString(target));
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    return "cannot create " + PackagePathToUtf8(target) + ": " + file.errorString().toStdString();
  }
  QCryptographicHash                hash(QCryptographicHash::Sha256);
  std::array<char, kCopyBlockBytes> buffer{};
  while (true) {
    if (stop.load()) {
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
      return "cannot write " + PackagePathToUtf8(target) + ": " + file.errorString().toStdString();
    }
    hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(read)));
    output->size += count;
  }
  if (!file.flush()) return "cannot write " + PackagePathToUtf8(target);
  file.close();
  output->sha256 = hash.result().toHex().toStdString();
  return {};
}

}  // namespace

auto PackagePathToUtf8(const fs::path& path) -> std::string {
  const std::u8string text = path.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

auto PackagePathFromUtf8(std::string_view text) -> fs::path {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

auto IsSafePackageRelativePath(std::string_view path) -> bool {
  if (path.empty() || HasLineBreakOrNul(path) || path.find('\\') != std::string_view::npos ||
      path.front() == '/' || path.find(':') != std::string_view::npos) {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    std::size_t end = path.find('/', start);
    if (end == std::string_view::npos) end = path.size();
    const std::string_view segment = path.substr(start, end - start);
    if (segment.empty() || segment == "." || segment == "..") return false;
    start = end + 1;
  }
  return true;
}

auto HashPackageFile(const fs::path& path) -> std::optional<PackageArchiveFile> {
  QFile input(ToQString(path));
  if (!input.open(QIODevice::ReadOnly)) return std::nullopt;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&input) || input.error() != QFileDevice::NoError) return std::nullopt;
  return PackageArchiveFile{static_cast<std::uint64_t>(input.size()),
                            hash.result().toHex().toStdString()};
}

auto VerifyPackageArchiveFile(const fs::path& archive_path, std::uint64_t size,
                              std::string_view sha256) -> std::string {
  const std::optional<PackageArchiveFile> hash = HashPackageFile(archive_path);
  if (!hash) return "the downloaded package archive cannot be read";
  if (hash->size != size || hash->sha256 != sha256) {
    return "the downloaded package archive does not match its signed size and SHA-256";
  }
  return {};
}

auto ExtractPackageArchive(const fs::path& archive_path, const fs::path& destination,
                           const PackageArchiveLimits& limits, const std::atomic<bool>& stop)
    -> PackageArchiveExtraction {
  PackageArchiveExtraction result;
  std::error_code          error;
  if (fs::exists(destination, error) || !fs::create_directories(destination, error)) {
    result.error =
        "the package content directory cannot be created: " + PackagePathToUtf8(destination);
    return result;
  }
  std::unique_ptr<archive, ArchiveDeleter> reader = OpenArchive(archive_path, &result.error);
  if (!reader) return result;

  std::set<std::string> folded_names;
  std::uint64_t         budget_left = limits.maximum_bytes;
  archive_entry*        entry       = nullptr;
  while (true) {
    if (stop.load()) {
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
    if (!IsSafePackageRelativePath(name)) {
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
    if (folded_names.size() > limits.maximum_files) {
      result.error = "the package archive holds more files than its descriptor";
      return result;
    }
    PackageArchiveFile extracted;
    std::string write_error = WriteEntry(reader.get(), destination / PackagePathFromUtf8(name),
                                         &budget_left, stop, &extracted, &result.canceled);
    if (!write_error.empty()) {
      result.error = std::move(write_error);
      return result;
    }
    result.files.emplace(name, std::move(extracted));
  }
  return result;
}

auto VerifyExtractedPackageFiles(const std::vector<PackageInventoryFile>&         listed,
                                 const std::map<std::string, PackageArchiveFile>& files,
                                 std::string_view inventory_file_name) -> std::string {
  for (const PackageInventoryFile& record : listed) {
    const auto found = files.find(record.path);
    if (found == files.end()) return "the package archive does not contain " + record.path;
    if (found->second.size != record.size || found->second.sha256 != record.sha256) {
      return "an extracted file differs from the package inventory: " + record.path;
    }
  }
  // The inventory file itself is the one extracted file that is not listed.
  const std::size_t inventory_count = files.contains(std::string(inventory_file_name)) ? 1 : 0;
  if (files.size() != listed.size() + inventory_count) {
    return "the package archive contains unlisted files";
  }
  return {};
}

auto ReadSmallPackageFile(const fs::path& path, std::uint64_t limit, std::string* bytes) -> bool {
  QFile input(ToQString(path));
  if (!input.open(QIODevice::ReadOnly) || static_cast<std::uint64_t>(input.size()) > limit) {
    return false;
  }
  const QByteArray data = input.readAll();
  bytes->assign(data.constData(), static_cast<std::size_t>(data.size()));
  return true;
}

auto RenamePackageDirectory(const fs::path& from, const fs::path& to,
                            std::chrono::milliseconds wait_limit) -> std::error_code {
  std::error_code error;
  fs::rename(from, to, error);
#if defined(_WIN32)
  // The STL reports the Win32 error of MoveFileExW in the system category.
  const auto transient = [](const std::error_code& value) {
    if (value.category() != std::system_category()) return false;
    const int code = value.value();
    return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ||
           code == ERROR_LOCK_VIOLATION;
  };
  const auto                deadline = std::chrono::steady_clock::now() + wait_limit;
  std::chrono::milliseconds delay{25};
  while (error && transient(error) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(delay);
    delay = (std::min)(delay * 2, std::chrono::milliseconds{500});  // Windows.h defines min.
    error.clear();
    fs::rename(from, to, error);
  }
#else
  (void)wait_limit;
#endif
  return error;
}

}  // namespace alcedo
