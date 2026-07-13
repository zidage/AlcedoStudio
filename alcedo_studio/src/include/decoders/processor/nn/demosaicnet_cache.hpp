//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include <cuda_runtime.h>

#include "decoders/processor/nn/demosaicnet_bayer.hpp"
#include "decoders/processor/nn/demosaicnet_xtrans.hpp"

namespace alcedo {

// Lazy cache of hard-coded DemosaicNet module instances (RAW domain only).
//
// Not a platform GPU context (never name this *Context next to OpenClContext /
// MetalContext). Owns immutable device weights after first EnsureLoaded per
// variant. Does not own a process-global WorkspacePool.
//
// Lifecycle: cold by default → EnsureLoaded on first NN demosaic need →
// resident for the process (optional Unload for tests / low-VRAM).

enum class DemosaicNetVariant {
  Bayer,
  XTrans,
};

struct DemosaicNetLoadOptions {
  // Directory containing bayer.safetensors / xtrans.safetensors.
  // Empty → resolve via ALCEDO_DEMOASICNET_MODEL_DIR, then install/source defaults.
  std::filesystem::path model_dir;
  cudaStream_t          stream = nullptr;  // optional async H2D during load
};

class DemosaicNetModelCache {
 public:
  // Process-wide singleton used by the RAW pipeline.
  static auto Instance() -> DemosaicNetModelCache&;

  DemosaicNetModelCache() = default;

  DemosaicNetModelCache(const DemosaicNetModelCache&)            = delete;
  DemosaicNetModelCache& operator=(const DemosaicNetModelCache&) = delete;

  // Lazy: no-op if already loaded. Thread-safe. Returns false on failure
  // (LastError() set); does not throw across the API boundary.
  auto EnsureLoaded(DemosaicNetVariant variant, const DemosaicNetLoadOptions& options = {})
      -> bool;

  [[nodiscard]] auto IsLoaded(DemosaicNetVariant variant) const -> bool;
  [[nodiscard]] auto LastError() const -> std::string;
  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  // Precondition: EnsureLoaded succeeded for that variant.
  [[nodiscard]] auto Bayer() const -> const BayerDemosaicNet&;
  [[nodiscard]] auto XTrans() const -> const XTransDemosaicNet&;

  // Drop a variant (or all). Intended for tests / optional VRAM reclaim.
  void Unload(DemosaicNetVariant variant);
  void UnloadAll();

  // Resolve model directory (explicit option → env → known source/install paths).
  [[nodiscard]] static auto ResolveModelDir(const DemosaicNetLoadOptions& options = {})
      -> std::filesystem::path;

 private:
  auto LoadVariantLocked(DemosaicNetVariant variant, const DemosaicNetLoadOptions& options)
      -> bool;

  mutable std::mutex                 mutex_;
  std::unique_ptr<BayerDemosaicNet>  bayer_;
  std::unique_ptr<XTransDemosaicNet> xtrans_;
  std::string                        last_error_;
};

}  // namespace alcedo
