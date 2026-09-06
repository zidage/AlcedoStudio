//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "edit/runtime/result_representation.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/runtime_revision.hpp"
#include "edit/runtime/texture_pool.hpp"
#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {

/**
 * @brief GPU image results: one current published result per GraphValueId.
 *
 * Lookup uses required revision plus representation, not parameter hashes.
 * Callers must use @ref AcquireTextureForWrite, @ref FindValidResult /
 * @ref BindValidResult, and @ref PublishSuccessfulSubmission. A matching
 * ResourceId is allocation reuse, not a content hit. Not thread-safe. One
 * in-flight submission.
 *
 * Published results whose required revision still matches are retained. Memory
 * pressure reclaims invalid unleased results and unowned free textures first.
 * It does not LRU-evict a valid current result. The pool LRU byte budget is a
 * target for unused textures, not a hard cap on live in-flight writes.
 *
 * @tparam Backend Texture factory used by TexturePool.
 */
template <class Backend>
class GraphImageCache {
 public:
  /**
   * @brief True when the current published result matches revision and representation
   *        and @p last_writer has completed.
   */
  [[nodiscard]] auto FindValidResult(const GraphValueId& id, RuntimeRevision required_revision,
                                     const ResultRepresentation& needed,
                                     std::uint64_t completed_submission) const -> bool {
    const auto* published = FindPublished(id);
    return published != nullptr && published->revision == required_revision &&
           required_revision != 0 && RepresentationSatisfies(published->representation, needed) &&
           !WriterBusy(*published, completed_submission);
  }

  /**
   * @brief Bind the current published result of @p id when it satisfies the query.
   *
   * Counts a validity hit or a classified miss (missing, revision, representation,
   * or in-flight writer). Policy bypass must not call this.
   */
  auto BindValidResult(const GraphValueId& id, RuntimeRevision required_revision,
                       const ResultRepresentation& needed, std::uint64_t completed_submission)
      -> ResourceLease<Backend>* {
    ++lookups_;
    auto* published = FindPublished(id);
    if (published == nullptr) {
      ++missing_misses_;
      return nullptr;
    }
    if (required_revision == 0 || published->revision != required_revision) {
      ++revision_misses_;
      return nullptr;
    }
    if (!RepresentationSatisfies(published->representation, needed)) {
      ++representation_misses_;
      return nullptr;
    }
    if (WriterBusy(*published, completed_submission)) {
      ++writer_busy_misses_;
      return nullptr;
    }
    ++content_hits_;
    return &published->texture;
  }

  /**
   * @brief Allocate or reuse a texture for an unpublished write of @p id.
   *
   * Reuses an in-flight write slot of the same size. Reuses a matching free
   * allocation before reclaiming invalid published results. Does not steal a
   * valid published result of another id. Replacing a persisted id may reclaim
   * that id after GPU readers finish. A live write may exceed the pool LRU
   * byte budget while valid published results stay leased. Throws when the
   * backend cannot create the texture.
   *
   * @pre @p pool and @p backend outlive this cache.
   */
  auto AcquireTextureForWrite(TexturePool<Backend>& pool, Backend& backend, const GraphValueId& id,
                              const TextureRequest& request,
                              const RuntimeInvalidationState& invalidation,
                              ResultPersistenceScope persistence, const GraphValueId& sensor_linear)
      -> ResourceLease<Backend>& {
    if (request.width == 0 || request.height == 0) {
      throw std::runtime_error("GraphImageCache::AcquireTextureForWrite: invalid size");
    }
    if (auto* slot = FindWrite(id)) {
      if (!slot->texture.Empty()) {
        const auto& tex = slot->texture.Texture();
        if (tex.Width() == request.width && tex.Height() == request.height &&
            tex.Format() == request.format) {
          slot->has_revision   = false;
          slot->revision       = 0;
          slot->representation = {};
          return slot->texture;
        }
      }
      write_slots_.erase(id);
    }

    const auto needed_bytes = TextureBytes(request);
    if (auto* reused = TryReuseMatchingFree(pool, id, request)) {
      return *reused;
    }
    ReclaimInvalidResults(pool, backend, request, id, invalidation, persistence, sensor_linear);
    if (auto* reused = TryReuseMatchingFree(pool, id, request)) {
      return *reused;
    }
    pool.EvictUntil(needed_bytes);
    if (auto* reused = TryReuseMatchingFree(pool, id, request)) {
      return *reused;
    }

    WriteSlot slot;
    slot.texture               = pool.Acquire(request);
    slot.representation.extent = ImageExtent{request.width, request.height};
    slot.representation.format = request.format;
    auto [it, inserted]        = write_slots_.emplace(id, std::move(slot));
    (void)inserted;
    return it->second.texture;
  }

