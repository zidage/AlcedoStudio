//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/content_key.hpp"
#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {

/**
 * @brief KV cache of non-image node buffers keyed by producer node and output port.
 *
 * GPU image results live in GraphImageCache with content keys. This cache holds
 * runtime buffers such as adjustment command lists. Not thread-safe.
 */
template <class Backend>
class NodeResultCache {
 public:
  struct Metadata {
    ContentKey  content_key{};
    ImageExtent extent{};
  };

  void Store(GraphValueId id, typename Backend::Buffer buffer) {
    const auto bytes = buffer.Bytes();
    const auto key   = id;
    values_.insert_or_assign(std::move(id), std::move(buffer));
    metadata_.erase(key);
    if (ShouldTraceGpuPoolAlloc(bytes) && GpuPoolTraceVerbose()) {
      DumpToStderr("value-store");
    }
  }

  /** @brief Attach the content identity used to validate a cached node buffer. */
  void StoreMetadata(const GraphValueId& id, ContentKey content_key, ImageExtent extent) {
    if (!values_.contains(id)) {
      throw std::runtime_error("NodeResultCache::StoreMetadata: value buffer is missing");
    }
    metadata_.insert_or_assign(id, Metadata{content_key, extent});
  }

  /** @brief Return cached identity metadata for a node buffer, when available. */
  [[nodiscard]] auto GetMetadata(const GraphValueId& id) const -> std::optional<Metadata> {
    const auto it = metadata_.find(id);
    return it == metadata_.end() ? std::nullopt : std::optional<Metadata>{it->second};
  }

  [[nodiscard]] auto UsedBytes() const -> std::size_t {
    std::size_t total = 0;
    for (const auto& [id, buffer] : values_) {
      (void)id;
      total += buffer.Bytes();
    }
    return total;
  }

  void DumpToStderr(const char* reason) const {
    std::fprintf(stderr, "[GPU_POOL] values %s count=%zu total=%.1f MiB\n",
                 reason == nullptr ? "" : reason, values_.size(), GpuPoolMiB(UsedBytes()));
    if (!GpuPoolTraceVerbose()) {
      return;
    }
    for (const auto& [id, buffer] : values_) {
      std::fprintf(stderr, "[GPU_POOL]   value %.*s:%.*s %.1f MiB resource=%llu\n",
                   static_cast<int>(id.producer.Value().size()), id.producer.Value().data(),
                   static_cast<int>(id.output_port.Value().size()), id.output_port.Value().data(),
                   GpuPoolMiB(buffer.Bytes()),
                   static_cast<unsigned long long>(buffer.ResourceId()));
    }
  }

  [[nodiscard]] auto Find(const GraphValueId& id) -> typename Backend::Buffer* {
    const auto it = values_.find(id);
    return it == values_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] auto Find(const GraphValueId& id) const -> const typename Backend::Buffer* {
    const auto it = values_.find(id);
    return it == values_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] auto Find(const NodeId& producer, const PortId& output_port) ->
      typename Backend::Buffer* {
    return Find(GraphValueId{producer, output_port});
  }

  void Erase(const GraphValueId& id) {
    values_.erase(id);
    metadata_.erase(id);
  }
  void Clear() {
    values_.clear();
    metadata_.clear();
  }
  [[nodiscard]] auto Size() const -> std::size_t { return values_.size(); }

 private:
  std::map<GraphValueId, typename Backend::Buffer> values_;
  std::map<GraphValueId, Metadata>                 metadata_;
};

}  // namespace alcedo
