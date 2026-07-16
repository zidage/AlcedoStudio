//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/frame_presentation_lease.hpp"

#include <sstream>

namespace alcedo::editor_rhi {

auto DescribeLease(const WritableTargetLease& lease) -> std::string {
  std::ostringstream oss;
  oss << "lease backend=" << ToString(lease.backend)
      << " handle=" << ToString(lease.handle_kind) << " size=" << lease.dimensions.width << "x"
      << lease.dimensions.height << " target_gen=" << lease.generation.target_generation
      << " image_gen=" << lease.generation.image_generation
      << " layer_gen=" << lease.generation.layer_generation << " native=0x" << std::hex
      << lease.native_handle << std::dec
      << " has_lifetime=" << (lease.lifetime_token ? 1 : 0);
  return oss.str();
}

}  // namespace alcedo::editor_rhi
