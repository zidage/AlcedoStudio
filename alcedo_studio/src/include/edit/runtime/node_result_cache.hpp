//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

#include "edit/graph/graph_ids.hpp"

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
    values_.insert_or_assign(std::move(id), std::move(buffer));
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
