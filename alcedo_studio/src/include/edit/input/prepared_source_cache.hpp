//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string_view>

#include "edit/input/prepared_raw_input.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Host cache of unpacked RAW (or synthetic) develop inputs keyed by content.
 *
 * Preview and export quality share an entry when encoded bytes, DecodeRes, and the
 * preparation version match. A leased entry is never replaced when DecodeRes changes;
 * the new policy is inserted under a new key. Not thread-safe.
 */
class PreparedSourceCache {
  struct Entry;

 public:
  /**
   * @brief CPU unpack replacement used on a cache miss.
   *
   * Production uses LibRaw open/unpack. Tests may inject a counting function.
   * The cache overwrites encoded-hash identity on the returned input.
   */
  using UnpackFn = std::function<PreparedRawInput(std::span<const std::byte>, DecodeRes)>;

  /// The AI Denoise run of a miss of @ref AcquireDenoised: the denoised input of a full-size
  /// Bayer input (DenoiseModelSession::Denoise in the product).
  using DenoiseFn = std::function<PreparedRawInput(const PreparedRawInput&)>;

  struct Stats {
    std::uint64_t hits                   = 0;
    std::uint64_t misses                 = 0;
    std::uint64_t libraw_open_unpack_count = 0;
    /// Misses of @ref AcquireDenoised that ran the model.
    std::uint64_t denoise_runs             = 0;
  };

  class Lease {
   public:
    Lease() = default;
    Lease(const Lease& other);
    auto operator=(const Lease& other) -> Lease&;
    Lease(Lease&& other) noexcept;
    auto operator=(Lease&& other) noexcept -> Lease&;
    ~Lease();

    [[nodiscard]] explicit operator bool() const { return static_cast<bool>(entry_); }
    [[nodiscard]] auto     Get() const -> const PreparedRawInput&;
    [[nodiscard]] auto     Key() const -> const PreparedSourceKey&;
    [[nodiscard]] auto     Shared() const -> std::shared_ptr<const PreparedRawInput>;

   private:
    friend class PreparedSourceCache;
    std::shared_ptr<Entry> entry_;
  };

  PreparedSourceCache();
  explicit PreparedSourceCache(UnpackFn unpack);

  /**
   * @brief Return a leased host result for @p encoded and @p decode_res.
   *
   * Hashes encoded bytes, looks up by that hash plus downsample policy, and unpacks only
   * on a miss. Quality is not an argument; callers share this result across preview/export.
   *
   * @pre @p encoded may be empty only for a miss that the unpacker accepts.
   * @throws whatever the unpacker throws on a miss.
   */
  [[nodiscard]] auto AcquireEncoded(std::span<const std::byte> encoded, DecodeRes decode_res)
      -> Lease;

  /**
   * @brief Return the leased AI Denoise input of @p encoded at full size (integration plan, DN6).
   *
   * The entry has its own lookup: the full-size lookup of the source and the hash of
   * @p model_version (the installed model) and @p xyz_to_camera (the camera matrix of the gain
   * match, HashAiDenoiseRun). On a miss, takes the full-size entry
   * (@ref AcquireEncoded). A Bayer CFA input runs @p denoise, the lease of the CFA entry ends
   * (the entry is then the first to be evicted), and the result is inserted. Another input
   * (X-Trans, direct RGB, raster) runs no model: the lease of its own entry is returned, and
   * the Develop input check reports it.
   *
   * @throws whatever the unpacker or @p denoise throws; then no denoised entry is inserted.
   */
  [[nodiscard]] auto AcquireDenoised(std::span<const std::byte>   encoded,
                                     std::string_view             model_version,
                                     const std::array<double, 9>& xyz_to_camera,
                                     const DenoiseFn&             denoise) -> Lease;

  /**
   * @brief True when @ref AcquireEncoded of @p encoded and @p decode_res would find an entry:
   *        the acquire then reads no RAW file (DN14, the step `Reading RAW`).
   *
   * Changes no entry, no use order, and no stats.
   */
  [[nodiscard]] auto HoldsEncoded(std::span<const std::byte> encoded, DecodeRes decode_res)
      -> bool;

  /**
   * @brief True when @ref AcquireDenoised with these arguments would find its entry: the acquire
   *        then runs no model (DN14, the step `AI Denoise`).
   *
   * Changes no entry, no use order, and no stats.
   */
  [[nodiscard]] auto HoldsDenoised(std::span<const std::byte>   encoded,
                                   std::string_view             model_version,
                                   const std::array<double, 9>& xyz_to_camera) -> bool;

  /**
   * @brief Host byte budget for unleased entries. Zero means unlimited.
   *
   * Insertion of a leased miss never fails because the budget is full. Unleased entries
   * are evicted first, in least-recently-used order.
   */
  void               SetHostByteBudget(std::size_t bytes);
  [[nodiscard]] auto HostByteBudget() const -> std::size_t { return byte_budget_; }
  [[nodiscard]] auto HostBytesUsed() const -> std::size_t { return bytes_used_; }
  [[nodiscard]] auto EntryCount() const -> std::size_t { return entries_.size(); }
  [[nodiscard]] auto GetStats() const -> Stats { return stats_; }
  void               ResetStats() { stats_ = {}; }

  /**
   * @brief Drop every host prepared-source entry, including leased ones.
   *
   * Call after WaitIdle when returning a pipeline session. Live Lease objects
   * then see an empty cache on next use.
   */
  void Clear();

 private:
  struct Entry {
    PreparedSourceKey                       key{};
    PreparedSourceLookup                    lookup{};
    std::shared_ptr<const PreparedRawInput> input;
    std::size_t                             bytes     = 0;
    std::uint64_t                           last_used = 0;
  };

  auto HashEncoded(std::span<const std::byte> encoded) -> std::uint64_t;
  void EvictUnleasedToFit(std::size_t extra_bytes);
  auto Insert(const PreparedSourceLookup& lookup, PreparedRawInput prepared) -> Lease;
  auto MakeLease(std::shared_ptr<Entry> entry) -> Lease;

  UnpackFn                                        unpack_;
  std::map<PreparedSourceLookup, std::shared_ptr<Entry>> entries_;
  std::size_t                                     byte_budget_ = 512ull * 1024ull * 1024ull;
  std::size_t                                     bytes_used_  = 0;
  std::uint64_t                                   use_clock_   = 1;
  Stats                                           stats_{};
  const std::byte*                                memo_ptr_    = nullptr;
  std::size_t                                     memo_size_   = 0;
  std::uint64_t                                   memo_hash_   = 0;
};

}  // namespace alcedo
