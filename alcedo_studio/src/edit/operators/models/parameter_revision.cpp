//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/parameter_revision.hpp"

#include <atomic>

namespace alcedo {
namespace {

// Defined in one translation unit of EditGraph so every DLL and executable shares one counter.
std::atomic<ParameterRevision> g_last_parameter_revision{kNoParameterRevision};

}  // namespace

auto NextParameterRevision() noexcept -> ParameterRevision {
  return g_last_parameter_revision.fetch_add(1, std::memory_order_relaxed) + 1;
}

}  // namespace alcedo
