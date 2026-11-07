//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_film_grain_pass.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_grain_post.h"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

// film_grain_post.cl reads these structs from the upload with the layout of OpenCL C, where every
// member is a 4-byte scalar or an array of them.
static_assert(sizeof(FilmGrainNoiseParams) == 36);
static_assert(sizeof(FilmGrainFieldParams) == 388);
static_assert(sizeof(FilmGrainCurve) == 184);
static_assert(sizeof(FilmGrainPostParams) == 612);

/// The side of the tile of one work-group: FILM_GRAIN_POST_TILE of film_grain_post.cl.
constexpr std::uint32_t kTile = 16;

/// The fixed values of one render, in one upload.
struct FilmGrainPostDeviceData {
  FilmGrainFieldParams field;
  FilmGrainPostParams  post;
};

/// Local memory of one work-group: the noise band and the horizontal blur, 3 channels of
/// kTile x (kTile + 2 halo) floats each.
auto LocalBytes(int halo) -> std::size_t {
  const auto band = static_cast<std::size_t>(kTile) * (kTile + 2U * static_cast<std::size_t>(halo));
  return 2U * 3U * band * sizeof(float);
}

}  // namespace

void RecordOpenClFilmGrainPost(OpenClRenderDevice& device, const FilmGrainPlan& plan,
                               const ResolvedRenderGeometry& geometry,
                               FilmGrainDisplayEncoding encoding, const OpenClSceneView& src,
                               const OpenClSceneView& dst, std::uint32_t width,
                               std::uint32_t height) {
  if (!plan.enabled) {
    throw std::invalid_argument("RecordOpenClFilmGrainPost: the grain plan is disabled");
  }
  if (width == 0 || height == 0 || src.width != width || src.height != height ||
      dst.width != width || dst.height != height) {
    throw std::invalid_argument(
        "RecordOpenClFilmGrainPost: the images do not have the render size");
  }
  if (src.native == dst.native) {
    throw std::invalid_argument("RecordOpenClFilmGrainPost: the source is the destination");
  }
  FilmGrainPostDeviceData data{};
  data.field = MakeFilmGrainFieldParams(plan, geometry);
  data.post  = MakeFilmGrainPostParams(plan, encoding.transfer, encoding.decode_scale);
  if (data.field.halo < 0 || data.field.halo > FILM_GRAIN_MAX_BLUR_RADIUS) {
    throw std::invalid_argument("RecordOpenClFilmGrainPost: the blur radius is outside 0 to 24");
  }

  auto& workspace = device.Workspace();
  auto& backend   = workspace.Device();
  void* memory    = workspace.TransientBuffers().Allocate(sizeof(FilmGrainPostDeviceData));
  if (memory == nullptr) {
    throw std::runtime_error("RecordOpenClFilmGrainPost: transient allocation failed");
  }
  backend.UploadDeviceMemory(memory,
                             std::as_bytes(std::span<const FilmGrainPostDeviceData>(&data, 1)),
                             device.CommandContext());
  const auto location = backend.ResolveDeviceMemory(memory, sizeof(FilmGrainPostDeviceData));

  auto       kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kFilmGrainProgramName,
                                                              OpenCL::GpuDag::kFilmGrainPostKernelName);
  BindOpenClSceneView(kernel, 0, src, backend, "RecordOpenClFilmGrainPost: source",
                      OpenClSceneArgAccess::Read);
  BindOpenClSceneView(kernel, 3, dst, backend, "RecordOpenClFilmGrainPost: destination",
                      OpenClSceneArgAccess::Write);
  cl_mem        data_buffer  = location.first;
  const cl_uint field_offset = location.second + offsetof(FilmGrainPostDeviceData, field);
  const cl_uint post_offset  = location.second + offsetof(FilmGrainPostDeviceData, post);
  cl_int        error        = CL_SUCCESS;
  error |= clSetKernelArg(kernel, 6, sizeof(cl_mem), &data_buffer);
  error |= clSetKernelArg(kernel, 7, sizeof(cl_uint), &field_offset);
  error |= clSetKernelArg(kernel, 8, sizeof(cl_uint), &post_offset);
  error |= clSetKernelArg(kernel, 9, sizeof(width), &width);
  error |= clSetKernelArg(kernel, 10, sizeof(height), &height);
  error |= clSetKernelArg(kernel, 11, LocalBytes(data.field.halo), nullptr);
  CheckOpenCl(error, "RecordOpenClFilmGrainPost: clSetKernelArg");

  const std::size_t local[2]  = {kTile, kTile};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + kTile - 1) / kTile) * kTile,
                                 ((static_cast<std::size_t>(height) + kTile - 1) / kTile) * kTile};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(backend.NativeQueue(), kernel, 2, nullptr, global, local, 0,
                                     nullptr, &event),
              "RecordOpenClFilmGrainPost: clEnqueueNDRangeKernel");
  NoteOpenClEnqueueNdRange();
  backend.TrackKernelEvent(device.CommandContext(), event);
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
