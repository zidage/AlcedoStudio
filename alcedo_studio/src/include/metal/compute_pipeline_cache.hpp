//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <alcedo/metal/Metal.hpp>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "metal/metal_context.hpp"

namespace alcedo {
namespace metal {

class ComputePipelineCache {
 private:
  struct LibrarySlot {
    NS::SharedPtr<MTL::Library> library;
    bool                        is_loading = false;
    std::exception_ptr          error;
    std::condition_variable     cv;
  };

  struct PipelineSlot {
    NS::SharedPtr<MTL::ComputePipelineState> pipeline;
    bool                                     is_creating = false;
    std::exception_ptr                       error;
    std::condition_variable                  cv;
  };

  mutable std::mutex                                             mutex_;
  std::unordered_map<std::string, std::shared_ptr<LibrarySlot>>  libraries_;
  std::unordered_map<std::string, std::shared_ptr<PipelineSlot>> pipelines_;
  std::uint64_t                                                  hits_    = 0;
  std::uint64_t                                                  misses_  = 0;
  std::uint64_t                                                  creates_ = 0;

  ComputePipelineCache()                                                  = default;

  auto GetLibrarySlot(const std::string& metallib_path) -> std::shared_ptr<LibrarySlot> {
    std::lock_guard<std::mutex> lock(mutex_);
    auto&                       slot = libraries_[metallib_path];
    if (!slot) {
      slot = std::make_shared<LibrarySlot>();
    }
    return slot;
  }

  auto GetPipelineSlot(const std::string& cache_key) -> std::shared_ptr<PipelineSlot> {
    std::lock_guard<std::mutex> lock(mutex_);
    auto&                       slot = pipelines_[cache_key];
    if (!slot) {
      slot = std::make_shared<PipelineSlot>();
    }
    return slot;
  }

  static void AppendUniquePath(std::vector<std::filesystem::path>& candidates,
                               std::unordered_set<std::string>&    seen,
                               const std::filesystem::path&        candidate) {
    if (candidate.empty()) {
      return;
    }

    const auto normalized = candidate.lexically_normal().string();
    if (normalized.empty()) {
      return;
    }
    if (seen.insert(normalized).second) {
      candidates.emplace_back(candidate);
    }
  }

  static auto ResolveMetallibPath(const char* configured_path, const char* debug_label)
      -> std::string {
    std::filesystem::path              configured(configured_path);
    const auto                         shader_name = configured.filename();

    std::vector<std::filesystem::path> candidates;
    std::unordered_set<std::string>    seen;
    AppendUniquePath(candidates, seen, configured);

    if (!shader_name.empty()) {
      if (auto* bundle = NS::Bundle::mainBundle(); bundle != nullptr) {
        if (auto* resource_path = bundle->resourcePath();
            resource_path != nullptr && resource_path->utf8String() != nullptr) {
          AppendUniquePath(
              candidates, seen,
              std::filesystem::path(resource_path->utf8String()) / "metallib" / shader_name);
        }

        if (auto* executable_path = bundle->executablePath();
            executable_path != nullptr && executable_path->utf8String() != nullptr) {
          const auto executable_dir =
              std::filesystem::path(executable_path->utf8String()).parent_path();
          AppendUniquePath(candidates, seen, executable_dir / "metallib" / shader_name);
          AppendUniquePath(candidates, seen,
                           executable_dir.parent_path() / "Resources" / "metallib" / shader_name);
        }
      }

      std::error_code cwd_error;
      const auto      cwd = std::filesystem::current_path(cwd_error);
      if (!cwd_error) {
        AppendUniquePath(candidates, seen, cwd / "metallib" / shader_name);
      }
    }

    std::error_code exists_error;
    for (const auto& candidate : candidates) {
      if (std::filesystem::exists(candidate, exists_error) && !exists_error) {
        return candidate.string();
      }
      exists_error.clear();
    }

    std::string error_message =
        std::string(debug_label) +
        ": metallib file was not found. Configured path: " + std::string(configured_path) +
        ". Tried:";
    for (const auto& candidate : candidates) {
      error_message += "\n  - ";
      error_message += candidate.string();
    }
    throw std::runtime_error(error_message);
  }

  auto LoadLibrary(const std::string& metallib_path, const char* debug_label)
      -> NS::SharedPtr<MTL::Library> {
    auto                         slot = GetLibrarySlot(metallib_path);

    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      if (slot->library) {
        return slot->library;
      }
      if (slot->error) {
        std::rethrow_exception(slot->error);
      }
      if (!slot->is_loading) {
        slot->is_loading = true;
        break;
      }
      slot->cv.wait(lock);
    }
    lock.unlock();

