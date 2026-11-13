//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>

namespace alcedo {

/// Author of a commit that has no attribution: the person who uses the editor.
inline constexpr std::string_view kEditCommitAuthorUser  = "user";

/// Author of a commit that an automation client made.
inline constexpr std::string_view kEditCommitAuthorAgent = "agent";

/**
 * @brief Who made one commit, and why.
 *
 * Attribution is not part of the commit: the commit hash does not include it, and a commit
 * without attribution has the author @ref kEditCommitAuthorUser. The Mini-Git journal record
 * carries it until materialization, which writes it into the EditCommitAttribution table.
 */
struct EditCommitAttribution {
  /// @ref kEditCommitAuthorAgent; the only author that is stored.
  std::string author_;
  /// Optional text from the author. Empty when there is none.
  std::string note_;

  auto        operator==(const EditCommitAttribution&) const -> bool = default;
};

/// True when @p attribution can be stored: its author is @ref kEditCommitAuthorAgent.
[[nodiscard]] inline auto IsStorableEditCommitAttribution(const EditCommitAttribution& attribution)
    -> bool {
  return attribution.author_ == kEditCommitAuthorAgent;
}

}  // namespace alcedo
