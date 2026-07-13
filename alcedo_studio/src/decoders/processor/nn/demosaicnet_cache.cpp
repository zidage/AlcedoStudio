//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/demosaicnet_cache.hpp"

#include <cstdlib>
#include <stdexcept>
#include <utility>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

#include "cuda/nn/safetensors.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] auto VariantFileName(DemosaicNetVariant variant) -> const char* {
  switch (variant) {
    case DemosaicNetVariant::Bayer:
      return "bayer.safetensors";
    case DemosaicNetVariant::XTrans:
      return "xtrans.safetensors";
  }
  return "bayer.safetensors";
}

[[nodiscard]] auto DirHasModels(const fs::path& dir) -> bool {
  std::error_code ec;
  return fs::is_regular_file(dir / "bayer.safetensors", ec) ||
         fs::is_regular_file(dir / "xtrans.safetensors", ec);
}

}  // namespace

auto DemosaicNetModelCache::Instance() -> DemosaicNetModelCache& {
  static DemosaicNetModelCache instance;
  return instance;
}

auto DemosaicNetModelCache::ResolveModelDir(const DemosaicNetLoadOptions& options) -> fs::path {
  if (!options.model_dir.empty()) {
    return options.model_dir;
  }

#if defined(_MSC_VER)
  char* env_buf = nullptr;
  std::size_t env_len = 0;
  if (_dupenv_s(&env_buf, &env_len, "ALCEDO_DEMOASICNET_MODEL_DIR") == 0 && env_buf != nullptr &&
      env_buf[0] != '\0') {
    fs::path result(env_buf);
    free(env_buf);
    return result;
  }
  free(env_buf);
#else
  if (const char* env = std::getenv("ALCEDO_DEMOASICNET_MODEL_DIR");
      env != nullptr && env[0] != '\0') {
    return fs::path(env);
  }
#endif

#ifdef ALCEDO_DEMOASICNET_MODEL_DIR
  {
    const fs::path compile_time{ALCEDO_DEMOASICNET_MODEL_DIR};
    if (DirHasModels(compile_time)) {
      return compile_time;
    }
  }
#endif

  // Dev convenience: walk common layouts relative to CWD / known absolute source tree.
  const char* candidates[] = {
      "alcedo_studio/src/config/models",
      "../alcedo_studio/src/config/models",
      "../../alcedo_studio/src/config/models",
      "../../../alcedo_studio/src/config/models",
      "src/config/models",
      "../src/config/models",
      "D:/Projects/pu-erh_lab/alcedo_studio/src/config/models",
  };
  for (const char* c : candidates) {
    fs::path p(c);
    if (DirHasModels(p)) {
      return p;
    }
  }
  return {};
}

auto DemosaicNetModelCache::EnsureLoaded(DemosaicNetVariant variant,
                                         const DemosaicNetLoadOptions& options) -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  switch (variant) {
    case DemosaicNetVariant::Bayer:
      if (bayer_ != nullptr && bayer_->weights_loaded()) {
        return true;
      }
      break;
    case DemosaicNetVariant::XTrans:
      if (xtrans_ != nullptr && xtrans_->weights_loaded()) {
        return true;
      }
      break;
  }
  return LoadVariantLocked(variant, options);
}

auto DemosaicNetModelCache::IsLoaded(DemosaicNetVariant variant) const -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  switch (variant) {
    case DemosaicNetVariant::Bayer:
      return bayer_ != nullptr && bayer_->weights_loaded();
    case DemosaicNetVariant::XTrans:
      return xtrans_ != nullptr && xtrans_->weights_loaded();
  }
  return false;
}

auto DemosaicNetModelCache::LastError() const -> std::string {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

auto DemosaicNetModelCache::ResidentWeightBytes() const -> std::size_t {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t total = 0;
  if (bayer_ != nullptr) {
    total += bayer_->ResidentWeightBytes();
  }
  if (xtrans_ != nullptr) {
    total += xtrans_->ResidentWeightBytes();
  }
  return total;
}

auto DemosaicNetModelCache::Bayer() const -> const BayerDemosaicNet& {
  std::lock_guard<std::mutex> lock(mutex_);
  if (bayer_ == nullptr || !bayer_->weights_loaded()) {
    throw std::runtime_error("DemosaicNetModelCache::Bayer: variant not loaded");
  }
  return *bayer_;
}

auto DemosaicNetModelCache::XTrans() const -> const XTransDemosaicNet& {
  std::lock_guard<std::mutex> lock(mutex_);
  if (xtrans_ == nullptr || !xtrans_->weights_loaded()) {
    throw std::runtime_error("DemosaicNetModelCache::XTrans: variant not loaded");
  }
  return *xtrans_;
}

void DemosaicNetModelCache::Unload(DemosaicNetVariant variant) {
  std::lock_guard<std::mutex> lock(mutex_);
  switch (variant) {
    case DemosaicNetVariant::Bayer:
      bayer_.reset();
      break;
    case DemosaicNetVariant::XTrans:
      xtrans_.reset();
      break;
  }
}

void DemosaicNetModelCache::UnloadAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  bayer_.reset();
  xtrans_.reset();
}

auto DemosaicNetModelCache::LoadVariantLocked(DemosaicNetVariant variant,
                                              const DemosaicNetLoadOptions& options) -> bool {
  try {
    const fs::path dir = ResolveModelDir(options);
    if (dir.empty()) {
      last_error_ = "DemosaicNet model directory not found (set ALCEDO_DEMOASICNET_MODEL_DIR "
                    "or pass DemosaicNetLoadOptions::model_dir)";
      return false;
    }
    const fs::path path = dir / VariantFileName(variant);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
      last_error_ = "DemosaicNet weight file missing: " + path.string();
      return false;
    }

    auto dto = cuda::nn::LoadSafetensors(path);

    switch (variant) {
      case DemosaicNetVariant::Bayer: {
        auto module = std::make_unique<BayerDemosaicNet>();
        module->LoadWeights(dto, options.stream);
        bayer_ = std::move(module);
        break;
      }
      case DemosaicNetVariant::XTrans: {
        auto module = std::make_unique<XTransDemosaicNet>();
        module->LoadWeights(dto, options.stream);
        xtrans_ = std::move(module);
        break;
      }
    }
    last_error_.clear();
    return true;
  } catch (const std::exception& e) {
    last_error_ = e.what();
    return false;
  }
}

}  // namespace alcedo
