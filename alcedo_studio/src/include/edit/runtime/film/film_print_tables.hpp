//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "edit/runtime/content_key.hpp"
#include "edit/runtime/film/film_stock_bundle.hpp"

namespace alcedo {

/// Nodes on each axis of the print table and of the paper stage.
inline constexpr std::int64_t kFilmPrintTableEdge = 65;

/**
 * @brief The tables of one selection of film, print material, and scene light.
 *
 * Immutable and shared. The double-precision arrays are the host values. @ref print_rgba is the
 * form that `AcquireLut` of a GPU backend takes: RGBA32F, tightly packed, with the red axis varying
 * fastest and an alpha of 1. Node values are the cube root of print XYZ.
 */
struct FilmPrintTableData {
  /// (uv_nodes, uv_nodes, 3), u varying slowest. A node that no read can reach holds 0.
  std::vector<double>    uv_exposure;
  std::vector<double>    print_cube_root_xyz;  ///< (edge, edge, edge, 3), red varying slowest
  std::vector<std::byte> print_rgba;
  /// The point kernels read these values. Layout: film_print_kernel_tables::Layout.
  std::vector<float>     kernel_values;
  std::int64_t           uv_nodes = 0;
  std::uint32_t          edge     = 0;
  ContentKey             uv_key{};
  ContentKey             print_key{};
  ContentKey             kernel_key{};
  std::int64_t           outside_curve     = 0;  ///< Layer values outside the paper curve
  std::int64_t           print_not_reached = 0;  ///< Nodes where the paper dye solve did not reach

  [[nodiscard]] auto     ByteSize() const -> std::size_t;
};

/// Counts and the last wall times of each table stage, for tests and records.
struct FilmPrintTableStats {
  std::int64_t uv_builds             = 0;
  std::int64_t negative_stage_builds = 0;
  std::int64_t paper_stage_builds    = 0;
  std::int64_t compose_builds        = 0;
  double       last_uv_seconds       = 0.0;
  double       last_negative_seconds = 0.0;
  double       last_paper_seconds    = 0.0;
  double       last_compose_seconds  = 0.0;
  std::size_t  kept_bytes            = 0;
};

/**
 * @brief Owner of the host tables of the film print. Keeps tables in memory by content key.
 *
 * The print table has three stages, so that a new selection repeats few spectral integrals:
 * stage A (negative to paper exposure) depends on the negative and the paper, stage B (paper Status
 * A to XYZ) depends on the paper, and the composition depends on the complete selection. The object
 * keeps stage A, stage B, and complete results, and removes the oldest when the kept bytes pass the
 * limit. No table goes to disk.
 *
 * Thread safety: all methods are safe to call from any thread. One lock serializes @ref Resolve, so
 * a build for one thread makes other callers wait, and each table builds one time.
 */
class FilmPrintTables {
 public:
  /// Holds about 5 complete selections with their stages.
  static constexpr std::size_t kDefaultByteLimit = 128ull << 20;

  /**
   * @param bundle The resources. Must not be null.
   * @param byte_limit The limit of the kept tables. A result larger than the limit stays valid for
   *        its callers but is not kept.
   * @throws std::invalid_argument when @p bundle is null.
   */
  explicit FilmPrintTables(std::shared_ptr<const FilmStockBundle> bundle,
                           std::size_t                            byte_limit = kDefaultByteLimit);

  /**
   * @brief The tables of a selection, from memory or from a new build.
   * @return Shared immutable data.
   * @throws std::runtime_error that names the identifier for an unknown selection, or that names
   *         the selection when a table has a value that is not finite. No result is kept then.
   */
  [[nodiscard]] auto Resolve(std::string_view negative, std::string_view print_material,
                             std::string_view scene_light)
      -> std::shared_ptr<const FilmPrintTableData>;

  [[nodiscard]] auto Stats() const -> FilmPrintTableStats;

  /// The resources of these tables. Read-only, and valid for the life of this object.
  [[nodiscard]] auto Bundle() const -> const FilmStockBundle& { return *bundle_; }

 private:
  struct NegativeStage {
    std::vector<double> paper_log;  ///< (edge^3, 3)
  };
  struct PaperStage {
    std::vector<double>       cube_root_xyz;  ///< (edge^3, 3)
    std::vector<std::uint8_t> not_reached;    ///< (edge^3)
  };
  template <class Table>
  struct KeptTable {
    std::shared_ptr<const Table> table;
    std::size_t                  bytes    = 0;
    std::uint64_t                lru_tick = 0;
  };
  template <class Table>
  using KeptTables = std::unordered_map<std::uint64_t, KeptTable<Table>>;

  [[nodiscard]] auto NegativeStageOf(std::string_view negative, std::string_view print_material)
      -> std::shared_ptr<const NegativeStage>;
  [[nodiscard]] auto PaperStageOf(std::string_view print_material)
      -> std::shared_ptr<const PaperStage>;
  void RemoveOldestAboveLimit();

  template <class Table>
  auto Find(KeptTables<Table>& kept, ContentKey key) -> std::shared_ptr<const Table>;
  template <class Table>
  void Keep(KeptTables<Table>& kept, ContentKey key, std::shared_ptr<const Table> table,
            std::size_t bytes);

  std::shared_ptr<const FilmStockBundle> bundle_;
  std::size_t                            byte_limit_ = 0;
  mutable std::mutex                     mutex_;
  KeptTables<NegativeStage>              negative_stages_;
  KeptTables<PaperStage>                 paper_stages_;
  KeptTables<FilmPrintTableData>         results_;
  FilmPrintTableStats                    stats_;
  std::uint64_t                          lru_clock_ = 0;
};

}  // namespace alcedo
