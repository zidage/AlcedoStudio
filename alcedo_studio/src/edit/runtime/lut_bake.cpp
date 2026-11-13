//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/lut_bake.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "color/color_encoding_math.h"
#include "concurrency/thread_pool.hpp"
#include "edit/runtime/aces2_reference_math.h"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/display_to_ap1_math.h"

namespace alcedo {
namespace {

using color::ColorEncoding;
using color::ColorReferral;
using color::TransferFunctionId;

auto IsWorkingSpace(const ColorEncoding& encoding) -> bool {
  return encoding.id_ == color::kDefaultColorEncodingId;
}

auto IsDisplay(const ColorEncoding& encoding) -> bool {
  return encoding.referral_ == ColorReferral::DisplayReferred;
}

/// Applied in double precision: the bake runs only on the host, and a float product loses the
/// small channel of a color whose channels differ by many stops.
auto Multiply(const color::Matrix33d& m, const LutRgb& v) -> LutRgb {
  const double r = v[0], g = v[1], b = v[2];
  return {static_cast<float>(m[0] * r + m[1] * g + m[2] * b),
          static_cast<float>(m[3] * r + m[4] * g + m[5] * b),
          static_cast<float>(m[6] * r + m[7] * g + m[8] * b)};
}

auto ResolveRuntime(const ColorEncoding& encoding) -> std::shared_ptr<const Aces2ReferenceRuntime> {
  return ResolveAces2ReferenceRuntime(color::GamutPrimariesXy(encoding.gamut_),
                                      encoding.peak_luminance_nits_);
}

auto TransferOf(const ColorEncoding& encoding) -> int {
  return static_cast<int>(encoding.transfer_);
}

/// Display light (1.0 = 100 nits) to code values of display encoding @p encoding.
auto DisplayEncode(const ColorEncoding& encoding, A2rFloat3 light) -> LutRgb {
  const int tf = TransferOf(encoding);
  if (encoding.transfer_ == TransferFunctionId::St2084) {
    return {CeDisplayEncodeChannel(tf, light.x * 100.0f, 0.0f),
            CeDisplayEncodeChannel(tf, light.y * 100.0f, 0.0f),
            CeDisplayEncodeChannel(tf, light.z * 100.0f, 0.0f)};
  }
  // SDR and HLG code 1.0 is the encoding's peak.
  const float to_peak = 100.0f / encoding.peak_luminance_nits_;
  const float r = light.x * to_peak, g = light.y * to_peak, b = light.z * to_peak;
  const float gain =
      encoding.transfer_ == TransferFunctionId::Hlg ? CeHlgDisplayGain(r, g, b) : 1.0f;
  return {CeDisplayEncodeChannel(tf, r, gain), CeDisplayEncodeChannel(tf, g, gain),
          CeDisplayEncodeChannel(tf, b, gain)};
}

/// Code values of display encoding @p encoding to display light (1.0 = 100 nits), with the
/// raster input rules (raster_linearize_math.h): codes below 0 have no light, PQ and HLG codes
/// above 1 clamp, HLG applies the luminance OOTF of the reference display.
auto DisplayDecode(const ColorEncoding& encoding, const LutRgb& code) -> A2rFloat3 {
  const float r = std::max(code[0], 0.0f), g = std::max(code[1], 0.0f), b = std::max(code[2], 0.0f);
  if (encoding.transfer_ == TransferFunctionId::St2084) {
    return A2rMake3(CePqDecode(std::min(r, 1.0f)) * 100.0f, CePqDecode(std::min(g, 1.0f)) * 100.0f,
                    CePqDecode(std::min(b, 1.0f)) * 100.0f);
  }
  const float from_peak = encoding.peak_luminance_nits_ / 100.0f;
  if (encoding.transfer_ == TransferFunctionId::Hlg) {
    const float sr = CeHlgDecode(std::min(r, 1.0f)), sg = CeHlgDecode(std::min(g, 1.0f)),
                sb = CeHlgDecode(std::min(b, 1.0f));
    const float k =
        CeHlgOotfGain(CE_BT2100_LUMA_R * sr + CE_BT2100_LUMA_G * sg + CE_BT2100_LUMA_B * sb) *
        from_peak;
    return A2rMake3(sr * k, sg * k, sb * k);
  }
  const int tf = TransferOf(encoding);
  return A2rMake3(CeDecode(tf, r) * from_peak, CeDecode(tf, g) * from_peak,
                  CeDecode(tf, b) * from_peak);
}

/// Worker pool of the bake. Leaked like the LUT caches: render workers may bake while static
/// teardown runs.
auto BakeWorkers() -> ThreadPool& {
  static auto* pool = new ThreadPool(std::max(1U, std::thread::hardware_concurrency()));
  return *pool;
}

/// Identity of one composite table: the source content, its domain and the two encodings.
struct CompositeKey {
  std::uint64_t        source_key = 0;
  std::array<float, 6> domain{};
  std::string          input_id;
  std::string          output_id;

