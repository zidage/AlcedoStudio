//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <exiv2/types.hpp>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

/// Value and text helpers of the metadata readers.
namespace alcedo::metadata_detail {

/// Value of an EXIF rational, or 0 when the denominator is 0.
auto RationalToFloat(const Exiv2::Rational& value) -> float;
auto IsFinitePositive(float value) -> bool;
/// Text of a zero-padded C string field of at most @p max_len bytes, without trailing zero bytes
/// and spaces.
auto TrimTrailingZeroPadded(const char* s, size_t max_len = 256) -> std::string;
/// @p value without leading and trailing zero bytes and spaces.
auto TrimAscii(const std::string& value) -> std::string;
auto ContainsCaseInsensitive(const std::string& text, const std::string& pattern) -> bool;
auto IsDngExtension(const std::filesystem::path& path) -> bool;
/// 35 mm equivalent focal length divided by the focal length, or 0 when one value is missing.
auto ResolveCropFactorHint(float focal_mm, float focal_35mm_mm) -> float;
auto PathToUtf8(const std::filesystem::path& path) -> std::string;
auto ToLowerAscii(std::string value) -> std::string;
/// True when @p text contains one of @p patterns (case-sensitive).
auto ContainsAny(const std::string& text, const std::initializer_list<std::string_view>& patterns)
    -> bool;

}  // namespace alcedo::metadata_detail
