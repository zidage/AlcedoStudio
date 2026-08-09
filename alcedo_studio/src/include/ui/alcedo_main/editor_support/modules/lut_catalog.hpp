#pragma once

#include <QString>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace alcedo::ui::lut_catalog {

enum class LutCatalogEntryKind {
  None,
  File,
  MissingCurrent,
};

struct LutCatalogEntry {
  LutCatalogEntryKind  kind_ = LutCatalogEntryKind::File;
  std::string          path_{};
  QString              display_name_{};
  QString              secondary_text_{};
  QString              status_text_{};
  std::uintmax_t       file_size_bytes_        = 0;
  std::int64_t         modified_time_sort_key_ = 0;
  bool                 has_modified_time_      = false;
  int                  size1d_                 = 0;
  int                  edge3d_                 = 0;
  std::array<float, 3> domain_min_{0.0f, 0.0f, 0.0f};
  std::array<float, 3> domain_max_{1.0f, 1.0f, 1.0f};
  bool                 valid_      = true;
  bool                 selectable_ = true;
};

struct LutCatalog {
  std::filesystem::path        directory_{};
  bool                         directory_exists_ = false;
  std::vector<LutCatalogEntry> entries_{};
};

auto ResolveLutDirectory() -> std::filesystem::path;
auto BuildCatalog(const std::string& current_lut_path, bool force_refresh = false) -> LutCatalog;
auto BuildCatalogForDirectory(const std::filesystem::path& directory,
                              const std::string&           current_lut_path) -> LutCatalog;
auto FindEntryIndexForPath(const LutCatalog& catalog, const std::string& lut_path) -> int;
auto DefaultLutPath(const LutCatalog& catalog) -> std::string;
auto FormatDirectoryDisplayText(const std::filesystem::path& directory) -> QString;
auto CatalogStatusText(const LutCatalog& catalog) -> QString;

}  // namespace alcedo::ui::lut_catalog
