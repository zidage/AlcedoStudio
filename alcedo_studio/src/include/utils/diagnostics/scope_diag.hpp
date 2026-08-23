//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

namespace alcedo::diag {

/// Stdout scope-path gates. Distinct from [RENDER_E2E] so a flooded present
/// log still shows whether histogram/waveform staging and polling ran.
/// Repeats of the same line after the first 16 prints are suppressed.
inline void NoteScope(std::string_view event) {
  static std::mutex  mutex;
  static int         count = 0;
  static std::string last;
  const std::string  line(event);
  std::lock_guard    lock(mutex);
  if (line == last && count >= 16) {
    return;
  }
  last = line;
  ++count;
  std::cout << "[SCOPE] " << line << std::endl;
}

}  // namespace alcedo::diag
