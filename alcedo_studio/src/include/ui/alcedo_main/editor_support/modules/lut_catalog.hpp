#pragma once

#include <QString>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "app/lut_library_service.hpp"

namespace alcedo::ui::lut_catalog {

enum class LutCatalogEntryKind {
  None,
  File,
  MissingCurrent,
};

/// One row of the editor LUT list. File rows are projections of
/// LutLibraryService entries; `path_` is the absolute UTF-8 path the LMT model stores.
struct LutCatalogEntry {
  LutCatalogEntryKind kind_ = LutCatalogEntryKind::File;
  std::string         path_{};
  QString             display_name_{};
  QString             secondary_text_{};
  QString             status_text_{};
  std::uintmax_t      file_size_bytes_        = 0;
  std::int64_t        modified_time_sort_key_ = 0;
  int                 size1d_                 = 0;
  int                 edge3d_                 = 0;
  bool                valid_                  = true;
  bool                selectable_             = true;
};

struct LutCatalog {
  std::filesystem::path        directory_{};
  bool                         directory_exists_ = false;
  bool                         complete_         = true;
  std::vector<LutCatalogEntry> entries_{};
};

/// Build the list from the library's published inventory. Without a library the
/// list holds only the None row. A current path that no row matches exactly is
/// shown as a missing row named @p current_lut_name (or the file name when empty);
/// file names are never matched across folders.
auto BuildCatalog(const alcedo::LutLibraryService* library, const std::string& current_lut_path,
                  const std::string& current_lut_name = {}) -> LutCatalog;
/// Index of the row whose path equals @p lut_path exactly, 0 for an empty path, else -1.
auto FindEntryIndexForPath(const LutCatalog& catalog, const std::string& lut_path) -> int;
auto FormatDirectoryDisplayText(const std::filesystem::path& directory) -> QString;
auto CatalogStatusText(const LutCatalog& catalog) -> QString;

}  // namespace alcedo::ui::lut_catalog
