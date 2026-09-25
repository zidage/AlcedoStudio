//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Test builders shared by the library search recall and benchmark tests
// (library_search_and_project_size_plan.md, Phase S0).

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "decoders/processor/raw_color_context.hpp"
#include "image/dng_color_profile.hpp"
#include "image/image.hpp"
#include "image/metadata.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/database.hpp"
#include "utils/string/convert.hpp"

namespace alcedo::library_search_test {

/// One synthetic library file. `image_path_` is the full source path; its parent folder name
/// is the "path tail" that search can match.
struct SyntheticImageSpec {
  std::wstring file_name_{};
  std::wstring image_path_{};
  std::string  make_{};
  std::string  model_{};
  std::string  lens_{};
  std::string  date_time_{};
  uint64_t     iso_             = 0;
  float        aperture_        = 0.0f;
  float        focal_           = 0.0f;
  int          rating_          = 0;
  bool         is_raw_file_     = true;   ///< Attach a RAW color context (matrices only).
  bool         has_dng_profile_ = false;  ///< Also attach the large DNG profile.
};

/// Build a DNG profile whose JSON form has the size of a real embedded camera profile
/// (about 400 KB, like the 421 KB profile measured in demo.alcd). Values are deterministic.
inline auto MakeLargeDngColorProfile() -> DngColorProfilePtr {
  const auto make_table = [](std::uint32_t hue, std::uint32_t sat, std::uint32_t val, float phase) {
    DngHueSatMap map;
    map.divisions = {hue, sat, val};
    map.entries.reserve(static_cast<std::size_t>(hue) * sat * val * 3);
    for (std::uint32_t v = 0; v < val; ++v) {
      for (std::uint32_t h = 0; h < hue; ++h) {
        for (std::uint32_t s = 0; s < sat; ++s) {
          const float t = static_cast<float>((h * 131 + s * 17 + v * 7) % 997) / 997.0f;
          map.entries.push_back(-3.1234567f + t * 6.2468f + phase);  // hue shift (degrees)
          map.entries.push_back(0.8765432f + t * 0.2468f);           // saturation scale
          map.entries.push_back(1.0f);                               // value scale
        }
      }
    }
    return map;
  };
  DngColorProfile profile;
  profile.name          = "Synthetic Camera Standard";
  profile.hue_sat_map_1 = make_table(90, 30, 1, 0.0f);
  profile.hue_sat_map_2 = make_table(90, 30, 1, 0.125f);
  profile.look_table    = make_table(36, 8, 16, 0.25f);
  return MakeDngColorProfile(std::move(profile));
}

/// Import-time RAW color context: small matrices plus, for DNG files, the shared profile.
inline auto MakeRawColorContext(const SyntheticImageSpec& spec, const DngColorProfilePtr& profile)
    -> RawRuntimeColorContext {
  RawRuntimeColorContext context;
  context.valid_                = true;
  context.camera_make_          = spec.make_;
  context.camera_model_         = spec.model_;
  context.lens_model_           = spec.lens_;
  context.color_matrices_valid_ = true;
  const double matrix[9]        = {0.6722, -0.0635, -0.0963, -0.4287, 1.2460,
                                   0.2028, -0.0908, 0.2162,  0.5668};
  std::copy(std::begin(matrix), std::end(matrix), context.color_matrix_1_);
  std::copy(std::begin(matrix), std::end(matrix), context.color_matrix_2_);
  if (spec.has_dng_profile_) {
    context.dng_profile_ = profile;
  }
  return context;
}

/// Write synthetic Image rows and library files through the image pool and Sleeve services.
/// Rows are written in batches so that a library of many thousands of files builds in a few
/// seconds; each batch keeps its images pinned until the pool has written them.
class SyntheticLibraryBuilder {
 public:
  explicit SyntheticLibraryBuilder(ProjectService& project)
      : project_(project), dng_profile_(MakeLargeDngColorProfile()) {}

