//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/lut_catalog.hpp"

#include <QCoreApplication>
#include <QStringList>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <string_view>
#include <system_error>

#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui::lut_catalog {
namespace {

auto PathToUtf8(const std::filesystem::path& path) -> std::string {
  const auto utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

auto Utf8OrNativeToPath(const std::string& raw_path) -> std::filesystem::path {
  if (raw_path.empty()) {
    return {};
  }
  try {
    const auto* begin = reinterpret_cast<const char8_t*>(raw_path.data());
    return std::filesystem::path(std::u8string(begin, begin + raw_path.size()));
  } catch (...) {
    // Backward compatibility for older projects that persisted native narrow paths.
    return std::filesystem::path(raw_path);
  }
}

auto PathFilenameToQString(const std::filesystem::path& path) -> QString {
  return QString::fromStdWString(path.filename().wstring());
}

struct CachedCatalogState {
  std::filesystem::path directory_{};
  bool                  initialized_ = false;
  LutCatalog            catalog_{};
};

auto CatalogCache() -> CachedCatalogState& {
  static CachedCatalogState cache;
  return cache;
}

auto CatalogCacheMutex() -> std::mutex& {
  static std::mutex mutex;
  return mutex;
}

struct LutHeaderInfo {
  int     size1d_ = 0;
  int     edge3d_ = 0;
  bool    valid_  = true;
  QString error_{};
};

auto LowercaseExtension(const std::filesystem::path& path) -> std::wstring {
  std::wstring extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
  return extension;
}

auto ListCubeLutFiles(const std::filesystem::path& directory)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> files;
  std::error_code                    ec;
  if (!std::filesystem::exists(directory, ec) || ec ||
      !std::filesystem::is_directory(directory, ec) || ec) {
    return files;
  }

  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    if (LowercaseExtension(entry.path()) == L".cube") {
      files.push_back(entry.path());
    }
  }

  std::sort(files.begin(), files.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              return a.filename().wstring() < b.filename().wstring();
            });
  return files;
}

auto TrimAscii(std::string_view text) -> std::string_view {
  size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                                 text[begin] == '\n')) {
    ++begin;
  }

  size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                         text[end - 1] == '\n')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

auto StartsWithToken(std::string_view line, std::string_view token) -> bool {
  if (line.size() < token.size() || line.compare(0, token.size(), token) != 0) {
    return false;
  }
  if (line.size() == token.size()) {
    return true;
  }
  const char separator = line[token.size()];
  return separator == ' ' || separator == '\t';
}

auto ParsePositiveInt(std::string_view text, int* value) -> bool {
  text = TrimAscii(text);
  if (text.empty()) {
    return false;
  }

  int parsed = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + (c - '0');
  }
  if (parsed <= 0) {
    return false;
  }
  *value = parsed;
  return true;
}

auto ParseCubeHeaderInfo(const std::filesystem::path& path) -> LutHeaderInfo {
  LutHeaderInfo info;

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    info.valid_ = false;
    info.error_ = Tr("Failed to open file");
    return info;
  }

  std::string line;
  while (std::getline(input, line)) {
    std::string_view trimmed = TrimAscii(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }

    if (StartsWithToken(trimmed, "LUT_1D_SIZE")) {
      int value = 0;
      if (!ParsePositiveInt(trimmed.substr(std::string_view("LUT_1D_SIZE").size()), &value)) {
        info.valid_ = false;
        info.error_ = Tr("Malformed LUT_1D_SIZE");
        return info;
      }
      info.size1d_ = value;
      continue;
    }

    if (StartsWithToken(trimmed, "LUT_3D_SIZE")) {
      int value = 0;
      if (!ParsePositiveInt(trimmed.substr(std::string_view("LUT_3D_SIZE").size()), &value)) {
        info.valid_ = false;
        info.error_ = Tr("Malformed LUT_3D_SIZE");
        return info;
      }
      info.edge3d_ = value;
      continue;
    }

    if (StartsWithToken(trimmed, "TITLE") || StartsWithToken(trimmed, "DOMAIN_MIN") ||
        StartsWithToken(trimmed, "DOMAIN_MAX")) {
      continue;
    }

    // Stop at first data line or unsupported directive. UI metadata only needs the header.
    break;
  }

  if (info.edge3d_ <= 0 && info.size1d_ <= 0) {
    info.valid_ = false;
    info.error_ = Tr("Missing LUT size header");
  }
  return info;
}