  [[nodiscard]] auto   operator==(const CompositeKey& other) const -> bool {
    return source_key == other.source_key &&
           std::memcmp(domain.data(), other.domain.data(), sizeof(domain)) == 0 &&
           input_id == other.input_id && output_id == other.output_id;
  }
};

struct CompositeCacheEntry {
  CompositeKey                          key;
  std::uint64_t                         lru_tick = 0;
  std::shared_ptr<const PackedGradeLut> table;
};

/**
 * @brief Process-wide memoization of baked composite tables, shared by all GPU backends.
 *
 * A composite table is 65^3 RGBA32F (about 4.4 MiB) and costs one bake, so up to
 * @ref kMaxCachedCompositeTables entries are kept with LRU eviction, like the packed source
 * cache in grade_lut.cpp. Entries are immutable once published. Leaked for the same reason as
 * that cache.
 */
struct CompositeCache {
  std::mutex                       mutex;
  std::vector<CompositeCacheEntry> entries;
  std::uint64_t                    lru_clock = 0;
};

constexpr std::size_t      kMaxCachedCompositeTables = 16;
std::atomic<std::uint64_t> g_bake_count{0};

auto                       SharedCompositeCache() -> CompositeCache& {
  static auto* cache = new CompositeCache();
  return *cache;
}

auto FindComposite(CompositeCache& cache, const CompositeKey& key)
    -> std::shared_ptr<const PackedGradeLut> {
  for (auto& entry : cache.entries) {
    if (entry.key == key) {
      entry.lru_tick = ++cache.lru_clock;
      return entry.table;
    }
  }
  return nullptr;
}

void StoreComposite(CompositeCache& cache, CompositeKey key,
                    std::shared_ptr<const PackedGradeLut> table) {
  if (cache.entries.size() >= kMaxCachedCompositeTables) {
    const auto oldest =
        std::min_element(cache.entries.begin(), cache.entries.end(),
                         [](const auto& a, const auto& b) { return a.lru_tick < b.lru_tick; });
    cache.entries.erase(oldest);
  }
  cache.entries.push_back({std::move(key), ++cache.lru_clock, std::move(table)});
}

auto RequireEncoding(std::string_view id) -> const ColorEncoding& {
  const auto* encoding = color::FindColorEncoding(id);
  if (encoding == nullptr) {
    throw std::invalid_argument("LMT color encoding '" + std::string(id) +
                                "' is not in the catalog");
  }
  return *encoding;
}

}  // namespace

auto HasUnitDomain(const PackedGradeLut& table) -> bool {
  for (std::size_t c = 0; c < 3; ++c) {
    if (table.domain_min[c] != 0.0f || table.domain_max[c] != 1.0f) {
      return false;
    }
  }
  return true;
}

auto SampleLutTable(const PackedGradeLut& table, const LutRgb& code) -> LutRgb {
  const auto                   edge   = table.edge;
  const auto*                  voxels = reinterpret_cast<const float*>(table.rgba.data());
  const float                  max_i  = static_cast<float>(edge - 1U);
  std::array<std::uint32_t, 3> lo{};
  std::array<std::uint32_t, 3> hi{};
  std::array<float, 3>         t{};
  for (std::size_t c = 0; c < 3; ++c) {
    const float span  = table.domain_max[c] - table.domain_min[c];
    const float scale = span != 0.0f ? 1.0f / span : 0.0f;
    const float u     = std::clamp((code[c] - table.domain_min[c]) * scale, 0.0f, 1.0f);
    const float pos   = std::clamp(u * max_i, 0.0f, max_i);
    lo[c]             = static_cast<std::uint32_t>(pos);
    hi[c]             = lo[c] + 1U < edge ? lo[c] + 1U : edge - 1U;
    t[c]              = pos - static_cast<float>(lo[c]);
  }
  const auto at = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z, std::size_t c) {
    return voxels[((static_cast<std::size_t>(z) * edge + y) * edge + x) * 4 + c];
  };
  LutRgb out{};
  for (std::size_t c = 0; c < 3; ++c) {
    const float c00 = at(lo[0], lo[1], lo[2], c) +
                      (at(hi[0], lo[1], lo[2], c) - at(lo[0], lo[1], lo[2], c)) * t[0];
    const float c10 = at(lo[0], hi[1], lo[2], c) +
                      (at(hi[0], hi[1], lo[2], c) - at(lo[0], hi[1], lo[2], c)) * t[0];
    const float c01 = at(lo[0], lo[1], hi[2], c) +
                      (at(hi[0], lo[1], hi[2], c) - at(lo[0], lo[1], hi[2], c)) * t[0];
    const float c11 = at(lo[0], hi[1], hi[2], c) +
                      (at(hi[0], hi[1], hi[2], c) - at(lo[0], hi[1], hi[2], c)) * t[0];
    const float c0 = c00 + (c10 - c00) * t[1];
    const float c1 = c01 + (c11 - c01) * t[1];
    out[c]         = c0 + (c1 - c0) * t[2];
  }
  return out;
}

