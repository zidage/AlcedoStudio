//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>

#include "edit/graph/graph_ids.hpp"
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
  void Store(GraphValueId id, typename Backend::Buffer buffer) {
    const auto bytes = buffer.Bytes();
    values_.insert_or_assign(std::move(id), std::move(buffer));
    if (ShouldTraceGpuPoolAlloc(bytes) && GpuPoolTraceVerbose()) {
      DumpToStderr("value-store");
    }
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

  [[nodiscard]] auto Find(const NodeId& producer, const PortId& output_port)
      -> typename Backend::Buffer* {
    return Find(GraphValueId{producer, output_port});
  }

  void Erase(const GraphValueId& id) { values_.erase(id); }
  void Clear() { values_.clear(); }
  [[nodiscard]] auto Size() const -> std::size_t { return values_.size(); }

 private:
  std::map<GraphValueId, typename Backend::Buffer> values_;
};

}  // namespace alcedo