  /**
   * @brief Bind @p dest to the current texture of @p source without allocating.
   *
   * Used when GeometryResample is identity. Revisions stay independent:
   * publishing @p dest does not replace @p source.
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

    const auto& source_tex         = source_lease->Texture();
    WriteSlot   slot;
    slot.texture                   = pool.DuplicateLease(source_lease->Handle());
    slot.representation.extent     = ImageExtent{source_tex.Width(), source_tex.Height()};
    slot.representation.format     = source_tex.Format();
    auto [it, inserted]            = write_slots_.emplace(dest, std::move(slot));
    (void)inserted;
    return it->second.texture;
  }

  /**
   * @brief Attach a revision to the unpublished write of @p id.
   *
   * The result is not valid until @ref PublishSuccessfulSubmission.
   *
   * @throws std::runtime_error when @p id has no write slot.
   */
  void RecordUnpublished(const GraphValueId& id, RuntimeRevision revision,
                         const ResultRepresentation& representation, std::uint64_t submission_id,
                         std::uint64_t auxiliary = 0) {
    auto* slot = FindWrite(id);
    if (slot == nullptr) {
      throw std::runtime_error("GraphImageCache::RecordUnpublished: no write slot");
    }
    if (slot->representation.extent != representation.extent ||
        slot->representation.format != representation.format) {
      throw std::runtime_error("GraphImageCache::RecordUnpublished: extent or format mismatch");
    }
    slot->revision       = revision;
    slot->representation = representation;
    slot->has_revision   = true;
    slot->last_writer    = submission_id;
    slot->auxiliary      = auxiliary;
  }

  /**
   * @brief Publish unpublished writes recorded for @p submission_id.
   *
   * Only values allowed by @p persistence become the current published result.
   * Other recorded writes stay unpublished until @ref DiscardUnpublished.
   * @ref ResultPersistenceScope::AllCurrentResults also drops unrecorded slots,
   * matching the previous publish-then-clear behavior.
   */
  void PublishSuccessfulSubmission(std::uint64_t submission_id,
                                   ResultPersistenceScope persistence =
                                       ResultPersistenceScope::AllCurrentResults,
                                   const GraphValueId& sensor_linear = {}) {
    for (auto it = write_slots_.begin(); it != write_slots_.end();) {
      auto&       slot = it->second;
      const auto& id   = it->first;
      if (!slot.has_revision || slot.last_writer != submission_id ||
          !PersistsGraphValue(persistence, id, sensor_linear)) {
        ++it;
        continue;
      }
      PublishedResult published;
      published.texture        = std::move(slot.texture);
      published.representation = slot.representation;
      published.revision       = slot.revision;
      published.last_writer    = slot.last_writer;
      published.auxiliary      = slot.auxiliary;
      published_.insert_or_assign(id, std::move(published));
      ++persistent_publishes_;
      it = write_slots_.erase(it);
    }
    if (persistence == ResultPersistenceScope::AllCurrentResults) {
      write_slots_.clear();
    }
  }

  /**
   * @brief Drop unpublished writes without publishing. Previously published results stay.
   */
  void               DiscardUnpublished() { write_slots_.clear(); }

  /**
   * @brief Drop unpublished writes other than @p keep.
   *
   * QualityBase presentation may keep the displayed output until the sink
   * returns it. Geometry and other submission-local writes are released after
   * GPU last-use so they do not shadow Interactive published results.
   */
  void ReleaseUnpublishedExcept(const GraphValueId& keep) {
    for (auto it = write_slots_.begin(); it != write_slots_.end();) {
      if (it->first == keep) {
        ++it;
        continue;
      }
      it = write_slots_.erase(it);
    }
  }

  /**
   * @brief Drop the unpublished write of @p id and return its texture to the pool.
   *
   * Published results are unchanged. No-op when @p id has no write slot.
   * Caller must satisfy GPU last-use of that texture first.
   */
  void ReleaseWrite(const GraphValueId& id) { write_slots_.erase(id); }

  [[nodiscard]] auto UnpublishedWriteCount() const -> std::size_t { return write_slots_.size(); }

  /**
   * @brief Current texture for @p id: this-frame write slot, else the published result.
   */
  [[nodiscard]] auto Find(const GraphValueId& id) -> ResourceLease<Backend>* {
    if (auto* slot = FindWrite(id)) {
      return &slot->texture;
    }
    auto* published = FindPublished(id);
    return published == nullptr ? nullptr : &published->texture;
  }

  [[nodiscard]] auto Find(const GraphValueId& id) const -> const ResourceLease<Backend>* {
    if (const auto* slot = FindWrite(id)) {
      return &slot->texture;
    }
    const auto* published = FindPublished(id);
    return published == nullptr ? nullptr : &published->texture;
  }

  [[nodiscard]] auto Find(const NodeId& producer, const PortId& output_port)
      -> ResourceLease<Backend>* {
    return Find(GraphValueId{producer, output_port});
  }

