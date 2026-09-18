//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/nn/opencl_demosaicnet_cache.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "nn/safetensors.hpp"
#include "opencl/opencl_backend_program_registry.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] auto VariantFileName(OpenClDemosaicNetVariant variant) -> const char* {
  switch (variant) {
    case OpenClDemosaicNetVariant::Bayer:
      return "bayer.safetensors";
    case OpenClDemosaicNetVariant::XTrans:
      return "xtrans.safetensors";
  }
  return "bayer.safetensors";
}

[[nodiscard]] auto DirHasModels(const fs::path& dir) -> bool {
  std::error_code ec;
  return fs::is_regular_file(dir / "bayer.safetensors", ec) ||
         fs::is_regular_file(dir / "xtrans.safetensors", ec);
}

[[nodiscard]] auto GetExecutableDir() -> fs::path {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD copied =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) {
      return {};
    }
    if (copied < buffer.size()) {
      buffer.resize(copied);
      return fs::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size == 0) {
    return {};
  }
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  return fs::path(buffer).parent_path();
#else
  return {};
#endif
}

}  // namespace

auto OpenClDemosaicNetModelCache::Instance() -> OpenClDemosaicNetModelCache& {
  static OpenClDemosaicNetModelCache instance;
  return instance;
}

auto OpenClNeuralDecodeMutex() -> std::mutex& {
  static std::mutex mutex;
  return mutex;
}

auto OpenClDemosaicNetModelCache::ResolveModelDir(const OpenClDemosaicNetLoadOptions& options)
    -> fs::path {
  if (!options.model_dir.empty()) {
    return options.model_dir;
  }

#if defined(_MSC_VER)
  char*       env_buf = nullptr;
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

  const fs::path exe_dir = GetExecutableDir();
  if (!exe_dir.empty()) {
    const fs::path install_candidates[] = {
        exe_dir / "config" / "models",
        exe_dir / "models",
    };
    for (const fs::path& candidate : install_candidates) {
      if (DirHasModels(candidate)) {
        return candidate;
      }
    }
  }

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

auto OpenClDemosaicNetModelCache::ContextIdentityLocked() const -> std::string {
  auto& ctx = OpenClContext::Instance();
  if (!ctx.IsInitialized()) {
    return {};
  }
  // Pointer identity of the process-wide context is stable for the app lifetime.
  // Device name provides human-readable diagnostics when comparing cache keys.
  return std::to_string(reinterpret_cast<std::uintptr_t>(ctx.Context())) + "|" +
         ctx.Capabilities().name;
}

auto OpenClDemosaicNetModelCache::EnsureLoaded(OpenClDemosaicNetVariant variant,
                                               const OpenClDemosaicNetLoadOptions& options)
    -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string identity = ContextIdentityLocked();
  const fs::path    dir      = ResolveModelDir(options);
  const std::string path_key =
      dir.empty() ? std::string{} : (dir / VariantFileName(variant)).string();
  const std::string full_key = identity + "|" + path_key;

  switch (variant) {
    case OpenClDemosaicNetVariant::Bayer:
      if (bayer_ != nullptr && bayer_->weights_loaded() && bayer_identity_ == full_key) {
        return true;
      }
      break;
    case OpenClDemosaicNetVariant::XTrans:
      if (xtrans_ != nullptr && xtrans_->weights_loaded() && xtrans_identity_ == full_key) {
        return true;
      }
      break;
  }
  return LoadVariantLocked(variant, options);
}

auto OpenClDemosaicNetModelCache::IsLoaded(OpenClDemosaicNetVariant variant) const -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  switch (variant) {
    case OpenClDemosaicNetVariant::Bayer:
      return bayer_ != nullptr && bayer_->weights_loaded();
    case OpenClDemosaicNetVariant::XTrans:
      return xtrans_ != nullptr && xtrans_->weights_loaded();
  }
  return false;
}

auto OpenClDemosaicNetModelCache::LastError() const -> std::string {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

auto OpenClDemosaicNetModelCache::ResidentWeightBytes() const -> std::size_t {
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

auto OpenClDemosaicNetModelCache::Bayer() const -> const OpenClBayerDemosaicNet& {
  std::lock_guard<std::mutex> lock(mutex_);
  if (bayer_ == nullptr || !bayer_->weights_loaded()) {
    throw std::runtime_error("OpenClDemosaicNetModelCache::Bayer: variant not loaded");
  }
  return *bayer_;
}

auto OpenClDemosaicNetModelCache::XTrans() const -> const OpenClXTransDemosaicNet& {
  std::lock_guard<std::mutex> lock(mutex_);
  if (xtrans_ == nullptr || !xtrans_->weights_loaded()) {
    throw std::runtime_error("OpenClDemosaicNetModelCache::XTrans: variant not loaded");
  }
  return *xtrans_;
}

void OpenClDemosaicNetModelCache::Unload(OpenClDemosaicNetVariant variant) {
  std::lock_guard<std::mutex> lock(mutex_);
  switch (variant) {
    case OpenClDemosaicNetVariant::Bayer:
      bayer_.reset();
      bayer_identity_.clear();
      break;
    case OpenClDemosaicNetVariant::XTrans:
      xtrans_.reset();
      xtrans_identity_.clear();
      break;
  }
}

void OpenClDemosaicNetModelCache::UnloadAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  bayer_.reset();
  xtrans_.reset();
  bayer_identity_.clear();
  xtrans_identity_.clear();
}

auto OpenClDemosaicNetModelCache::LoadVariantLocked(OpenClDemosaicNetVariant variant,
                                                    const OpenClDemosaicNetLoadOptions& options)
    -> bool {
  try {
    if (!OpenClContext::Instance().IsInitialized()) {
      last_error_ = "OpenCL context not initialized";
      return false;
    }

    const fs::path dir = ResolveModelDir(options);
    if (dir.empty()) {
      last_error_ =
          "DemosaicNet model directory not found (set ALCEDO_DEMOASICNET_MODEL_DIR "
          "or pass OpenClDemosaicNetLoadOptions::model_dir)";
      return false;
    }
    const fs::path path = dir / VariantFileName(variant);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
      last_error_ = "DemosaicNet weight file missing: " + path.string();
      return false;
    }

    // Parse + validate into a staging module. Only publish on full success so a
    // failed topology never becomes a cache hit.
    auto dto = nn::LoadSafetensors(path);
    RegisterOpenClBackendPrograms();

    const std::string identity = ContextIdentityLocked() + "|" + path.string();

    switch (variant) {
      case OpenClDemosaicNetVariant::Bayer: {
        auto module = std::make_unique<OpenClBayerDemosaicNet>();
        module->LoadWeights(dto, options.queue);
        bayer_          = std::move(module);
        bayer_identity_ = identity;
        break;
      }
      case OpenClDemosaicNetVariant::XTrans: {
        auto module = std::make_unique<OpenClXTransDemosaicNet>();
        module->LoadWeights(dto, options.queue);
        xtrans_          = std::move(module);
        xtrans_identity_ = identity;
        break;
      }
    }
    last_error_.clear();
    return true;
  } catch (const std::exception& e) {
    last_error_ = e.what();
    // Do not publish partial state: leave existing entry if any; on first load leave null.
    return false;
  }
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
