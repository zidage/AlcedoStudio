//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/texture_pool.hpp"
#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {

struct ResultIdentity {
  GraphValueId value_id;
  ContentKey   content_key;

  friend auto  operator<(const ResultIdentity& a, const ResultIdentity& b) -> bool {
    if (a.value_id != b.value_id) {
      return a.value_id < b.value_id;
    }
    return a.content_key < b.content_key;
  }
};

/**
 * @brief GPU image results keyed by GraphValueId and content, separate from allocation reuse.
 *
 * Callers must use @ref AcquireTextureForWrite, @ref FindValidResult / @ref BindValidResult,
 * and @ref PublishSuccessfulSubmission. A matching ResourceId is allocation reuse, not a
 * content hit. Not thread-safe. One in-flight submission.
 *
 * @tparam Backend Texture factory used by TexturePool.
 */
template <class Backend>
class GraphImageCache {
 public:
  /**
   * @brief True when a published result matches value, key, extent, and format, and
   *        @p last_writer has completed.
   *
   * Does not bind the value as current and does not update LRU.
   */
  [[nodiscard]] auto FindValidResult(const GraphValueId& id, ContentKey key, ImageExtent extent,
                                     TextureFormat format, std::uint64_t completed_submission) const
      -> bool {
    const auto* published = FindPublished(id, key);
    return published != nullptr && Matches(*published, extent, format) &&
           !WriterBusy(*published, completed_submission);
  }

  /**
   * @brief Bind a completed published result as the current texture for @p id.
   *
   * @return The bound lease, or nullptr on miss. Misses include size/format mismatch and
   *         an in-flight last_writer. Counts a content hit or miss.
   */
  auto BindValidResult(const GraphValueId& id, ContentKey key, ImageExtent extent,
                       TextureFormat format, std::uint64_t completed_submission)
      -> ResourceLease<Backend>* {
    auto* published = FindPublished(id, key);
    if (published == nullptr || !Matches(*published, extent, format) ||
        WriterBusy(*published, completed_submission)) {
      ++content_misses_;
      return nullptr;
    }
    ++content_hits_;
    published->lru_tick = ++lru_clock_;
    current_[id]        = key;
    return &published->texture;
  }

  /**
   * @brief Allocate or reuse a texture for an unpublished write of @p id.
   *
   * Reuses an in-flight write slot of the same size. Does not steal a published result of
   * another content key. Overwriting a same-size unpublished slot drops its unrecorded
   * identity. Evicts completed, non-current published results to fit the pool budget.
   *
   * @pre @p pool and @p backend outlive this cache.
   */
  auto AcquireTextureForWrite(TexturePool<Backend>& pool, Backend& backend, const GraphValueId& id,
                              const TextureRequest& request) -> ResourceLease<Backend>& {
    if (request.width == 0 || request.height == 0) {
      throw std::runtime_error("GraphImageCache::AcquireTextureForWrite: invalid size");
    }
    if (auto* slot = FindWrite(id)) {
      if (!slot->texture.Empty()) {
        const auto& tex = slot->texture.Texture();
        if (tex.Width() == request.width && tex.Height() == request.height &&
            tex.Format() == request.format) {
          slot->has_key = false;
          slot->key     = {};
          return slot->texture;
        }
      }
      write_slots_.erase(id);
    }

    current_.erase(id);
    EvictCompletedUnleased(pool, backend, request);

    WriteSlot slot;
    slot.texture        = pool.Acquire(request);
    slot.extent         = ImageExtent{request.width, request.height};
    slot.format         = request.format;
    auto [it, inserted] = write_slots_.emplace(id, std::move(slot));
    (void)inserted;
    return it->second.texture;
  }

