//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/lut_catalog.hpp"

#include <QStringList>
#include <system_error>

#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui::lut_catalog {
namespace {

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

auto BuildMetadataText(const LutCatalogEntry& entry, const QString& relative_path) -> QString {
  QStringList parts;
  parts.push_back(relative_path);
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

auto MakeMissingCurrentEntry(const std::string& current_lut_path,
                             const std::string& current_lut_name) -> LutCatalogEntry {
  LutCatalogEntry entry;
  entry.kind_ = LutCatalogEntryKind::MissingCurrent;
  entry.path_ = current_lut_path;
  entry.display_name_   = current_lut_name.empty()
                              ? QString::fromStdU16String(
                                  alcedo::LutPathFromUtf8(current_lut_path).filename().u16string())
                              : QString::fromStdString(current_lut_name);
  entry.secondary_text_ = Tr("Current LUT is missing from the LUT library.");
  entry.status_text_    = Tr("Missing");
  entry.valid_          = false;
  entry.selectable_     = false;
  return entry;
}

auto MakeFileEntry(const std::filesystem::path& root, const alcedo::LutLibraryEntry& source)
    -> LutCatalogEntry {
  LutCatalogEntry entry;
  entry.kind_         = LutCatalogEntryKind::File;
  entry.path_         = alcedo::LutPathToUtf8(root / alcedo::LutPathFromUtf8(source.relative_path));
  entry.display_name_ = QString::fromStdString(source.DisplayName());
  entry.file_size_bytes_        = source.size;
  entry.modified_time_sort_key_ = source.modified_time;
  const QString relative_path   = QString::fromStdString(source.relative_path);

  if (source.header_error != alcedo::LutHeaderError::kNone) {
    entry.secondary_text_ = QString::fromStdString(source.header_message);
    entry.status_text_    = Tr("Invalid");
    entry.valid_          = false;
    entry.selectable_     = false;
    return entry;
  }
  entry.size1d_ = source.header.lut_1d_size;
  entry.edge3d_ = source.header.lut_3d_size;
  if (!source.header.SupportsGradeApplication()) {
    entry.secondary_text_ = Tr("1D LUTs cannot be applied by the grade stage.");
    entry.status_text_    = Tr("Unsupported");
    entry.selectable_     = false;
    return entry;
  }
  entry.secondary_text_ = BuildMetadataText(entry, relative_path);
  if (const std::string print = source.PrintOptionName(); !print.empty()) {
    entry.secondary_text_ += QStringLiteral("  |  ") + QString::fromStdString(print);
  }
  return entry;
}

}  // namespace

auto BuildCatalog(const alcedo::LutLibraryService* library, const std::string& current_lut_path,
                  const std::string& current_lut_name) -> LutCatalog {
  LutCatalog catalog;
  catalog.entries_.push_back(MakeNoneEntry());
  if (library != nullptr) {
    catalog.directory_ = library->Root();
    std::error_code error;
    catalog.directory_exists_ = std::filesystem::is_directory(catalog.directory_, error) && !error;
    catalog.complete_         = library->inventory_complete();
    catalog.entries_.reserve(library->EntryCount() + 2);
    library->ForEachEntry([&](const alcedo::LutLibraryEntry& entry) {
      catalog.entries_.push_back(MakeFileEntry(catalog.directory_, entry));
    });
  }
  if (!current_lut_path.empty() && FindEntryIndexForPath(catalog, current_lut_path) < 0) {
    catalog.entries_.insert(catalog.entries_.begin() + 1,
                            MakeMissingCurrentEntry(current_lut_path, current_lut_name));
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
  return -1;
}

auto FormatDirectoryDisplayText(const std::filesystem::path& directory) -> QString {
  if (directory.empty()) {
    return Tr("Folder: unavailable");
  }
  return Tr("Folder: %1").arg(QString::fromStdU16String(directory.u16string()));
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

  QString text;
  if (file_count == 0) {
    text = Tr("No .cube LUT files found.");
  } else if (invalid_count == 0) {
    text = Tr("%1 LUTs available.").arg(file_count);
  } else {
    text = Tr("%1 LUTs available, %2 invalid.").arg(file_count).arg(invalid_count);
  }
  if (!catalog.complete_) {
    text += QStringLiteral(" ") + Tr("Some folders could not be read.");
  }
  return text;
}

}  // namespace alcedo::ui::lut_catalog
