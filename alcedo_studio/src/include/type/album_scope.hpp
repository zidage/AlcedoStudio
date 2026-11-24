//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "type/type.hpp"

namespace alcedo {

/**
 * @brief The photos that one library read covers (BuildScopedFileQuery).
 *
 * Each photo of the project has a link in the library root, and a collection holds a second
 * link. Three scopes follow from that:
 *
 * - All Photos: every photo of the project. The folder id form with kLibraryRootFolderId
 *   (`AlbumScope(0)`), the form of the library reads before #1076.
 * - A folder: the photos linked into one collection or mounted folder (`AlbumScope(id)`).
 * - The library root: the photos that no folder other than the root holds, so the root row
 *   shows the photos that are not in a collection (LibraryRoot, #1076).
 */
class AlbumScope {
 public:
  /// The photos of the folder @p folder_id; kLibraryRootFolderId is All Photos. The implicit
  /// form keeps the folder id parameter of the library reads.
  // NOLINTNEXTLINE(google-explicit-constructor): a folder id is a complete scope.
  constexpr AlbumScope(sl_element_id_t folder_id = kLibraryRootFolderId) : folder_id_(folder_id) {}

  /// The photos that no folder other than the library root holds.
  [[nodiscard]] static constexpr auto LibraryRoot() -> AlbumScope {
    AlbumScope scope(kLibraryRootFolderId);
    scope.library_root_ = true;
    return scope;
  }

  /// The folder of the scope: the collection, or kLibraryRootFolderId for All Photos and for
  /// LibraryRoot.
  [[nodiscard]] constexpr auto FolderId() const -> sl_element_id_t { return folder_id_; }
  /// True for All Photos.
  [[nodiscard]] constexpr auto IsAllPhotos() const -> bool {
    return folder_id_ == kLibraryRootFolderId && !library_root_;
  }
  /// True for LibraryRoot.
  [[nodiscard]] constexpr auto IsLibraryRoot() const -> bool { return library_root_; }

  constexpr auto operator==(const AlbumScope&) const -> bool = default;

 private:
  sl_element_id_t folder_id_    = kLibraryRootFolderId;
  bool            library_root_ = false;
};

}  // namespace alcedo
