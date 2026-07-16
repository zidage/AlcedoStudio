//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "decoders/processor/nn/metal_demosaicnet_module.hpp"

namespace alcedo {

// Lazy cache of fixed Metal MPSGraph DemosaicNet modules (RAW domain only).
//
// Two independent entries (Bayer / X-Trans). Each entry is either empty or
// ready. A failed load leaves the entry empty and does not publish a partial
// graph. Once ready, the second EnsureLoaded reuses the compiled executable and
// tile buffers with no re-parse and no re-compile.
//
// The cache never holds an alternate implementation, precision mode, or
// fallback network.

enum class MetalDemosaicNetVariant {
  Bayer,
  XTrans,
};

struct MetalDemosaicNetLoadOptions {
  // Directory containing bayer.safetensors / xtrans.safetensors.
  // Empty → resolve via ALCEDO_DEMOASICNET_MODEL_DIR, then install/source defaults.
  std::filesystem::path model_dir;
};

class MetalDemosaicNetModelCache {
 public:
  static auto Instance() -> MetalDemosaicNetModelCache&;

  MetalDemosaicNetModelCache() = default;

  MetalDemosaicNetModelCache(const MetalDemosaicNetModelCache&)            = delete;
  MetalDemosaicNetModelCache& operator=(const MetalDemosaicNetModelCache&) = delete;

  // Lazy: no-op if already ready for this device+variant+path. Thread-safe.
  // Returns false on failure (LastError set); does not throw across the API.
  auto EnsureLoaded(MetalDemosaicNetVariant variant,
                    const MetalDemosaicNetLoadOptions& options = {}) -> bool;

  [[nodiscard]] auto IsLoaded(MetalDemosaicNetVariant variant) const -> bool;
  [[nodiscard]] auto LastError() const -> std::string;
  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;
  [[nodiscard]] auto OwnedBufferBytes() const -> std::size_t;

  // Test / instrumentation counters. Never decrease except on Unload.
  [[nodiscard]] auto parse_count() const -> std::uint64_t;
  [[nodiscard]] auto compile_count() const -> std::uint64_t;
  [[nodiscard]] auto load_attempt_count() const -> std::uint64_t;
  [[nodiscard]] auto input_output_allocation_count() const -> std::uint64_t;
  [[nodiscard]] auto last_parse_ms() const -> double;
  [[nodiscard]] auto last_compile_ms() const -> double;

  // Precondition: EnsureLoaded succeeded for that variant.
  [[nodiscard]] auto Bayer() const -> const MetalBayerDemosaicNet&;
  [[nodiscard]] auto XTrans() const -> const MetalXTransDemosaicNet&;

  void Unload(MetalDemosaicNetVariant variant);
  void UnloadAll();

  [[nodiscard]] static auto ResolveModelDir(const MetalDemosaicNetLoadOptions& options = {})
      -> std::filesystem::path;

 private:
  auto LoadVariantLocked(MetalDemosaicNetVariant variant,
                         const MetalDemosaicNetLoadOptions& options) -> bool;

  [[nodiscard]] auto DeviceIdentityLocked() const -> std::string;

  mutable std::mutex                         mutex_;
  std::unique_ptr<MetalBayerDemosaicNet>     bayer_;
  std::unique_ptr<MetalXTransDemosaicNet>    xtrans_;
  std::string                                bayer_identity_;
  std::string                                xtrans_identity_;
  std::string                                last_error_;
  std::uint64_t                              parse_count_                 = 0;
  std::uint64_t                              compile_count_               = 0;
  std::uint64_t                              load_attempt_count_          = 0;
  std::uint64_t                              input_output_allocation_count_ = 0;
  double                                     last_parse_ms_               = 0.0;
  double                                     last_compile_ms_             = 0.0;
};

}  // namespace alcedo

#endif  // HAVE_METAL
