//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/search_query_classifier.hpp"

#include <algorithm>
#include <cwctype>
#include <optional>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

#include "storage/store/semantic/semantic_label_config.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {

namespace {

auto TrimWString(std::wstring value) -> std::wstring {
  const auto first = std::find_if_not(value.begin(), value.end(),
                                      [](wchar_t ch) { return std::iswspace(ch) != 0; });
  const auto last  = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
                      return std::iswspace(ch) != 0;
                    }).base();
  if (first >= last) {
    return {};
  }
  return std::wstring(first, last);
}

auto LowerCopy(std::wstring value) -> std::wstring {
  std::ranges::transform(value, value.begin(),
                         [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return value;
}

auto SplitTokens(const std::wstring& query) -> std::vector<std::wstring> {
  std::vector<std::wstring> tokens;
  std::wstring              current;
  for (const auto ch : query) {
    if (std::iswspace(ch) != 0) {
      if (!current.empty()) {
        tokens.push_back(std::move(current));
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    tokens.push_back(std::move(current));
  }
  return tokens;
}

// Match a full token against a regex (anchored). Compiled once and reused.
auto FullMatch(const std::wregex& re, const std::wstring& token) -> bool {
  std::wsmatch m;
  return std::regex_match(token, m, re);
}

const std::wregex& DateShortRe() {
  static const std::wregex re(LR"(^\d{4}$|^\d{6}$|^\d{8}$)");
  return re;
}
const std::wregex& DateGroupRe() {
  static const std::wregex re(LR"(^\d{4}[-/]\d{1,2}([-/]\d{1,2})?$)");
  return re;
}
const std::wregex& ApertureRe() {
  static const std::wregex re(LR"(^f/?\d+(\.\d+)?$)", std::regex::icase);
  return re;
}
const std::wregex& FocalLengthRe() {
  static const std::wregex re(LR"(^\d+(\.\d+)?mm$)", std::regex::icase);
  return re;
}
const std::wregex& IsoRe() {
  static const std::wregex re(LR"(^iso\s?\d+$)", std::regex::icase);
  return re;
}
const std::wregex& PureNumberRe() {
  static const std::wregex re(LR"(^\d+$)");
  return re;
}

const std::unordered_set<std::wstring>& CameraLensMakers() {
  static const std::unordered_set<std::wstring> makers = {
      L"canon",    L"nikon",     L"sony",       L"fuji",     L"fujifilm",
      L"leica",    L"panasonic", L"lumix",      L"olympus",  L"pentax",
      L"ricoh",    L"sigma",     L"tamron",     L"zeiss",    L"hasselblad",
      L"apple",    L"samsung",   L"phase",      L"mamiya",   L"contax",
      L"nikkor",   L"canon rf",  L"sony alpha", L"pentax k", L"pentax fa",
      L"sigma art"};
  return makers;
}

const std::unordered_set<std::wstring>& ImageExtensions() {
  static const std::unordered_set<std::wstring> exts = {
      L"jpg",  L"jpeg", L"png",  L"tif",  L"tiff", L"arw",  L"cr2",  L"cr3",
      L"nef",  L"raf",  L"dng",  L"orf",  L"rw2",  L"pef",  L"srf",  L"sr2",
      L"heic", L"heif", L"avif", L"webp", L"psd",  L"xcf",  L"mov",  L"mp4",
      L"m4v",  L"avi",  L"raw",  L"3fr",  L"iiq"};
  return exts;
}

auto TokenEndsInKnownExtension(const std::wstring& token_lower) -> bool {
  const auto dot = token_lower.rfind(L'.');
  if (dot == std::wstring::npos || dot == 0 || dot + 1 >= token_lower.size()) {
    return false;
  }
  const auto ext = token_lower.substr(dot + 1);
  return ImageExtensions().contains(ext);
}

auto IsMetadataToken(const std::wstring& raw_token) -> bool {
  const auto token = LowerCopy(raw_token);
  if (token.empty()) {
    return false;
  }
  if (token.find(L'/') != std::wstring::npos || token.find(L'\\') != std::wstring::npos) {
    return true;  // path / folder fragment
  }
  if (FullMatch(DateShortRe(), token) || FullMatch(DateGroupRe(), token)) {
    return true;
  }
  if (FullMatch(ApertureRe(), token) || FullMatch(FocalLengthRe(), token) ||
      FullMatch(IsoRe(), token)) {
    return true;
  }
  if (FullMatch(PureNumberRe(), token)) {
    return true;
  }
  if (TokenEndsInKnownExtension(token)) {
    return true;
  }
  if (CameraLensMakers().contains(token)) {
    return true;
  }
  return false;
}

auto IsCjk(wchar_t ch) -> bool {
  return (ch >= 0x3400 && ch <= 0x9FFF) || (ch >= 0xF900 && ch <= 0xFAFF) ||
         (ch >= 0x3000 && ch <= 0x303F) || (ch >= 0xFF00 && ch <= 0xFFEF);
}

// Resolve a (possibly #-prefixed) whole query to a canonical label id, or
// nullopt if it is not an exact label surface form in any language.
auto ResolveWholeQueryLabel(const std::wstring& query) -> std::optional<std::string> {
  std::wstring body = query;
  if (!body.empty() && body.front() == L'#') {
    body.erase(body.begin());
  }
  body = TrimWString(std::move(body));
  if (body.empty()) {
    return std::nullopt;
  }
  const auto utf8 = conv::ToBytes(body);
  return CanonicalSemanticLabel(utf8);
}

}  // namespace

std::size_t EstimatePromptTokens(const std::wstring& query) {
  std::size_t tokens   = 0;
  std::size_t word_len = 0;
  for (const auto ch : query) {
    if (IsCjk(ch)) {
      if (word_len > 0) {
        ++tokens;
        word_len = 0;
      }
      ++tokens;  // one CJK character ~ one token
    } else if (std::isspace(ch) != 0) {
      if (word_len > 0) {
        ++tokens;
        word_len = 0;
      }
    } else {
      ++word_len;
    }
  }
  if (word_len > 0) {
    ++tokens;
  }
  return tokens;
}

std::string_view SearchQueryRouteName(SearchQueryRoute route) {
  switch (route) {
    case SearchQueryRoute::Empty:
      return "empty";
    case SearchQueryRoute::Traditional:
      return "traditional";
    case SearchQueryRoute::Label:
      return "label";
    case SearchQueryRoute::Semantic:
      return "semantic";
  }
  return "empty";
}

SearchQueryClassification ClassifySearchQuery(const std::wstring& query,
                                              bool                semantic_toggle_enabled,
                                              std::size_t         max_prompt_tokens) {
  SearchQueryClassification result;
  result.normalized_query_ = TrimWString(query);
  if (result.normalized_query_.empty()) {
    result.route_ = SearchQueryRoute::Empty;
    return result;
  }

  // Exact label name, synonym, or #tag -> ordinary label search even with the
  // semantic toggle on.
  if (auto canonical = ResolveWholeQueryLabel(result.normalized_query_); canonical.has_value()) {
    result.route_         = SearchQueryRoute::Label;
    result.matched_label_ = std::move(*canonical);
    return result;
  }

  // Any metadata/EXIF/filename-shaped token -> ordinary search.
  const auto tokens = SplitTokens(result.normalized_query_);
  if (std::ranges::any_of(tokens, [](const std::wstring& token) {
        return IsMetadataToken(token);
      })) {
    result.route_ = SearchQueryRoute::Traditional;
    return result;
  }

  // Natural language.
  if (semantic_toggle_enabled) {
    result.route_    = SearchQueryRoute::Semantic;
    result.too_long_ = EstimatePromptTokens(result.normalized_query_) > max_prompt_tokens;
  } else {
    result.route_ = SearchQueryRoute::Traditional;
  }
  return result;
}

}  // namespace alcedo
