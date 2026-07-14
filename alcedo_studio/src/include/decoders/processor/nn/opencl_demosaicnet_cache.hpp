//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "decoders/processor/nn/opencl_demosaicnet_module.hpp"
#include "opencl/opencl_context.hpp"

namespace alcedo {

// Lazy cache of hard-coded OpenCL DemosaicNet modules (RAW domain only).
//
// Keyed by OpenCL context/device identity + variant + resolved model path.
// An entry transitions unloaded → ready or failed; failed loads never publish
// a partially initialized module. Second EnsureLoaded reuses resident weights
// and kernels with no re-parse / re-upload / re-create.

enum class OpenClDemosaicNetVariant {
  Bayer,
  XTrans,
};

struct OpenClDemosaicNetLoadOptions {
  // Directory containing bayer.safetensors / xtrans.safetensors.
  // Empty → resolve via ALCEDO_DEMOASICNET_MODEL_DIR, then install/source defaults.
  std::filesystem::path model_dir;
  cl_command_queue      queue = nullptr;  // optional H2D queue during load
};

class OpenClDemosaicNetModelCache {
 public:
  static auto Instance() -> OpenClDemosaicNetModelCache&;

  OpenClDemosaicNetModelCache() = default;

  OpenClDemosaicNetModelCache(const OpenClDemosaicNetModelCache&)            = delete;
  OpenClDemosaicNetModelCache& operator=(const OpenClDemosaicNetModelCache&) = delete;

  // Lazy: no-op if already ready for this context+variant+path. Thread-safe.
  auto EnsureLoaded(OpenClDemosaicNetVariant variant,
                    const OpenClDemosaicNetLoadOptions& options = {}) -> bool;

  [[nodiscard]] auto IsLoaded(OpenClDemosaicNetVariant variant) const -> bool;
  [[nodiscard]] auto LastError() const -> std::string;
  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  // Precondition: EnsureLoaded succeeded for that variant.
  [[nodiscard]] auto Bayer() const -> const OpenClBayerDemosaicNet&;
  [[nodiscard]] auto XTrans() const -> const OpenClXTransDemosaicNet&;

  void Unload(OpenClDemosaicNetVariant variant);
  void UnloadAll();

  [[nodiscard]] static auto ResolveModelDir(const OpenClDemosaicNetLoadOptions& options = {})
      -> std::filesystem::path;

 private:
  auto LoadVariantLocked(OpenClDemosaicNetVariant variant,
                         const OpenClDemosaicNetLoadOptions& options) -> bool;

  [[nodiscard]] auto ContextIdentityLocked() const -> std::string;

  mutable std::mutex                        mutex_;
  std::unique_ptr<OpenClBayerDemosaicNet>   bayer_;
  std::unique_ptr<OpenClXTransDemosaicNet>  xtrans_;
  std::string                               bayer_identity_;
  std::string                               xtrans_identity_;
  std::string                               last_error_;
};

}  // namespace alcedo

#endif  // HAVE_OPENCL
