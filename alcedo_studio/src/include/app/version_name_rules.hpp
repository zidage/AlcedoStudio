//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace alcedo {

/// Reason of @ref NormalizeVersionName for a name that is empty after trimming.
inline constexpr std::string_view kEmptyVersionNameMessage = "A Version name cannot be empty.";

/**
 * @brief The Version name that @p requested gives: the text without leading and trailing white
 *        space (ASCII and Unicode white space, like the JavaScript trim of the panel).
 *
 * @return nullopt when nothing is left; @p error then gets @ref kEmptyVersionNameMessage.
 */
[[nodiscard]] auto NormalizeVersionName(std::string_view requested, std::string* error = nullptr)
    -> std::optional<std::string>;

/**
 * @brief The name of a new Version when @p version_count Versions exist: @p pattern with "%1"
 *        replaced by version_count + 1.
 *
 * @p pattern is a translated text such as "Version %1". Without "%1" the number follows the
 * pattern after one space.
 */
[[nodiscard]] auto DefaultVersionName(std::size_t      version_count,
                                      std::string_view pattern = "Version %1") -> std::string;

/// True when renaming a Version called @p current to @p requested changes nothing: the
/// normalized names are equal. Such a rename succeeds without a change.
[[nodiscard]] auto IsUnchangedVersionName(std::string_view current, std::string_view requested)
    -> bool;

}  // namespace alcedo