  /**
   * @brief Bind @p dest to the current texture of @p source without allocating.
   *
   * Used when GeometryResample is identity. Content keys stay independent:
   * publishing @p dest does not replace @p source. Overwriting a dest write
   * slot drops its unrecorded identity.
   *
   * @throws std::runtime_error when @p dest equals @p source or @p source has
   *         no current texture.
   */
  auto AliasTextureFrom(TexturePool<Backend>& pool, const GraphValueId& dest,
                        const GraphValueId& source) -> ResourceLease<Backend>& {
    if (dest == source) {
      throw std::runtime_error("GraphImageCache::AliasTextureFrom: dest and source are the same");
    }
    auto* source_lease = Find(source);
    if (source_lease == nullptr || source_lease->Empty()) {
      throw std::runtime_error("GraphImageCache::AliasTextureFrom: source is missing");
    }
    if (FindWrite(dest) != nullptr) {
      write_slots_.erase(dest);
    }
    current_.erase(dest);

    const auto& source_tex = source_lease->Texture();
    WriteSlot   slot;
    slot.texture        = pool.DuplicateLease(source_lease->Handle());
    slot.extent         = ImageExtent{source_tex.Width(), source_tex.Height()};
    slot.format         = source_tex.Format();
    auto [it, inserted] = write_slots_.emplace(dest, std::move(slot));
    (void)inserted;
    return it->second.texture;
  }

  /**
   * @brief Attach a content key to the unpublished write of @p id.
   *
   * The result is not valid until @ref PublishSuccessfulSubmission. If this texture was
   * previously published under another key, that identity must already have been removed;
   * this method only annotates the write slot.
   *
   * @throws std::runtime_error when @p id has no write slot.
   */
  void RecordUnpublished(const GraphValueId& id, ContentKey key, ImageExtent extent,
                         TextureFormat format, std::uint64_t submission_id,
                         std::uint64_t auxiliary = 0) {
    auto* slot = FindWrite(id);
    if (slot == nullptr) {
      throw std::runtime_error("GraphImageCache::RecordUnpublished: no write slot");
    }
    if (slot->extent != extent || slot->format != format) {
      throw std::runtime_error("GraphImageCache::RecordUnpublished: extent or format mismatch");
    }
    slot->key         = key;
    slot->has_key     = true;
    slot->last_writer = submission_id;
    slot->auxiliary   = auxiliary;
  }

  /**
   * @brief Atomically publish unpublished writes recorded for @p submission_id.
   *
   * Write slots without a content key are dropped. Replacing a published (id, key) pair
   * releases the previous lease. Failed or cancelled submissions must call
   * @ref DiscardUnpublished instead.
   */
  void PublishSuccessfulSubmission(std::uint64_t submission_id) {
    for (auto& [id, slot] : write_slots_) {
      if (!slot.has_key || slot.last_writer != submission_id) {
        continue;
      }
      PublishedResult published;
      published.texture     = std::move(slot.texture);
      published.extent      = slot.extent;
      published.format      = slot.format;
      published.last_writer = slot.last_writer;
      published.auxiliary   = slot.auxiliary;
      published.lru_tick    = ++lru_clock_;
      published_.insert_or_assign(ResultIdentity{id, slot.key}, std::move(published));
      current_[id] = slot.key;
    }
    write_slots_.clear();
  }

  /**
   * @brief Drop unpublished writes without publishing. Previously published results stay.
   */
  void               DiscardUnpublished() { write_slots_.clear(); }

  /**
   * @brief Drop the unpublished write of @p id and return its texture to the pool.
   *
   * Published results are unchanged. No-op when @p id has no write slot.
   * Caller must satisfy GPU last-use of that texture first.
   */
  void ReleaseWrite(const GraphValueId& id) { write_slots_.erase(id); }

  [[nodiscard]] auto UnpublishedWriteCount() const -> std::size_t { return write_slots_.size(); }

  /**
   * @brief Current texture for @p id: this-frame write slot, else last bound/published result.
   */
  [[nodiscard]] auto Find(const GraphValueId& id) -> ResourceLease<Backend>* {
    if (auto* slot = FindWrite(id)) {
      return &slot->texture;
    }
    auto* published = FindCurrentPublished(id);
    return published == nullptr ? nullptr : &published->texture;
  }

  [[nodiscard]] auto Find(const GraphValueId& id) const -> const ResourceLease<Backend>* {
    if (const auto* slot = FindWrite(id)) {
      return &slot->texture;
    }
    const auto* published = FindCurrentPublished(id);
    return published == nullptr ? nullptr : &published->texture;
  }

