//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace alcedo::diag {

/// Stdout scope-path gates. Distinct from [RENDER_E2E] so a flooded present
/// log still shows whether histogram/waveform staging and polling ran.
/// Each distinct line prints at most 16 times, including when two lines
/// alternate (poll vs snapshot_reject).
inline void NoteScope(std::string_view event) {
  static std::mutex                           mutex;
  static std::unordered_map<std::string, int> counts;
  const std::string                           line(event);
  std::lock_guard                             lock(mutex);
  int&                                    count = counts[line];
  if (count >= 16) {
    return;
  }
  ++count;
  std::cout << "[SCOPE] " << line << std::endl;
}

}  // namespace alcedo::diag
