//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_film_grain_pass.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_grain_negative.h"
#include "edit/runtime/film/film_grain_post.h"
#include "edit/runtime/film/film_grain_tile.h"
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
static_assert(sizeof(FilmGrainNodeCurve) == 196);
static_assert(sizeof(FilmGrainNegativeParams) == 616);

/// The side of the tile of one work-group: FILM_GRAIN_TILE of film_grain_tile.h.
constexpr std::uint32_t kTile = FILM_GRAIN_TILE;

/// The fixed values of one render, in one upload.
struct FilmGrainPostDeviceData {
  FilmGrainFieldParams field;
  FilmGrainPostParams  post;
};

/// The fixed values of the grain in the negative of one render, in one upload (1004 bytes): above
/// the 1024 bytes of by-value arguments of OpenCL 1.2 with the other arguments of FP6.
struct FilmGrainNegativeDeviceData {
  FilmGrainFieldParams    field;
  FilmGrainNegativeParams negative;
};

/// Local memory of one work-group: the noise band and the horizontal blur of film_grain_tile.h.
auto LocalBytes(int halo, int tile_rows) -> std::size_t {
  return static_cast<std::size_t>(FilmGrainTileSharedFloats(halo, tile_rows)) * sizeof(float);
}

}  // namespace

auto UploadOpenClFilmGrainNegative(OpenClRenderDevice& device, const FilmGrainPlan& plan,
                                   const ResolvedRenderGeometry& geometry,
                                   std::span<const double> reversal_dye_max, std::uint32_t width,
                                   std::uint32_t height, const char* caller)
    -> OpenClFilmGrainNegative {
  const std::string prefix(caller);
  if (!plan.enabled) {
    throw std::invalid_argument(prefix + ": the grain plan is disabled");
  }
  if (width == 0 || height == 0) {
    throw std::invalid_argument(prefix + ": the render is empty");
  }
  FilmGrainNegativeDeviceData data{};
  data.field = MakeFilmGrainFieldParams(plan, geometry);
  if (data.field.halo < 0 || data.field.halo > FILM_GRAIN_MAX_BLUR_RADIUS) {
    throw std::invalid_argument(prefix + ": the blur radius is outside 0 to 24");
  }
  data.negative   = MakeFilmGrainNegativeParams(plan, reversal_dye_max);

  auto& workspace = device.Workspace();
  auto& backend   = workspace.Device();
  void* memory    = workspace.TransientBuffers().Allocate(sizeof(FilmGrainNegativeDeviceData));
  if (memory == nullptr) {
    throw std::runtime_error(prefix + ": transient allocation failed");
  }
  backend.UploadDeviceMemory(memory,
                             std::as_bytes(std::span<const FilmGrainNegativeDeviceData>(&data, 1)),
                             device.CommandContext());
  const auto location = backend.ResolveDeviceMemory(memory, sizeof(FilmGrainNegativeDeviceData));

  constexpr std::size_t   kRows = FILM_GRAIN_NEGATIVE_TILE_ROWS;
  OpenClFilmGrainNegative result;
  result.data            = location.first;
  result.field_offset    = location.second + offsetof(FilmGrainNegativeDeviceData, field);
  result.negative_offset = location.second + offsetof(FilmGrainNegativeDeviceData, negative);
  result.local_bytes     = LocalBytes(data.field.halo, FILM_GRAIN_NEGATIVE_TILE_ROWS);
  result.global[0]       = ((static_cast<std::size_t>(width) + kTile - 1) / kTile) * kTile;
  result.global[1]       = ((static_cast<std::size_t>(height) + kRows - 1) / kRows) * kTile;
  return result;
}

void RecordOpenClFilmGrainNegative(OpenClRenderDevice& device, const FilmGrainPlan& plan,
                                   const ResolvedRenderGeometry& geometry,
                                   std::span<const double> reversal_dye_max, cl_mem amounts,
                                   cl_uint amounts_offset, std::uint32_t width,
                                   std::uint32_t height) {
  constexpr const char* kCaller = "RecordOpenClFilmGrainNegative";
  const auto grain = UploadOpenClFilmGrainNegative(device, plan, geometry, reversal_dye_max, width,
                                                   height, kCaller);
  if (amounts == nullptr) {
    throw std::invalid_argument("RecordOpenClFilmGrainNegative: the amounts are null");
  }
  auto kernel = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kFilmPrintProgramName, OpenCL::GpuDag::kFilmGrainNegativeAmountsKernelName);
  cl_int error = CL_SUCCESS;
  error |= clSetKernelArg(kernel, 0, sizeof(cl_mem), &amounts);
  error |= clSetKernelArg(kernel, 1, sizeof(cl_uint), &amounts_offset);
  error |= clSetKernelArg(kernel, 2, sizeof(width), &width);
  error |= clSetKernelArg(kernel, 3, sizeof(height), &height);
  error |= clSetKernelArg(kernel, 4, sizeof(cl_mem), &grain.data);
  error |= clSetKernelArg(kernel, 5, sizeof(cl_uint), &grain.field_offset);
  error |= clSetKernelArg(kernel, 6, sizeof(cl_uint), &grain.negative_offset);
  error |= clSetKernelArg(kernel, 7, grain.local_bytes, nullptr);
  CheckOpenCl(error, "RecordOpenClFilmGrainNegative: clSetKernelArg");
  auto&             backend  = device.Workspace().Device();
  const std::size_t local[2] = {kTile, kTile};
  cl_event          event    = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(backend.NativeQueue(), kernel, 2, nullptr, grain.global, local,
                                     0, nullptr, &event),
              "RecordOpenClFilmGrainNegative: clEnqueueNDRangeKernel");
  NoteOpenClEnqueueNdRange();
  backend.TrackKernelEvent(device.CommandContext(), event);
}

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
  error |=
      clSetKernelArg(kernel, 11, LocalBytes(data.field.halo, FILM_GRAIN_POST_TILE_ROWS), nullptr);
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
