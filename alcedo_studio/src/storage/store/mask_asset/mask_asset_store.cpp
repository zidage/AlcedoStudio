//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/mask_asset/mask_asset_store.hpp"

#include <zlib.h>

#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"

namespace alcedo {
namespace {

constexpr const char* kPutSql =
    "INSERT INTO MaskAsset (asset_key, format_version, width, height, bounds_x, bounds_y, "
    "bounds_w, bounds_h, pixel_byte_count, codec, data) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (asset_key) DO NOTHING;";

constexpr const char* kReadSql =
    "SELECT format_version, width, height, bounds_x, bounds_y, bounds_w, bounds_h, "
    "pixel_byte_count, codec, data FROM MaskAsset WHERE asset_key = ?;";

constexpr const char* kListKeysSql = "SELECT asset_key FROM MaskAsset ORDER BY asset_key;";

constexpr const char* kDeleteSql   = "DELETE FROM MaskAsset WHERE asset_key = ?;";

/// One `MaskAsset` row without its key, in kReadSql column order.
struct MaskAssetRow {
  std::int32_t              format_version   = 0;
  std::int32_t              width            = 0;
  std::int32_t              height           = 0;
  float                     bounds_x         = 0.0F;
  float                     bounds_y         = 0.0F;
  float                     bounds_w         = 0.0F;
  float                     bounds_h         = 0.0F;
  std::int64_t              pixel_byte_count = 0;
  std::string               codec;
  std::vector<std::uint8_t> data;

  using Columns = duckorm::RowColumns<&MaskAssetRow::format_version, &MaskAssetRow::width,
                                      &MaskAssetRow::height, &MaskAssetRow::bounds_x,
                                      &MaskAssetRow::bounds_y, &MaskAssetRow::bounds_w,
                                      &MaskAssetRow::bounds_h, &MaskAssetRow::pixel_byte_count,
                                      &MaskAssetRow::codec, &MaskAssetRow::data>;
};

struct MaskAssetKeyRow {
  std::string asset_key;

  using Columns = duckorm::RowColumns<&MaskAssetKeyRow::asset_key>;
};

[[noreturn]] void FailCorrupt(const MaskAssetKey& key, std::string_view reason) {
  throw std::runtime_error("Mask asset " + std::string{key.Value()} +
                           " is corrupt: " + std::string{reason});
}

auto Compress(std::span<const std::uint8_t> pixels) -> std::vector<std::uint8_t> {
  if (pixels.size() > std::numeric_limits<uLong>::max()) {
    throw std::runtime_error("MaskAssetStore: the raster is too large for zlib");
  }
  const auto                source_size = static_cast<uLong>(pixels.size());
  uLongf                    packed_size = compressBound(source_size);
  std::vector<std::uint8_t> packed(packed_size);
  const int                 status =
      compress2(packed.data(), &packed_size, pixels.data(), source_size, Z_DEFAULT_COMPRESSION);
  if (status != Z_OK) {
    throw std::runtime_error("MaskAssetStore: zlib compression failed with status " +
                             std::to_string(status));
  }
  packed.resize(packed_size);
  return packed;
}

}  // namespace

auto MaskAssetStore::Put(const MaskAsset& asset) -> bool {
  try {
    ValidateMaskAsset(asset);
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string{"MaskAssetStore: invalid Mask asset: "} + error.what());
  }
  if (MakeMaskAssetKey(asset.descriptor, asset.pixels) != asset.key) {
    throw std::runtime_error("MaskAssetStore: Mask asset key " + std::string{asset.key.Value()} +
                             " does not match its pixels");
  }
  const auto& bounds = asset.descriptor.reference_bounds;
  // The float bounds bind as DOUBLE; DuckDB casts them back to the same FLOAT values.
  duckorm::SqlFragment put{
      kPutSql,
      {std::string{asset.key.Value()}, static_cast<std::int64_t>(kMaskAssetFormatVersion),
       static_cast<std::int64_t>(asset.descriptor.extent.width),
       static_cast<std::int64_t>(asset.descriptor.extent.height), static_cast<double>(bounds.x),
       static_cast<double>(bounds.y), static_cast<double>(bounds.w), static_cast<double>(bounds.h),
       static_cast<std::int64_t>(asset.pixels.size()), std::string{kMaskAssetZlibCodec},
       Compress(asset.pixels)}};
  return duckorm::execute_count(conn_, put) > 0;
}

auto MaskAssetStore::Read(const MaskAssetKey& key) -> MaskAsset {
  const auto row = duckorm::query_one<MaskAssetRow>(
      conn_, duckorm::SqlFragment{kReadSql, {std::string{key.Value()}}});
  if (!row.has_value()) {
    throw std::runtime_error("Mask asset " + std::string{key.Value()} +
                             " is missing from the project");
  }
  if (row->format_version != static_cast<std::int32_t>(kMaskAssetFormatVersion)) {
    FailCorrupt(key, "unsupported format version");
  }
  if (row->codec != kMaskAssetZlibCodec) {
    FailCorrupt(key, "unsupported codec");
  }
  const auto width      = row->width;
  const auto height     = row->height;
  const auto byte_count = row->pixel_byte_count;
  if (width < 1 || height < 1 || width > static_cast<std::int32_t>(kMaximumRasterMaskAxis) ||
      height > static_cast<std::int32_t>(kMaximumRasterMaskAxis) ||
      byte_count != static_cast<std::int64_t>(width) * height) {
    FailCorrupt(key, "dimensions do not match the byte count");
  }
  MaskAsset asset;
  asset.key = key;
  asset.descriptor.extent =
      Extent2D{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
  asset.descriptor.reference_bounds =
      NormalizedRect{row->bounds_x, row->bounds_y, row->bounds_w, row->bounds_h};
  asset.pixels.resize(static_cast<std::size_t>(byte_count));
  uLongf    pixel_size = static_cast<uLongf>(asset.pixels.size());
  const int status     = uncompress(asset.pixels.data(), &pixel_size, row->data.data(),
                                    static_cast<uLong>(row->data.size()));
  if (status != Z_OK) {
    FailCorrupt(key, "zlib data cannot be inflated (status " + std::to_string(status) + ")");
  }
  if (pixel_size != asset.pixels.size()) {
    FailCorrupt(key, "byte count mismatch");
  }
  if (MakeMaskAssetKey(asset.descriptor, asset.pixels) != key) {
    FailCorrupt(key, "key mismatch");
  }
  return asset;
}

auto MaskAssetStore::ListKeys() -> std::vector<MaskAssetKey> {
  auto rows = duckorm::query<MaskAssetKeyRow>(conn_, duckorm::SqlFragment{kListKeysSql, {}});
  std::vector<MaskAssetKey> keys;
  keys.reserve(rows.size());
  for (auto& row : rows) {
    keys.emplace_back(std::move(row.asset_key));
  }
  return keys;
}

auto MaskAssetStore::DeleteKeysNotIn(const std::set<MaskAssetKey>& keep) -> std::size_t {
  std::vector<MaskAssetKey> remove;
  for (auto& key : ListKeys()) {
    if (!keep.contains(key)) {
      remove.push_back(std::move(key));
    }
  }
  if (remove.empty()) {
    return 0;
  }
  duckorm::Transaction transaction(conn_);
  duckorm::Statement   statement(conn_, kDeleteSql);
  for (const auto& key : remove) {
    statement.Bind(1, key.Value());
    (void)statement.Execute();
  }
  transaction.commit();
  return remove.size();
}

}  // namespace alcedo