LmtEncodingConversion::LmtEncodingConversion(const ColorEncoding& input,
                                             const ColorEncoding& output)
    : input_(&input), output_(&output) {
  if (IsDisplay(input)) {
    input_runtime_ = ResolveRuntime(input);
    input_matrix_ =
        color::GamutConversionMatrix(color::ColorGamutId::Ap1, color::ColorGamutId::Ap0);
  } else if (!IsWorkingSpace(input)) {
    input_matrix_ = color::GamutConversionMatrix(color::ColorGamutId::Ap1, input.gamut_);
  }
  if (IsDisplay(output)) {
    output_runtime_ = ResolveRuntime(output);
  } else if (!IsWorkingSpace(output)) {
    output_matrix_ = color::GamutConversionMatrix(output.gamut_, color::ColorGamutId::Ap1);
  }
}

auto LmtEncodingConversion::AcesccToLutInput(const LutRgb& acescc) const -> LutRgb {
  if (IsWorkingSpace(*input_)) {
    return acescc;
  }
  const LutRgb ap1 = {CeAcesccDecode(acescc[0]), CeAcesccDecode(acescc[1]),
                      CeAcesccDecode(acescc[2])};
  if (input_runtime_ != nullptr) {
    const LutRgb ap0 = Multiply(input_matrix_, ap1);
    return DisplayEncode(
        *input_, A2rAp0ToDisplay(A2rMake3(ap0[0], ap0[1], ap0[2]), input_runtime_->packed_.data()));
  }
  const LutRgb linear = Multiply(input_matrix_, ap1);
  const int    tf     = TransferOf(*input_);
  return {CeEncode(tf, linear[0]), CeEncode(tf, linear[1]), CeEncode(tf, linear[2])};
}

auto LmtEncodingConversion::LutOutputToAcescc(const LutRgb& code) const -> LutRgb {
  if (IsWorkingSpace(*output_)) {
    return code;
  }
  if (output_runtime_ != nullptr) {
    // The R2 raster conversion: ACES 2.0 inverse, AP0 to AP1, clamp to the forward limit.
    const A2rFloat3 acescc =
        D2aSourceToAcesccAp1(DisplayDecode(*output_, code), output_runtime_->packed_.data());
    return {acescc.x, acescc.y, acescc.z};
  }
  const int    tf     = TransferOf(*output_);
  const LutRgb linear = {CeDecode(tf, code[0]), CeDecode(tf, code[1]), CeDecode(tf, code[2])};
  const LutRgb ap1    = Multiply(output_matrix_, linear);
  return {CeAcesccEncode(ap1[0]), CeAcesccEncode(ap1[1]), CeAcesccEncode(ap1[2])};
}

