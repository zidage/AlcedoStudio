//  Copyright 2026 Alcedo Studio contributors
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <vector>

#include "type/type.hpp"

namespace alcedo {

/// Direction in persisted member order, independent of Library filters.
enum class PhotoStackMove { kEarlier, kLater };

/// Committed change facts, owned by the caller. No membership cache or editable stack state.
struct PhotoStackChange {
  std::string stack_id;  ///< Created, existing, or targeted stack; empty for bulk removal.
  std::vector<std::string>     changed_stack_ids;
  std::vector<sl_element_id_t> changed_file_ids;

  /// False when the command made no persistent change.
  [[nodiscard]] auto           Changed() const -> bool { return !changed_stack_ids.empty(); }
};

}  // namespace alcedo
