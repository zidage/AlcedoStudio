//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// CUDA implementations of helper functions in Lib.Academy.DisplayEncoding.ctl
// Reference:
// https://github.com/aces-aswf/aces-core/blob/main/lib/Lib.Academy.DisplayEncoding.ctl

#pragma once

#include <cuda_runtime.h>

namespace alcedo {
namespace CUDA {
// SMPTE ST 2084-2014 Constants
__constant__ float pq_m1 = 0.1593017578125f; // ( 2610.0 / 4096.0 ) / 4.0;
__constant__ float pq_m2 = 78.84375f;        // ( 2523.0 / 4096.0 ) * 128.0;
__constant__ float pq_c1 = 0.8359375f;       // 3424.0 / 4096.0 or pq_c3 - pq_c2 + 1.0;
__constant__ float pq_c2 = 18.8515625f;      // ( 2413.0 / 4096.0 ) * 32.0;
__constant__ float pq_c3 = 18.6875f;         // ( 2392.0 / 4096.0 ) * 32.0;

__constant__ float pq_C  = 10000.f;


}
}  // namespace alcedo