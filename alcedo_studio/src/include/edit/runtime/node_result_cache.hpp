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
#include "edit/runtime/result_representation.hpp"
#include "edit/runtime/runtime_revision.hpp"
#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {

/**
 * @brief KV cache of non-image node buffers keyed by producer node and output port.
 *
 * GPU image results live in GraphImageCache. This cache holds runtime buffers
 * such as LLF planes. One current buffer per GraphValueId. Not thread-safe.
 */
template <class Backend>
class NodeResultCache {
 public:
  struct Metadata {
    RuntimeRevision      completed_revision = 0;
    ResultRepresentation representation{};
    ImageExtent          extent{};
    std::uint32_t        source_long_edge = 0;
    bool                 canonical        = false;
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

  /**
   * @brief Attach the validity metadata used to reuse a cached node buffer.
   *
   * @p source_long_edge and @p canonical describe an LLF reference plane. Other
   * buffers leave them at the defaults. A failed write must not call this.
   */
  void StoreMetadata(const GraphValueId& id, RuntimeRevision completed_revision,
                     const ResultRepresentation& representation, ImageExtent extent,
                     std::uint32_t source_long_edge = 0, bool canonical = false) {
    if (!values_.contains(id)) {
      throw std::runtime_error("NodeResultCache::StoreMetadata: value buffer is missing");
    }
    metadata_.insert_or_assign(
        id, Metadata{completed_revision, representation, extent, source_long_edge, canonical});
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
