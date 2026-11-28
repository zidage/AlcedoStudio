//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

// Wide character path c string
#define image_path_t    std::filesystem::path
#define file_path_t     std::filesystem::path
#define file_name_t     std::wstring

#define sl_path_t       std::wstring

#define image_id_t      uint32_t

// Used in sleeve
#define sleeve_id_t     uint32_t
#define sl_element_id_t uint32_t

/// The element id of the library root folder. Each photo of the project has a link in it, and
/// each mount root is one of its children (#1076).
inline constexpr sl_element_id_t kLibraryRootFolderId = 0;

// Used in mounted folders: MountRoot.id and Element.mount_id
#define mount_id_t      uint32_t

// Used in filters
#define filter_id_t     uint32_t

// Hash type for version control
#define p_hash_t        uint64_t

// Used in buffer-like structures to represent frame id
#define frame_id_t      uint32_t

// Used in DecodeRequest type
#define request_id_t    size_t

#define BufferQueue     ConcurrentBlockingQueue<std::shared_ptr<Image>>

#define PriorityLevel   int

// #define version_id_t   XXH128_hash_t

enum class DecodeRes {
  FULL,
  HALF,
  QUARTER,
  EIGHTH,
};


};  // namespace alcedo