  [[nodiscard]] auto PublishedCount() const -> std::size_t { return published_.size(); }
  [[nodiscard]] auto UnpublishedCount() const -> std::size_t { return write_slots_.size(); }
  [[nodiscard]] auto ContentHitCount() const -> std::uint64_t { return content_hits_; }
  [[nodiscard]] auto ContentMissCount() const -> std::uint64_t {
    return missing_misses_ + revision_misses_ + representation_misses_ + writer_busy_misses_;
  }
  [[nodiscard]] auto LookupCount() const -> std::uint64_t { return lookups_; }
  [[nodiscard]] auto RevisionMissCount() const -> std::uint64_t { return revision_misses_; }
  [[nodiscard]] auto RepresentationMissCount() const -> std::uint64_t {
    return representation_misses_;
  }
  [[nodiscard]] auto PersistentPublishCount() const -> std::uint64_t {
    return persistent_publishes_;
  }

  [[nodiscard]] auto PublishedRevision(const GraphValueId& id) const -> RuntimeRevision {
    const auto* published = FindPublished(id);
    return published == nullptr ? 0 : published->revision;
  }

  [[nodiscard]] auto PublishedRepresentation(const GraphValueId& id) const -> ResultRepresentation {
    const auto* published = FindPublished(id);
    return published == nullptr ? ResultRepresentation{} : published->representation;
  }

  [[nodiscard]] auto PublishedLastWriter(const GraphValueId& id) const -> std::uint64_t {
    const auto* published = FindPublished(id);
    return published == nullptr ? 0 : published->last_writer;
  }

  /**
   * @brief Return optional caller-owned metadata attached to the current image result.
   *
   * Intended for a canonical source long-edge; it is not a second identity.
   */
  [[nodiscard]] auto PublishedAuxiliary(const GraphValueId& id) const -> std::uint64_t {
    const auto* published = FindPublished(id);
    return published == nullptr ? 0 : published->auxiliary;
  }

  void Clear() {
    write_slots_.clear();
    published_.clear();
  }

