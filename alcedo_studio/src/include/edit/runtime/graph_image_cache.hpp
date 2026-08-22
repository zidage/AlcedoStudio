//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/texture_pool.hpp"

namespace alcedo {

/**
 * @brief KV cache of node image outputs keyed by producer node and output port.
 *
 * Values are Texture2D leases held across frames so a matching size is reused
 * without a second Acquire. Not thread-safe.
 */
template <class Backend>
class GraphImageCache {
 public:
  void Store(GraphValueId id, ResourceLease<Backend> lease) {
    images_.insert_or_assign(std::move(id), std::move(lease));
  }

  [[nodiscard]] auto Find(const GraphValueId& id) -> ResourceLease<Backend>* {
    const auto it = images_.find(id);
    return it == images_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] auto Find(const GraphValueId& id) const -> const ResourceLease<Backend>* {
    const auto it = images_.find(id);
    return it == images_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] auto Find(const NodeId& producer, const PortId& output_port)
      -> ResourceLease<Backend>* {
    return Find(GraphValueId{producer, output_port});
  }

  void Erase(const GraphValueId& id) { images_.erase(id); }
  void Clear() { images_.clear(); }
  [[nodiscard]] auto Size() const -> std::size_t { return images_.size(); }

 private:
  std::map<GraphValueId, ResourceLease<Backend>> images_;
};

}  // namespace alcedo
