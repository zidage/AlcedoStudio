//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>

namespace alcedo {

/// True for a character that library search ignores when it compares text: white space and
/// the punctuation that separates parts of file names, dates, and camera names
/// (`_ - . / \ : ;` and similar).
auto IsSearchSeparator(wchar_t ch) -> bool;

/**
 * @brief Fold text into the form that library search compares.
 *
 * Removes every search separator (see IsSearchSeparator) and lowercases the remaining
 * characters with `std::towlower`. The Image search columns and the AI search columns are
 * written with this fold, and the search query uses the same fold, so a query such as
 * `d3x_01` matches the file name `nikon_d3x_01.nef`.
 *
 * Folding removes characters one by one. When a text A is a substring of a text B, the fold
 * of A is a substring of the fold of B.
 */
auto FoldSearchText(std::wstring_view text) -> std::wstring;

/// UTF-8 form of FoldSearchText. Returns an empty string when @p text is not valid UTF-8.
auto FoldSearchTextUtf8(std::string_view text) -> std::string;

/**
 * @brief Fold text like FoldSearchText, but keep one space between the words.
 *
 * A word is a run of characters that are not search separators. `NIKKOR Z 85mm f/1.8` gives
 * `nikkor z 85mm f 1 8`. Removing the spaces gives FoldSearchText of the same text. Library
 * search reads this form to see where a folded match crosses a word boundary.
 */
auto FoldSearchWords(std::wstring_view text) -> std::wstring;

/// UTF-8 form of FoldSearchWords. Returns an empty string when @p text is not valid UTF-8.
auto FoldSearchWordsUtf8(std::string_view text) -> std::string;

}  // namespace alcedo