  [[nodiscard]] auto Find(const NodeId& producer, const PortId& output_port)
      -> ResourceLease<Backend>* {
    return Find(GraphValueId{producer, output_port});
  }

  /**
   * @brief Drop completed published results that are not the current binding, oldest first.
   *
   * Releasing a lease makes the texture reusable by TexturePool without treating that reuse
   * as a content hit. Stops once a matching-size texture is unleased or no victim remains.
   * Does not evict in-flight write slots or results whose last_writer is still busy.
   */
  void EvictCompletedUnleased(TexturePool<Backend>& pool, Backend& backend,
                              const TextureRequest& needed) {
    if (pool.ByteBudget() == 0) {
      return;
    }
    const auto needed_bytes = static_cast<std::size_t>(needed.width) * needed.height *
                              TextureFormatBytesPerPixel(needed.format);
    bool released_matching = false;
    while (!released_matching && pool.UsedBytes() + needed_bytes > pool.ByteBudget()) {
      ResultIdentity   victim_id;
      PublishedResult* victim = nullptr;
      std::uint64_t    oldest = (std::numeric_limits<std::uint64_t>::max)();
      for (auto& [identity, entry] : published_) {
        if (backend.IsResourceBusy(entry.last_writer)) {
          continue;
        }
        const auto current = current_.find(identity.value_id);
        if (current != current_.end() && current->second == identity.content_key) {
          continue;
        }
        if (entry.lru_tick < oldest) {
          oldest    = entry.lru_tick;
          victim    = &entry;
          victim_id = identity;
        }
      }
      if (victim == nullptr) {
        break;
      }
      released_matching = victim->extent.width == needed.width &&
                          victim->extent.height == needed.height && victim->format == needed.format;
      published_.erase(victim_id);
    }
  }

  [[nodiscard]] auto PublishedCount() const -> std::size_t { return published_.size(); }
  [[nodiscard]] auto UnpublishedCount() const -> std::size_t { return write_slots_.size(); }
  [[nodiscard]] auto ContentHitCount() const -> std::uint64_t { return content_hits_; }
  [[nodiscard]] auto ContentMissCount() const -> std::uint64_t { return content_misses_; }

  [[nodiscard]] auto PublishedContentKey(const GraphValueId& id) const -> ContentKey {
    const auto it = current_.find(id);
    return it == current_.end() ? ContentKey{} : it->second;
  }

  [[nodiscard]] auto PublishedLastWriter(const GraphValueId& id, ContentKey key) const
      -> std::uint64_t {
    const auto* published = FindPublished(id, key);
    return published == nullptr ? 0 : published->last_writer;
  }

  /**
   * @brief Return optional caller-owned metadata attached to a published image result.
   *
   * The value is published and discarded with the image identity. It is intended for small
   * resource descriptors such as a canonical source resolution; it is not a second content key.
   */
  [[nodiscard]] auto PublishedAuxiliary(const GraphValueId& id, ContentKey key) const
      -> std::uint64_t {
    const auto* published = FindPublished(id, key);
    return published == nullptr ? 0 : published->auxiliary;
  }

  void Clear() {
    write_slots_.clear();
    published_.clear();
    current_.clear();
  }

  [[nodiscard]] auto CurrentValueIds() const -> std::vector<GraphValueId> {
    std::vector<GraphValueId> ids;
    ids.reserve(current_.size());
    for (const auto& [id, key] : current_) {
      ids.push_back(id);
    }
    return ids;
  }

  /**
   * @brief Print published and unpublished image results with pool handles.
   *
   * @p pool is the TexturePool that owns the leases.
   */
  void DumpToStderr(const char* reason, const TexturePool<Backend>& pool) const {
    (void)pool;
    std::fprintf(stderr, "[GPU_POOL] images %s published=%zu write=%zu current=%zu\n",
                 reason == nullptr ? "" : reason, published_.size(), write_slots_.size(),
                 current_.size());
    if (!GpuPoolTraceVerbose()) {
      return;
    }
    for (const auto& [id, slot] : write_slots_) {
      PrintSlot("write", id, slot.key, slot.has_key, slot.extent, slot.format, slot.texture,
                slot.last_writer, false);
    }
    for (const auto& [identity, entry] : published_) {
      const auto current    = current_.find(identity.value_id);
      const bool is_current = current != current_.end() && current->second == identity.content_key;
      PrintSlot("published", identity.value_id, identity.content_key, true, entry.extent,
                entry.format, entry.texture, entry.last_writer, is_current);
    }
  }

