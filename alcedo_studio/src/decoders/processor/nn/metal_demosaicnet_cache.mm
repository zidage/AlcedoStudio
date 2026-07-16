//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/nn/metal_demosaicnet_cache.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "metal/metal_context.hpp"
#include "nn/safetensors.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

[[nodiscard]] auto ElapsedMs(const Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] auto VariantFileName(MetalDemosaicNetVariant variant) -> const char* {
  switch (variant) {
    case MetalDemosaicNetVariant::Bayer:
      return "bayer.safetensors";
    case MetalDemosaicNetVariant::XTrans:
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

auto MetalDemosaicNetModelCache::Instance() -> MetalDemosaicNetModelCache& {
  static MetalDemosaicNetModelCache instance;
  return instance;
}

auto MetalDemosaicNetModelCache::ResolveModelDir(const MetalDemosaicNetLoadOptions& options)
    -> fs::path {
  if (!options.model_dir.empty()) {
    return options.model_dir;
  }

  if (const char* env = std::getenv("ALCEDO_DEMOASICNET_MODEL_DIR");
      env != nullptr && env[0] != '\0') {
    return fs::path(env);
  }

#ifdef ALCEDO_DEMOASICNET_MODEL_DIR
  {
    const fs::path compile_time{ALCEDO_DEMOASICNET_MODEL_DIR};
    if (DirHasModels(compile_time)) {
      return compile_time;
    }
  }
#endif

  const char* candidates[] = {
      "alcedo_studio/src/config/models",
      "../alcedo_studio/src/config/models",
      "../../alcedo_studio/src/config/models",
      "../../../alcedo_studio/src/config/models",
      "src/config/models",
      "../src/config/models",
  };
  for (const char* c : candidates) {
    fs::path p(c);
    if (DirHasModels(p)) {
      return p;
    }
  }
  return {};
}

auto MetalDemosaicNetModelCache::DeviceIdentityLocked() const -> std::string {
  auto& ctx = MetalContext::Instance();
  MTL::Device* device = ctx.Device();
  if (device == nullptr) {
    return {};
  }
  const char* name = device->name() != nullptr ? device->name()->utf8String() : "unknown";
  return std::to_string(reinterpret_cast<std::uintptr_t>(device)) + "|" +
         (name != nullptr ? name : "unknown");
}

auto MetalDemosaicNetModelCache::EnsureLoaded(MetalDemosaicNetVariant variant,
                                              const MetalDemosaicNetLoadOptions& options) -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string identity = DeviceIdentityLocked();
  const fs::path    dir      = ResolveModelDir(options);
  const std::string path_key =
      dir.empty() ? std::string{} : (dir / VariantFileName(variant)).string();
  const std::string full_key = identity + "|" + path_key;

  switch (variant) {
    case MetalDemosaicNetVariant::Bayer:
      if (bayer_ != nullptr && bayer_->ready() && bayer_identity_ == full_key) {
        return true;
      }
      break;
    case MetalDemosaicNetVariant::XTrans:
      if (xtrans_ != nullptr && xtrans_->ready() && xtrans_identity_ == full_key) {
        return true;
      }
      break;
  }
  return LoadVariantLocked(variant, options);
}

auto MetalDemosaicNetModelCache::IsLoaded(MetalDemosaicNetVariant variant) const -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  switch (variant) {
    case MetalDemosaicNetVariant::Bayer:
      return bayer_ != nullptr && bayer_->ready();
    case MetalDemosaicNetVariant::XTrans:
      return xtrans_ != nullptr && xtrans_->ready();
  }
  return false;
}

auto MetalDemosaicNetModelCache::LastError() const -> std::string {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

auto MetalDemosaicNetModelCache::ResidentWeightBytes() const -> std::size_t {
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

auto MetalDemosaicNetModelCache::OwnedBufferBytes() const -> std::size_t {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t total = 0;
  if (bayer_ != nullptr) {
    total += bayer_->OwnedBufferBytes();
  }
  if (xtrans_ != nullptr) {
    total += xtrans_->OwnedBufferBytes();
  }
  return total;
}

auto MetalDemosaicNetModelCache::parse_count() const -> std::uint64_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return parse_count_;
}

auto MetalDemosaicNetModelCache::compile_count() const -> std::uint64_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return compile_count_;
}