  [[nodiscard]] auto CurrentValueIds() const -> std::vector<GraphValueId> {
    std::vector<GraphValueId> ids;
    ids.reserve(published_.size());
    for (const auto& [id, entry] : published_) {
      (void)entry;
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
    std::fprintf(stderr, "[GPU_POOL] images %s published=%zu write=%zu\n",
                 reason == nullptr ? "" : reason, published_.size(), write_slots_.size());
    if (!GpuPoolTraceVerbose()) {
      return;
    }
    for (const auto& [id, slot] : write_slots_) {
      PrintSlot("write", id, slot.revision, slot.has_revision, slot.representation.extent,
                slot.representation.format, slot.texture, slot.last_writer);
    }
    for (const auto& [id, entry] : published_) {
      PrintSlot("published", id, entry.revision, true, entry.representation.extent,
                entry.representation.format, entry.texture, entry.last_writer);
    }
  }

 private:
  static void PrintSlot(const char* kind, const GraphValueId& id, RuntimeRevision revision,
                        bool has_revision, ImageExtent extent, TextureFormat format,
                        const ResourceLease<Backend>& texture, std::uint64_t last_writer) {
    const auto handle = texture.Empty() ? 0 : texture.Handle();
    std::fprintf(stderr,
                 "[GPU_POOL]   %-9s %.*s:%.*s %ux%u %s handle=%llu rev=%llu writer=%llu "
                 "has_rev=%d\n",
                 kind, static_cast<int>(id.producer.Value().size()), id.producer.Value().data(),
                 static_cast<int>(id.output_port.Value().size()), id.output_port.Value().data(),
                 extent.width, extent.height, TextureFormatName(format),
                 static_cast<unsigned long long>(handle),
                 static_cast<unsigned long long>(has_revision ? revision : 0),
                 static_cast<unsigned long long>(last_writer), has_revision ? 1 : 0);
  }

  struct WriteSlot {
    ResourceLease<Backend> texture;
    ResultRepresentation   representation{};
    RuntimeRevision        revision     = 0;
    std::uint64_t          last_writer  = 0;
    std::uint64_t          auxiliary    = 0;
    bool                   has_revision = false;
  };

  struct PublishedResult {
    ResourceLease<Backend> texture;
    ResultRepresentation   representation{};
    RuntimeRevision        revision    = 0;
    std::uint64_t          last_writer = 0;
    std::uint64_t          auxiliary   = 0;
  };

  static auto WriterBusy(const PublishedResult& entry, std::uint64_t completed_submission) -> bool {
    return entry.last_writer != 0 && entry.last_writer > completed_submission;
  }

  static auto TextureBytes(const TextureRequest& request) -> std::size_t {
    return static_cast<std::size_t>(request.width) * request.height *
           TextureFormatBytesPerPixel(request.format);
  }

  auto FindWrite(const GraphValueId& id) -> WriteSlot* {
    const auto it = write_slots_.find(id);
    return it == write_slots_.end() ? nullptr : &it->second;
  }

  auto FindWrite(const GraphValueId& id) const -> const WriteSlot* {
    const auto it = write_slots_.find(id);
    return it == write_slots_.end() ? nullptr : &it->second;
  }

  auto FindPublished(const GraphValueId& id) -> PublishedResult* {
    const auto it = published_.find(id);
    return it == published_.end() ? nullptr : &it->second;
  }

  auto FindPublished(const GraphValueId& id) const -> const PublishedResult* {
    const auto it = published_.find(id);
    return it == published_.end() ? nullptr : &it->second;
  }

  auto TryReuseMatchingFree(TexturePool<Backend>& pool, const GraphValueId& id,
                            const TextureRequest& request) -> ResourceLease<Backend>* {
    if (!pool.HasReusable(request)) {
      return nullptr;
    }
    WriteSlot slot;
    slot.texture               = pool.Acquire(request);
    slot.representation.extent = ImageExtent{request.width, request.height};
    slot.representation.format = request.format;
    auto [it, inserted]        = write_slots_.emplace(id, std::move(slot));
    (void)inserted;
    return &it->second.texture;
  }

  [[nodiscard]] auto HandleOwnerCount(std::uint64_t handle) const -> std::uint32_t {
    std::uint32_t count = 0;
    for (const auto& [id, entry] : published_) {
      (void)id;
      if (!entry.texture.Empty() && entry.texture.Handle() == handle) {
        ++count;
      }
    }
    for (const auto& [id, slot] : write_slots_) {
      (void)id;
      if (!slot.texture.Empty() && slot.texture.Handle() == handle) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] auto MayReclaim(const GraphValueId& id, const PublishedResult& entry,
                                const GraphValueId& allocating_id, Backend& backend,
                                const TexturePool<Backend>& pool,
                                const RuntimeInvalidationState& invalidation,
                                ResultPersistenceScope persistence,
                                const GraphValueId& sensor_linear) const -> bool {
    if (entry.texture.Empty()) {
      return false;
    }
    if (backend.IsResourceBusy(entry.last_writer)) {
      return false;
    }
    const auto handle = entry.texture.Handle();
    if (pool.LeaseCount(handle) > HandleOwnerCount(handle)) {
      return false;
    }
    const auto required = invalidation.RequiredRevision(id);
    if (entry.revision == 0 || required != entry.revision) {
      return true;
    }
    return allocating_id == id && PersistsGraphValue(persistence, id, sensor_linear);
  }

  void ReclaimInvalidResults(TexturePool<Backend>& pool, Backend& backend,
                             const TextureRequest& request, const GraphValueId& allocating_id,
                             const RuntimeInvalidationState& invalidation,
                             ResultPersistenceScope persistence,
                             const GraphValueId& sensor_linear) {
    if (pool.ByteBudget() == 0) {
      return;
    }
    const auto needed_bytes = TextureBytes(request);
    auto reclaim_matching   = [&](bool matching_size_only) -> bool {
      GraphValueId victim_id;
      bool         found = false;
      for (auto& [id, entry] : published_) {
        if (!MayReclaim(id, entry, allocating_id, backend, pool, invalidation, persistence,
                        sensor_linear)) {
          continue;
        }
        const bool matches = entry.representation.extent.width == request.width &&
                             entry.representation.extent.height == request.height &&
                             entry.representation.format == request.format;
        if (matching_size_only && !matches) {
          continue;
        }
        victim_id = id;
        found     = true;
        break;
      }
      if (!found) {
        return false;
      }
      published_.erase(victim_id);
      ++invalid_reclaims_;
      return true;
    };

    while (!pool.HasReusable(request) && pool.UsedBytes() + needed_bytes > pool.ByteBudget()) {
      if (reclaim_matching(true)) {
        continue;
      }
      if (!reclaim_matching(false)) {
        break;
      }
      pool.EvictUntil(needed_bytes);
    }
  }

  std::map<GraphValueId, WriteSlot>       write_slots_;
  std::map<GraphValueId, PublishedResult> published_;
  std::uint64_t                           lookups_                = 0;
  std::uint64_t                           content_hits_           = 0;
  std::uint64_t                           missing_misses_         = 0;
  std::uint64_t                           revision_misses_        = 0;
  std::uint64_t                           representation_misses_  = 0;
  std::uint64_t                           writer_busy_misses_     = 0;
  std::uint64_t                           persistent_publishes_   = 0;
  std::uint64_t                           invalid_reclaims_       = 0;
};

}  // namespace alcedo