    try {
      auto* device = MetalContext::Instance().Device();
      if (device == nullptr) {
        throw std::runtime_error(std::string(debug_label) + ": Metal device is unavailable.");
      }

      NS::Error* error          = nullptr;
      auto       library_path   = NS::String::string(metallib_path.c_str(), NS::UTF8StringEncoding);
      auto       loaded_library = NS::TransferPtr(device->newLibrary(library_path, &error));
      if (!loaded_library) {
        std::string error_message = std::string(debug_label) + ": failed to load metallib.";
        if (error != nullptr) {
          error_message += " ";
          error_message += error->localizedDescription()->utf8String();
        }
        throw std::runtime_error(error_message);
      }

      lock.lock();
      slot->library    = loaded_library;
      slot->is_loading = false;
      slot->cv.notify_all();
      return slot->library;
    } catch (...) {
      lock.lock();
      slot->error      = std::current_exception();
      slot->is_loading = false;
      slot->cv.notify_all();
      throw;
    }
  }

 public:
  ComputePipelineCache(const ComputePipelineCache&)                      = delete;
  auto operator=(const ComputePipelineCache&) -> ComputePipelineCache&   = delete;
  ComputePipelineCache(ComputePipelineCache&&)                           = delete;
  auto        operator=(ComputePipelineCache&&) -> ComputePipelineCache& = delete;

  static auto Instance() -> ComputePipelineCache& {
    static ComputePipelineCache cache;
    return cache;
  }

  struct Stats {
    std::uint64_t hits    = 0;
    std::uint64_t misses  = 0;
    std::uint64_t creates = 0;
  };

  [[nodiscard]] auto GetStats() const -> Stats {
    std::lock_guard<std::mutex> lock(mutex_);
    return Stats{hits_, misses_, creates_};
  }

  void ResetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    hits_    = 0;
    misses_  = 0;
    creates_ = 0;
  }

  // Metal pipeline states are immutable, so the same compiled object can be reused safely across
  // concurrent command buffers.
  auto GetPipelineState(const char* metallib_path, const char* function_name,
                        const char* debug_label) -> NS::SharedPtr<MTL::ComputePipelineState> {
    if (metallib_path == nullptr || metallib_path[0] == '\0') {
      throw std::runtime_error(std::string(debug_label) + ": metallib path is not configured.");
    }
    if (function_name == nullptr || function_name[0] == '\0') {
      throw std::runtime_error(std::string(debug_label) + ": compute function name is empty.");
    }

    const std::string resolved_metallib_path = ResolveMetallibPath(metallib_path, debug_label);
    const std::string library_key            = resolved_metallib_path;
    const std::string pipeline_key           = library_key + '\n' + std::string(function_name);
    auto              pipeline_slot          = GetPipelineSlot(pipeline_key);

    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      if (pipeline_slot->pipeline) {
        ++hits_;
        return pipeline_slot->pipeline;
      }
      if (pipeline_slot->error) {
        std::rethrow_exception(pipeline_slot->error);
      }
      if (!pipeline_slot->is_creating) {
        pipeline_slot->is_creating = true;
        ++misses_;
        break;
      }
      pipeline_slot->cv.wait(lock);
    }
    lock.unlock();

    try {
      auto library          = LoadLibrary(library_key, debug_label);

      auto function_name_ns = NS::String::string(function_name, NS::UTF8StringEncoding);
      auto function         = NS::TransferPtr(library->newFunction(function_name_ns));
      if (!function) {
        throw std::runtime_error(std::string(debug_label) +
                                 ": failed to load compute function from metallib.");
      }

      auto* device = MetalContext::Instance().Device();
      if (device == nullptr) {
        throw std::runtime_error(std::string(debug_label) + ": Metal device is unavailable.");
      }

      NS::Error* error = nullptr;
      auto       created_pipeline =
          NS::TransferPtr(device->newComputePipelineState(function.get(), &error));
      if (!created_pipeline) {
        std::string error_message =
            std::string(debug_label) + ": failed to create compute pipeline.";
        if (error != nullptr) {
          error_message += " ";
          error_message += error->localizedDescription()->utf8String();
        }
        throw std::runtime_error(error_message);
      }

      lock.lock();
      pipeline_slot->pipeline    = created_pipeline;
      pipeline_slot->is_creating = false;
      ++creates_;
      pipeline_slot->cv.notify_all();
      return pipeline_slot->pipeline;
    } catch (...) {
      lock.lock();
      pipeline_slot->error       = std::current_exception();
      pipeline_slot->is_creating = false;
      pipeline_slot->cv.notify_all();
      throw;
    }
  }
};

}  // namespace metal
}  // namespace alcedo

#endif