auto MetalDemosaicNetModelCache::load_attempt_count() const -> std::uint64_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return load_attempt_count_;
}

auto MetalDemosaicNetModelCache::input_output_allocation_count() const -> std::uint64_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return input_output_allocation_count_;
}

auto MetalDemosaicNetModelCache::last_parse_ms() const -> double {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_parse_ms_;
}

auto MetalDemosaicNetModelCache::last_compile_ms() const -> double {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_compile_ms_;
}

auto MetalDemosaicNetModelCache::Bayer() const -> const MetalBayerDemosaicNet& {
  std::lock_guard<std::mutex> lock(mutex_);
  if (bayer_ == nullptr || !bayer_->ready()) {
    throw std::runtime_error("MetalDemosaicNetModelCache::Bayer: variant not loaded");
  }
  return *bayer_;
}

auto MetalDemosaicNetModelCache::XTrans() const -> const MetalXTransDemosaicNet& {
  std::lock_guard<std::mutex> lock(mutex_);
  if (xtrans_ == nullptr || !xtrans_->ready()) {
    throw std::runtime_error("MetalDemosaicNetModelCache::XTrans: variant not loaded");
  }
  return *xtrans_;
}

void MetalDemosaicNetModelCache::Unload(MetalDemosaicNetVariant variant) {
  std::lock_guard<std::mutex> lock(mutex_);
  switch (variant) {
    case MetalDemosaicNetVariant::Bayer:
      bayer_.reset();
      bayer_identity_.clear();
      break;
    case MetalDemosaicNetVariant::XTrans:
      xtrans_.reset();
      xtrans_identity_.clear();
      break;
  }
}

void MetalDemosaicNetModelCache::UnloadAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  bayer_.reset();
  xtrans_.reset();
  bayer_identity_.clear();
  xtrans_identity_.clear();
}

auto MetalDemosaicNetModelCache::LoadVariantLocked(MetalDemosaicNetVariant variant,
                                                   const MetalDemosaicNetLoadOptions& options)
    -> bool {
  ++load_attempt_count_;
  last_parse_ms_   = 0.0;
  last_compile_ms_ = 0.0;
  try {
    if (MetalContext::Instance().Device() == nullptr) {
      last_error_ = "Metal context device is null";
      return false;
    }

    const fs::path dir = ResolveModelDir(options);
    if (dir.empty()) {
      last_error_ =
          "DemosaicNet model directory not found (set ALCEDO_DEMOASICNET_MODEL_DIR "
          "or pass MetalDemosaicNetLoadOptions::model_dir)";
      return false;
    }
    const fs::path path = dir / VariantFileName(variant);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
      last_error_ = "DemosaicNet weight file missing: " + path.string();
      return false;
    }

    // Parse + validate + compile into a staging module. Publish only on success.
    ++parse_count_;
    const auto parse_start = Clock::now();
    auto dto = nn::LoadSafetensors(path);
    last_parse_ms_ = ElapsedMs(parse_start);

    const std::string identity = DeviceIdentityLocked() + "|" + path.string();

    switch (variant) {
      case MetalDemosaicNetVariant::Bayer: {
        auto module = std::make_unique<MetalBayerDemosaicNet>();
        const auto compile_start = Clock::now();
        module->LoadAndCompile(dto, MetalContext::Instance().Device());
        last_compile_ms_ = ElapsedMs(compile_start);
        compile_count_ += module->compile_count();
        input_output_allocation_count_ += module->input_output_allocation_count();
        bayer_          = std::move(module);
        bayer_identity_ = identity;
        break;
      }
      case MetalDemosaicNetVariant::XTrans: {
        auto module = std::make_unique<MetalXTransDemosaicNet>();
        const auto compile_start = Clock::now();
        module->LoadAndCompile(dto, MetalContext::Instance().Device());
        last_compile_ms_ = ElapsedMs(compile_start);
        compile_count_ += module->compile_count();
        input_output_allocation_count_ += module->input_output_allocation_count();
        xtrans_          = std::move(module);
        xtrans_identity_ = identity;
        break;
      }
    }
    last_error_.clear();
    return true;
  } catch (const std::exception& e) {
    last_error_ = e.what();
    // Do not publish partial state.
    return false;
  }
}

}  // namespace alcedo

#endif  // HAVE_METAL