auto LmtEncodingConversion::Compose(const PackedGradeLut& source, const LutRgb& acescc) const
    -> LutRgb {
  return LutOutputToAcescc(SampleLutTable(source, AcesccToLutInput(acescc)));
}

auto BakeLmtCompositeTable(const PackedGradeLut& source, const ColorEncoding& input,
                           const ColorEncoding& output) -> std::vector<std::byte> {
  if (source.edge < 2U || source.rgba.size() != static_cast<std::size_t>(source.edge) *
                                                    source.edge * source.edge * 4 * sizeof(float)) {
    throw std::invalid_argument("BakeLmtCompositeTable: the source LUT has no 3D table");
  }
  const LmtEncodingConversion conversion(input, output);
  constexpr std::size_t       kEdge = kLmtCompositeEdge;
  std::vector<std::byte>      composite(kEdge * kEdge * kEdge * 4 * sizeof(float));
  auto*                       out        = reinterpret_cast<float*>(composite.data());

  const auto                  bake_slice = [&conversion, &source, out](std::size_t blue) {
    constexpr float kStep = 1.0f / static_cast<float>(kEdge - 1);
    for (std::size_t green = 0; green < kEdge; ++green) {
      for (std::size_t red = 0; red < kEdge; ++red) {
        const LutRgb node = {static_cast<float>(red) * kStep, static_cast<float>(green) * kStep,
                             static_cast<float>(blue) * kStep};
        const LutRgb value = conversion.Compose(source, node);
        float*       voxel = out + ((blue * kEdge + green) * kEdge + red) * 4;
        voxel[0]           = value[0];
        voxel[1]           = value[1];
        voxel[2]           = value[2];
        voxel[3]           = 1.0f;
      }
    }
  };
  auto done = std::make_shared<std::latch>(static_cast<std::ptrdiff_t>(kEdge));
  for (std::size_t blue = 0; blue < kEdge; ++blue) {
    // Compose does not throw: it is arithmetic on validated, immutable inputs.
    BakeWorkers().Submit([bake_slice, blue, done] {
      bake_slice(blue);
      done->count_down();
    });
  }
  done->wait();
  return composite;
}

auto ResolveLmtSampledTable(std::shared_ptr<const PackedGradeLut> source,
                            std::string_view input_encoding_id, std::string_view output_encoding_id)
    -> std::shared_ptr<const PackedGradeLut> {
  const auto& input  = RequireEncoding(input_encoding_id);
  const auto& output = RequireEncoding(output_encoding_id);
  if (source == nullptr ||
      (IsWorkingSpace(input) && IsWorkingSpace(output) && HasUnitDomain(*source))) {
    return source;
  }
  CompositeKey key;
  key.source_key = source->key.hash;
  std::copy(source->domain_min.begin(), source->domain_min.end(), key.domain.begin());
  std::copy(source->domain_max.begin(), source->domain_max.end(), key.domain.begin() + 3);
  key.input_id  = std::string(input.id_);
  key.output_id = std::string(output.id_);

  auto& cache   = SharedCompositeCache();
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (auto table = FindComposite(cache, key)) {
      return table;
    }
  }

  auto table  = std::make_shared<PackedGradeLut>();
  table->rgba = BakeLmtCompositeTable(*source, input, output);
  table->edge = kLmtCompositeEdge;
  ContentHash hash;
  hash.MixBytes(table->rgba);
  hash.MixU32(table->edge);
  table->key = hash.Key();
  g_bake_count.fetch_add(1, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(cache.mutex);
  // A concurrent bake for the same key wins; both baked identical content.
  if (auto raced = FindComposite(cache, key)) {
    return raced;
  }
  StoreComposite(cache, std::move(key), table);
  return table;
}

auto LmtCompositeBakeCount() -> std::uint64_t {
  return g_bake_count.load(std::memory_order_relaxed);
}

}  // namespace alcedo
