//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/frame_presentation_lease.hpp"

#include <sstream>

namespace alcedo::editor_rhi {

auto DescribeLease(const WritableTargetLease& lease) -> std::string {
  std::ostringstream oss;
  oss << "lease backend=" << ToString(lease.backend)
      << " handle=" << ToString(lease.handle_kind)
      << " writable=" << ToString(lease.writable_kind) << " size=" << lease.dimensions.width
      << "x" << lease.dimensions.height << " target_gen=" << lease.generation.target_generation
      << " session_epoch=" << lease.generation.session_epoch
      << " image_id=" << lease.generation.image_identity
      << " layer_gen=" << lease.generation.layer_generation << " layer=" << ToString(lease.layer)
      << " native=0x" << std::hex << lease.native_handle << " writable_res=0x"
      << lease.writable_resource << std::dec
      << " has_lifetime=" << (lease.lifetime_token ? 1 : 0)
      << " sync=0x" << std::hex << lease.sync_object << std::dec;
  return oss.str();
}

}  // namespace alcedo::editor_rhi