 private:
  static void PrintSlot(const char* kind, const GraphValueId& id, ContentKey key, bool has_key,
                        ImageExtent extent, TextureFormat format,
                        const ResourceLease<Backend>& texture, std::uint64_t last_writer,
                        bool is_current) {
    const auto handle = texture.Empty() ? 0 : texture.Handle();
    std::fprintf(stderr,
                 "[GPU_POOL]   %-9s %.*s:%.*s %ux%u %s handle=%llu key=%llx writer=%llu "
                 "has_key=%d current=%d\n",
                 kind, static_cast<int>(id.producer.Value().size()), id.producer.Value().data(),
                 static_cast<int>(id.output_port.Value().size()), id.output_port.Value().data(),
                 extent.width, extent.height, TextureFormatName(format),
                 static_cast<unsigned long long>(handle),
                 static_cast<unsigned long long>(has_key ? key.hash : 0),
                 static_cast<unsigned long long>(last_writer), has_key ? 1 : 0, is_current ? 1 : 0);
  }
  struct WriteSlot {
    ResourceLease<Backend> texture;
    ContentKey             key{};
    ImageExtent            extent{};
    TextureFormat          format      = TextureFormat::R8;
    std::uint64_t          last_writer = 0;
    std::uint64_t          auxiliary   = 0;
    bool                   has_key     = false;
  };

  struct PublishedResult {
    ResourceLease<Backend> texture;
    ImageExtent            extent{};
    TextureFormat          format      = TextureFormat::R8;
    std::uint64_t          last_writer = 0;
    std::uint64_t          auxiliary   = 0;
    std::uint64_t          lru_tick    = 0;
  };

  static auto Matches(const PublishedResult& entry, ImageExtent extent, TextureFormat format)
      -> bool {
    return entry.extent == extent && entry.format == format && !entry.texture.Empty();
  }

  static auto WriterBusy(const PublishedResult& entry, std::uint64_t completed_submission) -> bool {
    return entry.last_writer != 0 && entry.last_writer > completed_submission;
  }

  auto FindWrite(const GraphValueId& id) -> WriteSlot* {
    const auto it = write_slots_.find(id);
    return it == write_slots_.end() ? nullptr : &it->second;
  }

  auto FindWrite(const GraphValueId& id) const -> const WriteSlot* {
    const auto it = write_slots_.find(id);
    return it == write_slots_.end() ? nullptr : &it->second;
  }

  auto FindPublished(const GraphValueId& id, ContentKey key) -> PublishedResult* {
    const auto it = published_.find(ResultIdentity{id, key});
    return it == published_.end() ? nullptr : &it->second;
  }

  auto FindPublished(const GraphValueId& id, ContentKey key) const -> const PublishedResult* {
    const auto it = published_.find(ResultIdentity{id, key});
    return it == published_.end() ? nullptr : &it->second;
  }

  auto FindCurrentPublished(const GraphValueId& id) -> PublishedResult* {
    const auto it = current_.find(id);
    if (it == current_.end()) {
      return nullptr;
    }
    return FindPublished(id, it->second);
  }

  auto FindCurrentPublished(const GraphValueId& id) const -> const PublishedResult* {
    const auto it = current_.find(id);
    if (it == current_.end()) {
      return nullptr;
    }
    return FindPublished(id, it->second);
  }

  std::map<GraphValueId, WriteSlot>         write_slots_;
  std::map<ResultIdentity, PublishedResult> published_;
  std::map<GraphValueId, ContentKey>        current_;
  std::uint64_t                             lru_clock_      = 0;
  std::uint64_t                             content_hits_   = 0;
  std::uint64_t                             content_misses_ = 0;
};

}  // namespace alcedo
