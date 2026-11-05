//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film/film_print_tables.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/runtime/film/film_print_kernel_tables.hpp"
#include "edit/runtime/film/film_tables.hpp"

namespace alcedo {
namespace {

// Values of the lab evaluator (film_preview.py): PAPER_AXIS_POWER, SOLVE_ITERATIONS, and
// SOLVE_REACHED_LIMIT.
constexpr double       kPaperAxisPower    = 2.0;
constexpr int          kSolveIterations   = 16;
constexpr double       kSolveReachedLimit = 1e-4;
constexpr double       kSmallestRelativeLuminance = 1e-12;
constexpr std::int64_t kTableNodes =
    kFilmPrintTableEdge * kFilmPrintTableEdge * kFilmPrintTableEdge;
constexpr auto          kTableValues = static_cast<std::size_t>(kTableNodes * film_tables::kLayers);

/// Bump when a table of an unchanged resource changes its values.
constexpr std::uint32_t kTableImplementationVersion = 1;

auto                    SecondsSince(std::chrono::steady_clock::time_point start) -> double {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

/// The spectral values of a band on the wavelengths where its weights are not zero. Each integral
/// reads only these wavelengths.
struct BandOnSupport {
  std::vector<double> base;
  std::vector<double> dyes;
  std::vector<double> weights;

  [[nodiscard]] auto  View() const -> film_tables::SpectralBand {
    return {static_cast<std::int64_t>(base.size()), base.data(), dyes.data(), weights.data()};
  }
};

auto MakeBandOnSupport(std::span<const double> base, std::span<const double> dyes,
                       std::span<const double> weights) -> BandOnSupport {
  const auto wavelengths = base.size();
  const auto copy_rows   = [wavelengths](std::span<const double>         rows,
                                       const std::vector<std::size_t>& support) {
    std::vector<double> out;
    out.reserve(support.size() * film_tables::kLayers);
    for (std::size_t row = 0; row < film_tables::kLayers; ++row) {
      for (const auto w : support) {
        out.push_back(rows[row * wavelengths + w]);
      }
    }
    return out;
  };
  std::vector<std::size_t> support;
  for (std::size_t w = 0; w < wavelengths; ++w) {
    if (weights[w] != 0.0 || weights[wavelengths + w] != 0.0 ||
        weights[2 * wavelengths + w] != 0.0) {
      support.push_back(w);
    }
  }
  BandOnSupport band;
  band.base.reserve(support.size());
  for (const auto w : support) {
    band.base.push_back(base[w]);
  }
  band.dyes    = copy_rows(dyes, support);
  band.weights = copy_rows(weights, support);
  return band;
}

/// A node of numpy.linspace(start, stop, kFilmPrintTableEdge).
auto GridNode(double start, double stop, std::int64_t node) -> double {
  if (node == kFilmPrintTableEdge - 1) {
    return stop;
  }
  return static_cast<double>(node) * ((stop - start) / (kFilmPrintTableEdge - 1)) + start;
}

auto StatusAxes(const FilmPaperView& paper) -> film_tables::StatusAxes {
  return {kFilmPrintTableEdge, paper.status_axis_start.data(), paper.status_axis_range.data(),
          kPaperAxisPower};
}

/// RGBA32F with red varying fastest, from values with red varying slowest.
auto PackPrintRgba(std::span<const double> cube_root_xyz) -> std::vector<std::byte> {
  constexpr auto         kEdge = static_cast<std::size_t>(kFilmPrintTableEdge);
  std::vector<std::byte> packed(static_cast<std::size_t>(kTableNodes) * 4 * sizeof(float));
  auto*                  out = reinterpret_cast<float*>(packed.data());
  for (std::size_t b = 0; b < kEdge; ++b) {
    for (std::size_t g = 0; g < kEdge; ++g) {
      for (std::size_t r = 0; r < kEdge; ++r) {
        const auto  source = ((r * kEdge + g) * kEdge + b) * film_tables::kLayers;
        auto* const voxel  = out + ((b * kEdge + g) * kEdge + r) * 4;
        voxel[0]           = static_cast<float>(cube_root_xyz[source + 0]);
        voxel[1]           = static_cast<float>(cube_root_xyz[source + 1]);
        voxel[2]           = static_cast<float>(cube_root_xyz[source + 2]);
        voxel[3]           = 1.0f;
      }
    }
  }
  return packed;
}

/// Lab rule dark_surround_on_display: luminance relative to the clear print takes the exponent,
/// and the chromaticity stays. The table holds the cube root of XYZ.
void ApplyDarkSurround(const FilmPaperView& paper, std::vector<double>& cube_root_xyz) {
  if (paper.surround_exponent == 1.0) {
    return;
  }
  const double power = (paper.surround_exponent - 1.0) / 3.0;
  for (std::size_t node = 0; node + 2 < cube_root_xyz.size(); node += film_tables::kLayers) {
    const double y = cube_root_xyz[node + 1];
    const double relative =
        std::max(y * y * y / paper.clear_luminance, kSmallestRelativeLuminance);
    const double scale = std::pow(relative, power);
    for (std::size_t k = 0; k < film_tables::kLayers; ++k) {
      cube_root_xyz[node + k] *= scale;
    }
  }
}

auto AllFinite(std::span<const double> values) -> bool {
  return std::ranges::all_of(values, [](double value) { return std::isfinite(value); });
}

/// The scene light of a key: the two lights and the share of the second.
auto MixLight(ContentHash hash, const FilmSceneLightMix& light) -> ContentHash {
  return hash.MixText(light.first).MixText(light.second).MixF64(light.share);
}

/// The scene light of a message.
auto LightText(const FilmSceneLightMix& light) -> std::string {
  if (light.share == 0.0) {
    return light.first;
  }
  return light.first + " and " + light.second + " (share " + std::to_string(light.share) + ")";
}

}  // namespace

auto FilmPrintTableData::ByteSize() const -> std::size_t {
  return uv_exposure.size() * sizeof(double) + print_cube_root_xyz.size() * sizeof(double) +
         print_rgba.size() + kernel_values.size() * sizeof(float);
}

FilmPrintTables::FilmPrintTables(std::shared_ptr<const FilmStockBundle> bundle,
                                 std::size_t                            byte_limit)
    : bundle_(std::move(bundle)), byte_limit_(byte_limit) {
  if (bundle_ == nullptr) {
    throw std::invalid_argument("FilmPrintTables: the stock bundle is null");
  }
}

auto FilmPrintTables::Stats() const -> FilmPrintTableStats {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

template <class Table>
auto FilmPrintTables::Find(KeptTables<Table>& kept, ContentKey key)
    -> std::shared_ptr<const Table> {
  const auto entry = kept.find(key.hash);
  if (entry == kept.end()) {
    return nullptr;
  }
  entry->second.lru_tick = ++lru_clock_;
  return entry->second.table;
}

template <class Table>
void FilmPrintTables::Keep(KeptTables<Table>& kept, ContentKey key,
                           std::shared_ptr<const Table> table, std::size_t bytes) {
  kept[key.hash] = KeptTable<Table>{std::move(table), bytes, ++lru_clock_};
  stats_.kept_bytes += bytes;
  RemoveOldestAboveLimit();
}

void FilmPrintTables::RemoveOldestAboveLimit() {
  while (stats_.kept_bytes > byte_limit_) {
    auto       oldest      = (std::numeric_limits<std::uint64_t>::max)();
    const auto find_oldest = [&oldest](const auto& kept) {
      for (const auto& [hash, entry] : kept) {
        oldest = (std::min)(oldest, entry.lru_tick);
      }
    };
    find_oldest(negative_stages_);
    find_oldest(paper_stages_);
    find_oldest(light_tables_);
    find_oldest(scan_tables_);
    find_oldest(results_);
    const auto remove_oldest = [&oldest, this](auto& kept) {
      for (auto entry = kept.begin(); entry != kept.end(); ++entry) {
        if (entry->second.lru_tick == oldest) {
          stats_.kept_bytes -= entry->second.bytes;
          kept.erase(entry);
          return;
        }
      }
    };
    remove_oldest(negative_stages_);
    remove_oldest(paper_stages_);
    remove_oldest(light_tables_);
    remove_oldest(scan_tables_);
    remove_oldest(results_);
  }
}

auto FilmPrintTables::NegativeStageOf(std::string_view negative, std::string_view print_material)
    -> std::shared_ptr<const NegativeStage> {
  const auto key = ContentHash{}
                       .MixU32(kTableImplementationVersion)
                       .MixText("negative stage")
                       .MixText(bundle_->ContentDigest())
                       .MixText(negative)
                       .MixText(print_material)
                       .Key();
  if (auto kept = Find(negative_stages_, key)) {
    return kept;
  }
  const auto          start = std::chrono::steady_clock::now();
  const auto          stock = bundle_->Negative(negative);
  const auto          paper = bundle_->PrintMaterial(print_material);
  const auto          band  = MakeBandOnSupport(stock.base, stock.dyes, paper.printing_weights);

  // With rule 'dye_amount_curves' of the lab, the nodes of the negative record are the dye amounts.
  std::vector<double> amounts(kTableValues);
  std::size_t         out = 0;
  for (std::int64_t r = 0; r < kFilmPrintTableEdge; ++r) {
    const auto red = GridNode(stock.density_grid_start[0], stock.density_grid_stop[0], r);
    for (std::int64_t g = 0; g < kFilmPrintTableEdge; ++g) {
      const auto green = GridNode(stock.density_grid_start[1], stock.density_grid_stop[1], g);
      for (std::int64_t b = 0; b < kFilmPrintTableEdge; ++b) {
        amounts[out++] = red;
        amounts[out++] = green;
        amounts[out++] = GridNode(stock.density_grid_start[2], stock.density_grid_stop[2], b);
      }
    }
  }
  auto stage = std::make_shared<NegativeStage>();
  stage->paper_log.resize(kTableValues);
  film_tables::Log10BandResponse(band.View(), amounts.data(), kTableNodes, stage->paper_log.data());
  ++stats_.negative_stage_builds;
  stats_.last_negative_seconds = SecondsSince(start);
  const auto bytes             = stage->paper_log.size() * sizeof(double);
  Keep<NegativeStage>(negative_stages_, key, stage, bytes);
  return stage;
}

auto FilmPrintTables::PaperStageOf(std::string_view print_material)
    -> std::shared_ptr<const PaperStage> {
  const auto key = ContentHash{}
                       .MixU32(kTableImplementationVersion)
                       .MixText("paper stage")
                       .MixText(bundle_->ContentDigest())
                       .MixText(print_material)
                       .Key();
  if (auto kept = Find(paper_stages_, key)) {
    return kept;
  }
  const auto start   = std::chrono::steady_clock::now();
  const auto paper   = bundle_->PrintMaterial(print_material);
  const auto status  = MakeBandOnSupport(paper.base, paper.dyes, paper.status_a_weights);
  const auto viewing = MakeBandOnSupport(paper.base, paper.dyes, paper.viewing_weights);
  auto       stage   = std::make_shared<PaperStage>();
  stage->cube_root_xyz.resize(kTableValues);
  stage->not_reached.resize(static_cast<std::size_t>(kTableNodes));
  film_tables::BuildPaperTable(status.View(), viewing.View(), paper.xyz_scale, StatusAxes(paper),
                               kSolveIterations, kSolveReachedLimit, stage->cube_root_xyz.data(),
                               stage->not_reached.data());
  ++stats_.paper_stage_builds;
  stats_.last_paper_seconds = SecondsSince(start);
  const auto bytes = stage->cube_root_xyz.size() * sizeof(double) + stage->not_reached.size();
  Keep<PaperStage>(paper_stages_, key, stage, bytes);
  return stage;
}

auto FilmPrintTables::LightTableOf(std::string_view reversal)
    -> std::shared_ptr<const RecordTableStage> {
  const auto key = ContentHash{}
                       .MixU32(kTableImplementationVersion)
                       .MixText("light table")
                       .MixText(bundle_->ContentDigest())
                       .MixText(reversal)
                       .Key();
  if (auto kept = Find(light_tables_, key)) {
    return kept;
  }
  const auto  start = std::chrono::steady_clock::now();
  const auto  film  = bundle_->Reversal(reversal);
  const auto  view  = bundle_->LightTable();
  const auto  band  = MakeBandOnSupport(film.record.base, film.record.dyes, view.viewing_weights);

  // Lab rule 'removed_dye_record': the nodes are records of removed dye on numpy.linspace(0, top of
  // the record), and the dye amount of a node is dye_max - record, not below 0.
  const auto& grid_start = film.record.density_grid_start;
  const auto& grid_stop  = film.record.density_grid_stop;
  std::vector<double> amounts(kTableValues);
  std::size_t         out = 0;
  for (std::int64_t r = 0; r < kFilmPrintTableEdge; ++r) {
    const auto red = GridNode(grid_start[0], grid_stop[0], r);
    for (std::int64_t g = 0; g < kFilmPrintTableEdge; ++g) {
      const auto green = GridNode(grid_start[1], grid_stop[1], g);
      for (std::int64_t b = 0; b < kFilmPrintTableEdge; ++b) {
        const auto blue = GridNode(grid_start[2], grid_stop[2], b);
        amounts[out++]  = (std::max)(film.dye_max[0] - red, 0.0);
        amounts[out++]  = (std::max)(film.dye_max[1] - green, 0.0);
        amounts[out++]  = (std::max)(film.dye_max[2] - blue, 0.0);
      }
    }
  }
  auto stage = std::make_shared<RecordTableStage>();
  stage->key = key;
  stage->values.resize(kTableValues);
  film_tables::CubeRootBandResponse(band.View(), view.xyz_scale, amounts.data(), kTableNodes,
                                    stage->values.data());
  ++stats_.light_table_builds;
  stats_.last_light_table_seconds = SecondsSince(start);
  const auto bytes                = stage->values.size() * sizeof(double);
  Keep<RecordTableStage>(light_tables_, key, stage, bytes);
  return stage;
}

auto FilmPrintTables::ScannerTableOf(std::string_view                          negative,
                                     const film_negative_scan::ScannerWeights& weights,
                                     ContentKey scanner_key) -> std::shared_ptr<const RecordTableStage> {
  const auto key = ContentHash{}
                       .MixU32(kTableImplementationVersion)
                       .MixText("scanner table")
                       .MixText(bundle_->ContentDigest())
                       .MixText(negative)
                       .MixKey(scanner_key)
                       .Key();
  if (auto kept = Find(scan_tables_, key)) {
    return kept;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto stock = bundle_->Negative(negative);
  const auto band  = MakeBandOnSupport(stock.base, stock.dyes, weights.weights);

  // With rule 'dye_amount_curves' of the lab, the nodes of the negative record are the dye amounts,
  // as in the negative stage of the print table.
  std::vector<double> amounts(kTableValues);
  std::size_t         out = 0;
  for (std::int64_t r = 0; r < kFilmPrintTableEdge; ++r) {
    const auto red = GridNode(stock.density_grid_start[0], stock.density_grid_stop[0], r);
    for (std::int64_t g = 0; g < kFilmPrintTableEdge; ++g) {
      const auto green = GridNode(stock.density_grid_start[1], stock.density_grid_stop[1], g);
      for (std::int64_t b = 0; b < kFilmPrintTableEdge; ++b) {
        amounts[out++] = red;
        amounts[out++] = green;
        amounts[out++] = GridNode(stock.density_grid_start[2], stock.density_grid_stop[2], b);
      }
    }
  }
  // Lab rule 'ideal_scanner_bands' (ScanPreview.apd with the scanner weights):
  // T = log10(sum w) - log10(sum w 10^-(base + dyes a)).
  const auto            wavelengths = static_cast<std::size_t>(bundle_->Wavelengths());
  std::array<double, 3> log_weight_sum{};
  for (std::size_t k = 0; k < film_tables::kLayers; ++k) {
    double sum = 0.0;
    for (std::size_t w = 0; w < wavelengths; ++w) {
      sum += weights.weights[k * wavelengths + w];
    }
    if (!(sum > 0.0)) {
      throw std::runtime_error("Film print: a band of the scanner of Negative Scan has no weight");
    }
    log_weight_sum[k] = std::log10(sum);
  }
  auto stage = std::make_shared<RecordTableStage>();
  stage->key = key;
  stage->values.resize(kTableValues);
  film_tables::Log10BandResponse(band.View(), amounts.data(), kTableNodes, stage->values.data());
  for (std::size_t index = 0; index < stage->values.size(); ++index) {
    stage->values[index] = log_weight_sum[index % film_tables::kLayers] - stage->values[index];
  }
  ++stats_.scan_table_builds;
  stats_.last_scan_table_seconds = SecondsSince(start);
  const auto bytes               = stage->values.size() * sizeof(double);
  Keep<RecordTableStage>(scan_tables_, key, stage, bytes);
  return stage;
}

void FilmPrintTables::BuildUvTable(std::span<const double> exposure_weights,
                                   std::span<const double> scene_spectrum,
                                   FilmPrintTableData&     result) {
  const auto          start      = std::chrono::steady_clock::now();
  const auto          wavelength = static_cast<std::size_t>(bundle_->Wavelengths());
  const auto          first      = static_cast<std::size_t>(bundle_->ReflectanceFirstWavelength());
  const auto          band       = static_cast<std::size_t>(bundle_->ReflectanceWavelengths());
  const auto          kept       = bundle_->ReflectanceNodes();
  std::vector<double> weights(band * film_tables::kLayers);
  for (std::size_t layer = 0; layer < film_tables::kLayers; ++layer) {
    for (std::size_t w = 0; w < band; ++w) {
      weights[layer * band + w] =
          exposure_weights[layer * wavelength + first + w] * scene_spectrum[first + w];
    }
  }
  std::vector<double> kept_exposure(kept.size() * film_tables::kLayers);
  film_tables::TableBandIntegral(
      bundle_->ReflectanceRows().data(), static_cast<std::int64_t>(kept.size()),
      bundle_->ReflectanceWavelengths(), weights.data(), kept_exposure.data());
  const auto nodes = result.uv_nodes * result.uv_nodes;
  result.uv_exposure.assign(static_cast<std::size_t>(nodes) * film_tables::kLayers, 0.0);
  for (std::size_t row = 0; row < kept.size(); ++row) {
    const auto node = static_cast<std::size_t>(kept[row]) * film_tables::kLayers;
    for (std::size_t layer = 0; layer < film_tables::kLayers; ++layer) {
      result.uv_exposure[node + layer] = kept_exposure[row * film_tables::kLayers + layer];
    }
  }
  ++stats_.uv_builds;
  stats_.last_uv_seconds = SecondsSince(start);
}

auto FilmPrintTables::ResolveReversal(std::string_view         reversal,
                                      const FilmSceneLightMix& scene_light)
    -> std::shared_ptr<const FilmPrintTableData> {
  const auto result_key = MixLight(ContentHash{}
                                       .MixU32(kTableImplementationVersion)
                                       .MixText("reversal selection")
                                       .MixText(bundle_->ContentDigest())
                                       .MixText(reversal),
                                   scene_light)
                              .Key();
  if (auto kept = Find(results_, result_key)) {
    return kept;
  }
  const auto setup    = bundle_->ReversalSelection(reversal, scene_light);
  const auto film     = bundle_->Reversal(reversal);

  auto       result   = std::make_shared<FilmPrintTableData>();
  result->uv_nodes    = bundle_->UvNodes();
  result->edge        = static_cast<std::uint32_t>(kFilmPrintTableEdge);
  result->light_table = true;
  result->uv_key      = MixLight(ContentHash{}
                                     .MixU32(kTableImplementationVersion)
                                     .MixText("uv table")
                                     .MixText(bundle_->ContentDigest())
                                     .MixText(reversal),
                                 scene_light)
                       .Key();
  result->kernel_key = MixLight(ContentHash{}
                                    .MixU32(kTableImplementationVersion)
                                    .MixText("kernel values")
                                    .MixText(bundle_->ContentDigest())
                                    .MixText(reversal),
                                scene_light)
                           .Key();
  BuildUvTable(film.record.exposure_weights, setup.scene_spectrum, *result);

  // The light table depends on the film only, so all scene lights share its GPU copy.
  const auto light_table      = LightTableOf(reversal);
  result->print_key           = light_table->key;
  result->print_cube_root_xyz = light_table->values;
  if (!AllFinite(result->uv_exposure) || !AllFinite(result->print_cube_root_xyz)) {
    throw std::runtime_error("Film print: a table of the reversal film " + std::string(reversal) +
                             " under " + LightText(scene_light) +
                             " has a value that is not finite");
  }
  result->print_rgba = PackPrintRgba(result->print_cube_root_xyz);
  result->kernel_values =
      film_print_kernel_tables::MakeValues<float>(*bundle_, reversal, result->uv_exposure);
  const auto bytes = result->ByteSize();
  Keep<FilmPrintTableData>(results_, result_key, result, bytes);
  return result;
}

auto FilmPrintTables::ResolveNegativeScan(std::string_view                   negative,
                                          const FilmSceneLightMix&           scene_light,
                                          const film_negative_scan::Scanner& scanner)
    -> std::shared_ptr<const FilmPrintTableData> {
  std::lock_guard<std::mutex> lock(mutex_);
  return ResolveNegativeScanLocked(negative, scene_light, scanner);
}

auto FilmPrintTables::ResolveNegativeScanLocked(std::string_view                   negative,
                                                const FilmSceneLightMix&           scene_light,
                                                const film_negative_scan::Scanner& scanner)
    -> std::shared_ptr<const FilmPrintTableData> {
  const auto& constants   = bundle_->NegativeScanConstants();
  const bool  default_one = scanner == film_negative_scan::DefaultScanner(constants);
  const auto  scanner_key = ContentHash{}
                               .MixText("scanner")
                               .MixF64(scanner.centres_nm[0])
                               .MixF64(scanner.centres_nm[1])
                               .MixF64(scanner.centres_nm[2])
                               .MixF64(scanner.width_nm)
                               .MixU32(scanner.equal_energy ? 1U : 0U)
                               .Key();
  const auto result_key = MixLight(ContentHash{}
                                       .MixU32(kTableImplementationVersion)
                                       .MixText("negative scan selection")
                                       .MixText(bundle_->ContentDigest())
                                       .MixText(negative)
                                       .MixKey(scanner_key),
                                   scene_light)
                              .Key();
  if (auto kept = Find(results_, result_key)) {
    return kept;
  }
  const auto setup  = bundle_->NegativeScanSelection(negative, scene_light);
  const auto stock  = bundle_->Negative(negative);
  const auto weights =
      film_negative_scan::MakeScannerWeights(scanner, constants, bundle_->WavelengthsNm());

  auto result      = std::make_shared<FilmPrintTableData>();
  result->uv_nodes = bundle_->UvNodes();
  result->edge     = static_cast<std::uint32_t>(kFilmPrintTableEdge);
  result->scan     = true;
  // The u'v' table and the kernel values are those of a print of the same negative and light, so
  // a change between Negative Scan and a print material keeps their GPU copies.
  result->uv_key   = MixLight(ContentHash{}
                                  .MixU32(kTableImplementationVersion)
                                  .MixText("uv table")
                                  .MixText(bundle_->ContentDigest())
                                  .MixText(negative),
                              scene_light)
                       .Key();
  result->kernel_key = MixLight(ContentHash{}
                                    .MixU32(kTableImplementationVersion)
                                    .MixText("kernel values")
                                    .MixText(bundle_->ContentDigest())
                                    .MixText(negative),
                                scene_light)
                           .Key();
  BuildUvTable(stock.exposure_weights, setup.scene_spectrum, *result);

  // The scanner table depends on the negative and the scanner only, so all scene lights share its
  // GPU copy.
  const auto scan_table       = ScannerTableOf(negative, weights, scanner_key);
  result->print_key           = scan_table->key;
  result->print_cube_root_xyz = scan_table->values;
  if (default_one) {
    result->negative_scan = film_negative_scan::StoredSelectionValues(setup);
  } else {
    const auto& medium    = bundle_->ScanMedium(negative);
    const auto  gamma_key = ContentHash{}
                               .MixU32(kTableImplementationVersion)
                               .MixText("scan gamma")
                               .MixText(bundle_->ContentDigest())
                               .MixText(medium.id)
                               .MixKey(scanner_key)
                               .Key();
    auto gamma = derived_gamma_.find(gamma_key.hash);
    if (gamma == derived_gamma_.end()) {
      // The pool tables stay alive here while the derivation reads them.
      std::vector<std::shared_ptr<const RecordTableStage>> held;
      const auto table_of = [&](std::string_view stock_id) {
        held.push_back(ScannerTableOf(stock_id, weights, scanner_key));
        return film_negative_scan::ScannerTableView{held.back()->values, kFilmPrintTableEdge};
      };
      const auto derived =
          film_negative_scan::DeriveMediumGamma(*bundle_, medium, weights, table_of);
      gamma = derived_gamma_.emplace(gamma_key.hash, derived).first;
      ++stats_.scan_gamma_derivations;
    }
    result->negative_scan = film_negative_scan::DeriveSelectionValues(
        *bundle_, negative, setup, weights,
        film_negative_scan::ScannerTableView{scan_table->values, kFilmPrintTableEdge},
        gamma->second);
  }
  if (!AllFinite(result->uv_exposure) || !AllFinite(result->print_cube_root_xyz)) {
    throw std::runtime_error("Film print: a table of Negative Scan of " + std::string(negative) +
                             " under " + LightText(scene_light) +
                             " has a value that is not finite");
  }
  result->print_rgba = PackPrintRgba(result->print_cube_root_xyz);
  result->kernel_values =
      film_print_kernel_tables::MakeValues<float>(*bundle_, negative, result->uv_exposure);
  const auto bytes = result->ByteSize();
  Keep<FilmPrintTableData>(results_, result_key, result, bytes);
  return result;
}

auto FilmPrintTables::Resolve(std::string_view negative, std::string_view print_material,
                              std::string_view scene_light)
    -> std::shared_ptr<const FilmPrintTableData> {
  return Resolve(negative, print_material,
                 FilmSceneLightMix{std::string(scene_light), std::string(scene_light), 0.0});
}

auto FilmPrintTables::Resolve(std::string_view negative, std::string_view print_material,
                              const FilmSceneLightMix& scene_light)
    -> std::shared_ptr<const FilmPrintTableData> {
  std::lock_guard<std::mutex> lock(mutex_);
  if (bundle_->IsReversal(negative)) {
    return ResolveReversal(negative, scene_light);
  }
  if (bundle_->IsNegativeScan(print_material)) {
    return ResolveNegativeScanLocked(
        negative, scene_light, film_negative_scan::DefaultScanner(bundle_->NegativeScanConstants()));
  }
  const auto print_key = MixLight(ContentHash{}
                                      .MixU32(kTableImplementationVersion)
                                      .MixText("print table")
                                      .MixText(bundle_->ContentDigest())
                                      .MixText(negative)
                                      .MixText(print_material),
                                  scene_light)
                             .Key();
  if (auto kept = Find(results_, print_key)) {
    return kept;
  }

  const auto selection  = bundle_->Selection(negative, print_material, scene_light);
  const auto stock      = bundle_->Negative(negative);
  const auto paper      = bundle_->PrintMaterial(print_material);

  auto       result     = std::make_shared<FilmPrintTableData>();
  result->uv_nodes      = bundle_->UvNodes();
  result->edge          = static_cast<std::uint32_t>(kFilmPrintTableEdge);
  result->print_key     = print_key;
  result->uv_key        = MixLight(ContentHash{}
                                       .MixU32(kTableImplementationVersion)
                                       .MixText("uv table")
                                       .MixText(bundle_->ContentDigest())
                                       .MixText(negative),
                                   scene_light)
                       .Key();
  result->kernel_key = MixLight(ContentHash{}
                                    .MixU32(kTableImplementationVersion)
                                    .MixText("kernel values")
                                    .MixText(bundle_->ContentDigest())
                                    .MixText(negative),
                                scene_light)
                           .Key();

  BuildUvTable(stock.exposure_weights, selection.scene_spectrum, *result);

  const auto negative_stage = NegativeStageOf(negative, print_material);
  const auto paper_stage    = PaperStageOf(print_material);

  {
    const auto            start = std::chrono::steady_clock::now();
    std::array<double, 3> contrast{};
    std::array<double, 3> surface{};
    for (std::size_t k = 0; k < contrast.size(); ++k) {
      contrast[k] = bundle_->PrintContrast() * selection.printing_gain[k];
      surface[k]  = bundle_->ViewingFlare() * paper.white_xyz[k];
    }
    const film_tables::PrintControls controls{selection.printer_balance_log10.data(),
                                              selection.aim_log_h.data(), contrast.data(),
                                              bundle_->PrintFlash(), surface.data()};
    const film_tables::PaperCurves   curves{paper.curves.stored_length, paper.curves.density.data(),
                                          paper.curves.lengths.data(),
                                          paper.curves.log_h_start.data(), paper.curves.log_h_step};
    result->print_cube_root_xyz.resize(kTableValues);
    const auto counts = film_tables::ComposePrintTable(
        negative_stage->paper_log.data(), kTableNodes, controls, curves, StatusAxes(paper),
        paper_stage->cube_root_xyz.data(), paper_stage->not_reached.data(),
        result->print_cube_root_xyz.data());
    ApplyDarkSurround(paper, result->print_cube_root_xyz);
    result->outside_curve     = counts.outside_curve;
    result->print_not_reached = counts.print_not_reached;
    ++stats_.compose_builds;
    stats_.last_compose_seconds = SecondsSince(start);
  }

  if (!AllFinite(result->uv_exposure) || !AllFinite(result->print_cube_root_xyz)) {
    throw std::runtime_error("Film print: a table of " + std::string(negative) + " on " +
                             std::string(print_material) + " under " + LightText(scene_light) +
                             " has a value that is not finite");
  }
  result->print_rgba = PackPrintRgba(result->print_cube_root_xyz);
  result->kernel_values =
      film_print_kernel_tables::MakeValues<float>(*bundle_, negative, result->uv_exposure);
  const auto bytes = result->ByteSize();
  Keep<FilmPrintTableData>(results_, print_key, result, bytes);
  return result;
}

}  // namespace alcedo
