//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "edit/runtime/display_to_ap1_output.hpp"

namespace alcedo {

/**
 * @brief Device copy of a packed DisplayToAp1 parameter block (display_to_ap1_math.h layout).
 *
 * `Upload` copies the block only when its contents differ from the uploaded block, so a
 * cached ACES 2.0 inverse runtime is transferred once per key. Not thread-safe; the owning
 * render device serializes access.
 */
class CudaDisplayToAp1Parameters {
 public:
  CudaDisplayToAp1Parameters() = default;
  ~CudaDisplayToAp1Parameters();
  CudaDisplayToAp1Parameters(const CudaDisplayToAp1Parameters&)                    = delete;
  auto operator=(const CudaDisplayToAp1Parameters&) -> CudaDisplayToAp1Parameters& = delete;

  /// @return Device pointer to the block. @throws std::runtime_error on a CUDA error.
  auto Upload(std::span<const float> packed, cudaStream_t stream) -> const float*;
  /// Number of host-to-device copies made so far.
  [[nodiscard]] auto UploadCount() const -> std::uint64_t { return upload_count_; }

 private:
  float*             device_   = nullptr;
  std::size_t        capacity_ = 0;
  std::vector<float> uploaded_;
  std::uint64_t      upload_count_ = 0;
};

/**
 * @brief Convert @p pixel_count RGBA32F source pixels (display-linear or scene-linear source RGB)
 * with the parameter block at @p device_params. Alpha is copied.
 * @throws std::runtime_error when the launch fails.
 */
void LaunchCudaDisplayToAp1(const float4* input, float4* output, std::uint32_t pixel_count,
                            const float* device_params, DisplayToAp1Output output_kind,
                            cudaStream_t stream);

}  // namespace alcedo
