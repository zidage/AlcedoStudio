//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/string/search_text.hpp"

#include <cwctype>
#include <string>
#include <string_view>

#include "utils/string/convert.hpp"

namespace alcedo {

auto IsSearchSeparator(wchar_t ch) -> bool {
  switch (ch) {
    case L' ':
    case L'\t':
    case L'\n':
    case L'\r':
    case L'_':
    case L'-':
    case L'.':
    case L'/':
    case L'\\':
    case L':':
    case L';':
    case L',':
    case L'\'':
    case L'"':
    case L'(':
    case L')':
    case L'[':
    case L']':
    case L'{':
    case L'}':
    case L'%':
    case L'*':
    case L'?':
    case L'!':
    case L'@':
    case L'#':
    case L'$':
    case L'&':
    case L'+':
    case L'=':
    case L'|':
    case L'`':
    case L'~':
      return true;
    default:
      return false;
  }
}

auto FoldSearchText(std::wstring_view text) -> std::wstring {
  std::wstring out;
  out.reserve(text.size());
  for (const auto ch : text) {
    if (!IsSearchSeparator(ch)) {
      out.push_back(static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(ch))));
    }
  }
  return out;
}

auto FoldSearchTextUtf8(std::string_view text) -> std::string {
  if (text.empty()) {
    return {};
  }
  try {
    return conv::ToBytes(FoldSearchText(conv::FromBytes(std::string(text))));
  } catch (...) {
    return {};
  }
}

}  // namespace alcedo
