//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/project_mask_cache_settings.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Stable Mix-cache slot: one published file per image and Color Grade.
 *
 * Filenames use this identity, never stroke, commit, Version, or content hash.
 */
struct ProjectMaskCacheSlotIdentity {
  sl_element_id_t image_id = 0;
  NodeId          node_id;
};

/**
 * @brief One settled Grade Mix coverage payload plus identity for disk writeback.
 *
 * @p recipe_fingerprint and @p producer_id are caller-owned reopen checks. The
 * service stores and verifies them; it does not compute coverage.
 */
struct ProjectMaskCacheRecord {
  ProjectMaskCacheSlotIdentity identity;
  Extent2D                     output_extent{};
  NormalizedRect               reference_bounds{};
  Extent2D                     source_grid{};
  std::string                  recipe_fingerprint;
  std::string                  producer_id;
  std::uint32_t                raster_algorithm_version = kBrushRasterAlgorithmVersion;
  std::vector<std::uint8_t>    pixels;
};

struct ProjectMaskCacheUsage {
  std::size_t published_file_count = 0;
  std::size_t published_byte_count = 0;
  std::size_t pending_write_count  = 0;
  bool        has_dirty_slot       = false;
  std::string last_error;
};

/**
 * @brief In-memory Mix-cache pixels plus a reader fence for Clear/close.
 *
 * Copies pixels out of the file so a later atomic replace does not mutate a
 * live reader. Must be released before the owning service destructor.
 */
class ProjectMaskCacheReader {
 public:
  ProjectMaskCacheReader() = default;
  ~ProjectMaskCacheReader();
  ProjectMaskCacheReader(ProjectMaskCacheReader&& other) noexcept;
  auto operator=(ProjectMaskCacheReader&& other) noexcept -> ProjectMaskCacheReader&;
  ProjectMaskCacheReader(const ProjectMaskCacheReader&)            = delete;
  auto operator=(const ProjectMaskCacheReader&) -> ProjectMaskCacheReader& = delete;

  [[nodiscard]] auto HasRecord() const -> bool { return static_cast<bool>(record_); }
  [[nodiscard]] auto Get() const -> const ProjectMaskCacheRecord&;

 private:
  friend class ProjectMaskCacheService;
  std::shared_ptr<const ProjectMaskCacheRecord> record_;
  std::function<void()>                         release_;
};

/**
 * @brief Disposable current Mix R8 slot writer for one project UUID.
 *
 * Coalesces pending writes for the same slot, atomically replaces one file, and
 * fences work with a storage generation so Clear and root changes cannot be
 * undone by an older writer. Parameter/history files are never opened.
 *
 * @threadsafe Enqueue, Flush, Clear, ChangeChosenRoot, and TryAcquireReader may
 *             run concurrently. The owner thread still owns ProjectService settings.
 */
class ProjectMaskCacheService {
 public:
  /**
   * @brief Bind this writer to @p project_uuid under @p chosen_root.
   *
   * Does not create directories until the first write. Does not use the process
   * temp directory as a substitute root.
   *
   * @throws std::invalid_argument when @p project_uuid or @p chosen_root is invalid.
   */
  ProjectMaskCacheService(std::string project_uuid, std::filesystem::path chosen_root);
  ~ProjectMaskCacheService();

  ProjectMaskCacheService(const ProjectMaskCacheService&)            = delete;
  auto operator=(const ProjectMaskCacheService&) -> ProjectMaskCacheService& = delete;
  ProjectMaskCacheService(ProjectMaskCacheService&&)                 = delete;
  auto operator=(ProjectMaskCacheService&&) -> ProjectMaskCacheService&      = delete;

  [[nodiscard]] auto ProjectUuid() const -> std::string;
  [[nodiscard]] auto ChosenRoot() const -> std::filesystem::path;
  [[nodiscard]] auto StorageGeneration() const -> std::uint64_t;

  /**
   * @brief Create `<root>/alcedo-mask-cache/<uuid>` when it does not exist.
   *
   * @return false when the path cannot be used as a directory. @p error names the
   *         real filesystem failure. Never substitutes another root.
   */
  [[nodiscard]] static auto PrepareNamespace(const std::filesystem::path& chosen_root,
                                             std::string_view project_uuid, std::string* error)
      -> bool;

  /**
   * @brief Queue the latest settled Mix for @p record.identity, replacing any
   *        not-yet-started write for that slot.
   *
   * Does not write on the caller thread. A later Flush, save, or idle write
   * publishes at most one file per slot. Failure keeps the slot dirty.
   */
  auto EnqueueSettledWrite(ProjectMaskCacheRecord record, std::string* error) -> bool;