auto FormatByteSize(std::uintmax_t bytes) -> QString {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = 1024.0 * 1024.0;

  if (bytes >= static_cast<std::uintmax_t>(kMiB)) {
    return Tr("%1 MB").arg(static_cast<double>(bytes) / kMiB, 0, 'f', 2);
  }
  if (bytes >= static_cast<std::uintmax_t>(kKiB)) {
    return Tr("%1 KB").arg(static_cast<double>(bytes) / kKiB, 0, 'f', 1);
  }
  return Tr("%1 B").arg(static_cast<qulonglong>(bytes));
}

auto BuildMetadataText(const LutCatalogEntry& entry) -> QString {
  QStringList parts;
  parts.push_back(FormatByteSize(entry.file_size_bytes_));
  if (entry.edge3d_ > 0) {
    parts.push_back(Tr("3D %1").arg(entry.edge3d_));
  }
  if (entry.size1d_ > 0) {
    parts.push_back(Tr("1D %1").arg(entry.size1d_));
  }
  return parts.join(QStringLiteral("  |  "));
}

auto MakeNoneEntry() -> LutCatalogEntry {
  LutCatalogEntry entry;
  entry.kind_           = LutCatalogEntryKind::None;
  entry.display_name_   = Tr("None");
  entry.secondary_text_ = Tr("Disable the LUT stage.");
  return entry;
}

auto MakeMissingCurrentEntry(const std::string& current_lut_path) -> LutCatalogEntry {
  LutCatalogEntry entry;
  entry.kind_           = LutCatalogEntryKind::MissingCurrent;
  entry.path_           = current_lut_path;
  entry.display_name_   = PathFilenameToQString(Utf8OrNativeToPath(current_lut_path));
  entry.secondary_text_ = Tr("Current LUT is missing from the active LUT folder.");
  entry.status_text_    = Tr("Missing");
  entry.valid_          = false;
  entry.selectable_     = false;
  return entry;
}

auto MakeFileEntry(const std::filesystem::path& path) -> LutCatalogEntry {
  LutCatalogEntry entry;
  entry.kind_         = LutCatalogEntryKind::File;
  entry.path_         = PathToUtf8(path);
  entry.display_name_ = PathFilenameToQString(path);

  std::error_code size_ec;
  entry.file_size_bytes_ = std::filesystem::file_size(path, size_ec);
  if (size_ec) {
    entry.file_size_bytes_ = 0;
  }

  std::error_code mtime_ec;
  const auto      modified_time = std::filesystem::last_write_time(path, mtime_ec);
  if (!mtime_ec) {
    entry.modified_time_sort_key_ =
        static_cast<std::int64_t>(modified_time.time_since_epoch().count());
    entry.has_modified_time_ = true;
  }

  const LutHeaderInfo header = ParseCubeHeaderInfo(path);
  if (!header.valid_) {
    entry.secondary_text_ = header.error_;
    entry.status_text_    = Tr("Invalid");
    entry.valid_          = false;
    entry.selectable_     = false;
    return entry;
  }

  entry.size1d_         = header.size1d_;
  entry.edge3d_         = header.edge3d_;
  entry.secondary_text_ = BuildMetadataText(entry);
  return entry;
}

auto CloneCatalogWithSelection(const LutCatalog& base_catalog, const std::string& current_lut_path)
    -> LutCatalog {
  LutCatalog catalog = base_catalog;
  if (!current_lut_path.empty() && FindEntryIndexForPath(catalog, current_lut_path) < 0) {
    catalog.entries_.insert(catalog.entries_.begin() + 1,
                            MakeMissingCurrentEntry(current_lut_path));
  }
  return catalog;
}

}  // namespace

