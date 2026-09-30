//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/grade_lut.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"

namespace alcedo {
namespace {

/**
 * @brief Identity used to validate a memoized packed cube.
 *
 * Path + size + write time, plus the library's verified SHA-256 when the resolved file is
 * an official package file, so replaced official content is parsed again even when its
 * stamp did not change. The check is one stat pair per call instead of a full .cube read.
 */
struct CubeFileStamp {
  std::string    normalized_path;
  std::uintmax_t file_size   = 0;
  std::int64_t   write_ticks = 0;
  std::string    content_sha256;
};

struct GradeLutCacheEntry {
  std::uintmax_t                        file_size   = 0;
  std::int64_t                          write_ticks = 0;
  std::string                           content_sha256;
  std::uint64_t                         lru_tick    = 0;
  std::shared_ptr<const PackedGradeLut> packed;
};

/**
 * @brief Process-wide packed-LUT memoization shared by all GPU backends.
 *
 * Packed cubes are large (a 64^3 cube is ~4 MiB of RGBA32F) and .cube parsing
 * dominates a grade encode, so entries are retained per normalized path and
 * validated by file stamp and digest. Entries are immutable once published; changed
 * content replaces the slot and the previous packed copy is released when its
 * in-flight callers drop it. The map is bounded to @ref kMaxCachedGradeLuts
 * with LRU eviction so browsing many cubes cannot grow it without limit.
 * The instance is intentionally leaked: render and thumbnail worker threads
 * may still pack LUTs while static teardown runs.
 */
struct GradeLutCache {
  std::mutex                                        mutex;
  std::unordered_map<std::string, GradeLutCacheEntry> entries;
  std::uint64_t                                     lru_clock = 0;
};

constexpr std::size_t kMaxCachedGradeLuts = 16;

auto SharedGradeLutCache() -> GradeLutCache& {
  static auto* cache = new GradeLutCache();
  return *cache;
}

auto StampOf(const LutResourceResolution& resolution) -> CubeFileStamp {
  std::error_code ec;
  const auto      abs        = std::filesystem::absolute(resolution.path, ec);
  const auto      normalized = (ec ? resolution.path : abs).lexically_normal();
  const auto      utf8       = normalized.generic_u8string();

  CubeFileStamp stamp;
  stamp.normalized_path.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
  stamp.file_size      = resolution.file_size;
  stamp.write_ticks    = resolution.write_ticks;
  stamp.content_sha256 = resolution.content_sha256;
  return stamp;
}

auto FindCachedPacked(GradeLutCache& cache, const CubeFileStamp& stamp)
    -> std::shared_ptr<const PackedGradeLut> {
  const auto it = cache.entries.find(stamp.normalized_path);
  if (it == cache.entries.end() || it->second.file_size != stamp.file_size ||
      it->second.write_ticks != stamp.write_ticks ||
      it->second.content_sha256 != stamp.content_sha256) {
    return nullptr;
  }
  it->second.lru_tick = ++cache.lru_clock;
  return it->second.packed;
}

void StorePacked(GradeLutCache& cache, const CubeFileStamp& stamp,
                 std::shared_ptr<const PackedGradeLut> packed) {
  if (cache.entries.size() >= kMaxCachedGradeLuts &&
      cache.entries.find(stamp.normalized_path) == cache.entries.end()) {
    auto oldest = cache.entries.begin();
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
      if (it->second.lru_tick < oldest->second.lru_tick) {
        oldest = it;
      }
    }
    cache.entries.erase(oldest);
  }
  auto& entry          = cache.entries[stamp.normalized_path];
  entry.file_size      = stamp.file_size;
  entry.write_ticks    = stamp.write_ticks;
  entry.content_sha256 = stamp.content_sha256;
  entry.lru_tick       = ++cache.lru_clock;
  entry.packed         = std::move(packed);
}

auto FindLmt(const ColorGradeNodeModel& grade) -> const LmtModel* {
  return dynamic_cast<const LmtModel*>(grade.FindAdjustmentByType(type_ids::Lmt()));
}

/// Parse the resolved file, or return the memoized instance for unchanged content.
auto PackResolvedCube(const LutResourceResolution& resolution)
    -> std::shared_ptr<const PackedGradeLut> {
  const auto stamp = StampOf(resolution);
  auto&      cache = SharedGradeLutCache();
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (auto packed = FindCachedPacked(cache, stamp)) {
      return packed;
    }
  }

  CubeLut     cube;
  std::string error;
  const auto        utf8 = resolution.path.u8string();
  const std::string path_text(reinterpret_cast<const char*>(utf8.data()), utf8.size());
  if (!ParseCubeFile(resolution.path, cube, &error) || !cube.Has3D()) {
    throw std::runtime_error("Primary grade: failed to load LMT cube '" + path_text +
                             "': " + (error.empty() ? "the file has no 3D table" : error));
  }

  auto packed  = std::make_shared<PackedGradeLut>();
  packed->rgba = PackCubeLutRgba(cube);
  packed->edge = static_cast<std::uint32_t>(cube.edge3d_);
  ContentHash hash;
  hash.MixBytes(packed->rgba);
  hash.MixU32(packed->edge);
  packed->key = hash.Key();

  std::lock_guard<std::mutex> lock(cache.mutex);
  // A concurrent pack for the same stamp wins; both parsed identical content.
  if (auto raced = FindCachedPacked(cache, stamp)) {
    return raced;
  }
  StorePacked(cache, stamp, packed);
  return packed;
}

}  // namespace

auto PackCubeLutRgba(const CubeLut& lut) -> std::vector<std::byte> {
  const auto             edge   = static_cast<std::size_t>(lut.edge3d_);
  const auto             voxels = edge * edge * edge;
  std::vector<std::byte> packed(voxels * 4 * sizeof(float));
  auto*                  out = reinterpret_cast<float*>(packed.data());
  for (std::size_t i = 0; i < voxels; ++i) {
    out[i * 4 + 0] = lut.lut3d_[i * 3 + 0];
    out[i * 4 + 1] = lut.lut3d_[i * 3 + 1];
    out[i * 4 + 2] = lut.lut3d_[i * 3 + 2];
    out[i * 4 + 3] = 1.0f;
  }
  return packed;
}

auto TryPackGradeLut(const ColorGradeNodeModel& grade, const LutResourceResolver& resources)
    -> std::shared_ptr<const PackedGradeLut> {
  const auto* model = FindLmt(grade);
  if (model == nullptr || model->Strength() <= 0.0f) {
    return nullptr;
  }
  const LutReference reference = model->Reference();
  if (IsEmptyLutReference(reference)) {
    return nullptr;
  }
  std::shared_ptr<const PackedGradeLut> packed;
  resources.ReadResource(reference, [&packed](const LutResourceResolution& resolution) {
    if (resolution.status == LutResourceStatus::kAvailable) {
      packed = PackResolvedCube(resolution);
    }
  });
  return packed;
}

auto GradeLutResourceIdentity(const ColorGradeNodeModel& grade,
                              const LutResourceResolver& resources) -> std::uint64_t {
  const auto* model = FindLmt(grade);
  if (model == nullptr) {
    return 0;
  }
  const LutReference reference = model->Reference();
  if (IsEmptyLutReference(reference)) {
    return 0;
  }
  return resources.Resolve(reference).ContentIdentity();
}

}  // namespace alcedo
