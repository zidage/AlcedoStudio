//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// CUDA-path compatibility aliases for the backend-neutral Neural tile planner.
// Prefer neural_tile_jobs.hpp for new OpenCL / shared code.

#include "decoders/processor/neural_tile_jobs.hpp"

namespace alcedo::detail {

using CudaTilePolicy = NeuralTilePolicy;
using CudaTileJob    = NeuralTileJob;

}  // namespace alcedo::detail
