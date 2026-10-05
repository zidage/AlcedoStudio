//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <CL/cl.h>

#include <cstdint>
#include <span>
#include <vector>

#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/display_to_ap1_output.hpp"

namespace alcedo {

/**
 * @brief Device buffer of a packed DisplayToAp1 parameter block (display_to_ap1_math.h layout).
 *
 * `Upload` writes the block only when its contents differ from the uploaded block, so a cached
 * ACES 2.0 inverse runtime is transferred once per key. Not thread-safe; the owning render
 * device serializes access.
 */
class OpenClDisplayToAp1Parameters {
 public:
  OpenClDisplayToAp1Parameters() = default;
  ~OpenClDisplayToAp1Parameters();
  OpenClDisplayToAp1Parameters(const OpenClDisplayToAp1Parameters&)                    = delete;
  auto operator=(const OpenClDisplayToAp1Parameters&) -> OpenClDisplayToAp1Parameters& = delete;

  /// @return The buffer that holds the block. @throws std::runtime_error on an OpenCL error.
  auto Upload(cl_context context, cl_command_queue queue, std::span<const float> packed) -> cl_mem;
  [[nodiscard]] auto UploadCount() const -> std::uint64_t { return upload_count_; }

 private:
  cl_mem             buffer_   = nullptr;
  std::size_t        capacity_ = 0;
  std::vector<float> uploaded_;
  std::uint64_t      upload_count_ = 0;
};

/**
 * @brief Enqueue the DisplayToAp1 kernel from RGBA32F image @p src to @p dst on @p queue.
 * @param event Receives the kernel event when not null; the caller releases it.
 * @throws std::runtime_error on an OpenCL error.
 */
void EnqueueOpenClDisplayToAp1(cl_command_queue queue, cl_mem src, cl_mem dst, std::uint32_t width,
                               std::uint32_t height, cl_mem params,
                               std::uint32_t params_offset_floats, DisplayToAp1Output output_kind,
                               cl_event* event);

/**
 * @brief Enqueue LinearizeRaster: tightly packed RGBA host-format pixels at @p src (byte offset
 * @p src_offset_bytes) to F32 RGBA at @p dst (float offset @p dst_offset_floats).
 * @throws std::runtime_error for a CFA format or an OpenCL error.
 */
void EnqueueOpenClLinearizeRaster(cl_command_queue queue, cl_mem src,
                                  std::uint32_t src_offset_bytes, HostPixelFormat format,
                                  std::uint32_t width, std::uint32_t height, cl_mem params,
                                  std::uint32_t params_offset_floats, cl_mem dst,
                                  std::uint32_t dst_offset_floats, cl_event* event);

}  // namespace alcedo

#endif  // HAVE_OPENCL
