//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "app/lut_library_inventory.hpp"
#include "edit/operators/models/lut_reference.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"

namespace alcedo {

/**
 * @brief The published state of the LUT library and the render-side resolver that reads it.
 *
 * Owner: LutLibraryService, which holds it through a shared pointer and is its only writer.
 * Render executors hold the same pointer as their LutResourceResolver, so the state they read
 * stays valid while a renderer outlives the service during shutdown.
 *
 * State: the active root, the published inventory, the package receipts read with it, and the
 * user state (favorites and previous roots). This is the single owner of those values; the
 * service does not keep another copy.
 *
 * Synchronization:
 * - Writers run on the service owner thread and take the state lock exclusively, so a render
 *   thread never observes a half-replaced inventory.
 * - The owner thread, and the library worker while the owner does not write (the L2 rule of
 *   the service), read the accessors without a lock.
 * - Render threads read only through Resolve and ReadResource, which take the state lock shared.
 * - ReadResource additionally holds the content lock shared while its visitor runs; the library
 *   worker takes it exclusively (@ref LockContentForRemoval) before it deletes or moves files
 *   of retired package content or of a migrated source root. A render that resolved a file
 *   therefore finishes reading it before the file disappears.
 * Lock order: content lock, then state lock. Writers never take the content lock.
 */
class LutLibraryPublication final : public LutResourceResolver {
 public:
  explicit LutLibraryPublication(std::filesystem::path root);

  // ── Owner-thread writers ────────────────────────────────────────────────
  /// Replace the inventory and, when given, the receipts it was scanned with.
  void               PublishInventory(LutLibraryInventory                           inventory,
                                      std::optional<std::vector<LutPackageReceipt>> receipts);
  void               SetRoot(std::filesystem::path root);
  void               SetUserState(LutLibraryUserState state);

  // ── Owner-thread (and idle-owner worker) reads ─────────────────────────
  [[nodiscard]] auto Root() const -> const std::filesystem::path& { return root_; }
  [[nodiscard]] auto Inventory() const -> const LutLibraryInventory& { return inventory_; }
  [[nodiscard]] auto Receipts() const -> const std::vector<LutPackageReceipt>& { return receipts_; }
  [[nodiscard]] auto UserState() const -> const LutLibraryUserState& { return user_state_; }

  /// Reference that selects @p entry: its official ID when the entry is package-owned official
  /// content with a metadata ID, else its library path.
  [[nodiscard]] static auto ReferenceForEntry(const LutLibraryEntry& entry) -> LutReference;
  /// Stable identity of @p entry for favorites and browser rows: the DescribeLutReference text
  /// of ReferenceForEntry (`official:<package>/<lut id>` or `library:<relative path>`).
  [[nodiscard]] static auto EntryIdOf(const LutLibraryEntry& entry) -> std::string;

  /// Exclusive content lock; hold it while deleting or moving library files that a published
  /// resolution may name. Blocks until running ReadResource visitors return.
  [[nodiscard]] auto        LockContentForRemoval() const -> std::unique_lock<std::shared_mutex>;

  /**
   * @brief Observer called on the resolving thread for each Missing resolution.
   *
   * The service uses it to request one inventory refresh per unresolved reference on its owner
   * thread. Clearing it (an empty function) waits for running calls, so the observer's owner
   * can be destroyed afterwards.
   */
  void                      SetMissingObserver(std::function<void(const LutReference&)> observer);

  // ── Any thread ──────────────────────────────────────────────────────────
  /**
   * Official: the package-owned entry with that package ID, `origin: alcedo`, and metadata ID,
   * in the package's active content (its verified SHA-256 is the digest). Library: the file at
   * the root-relative path. File: the exact path, then the same relative path under the current
   * root for each recorded previous root that contains it. Anything else is Missing; file names
   * are never matched across folders.
   */
  [[nodiscard]] auto Resolve(const LutReference& reference) const -> LutResourceResolution override;
  void               ReadResource(
                    const LutReference&                                      reference,
                    const std::function<void(const LutResourceResolution&)>& visitor) const override;

 private:
  /// A file a reference may resolve to, with the inventory digest of that file if any.
  struct Candidate {
    std::filesystem::path path;
    std::string           content_sha256;
  };

  /// Candidate files of @p reference in resolution order. Reads published values only.
  /// @pre The caller holds the state lock.
  [[nodiscard]] auto CandidatesLocked(const LutReference& reference) const
      -> std::vector<Candidate>;
  [[nodiscard]] auto CandidateInRoot(std::string_view relative_path) const
      -> std::optional<Candidate>;
  void                                     ReportMissing(const LutReference& reference) const;

  std::filesystem::path                    root_;
  LutLibraryInventory                      inventory_;
  std::vector<LutPackageReceipt>           receipts_;
  LutLibraryUserState                      user_state_;

  mutable std::shared_mutex                state_mutex_;
  mutable std::shared_mutex                content_mutex_;
  mutable std::shared_mutex                observer_mutex_;
  std::function<void(const LutReference&)> missing_observer_;
};

}  // namespace alcedo
