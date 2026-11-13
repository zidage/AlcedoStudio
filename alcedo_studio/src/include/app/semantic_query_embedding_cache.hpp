//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace alcedo {

/**
 * @brief Least-recently-used cache of text embeddings, keyed by model key and text.
 *
 * A natural-language search embeds its query text and the neutral text through the semantic
 * sidecar. The cache keeps those vectors, so a repeated text for the same model does not start
 * or call the sidecar again. The text is used as given (the caller trims it); it is not
 * lowercased. A hit moves the entry to the front; an insert at capacity removes the least
 * recently used entry.
 *
 * Thread safety: all methods are thread-safe. The mutex is held only for the lookup and the
 * insert, not while @p encode runs, so two threads that miss on the same key can both encode;
 * the second insert replaces the first.
 */
class SemanticQueryEmbeddingCache {
 public:
  static constexpr size_t kDefaultCapacity = 64;

  /// Encodes the text when it is not cached. Throws to report a failure.
  using Encoder = std::function<std::vector<float>()>;

  explicit SemanticQueryEmbeddingCache(size_t capacity = kDefaultCapacity);

  /**
   * @brief The cached vector of (@p model_key, @p text), or the result of @p encode.
   *
   * On a miss, calls @p encode without the lock and stores its result. When @p encode throws,
   * the exception passes to the caller and no entry is stored.
   */
  [[nodiscard]] auto GetOrEmbed(const std::string& model_key, const std::string& text,
                                const Encoder& encode) -> std::vector<float>;

  /// True when (@p model_key, @p text) is cached. Does not change the use order.
  [[nodiscard]] auto Contains(const std::string& model_key, const std::string& text) const -> bool;

  /// Number of stored entries.
  [[nodiscard]] auto Size() const -> size_t;

 private:
  using Key   = std::pair<std::string, std::string>;
  using Entry = std::pair<Key, std::vector<float>>;

  /// Moves an existing entry to the front or inserts a new one, then trims to capacity.
  /// Requires mutex_.
  void StoreLocked(Key key, std::vector<float> embedding);

  size_t                                       capacity_;
  mutable std::mutex                           mutex_;
  /// Most recently used first.
  std::list<Entry>                             entries_;
  std::map<Key, std::list<Entry>::iterator>    index_;
};

}  // namespace alcedo
