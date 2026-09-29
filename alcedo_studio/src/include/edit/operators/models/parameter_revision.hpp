//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

/**
 * @brief Stamp of one parameter write.
 *
 * Every Model field, Color Grade mix, and document topology write takes a new stamp from one
 * process-wide counter. A stamp identifies one write, so two objects that report the same stamp
 * for a field hold the same value for it (a clone copies values and stamps together). Readers
 * compare stamps with `!=`, never `<`: an unrelated Model can carry an older stamp.
 *
 * Renderers keep the stamps they last applied in their own workspace. They do not write the
 * document, so any number of renderers can read one document.
 */
using ParameterRevision                                 = std::uint64_t;

/// Never returned by @ref NextParameterRevision. Means "nothing applied yet".
inline constexpr ParameterRevision kNoParameterRevision = 0;

/**
 * @brief Take the next process-wide parameter stamp.
 *
 * Thread-safe and lock-free. Values increase strictly and are never @ref kNoParameterRevision.
 */
[[nodiscard]] auto                 NextParameterRevision() noexcept -> ParameterRevision;

}  // namespace alcedo
