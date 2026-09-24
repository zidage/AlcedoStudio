//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstddef>

namespace alcedo {

struct MetalExecutionStats {
  double input_prepare_ms      = 0.0;
  double fused_encode_ms       = 0.0;
  double hs_encode_ms          = 0.0;
  double hs_source_encode_ms   = 0.0;
  double hs_remap_encode_ms    = 0.0;
  double hs_select_encode_ms   = 0.0;
  double hs_collapse_encode_ms = 0.0;
  double hs_apply_encode_ms    = 0.0;
  double neighbor_encode_ms    = 0.0;
  double gpu_wait_ms           = 0.0;
  double host_download_ms      = 0.0;
  double host_copy_submit_ms   = 0.0;
  double output_wrap_ms        = 0.0;
  double total_ms              = 0.0;
  size_t detail_stage_count    = 0;
};

}  // namespace alcedo

#endif  // HAVE_METAL