  /// Add every spec to the library root. Returns the created file element ids in spec order.
  auto AddFiles(const std::vector<SyntheticImageSpec>& specs) -> std::vector<sl_element_id_t> {
    constexpr std::size_t        kBatchSize = 500;
    std::vector<sl_element_id_t> file_ids;
    file_ids.reserve(specs.size());
    for (std::size_t begin = 0; begin < specs.size(); begin += kBatchSize) {
      const auto end = std::min(specs.size(), begin + kBatchSize);
      AddBatch(specs, begin, end, file_ids);
    }
    return file_ids;
  }

 private:
  void AddBatch(const std::vector<SyntheticImageSpec>& specs, std::size_t begin, std::size_t end,
                std::vector<sl_element_id_t>& file_ids) {
    auto                                             image_pool = project_.GetImagePoolService();
    std::vector<ImagePoolManager::PinnedImageHandle> pinned;
    std::vector<image_id_t>                          image_ids;
    pinned.reserve(end - begin);
    image_ids.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      const auto& spec   = specs[i];
      auto        handle = image_pool->CreateAndReturnPinnedEmpty();
      auto&       image  = *handle.Get();
      image.image_name_  = spec.file_name_;
      image.image_path_  = std::filesystem::path(spec.image_path_);
      image.image_type_  = spec.has_dng_profile_ ? ImageType::DNG : ImageType::DEFAULT;

      ExifDisplayMetaData metadata;
      metadata.make_          = spec.make_;
      metadata.model_         = spec.model_;
      metadata.lens_          = spec.lens_;
      metadata.date_time_str_ = spec.date_time_;
      metadata.iso_           = spec.iso_;
      metadata.aperture_      = spec.aperture_;
      metadata.focal_         = spec.focal_;
      metadata.rating_        = spec.rating_;
      image.SetExifDisplayMetaData(std::move(metadata));
      if (spec.is_raw_file_) {
        image.SetRawColorContext(MakeRawColorContext(spec, dng_profile_));
      }
      image_ids.push_back(image.image_id_);
      pinned.push_back(std::move(handle));
    }
    image_pool->SyncWithStorage();

    auto created = project_.GetSleeveService()->Write<std::vector<sl_element_id_t>>(
        [&specs, &image_ids, begin, end](FileSystem& fs) {
          std::vector<sl_element_id_t> ids;
          ids.reserve(end - begin);
          for (std::size_t i = begin; i < end; ++i) {
            auto file       = fs.CreateFileInLibrary(specs[i].file_name_);
            file->image_id_ = image_ids[i - begin];
            ids.push_back(file->element_id_);
          }
          return ids;
        });
    ASSERT_TRUE(created.second.success_);
    file_ids.insert(file_ids.end(), created.first.begin(), created.first.end());
  }

  ProjectService&    project_;
  DngColorProfilePtr dng_profile_;
};

/// Element id of the library root folder, which holds files added by `CreateFileInLibrary`.
inline auto LibraryRootFolderId(ProjectService& project) -> sl_element_id_t {
  auto root = project.GetSleeveService()->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  EXPECT_NE(root, nullptr);
  return root ? root->element_id_ : 0;
}

/// Run `SELECT COUNT(*)` on one project table.
inline auto CountTableRows(ProjectService& project, const std::string& table) -> int64_t {
  auto          guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  const auto    sql = "SELECT COUNT(*) FROM " + table;
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    ADD_FAILURE() << "COUNT query failed on " << table << ": " << duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    return -1;
  }
  const auto count = duckdb_value_int64(&result, 0, 0);
  duckdb_destroy_result(&result);
  return count;
}

/// Run the fuzzy search the search dialog runs and return the matched file names.
inline auto SearchFileNames(const SleeveFilterService& filter_service, sl_element_id_t folder_id,
                            const std::wstring& query) -> std::set<std::string> {
  std::set<std::string> names;
  for (const auto& match : filter_service.SearchFolder(folder_id, query, 0, 0, kAllSearchFields)) {
    names.insert(match.file_name_);
  }
  return names;
}

}  // namespace alcedo::library_search_test