  /**
   * @brief Block until queued writes have been attempted and in-flight work ends.
   *
   * @return false when any slot in this flush failed. Last-good published files
   *         stay in place; pending dirty slots remain dirty.
   */
  auto FlushPendingWrites(std::string* error) -> bool;

  /**
   * @brief Load one published slot when identity, fingerprint, producer, and checksum match.
   *
   * Missing files and checksum mismatches are cache misses (`nullopt` with an
   * empty @p error). An unusable configured root sets @p error and returns
   * `nullopt` without reading another directory.
   */
  [[nodiscard]] auto TryAcquireReader(const ProjectMaskCacheSlotIdentity& identity,
                                      std::string_view expected_fingerprint,
                                      std::string_view expected_producer, std::string* error)
      -> std::optional<ProjectMaskCacheReader>;

  /**
   * @brief Stop writers, wait for readers, and delete only this project's Mix-cache files.
   *
   * Does not delete `.r8mask` assets, other project UUID directories, or files
   * outside the dedicated namespace. Does not enqueue a rebuild write.
   */
  auto ClearOwnedCache(std::string* error) -> bool;

  /**
   * @brief Fence current jobs, then publish @p new_root for later writes.
   *
   * Does not persist project metadata. The owner must save settings first.
   * Old-generation jobs refuse to write into the new root. Does not copy files.
   */
  auto PublishChosenRoot(const std::filesystem::path& new_root, std::string* error) -> bool;

  /**
   * @brief Delete this UUID's namespace under @p chosen_root after a successful root change.
   *
   * On failure, @p error describes the residual path. Callers must keep that
   * path in @ref ProjectMaskCacheSettings::previous_roots.
   */
  auto DeleteOwnedNamespace(const std::filesystem::path& chosen_root, std::string* error) -> bool;

  /**
   * @brief Increment storage generation, drop coalesced pending writes, wait for in-flight work.
   */
  auto FenceWrites(std::string* error) -> bool;

  [[nodiscard]] auto Usage() const -> ProjectMaskCacheUsage;
  [[nodiscard]] auto PublishedSlotPath(const ProjectMaskCacheSlotIdentity& identity) const
      -> std::filesystem::path;
  [[nodiscard]] auto CountPublishedSlots() const -> std::size_t;

  /**
   * @brief Test hook after the temporary file is closed and before atomic replace.
   *
   * Return false to fail the replace. The hook must not call Clear on this
   * instance. Pass an empty function to clear. Not used by product rendering.
   */
  void SetPublishHookForTesting(
      std::function<bool(const std::filesystem::path& temporary,
                         const std::filesystem::path& destination)>
          hook);

  void ShutdownWriter();

 private:
  struct SlotKey {
    sl_element_id_t image_id = 0;
    std::string     node_id;

    friend auto operator<(const SlotKey& lhs, const SlotKey& rhs) -> bool {
      if (lhs.image_id != rhs.image_id) {
        return lhs.image_id < rhs.image_id;
      }
      return lhs.node_id < rhs.node_id;
    }
  };

  struct PendingWrite {
    ProjectMaskCacheRecord  record;
    std::uint64_t           generation = 0;
    std::filesystem::path   chosen_root;
  };

  auto SlotKeyFrom(const ProjectMaskCacheSlotIdentity& identity) const -> SlotKey;
  auto SlotPath(const std::filesystem::path& chosen_root,
                const ProjectMaskCacheSlotIdentity& identity) const -> std::filesystem::path;
  void WriterLoop();
  auto WriteOne(const PendingWrite& pending, std::string* error) -> bool;
  auto DeleteNamespaceFiles(const std::filesystem::path& chosen_root, std::string* error) -> bool;
  auto ScanUsage(ProjectMaskCacheUsage* usage) const -> void;
  auto ChosenRootUsable(std::string* error) const -> bool;
  void ReleaseReader();

  std::string           project_uuid_;
  std::filesystem::path chosen_root_;

  mutable std::mutex      mutex_;
  std::condition_variable cv_;
  std::uint64_t           generation_     = 1;
  std::uint32_t           inflight_writes_ = 0;
  std::uint32_t           reader_count_    = 0;
  bool                    stop_writer_     = false;
  bool                    maintenance_     = false;
  bool                    has_dirty_slot_  = false;
  std::string             last_error_;
  std::map<SlotKey, PendingWrite> pending_;
  std::function<bool(const std::filesystem::path&, const std::filesystem::path&)> publish_hook_;
  std::thread writer_;
};

/**
 * @brief Validate Mix-cache record identity, fingerprint, producer, and packed R8 size.
 *
 * @throws std::invalid_argument when a field is empty, non-finite, or oversized.
 */
void ValidateProjectMaskCacheRecord(const ProjectMaskCacheRecord& record);

}  // namespace alcedo