auto ResolveLutDirectory() -> std::filesystem::path {
  const auto app_luts_dir =
      std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) / "LUTs";
  std::error_code ec;
  if (std::filesystem::is_directory(app_luts_dir, ec) && !ec) {
    return app_luts_dir;
  }
  return std::filesystem::path(CONFIG_PATH) / "LUTs";
}

auto BuildCatalog(const std::string& current_lut_path, bool force_refresh) -> LutCatalog {
  const auto directory = ResolveLutDirectory();

  {
    std::lock_guard<std::mutex> lock(CatalogCacheMutex());
    auto&                       cache = CatalogCache();
    if (force_refresh || !cache.initialized_ || cache.directory_ != directory) {
      cache.directory_          = directory;
      cache.catalog_            = BuildCatalogForDirectory(directory, std::string{});
      cache.catalog_.directory_ = directory;
      cache.initialized_        = true;
    }
    return CloneCatalogWithSelection(cache.catalog_, current_lut_path);
  }
}

auto BuildCatalogForDirectory(const std::filesystem::path& directory,
                              const std::string&           current_lut_path) -> LutCatalog {
  LutCatalog catalog;
  catalog.directory_ = directory;

  std::error_code ec;
  catalog.directory_exists_ = std::filesystem::is_directory(directory, ec) && !ec;
  catalog.entries_.push_back(MakeNoneEntry());

  for (const auto& path : ListCubeLutFiles(directory)) {
    catalog.entries_.push_back(MakeFileEntry(path));
  }

  if (!current_lut_path.empty() && FindEntryIndexForPath(catalog, current_lut_path) < 0) {
    catalog.entries_.insert(catalog.entries_.begin() + 1,
                            MakeMissingCurrentEntry(current_lut_path));
  }

  return catalog;
}

auto FindEntryIndexForPath(const LutCatalog& catalog, const std::string& lut_path) -> int {
  if (lut_path.empty()) {
    return 0;
  }

  for (int i = 0; i < static_cast<int>(catalog.entries_.size()); ++i) {
    if (catalog.entries_[i].path_ == lut_path) {
      return i;
    }
  }

  const std::filesystem::path target_path = Utf8OrNativeToPath(lut_path);
  const auto                  target_name = target_path.filename().wstring();
  if (target_name.empty()) {
    return -1;
  }

  for (int i = 0; i < static_cast<int>(catalog.entries_.size()); ++i) {
    if (catalog.entries_[i].kind_ != LutCatalogEntryKind::File) {
      continue;
    }
    if (Utf8OrNativeToPath(catalog.entries_[i].path_).filename().wstring() == target_name) {
      return i;
    }
  }

  return -1;
}

auto DefaultLutPath(const LutCatalog& catalog) -> std::string {
  for (const auto& entry : catalog.entries_) {
    if (entry.kind_ != LutCatalogEntryKind::File) {
      continue;
    }
    if (Utf8OrNativeToPath(entry.path_).filename() == "5207.cube") {
      return entry.path_;
    }
  }
  return {};
}

auto FormatDirectoryDisplayText(const std::filesystem::path& directory) -> QString {
  if (directory.empty()) {
    return Tr("Folder: unavailable");
  }
  return Tr("Folder: %1").arg(QString::fromStdWString(directory.wstring()));
}

auto CatalogStatusText(const LutCatalog& catalog) -> QString {
  if (!catalog.directory_exists_) {
    return Tr("LUT folder unavailable.");
  }

  int file_count    = 0;
  int invalid_count = 0;
  for (const auto& entry : catalog.entries_) {
    if (entry.kind_ != LutCatalogEntryKind::File) {
      continue;
    }
    ++file_count;
    if (!entry.valid_) {
      ++invalid_count;
    }
  }

  if (file_count == 0) {
    return Tr("No .cube LUT files found.");
  }
  if (invalid_count == 0) {
    return Tr("%1 LUTs available.").arg(file_count);
  }
  return Tr("%1 LUTs available, %2 invalid.").arg(file_count).arg(invalid_count);
}

}  // namespace alcedo::ui::lut_catalog
