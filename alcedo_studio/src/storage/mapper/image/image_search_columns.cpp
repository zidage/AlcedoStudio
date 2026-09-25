//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/image/image_search_columns.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "utils/string/convert.hpp"
#include "utils/string/search_text.hpp"

namespace alcedo {
namespace {

auto ParseDigits(std::string_view text, size_t pos, size_t count) -> std::optional<int> {
  if (pos + count > text.size()) {
    return std::nullopt;
  }
  int value = 0;
  for (size_t i = pos; i < pos + count; ++i) {
    const auto ch = static_cast<unsigned char>(text[i]);
    if (std::isdigit(ch) == 0) {
      return std::nullopt;
    }
    value = value * 10 + (ch - '0');
  }
  return value;
}

auto IsLeapYear(int year) -> bool { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

auto DaysInMonth(int year, int month) -> int {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && IsLeapYear(year) ? 29 : kDays[month - 1];
}

auto IsDateSeparator(char ch) -> bool { return std::isdigit(static_cast<unsigned char>(ch)) == 0; }

// Join the non-empty folded parts with one space. See FillImageSearchColumns.
auto JoinFoldedParts(std::initializer_list<std::string> parts) -> std::string {
  std::string out;
  for (const auto& part : parts) {
    const auto folded = FoldSearchTextUtf8(part);
    if (folded.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += folded;
  }
  return out;
}

// Join the non-empty word-folded parts with `|`. See FillImageSearchColumns.
auto JoinWordFoldedParts(std::initializer_list<std::string> parts) -> std::string {
  std::string out;
  for (const auto& part : parts) {
    const auto folded = FoldSearchWordsUtf8(part);
    if (folded.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back('|');
    }
    out += folded;
  }
  return out;
}

auto Lowercase(std::wstring value) -> std::wstring {
  for (auto& ch : value) {
    ch = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(ch)));
  }
  return value;
}

// The metadata keeps focal length and aperture as float, so 2.8f reads as 2.7999999523 when
// widened. Round to two decimals, which is finer than any lens or aperture marking, so the
// column holds the value the camera reports (2.8, 35.0).
auto RoundedPositiveOrNull(float value) -> std::optional<double> {
  if (!(value > 0.0f)) {
    return std::nullopt;
  }
  return std::round(static_cast<double>(value) * 100.0) / 100.0;
}

auto PositiveOrNull(uint64_t value) -> std::optional<int64_t> {
  return value > 0 ? std::optional<int64_t>{static_cast<int64_t>(value)} : std::nullopt;
}

}  // namespace

auto ParseCaptureDateTime(std::string_view text) -> std::optional<CaptureDateTime> {
  if (text.size() < 10 || !IsDateSeparator(text[4]) || !IsDateSeparator(text[7])) {
    return std::nullopt;
  }
  const auto year  = ParseDigits(text, 0, 4);
  const auto month = ParseDigits(text, 5, 2);
  const auto day   = ParseDigits(text, 8, 2);
  if (!year || !month || !day || *year < 1 || *month < 1 || *month > 12 || *day < 1 ||
      *day > DaysInMonth(*year, *month)) {
    return std::nullopt;
  }

  int hour = 0, minute = 0, second = 0;
  if (text.size() > 10) {
    // `YYYY-MM-DD HH:MM:SS`: the time must follow one separator and be complete.
    const auto h = ParseDigits(text, 11, 2);
    const auto m = ParseDigits(text, 14, 2);
    const auto s = ParseDigits(text, 17, 2);
    if (text.size() < 19 || !IsDateSeparator(text[10]) || text[13] != ':' || text[16] != ':' ||
        !h || !m || !s || *h > 23 || *m > 59 || *s > 59) {
      return std::nullopt;
    }
    hour   = *h;
    minute = *m;
    second = *s;
  }

  CaptureDateTime out;
  out.date_      = std::format("{:04}-{:02}-{:02}", *year, *month, *day);
  out.date_time_ = std::format("{} {:02}:{:02}:{:02}", out.date_, hour, minute, second);
  return out;
}

void FillImageSearchColumns(const std::wstring& file_name, const std::filesystem::path& image_path,
                            const ExifDisplayMetaData& metadata, ImageMapperParams& row) {
  const std::filesystem::path name_path =
      file_name.empty() ? image_path.filename() : std::filesystem::path(file_name);
  row.file_stem_ = conv::ToBytes(name_path.stem().wstring());
  auto extension = name_path.extension().wstring();
  if (!extension.empty() && extension.front() == L'.') {
    extension.erase(extension.begin());
  }
  row.file_ext_ = conv::ToBytes(Lowercase(std::move(extension)));

  if (const auto capture = ParseCaptureDateTime(metadata.date_time_str_); capture.has_value()) {
    row.capture_at_   = capture->date_time_;
    row.capture_date_ = capture->date_;
  } else {
    row.capture_at_.clear();
    row.capture_date_.clear();
  }

  row.camera_make_  = metadata.make_;
  row.camera_model_ = metadata.model_;
  row.lens_         = metadata.lens_;
  row.iso_          = PositiveOrNull(metadata.iso_);
  row.focal_mm_     = RoundedPositiveOrNull(metadata.focal_);
  row.aperture_     = RoundedPositiveOrNull(metadata.aperture_);
  row.rating_       = ExifDisplayMetaData::NormalizeRating(metadata.rating_);
  row.pixel_count_  = PositiveOrNull(static_cast<uint64_t>(metadata.width_) * metadata.height_);

  const auto parent_folder = conv::ToBytes(image_path.parent_path().filename().wstring());
  const auto file_name_text = conv::ToBytes(name_path.wstring());
  row.file_search_text_     = JoinFoldedParts({file_name_text, parent_folder});
  row.exif_search_text_     = JoinFoldedParts({metadata.make_, metadata.model_, metadata.lens_,
                                               metadata.lens_make_, metadata.date_time_str_});
  row.exif_search_words_    = JoinWordFoldedParts({metadata.make_, metadata.model_, metadata.lens_,
                                                   metadata.lens_make_, metadata.date_time_str_});
}

}  // namespace alcedo
