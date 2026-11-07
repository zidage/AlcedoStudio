//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_film_print_pass.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/drt_post_film_grain.hpp"
#include "edit/runtime/film/film_negative_scan.hpp"
#include "edit/runtime/film/film_print_kernel_tables.hpp"
#include "edit/runtime/film/film_print_math.h"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film_print_plan.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/opencl/opencl_drt_gpu_params.hpp"
#include "edit/runtime/opencl/opencl_drt_params.hpp"
#include "edit/runtime/opencl/opencl_film_grain_pass.hpp"
#include "edit/runtime/opencl/opencl_scene_work.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/texture_pool.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

// film_print.cl has the same values, and a different size stops the build of its program.
static_assert(kFilmBlurMaxRadius == 128 && sizeof(FilmBlurKernel) == 1552);
static_assert(kFilmNarrowSpreadMaxRadius == 2 && sizeof(FilmNarrowKernel) == 76);
static_assert(sizeof(FilmKernelParams) == 416);
static_assert(sizeof(Matrix3x3) == 36);
static_assert(sizeof(FilmKernelRgb) == 12);

/// The fixed values of one render. One upload holds them, because a FilmBlurKernel is too large for
/// a by-value argument of OpenCL 1.2. The kernels read each member at its offset.
struct FilmPrintDeviceData {
  FilmKernelParams params;
  FilmBlurKernel   spread_x[2];
  FilmBlurKernel   spread_y[2];
  FilmNarrowKernel spread_narrow;
  FilmBlurKernel   inhibitor_x;
  FilmBlurKernel   inhibitor_y;
  FilmBlurKernel   halation_x;
  FilmBlurKernel   halation_y;
  FilmBlurKernel   flare_x;
  FilmBlurKernel   flare_y;
};

/// One image between the steps: 3 floats per pixel, in the transient arena of the render.
struct Plane {
  cl_mem  native = nullptr;
  cl_uint offset = 0;  ///< In floats from the start of @ref native
};

/// The uploaded @ref FilmPrintDeviceData.
struct DeviceData {
  cl_mem             native = nullptr;
  cl_uint            offset = 0;  ///< In bytes from the start of @ref native

  [[nodiscard]] auto Member(std::size_t member_offset) const -> cl_uint {
    return offset + static_cast<cl_uint>(member_offset);
  }
};

auto Kernel(const char* name) -> cl_kernel {
  return OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kFilmPrintProgramName, name);
}

/// The arguments of one kernel, in the order of its declaration.
class Arguments {
 public:
  Arguments(cl_kernel kernel, const char* label) : kernel_(kernel), label_(label) {}

  template <class Value>
  auto Add(const Value& value) -> Arguments& {
    CheckOpenCl(clSetKernelArg(kernel_, index_++, sizeof(Value), &value), label_);
    return *this;
  }

  auto Add(const Plane& plane) -> Arguments& { return Add(plane.native).Add(plane.offset); }

  /// A `__local` argument of @p bytes.
  auto AddLocal(std::size_t bytes) -> Arguments& {
    CheckOpenCl(clSetKernelArg(kernel_, index_++, bytes, nullptr), label_);
    return *this;
  }

  auto AddSceneView(const OpenClSceneView& view, OpenClBackend& backend,
                    OpenClSceneArgAccess access) -> Arguments& {
    BindOpenClSceneView(kernel_, index_, view, backend, label_, access);
    index_ += 3;
    return *this;
  }

 private:
  cl_kernel   kernel_;
  const char* label_;
  cl_uint     index_ = 0;
};

void Dispatch(OpenClRenderDevice& device, cl_kernel kernel, std::uint32_t width,
              std::uint32_t height, const char* label) {
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  auto&             backend   = device.Workspace().Device();
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(backend.NativeQueue(), kernel, 2, nullptr, global, local, 0,
                                     nullptr, &event),
              label);
  NoteOpenClEnqueueNdRange();
  backend.TrackKernelEvent(device.CommandContext(), event);
}

auto AllocatePlane(OpenClRenderDevice& device, std::uint32_t width, std::uint32_t height) -> Plane {
  auto&      workspace = device.Workspace();
  const auto bytes     = static_cast<std::size_t>(width) * height * 3U * sizeof(float);
  void*      memory    = workspace.TransientBuffers().Allocate(bytes);
  if (memory == nullptr) {
    throw std::runtime_error("RecordOpenClFilmPrint: transient allocation failed");
  }
  const auto resolved = workspace.Device().ResolveDeviceMemory(memory, bytes);
  return Plane{resolved.first, static_cast<cl_uint>(resolved.second / sizeof(float))};
}

/// The names of the kernel groups that a recording runs, for a test.
class PassNames {
 public:
  explicit PassNames(std::vector<std::string>* names) : names_(names) {}
  void Mark(const char* name) {
    if (names_ != nullptr) names_->emplace_back(name);
  }

 private:
  std::vector<std::string>* names_;
};

/// The context of one recording.
struct Recording {
  OpenClRenderDevice& device;
  DeviceData          data;
};

void BlurHorizontal(const Recording& r, const Plane& src, const Plane& dst, std::uint32_t width,
                    std::uint32_t height, std::size_t blur_member) {
  auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintBlurHorizontalKernelName);
  Arguments(kernel, "RecordOpenClFilmPrint: blur horizontal")
      .Add(src)
      .Add(dst)
      .Add(static_cast<cl_int>(width))
      .Add(static_cast<cl_int>(height))
      .Add(r.data.native)
      .Add(r.data.Member(blur_member));
  Dispatch(r.device, kernel, width, height, "RecordOpenClFilmPrint: blur horizontal enqueue");
}

void BlurVertical(const Recording& r, const Plane& src, const Plane& dst, std::uint32_t width,
                  std::uint32_t height, std::size_t blur_member, float weight, bool accumulate) {
  auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintBlurVerticalKernelName);
  Arguments(kernel, "RecordOpenClFilmPrint: blur vertical")
      .Add(src)
      .Add(dst)
      .Add(static_cast<cl_int>(width))
      .Add(static_cast<cl_int>(height))
      .Add(r.data.native)
      .Add(r.data.Member(blur_member))
      .Add(weight)
      .Add(static_cast<cl_int>(accumulate ? 1 : 0));
  Dispatch(r.device, kernel, width, height, "RecordOpenClFilmPrint: blur vertical enqueue");
}

void BlurVerticalToImage(const Recording& r, const Plane& src, cl_mem dst, std::uint32_t width,
                         std::uint32_t height, std::size_t blur_member) {
  auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintBlurVerticalToImageKernelName);
  Arguments(kernel, "RecordOpenClFilmPrint: blur to image")
      .Add(src)
      .Add(dst)
      .Add(static_cast<cl_int>(width))
      .Add(static_cast<cl_int>(height))
      .Add(r.data.native)
      .Add(r.data.Member(blur_member));
  Dispatch(r.device, kernel, width, height, "RecordOpenClFilmPrint: blur to image enqueue");
}

/// What a canvas holds. The same values as FILM_CANVAS_* of film_print.cl.
enum class CanvasValue : cl_int {
  kValue         = 0,  ///< The exposure, for Halation
  kTransmittance = 1,  ///< 10^-a of the amounts, for the Enlarger flare of a print
  kDisplayLight  = 2,  ///< Linear display light of the light table, for the loupe flare
};

/// @p print_rgba and @p params_offset are read only for CanvasValue::kDisplayLight.
void CanvasReduce(const Recording& r, const Plane& src, std::uint32_t width, std::uint32_t height,
                  const Plane& dst, ImageExtent extent, const Matrix3x3& canvas_to_render,
                  std::uint32_t samples, CanvasValue mode, cl_mem print_rgba,
                  cl_uint params_offset) {
  auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintCanvasReduceKernelName);
  Arguments(kernel, "RecordOpenClFilmPrint: canvas reduce")
      .Add(src)
      .Add(static_cast<cl_int>(width))
      .Add(static_cast<cl_int>(height))
      .Add(dst)
      .Add(static_cast<cl_int>(extent.width))
      .Add(static_cast<cl_int>(extent.height))
      .Add(canvas_to_render)
      .Add(static_cast<cl_int>(samples))
      .Add(static_cast<cl_int>(mode))
      .Add(print_rgba)
      .Add(r.data.native)
      .Add(params_offset);
  Dispatch(r.device, kernel, extent.width, extent.height,
           "RecordOpenClFilmPrint: canvas reduce enqueue");
}

using Lease = ResourceLease<OpenClBackend>;

/**
 * @brief The canonical result @p id: the published image, or an image from @p build.
 *
 * The rule of `DecideDiffusionScatter`: a render that covers the full edit space builds and
 * publishes; an ROI samples a published image, and otherwise builds a temporary image from its own
 * pixels. Nothing is visible to another render before `PublishResults`.
 */
template <class Build>
auto ReadOrBuildCanvasResult(OpenClRenderDevice& device, const ExecutionPlan& plan,
                             const GraphValueId& id, ImageExtent extent, std::uint32_t long_edge,
                             std::optional<Lease>& temporary, Build&& build) -> cl_mem {
  auto&      workspace    = device.Workspace();
  auto&      invalidation = workspace.ResultInvalidation();
  const bool persist      = workspace.PersistsResult(id);
  const bool full_edit    = CoversFullEditSpace(plan.geometry);
  Lease*     canonical    = nullptr;
  if (persist) {
    const auto needed = invalidation.MakeImageRepresentation(
        id, extent, TextureFormat::Rgba32f, DiffusionScatterRequiredDetail(full_edit, long_edge));
    canonical = workspace.Images().BindValidResult(id, invalidation.RequiredRevision(id), needed,
                                                   workspace.Device().CompletedSubmission());
  } else {
    ++device.PassStats().result_policy_bypass;
  }
  const auto decision = DecideDiffusionScatter(persist, canonical != nullptr, full_edit, long_edge);
  if (decision.action == DiffusionScatterAction::SampleCanonical) {
    ++device.PassStats().film_canvas_sample;
    return canonical->Texture().Native();
  }
  cl_mem target = nullptr;
  if (decision.persist_canonical) {
    target =
        workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::Rgba32f})
            .Texture()
            .Native();
  } else {
    temporary.emplace(
        workspace.Textures().Acquire({extent.width, extent.height, TextureFormat::Rgba32f}));
    target = temporary->Texture().Native();
  }
  build(target);
  if (decision.persist_canonical) {
    const auto published = invalidation.MakeImageRepresentation(id, extent, TextureFormat::Rgba32f,
                                                                decision.current_long_edge);
    workspace.Images().RecordUnpublished(id, invalidation.RequiredRevision(id), published,
                                         device.CommandContext().SubmissionId(),
                                         decision.current_long_edge);
  }
  ++device.PassStats().film_canvas_rebuild;
  return target;
}

}  // namespace

void RecordOpenClFilmPrint(OpenClRenderDevice& device, const ExecutionPlan& plan,
                           const DrtNodeModel& drt, std::optional<double> raw_cct,
                           const FrameSceneBinding& scene, const FrameSceneBinding& display,
                           std::uint32_t width, std::uint32_t height, float grain_strength,
                           std::vector<std::string>* pass_names, bool separate_narrow_spread) {
  const auto resolved =
      DrtOutputResolver::ResolveNode(drt, plan.output_color_override, "ExecuteOpenClDrt");
  const auto  film        = drt.Params().FilmPrint();
  auto&       tables      = SharedFilmPrintTables();
  const auto& bundle      = tables.Bundle();
  // Phase G6: the grain of the negative. A strength outside [0, 2] stops the render here, before
  // any GPU work.
  std::optional<FilmGrainPlan> grain;
  if (grain_strength != 0.0f) {
    grain = MakeDrtFilmPrintGrainPlan(drt, grain_strength, plan.geometry);
  }
  const auto  light       = bundle.SceneLightMix(film.scene_light, raw_cct);
  // Negative Scan reads the scanner table of the scanner of the document. A rendering setting reads
  // the kept table.
  const bool  scan        = IsFilmNegativeScan(film.negative, film.print_material);
  const auto  tables_data =
      scan ? tables.ResolveNegativeScan(film.negative, light,
                                        film_negative_scan::ScannerOf(film.scan))
           : tables.Resolve(film.negative, film.print_material, light);
  // With auto colour off the terms are known now, so a black at or above the grey stops the render
  // before any GPU work.
  const bool  auto_colour = scan && film.scan.auto_colour != kFilmScanAutoIds[0];
  std::optional<film_negative_scan::RenderTerms> scan_terms;
  if (scan && !auto_colour) {
    scan_terms = film_negative_scan::MakeRenderTerms(tables_data->negative_scan, film.scan, {});
  }

  auto&       workspace   = device.Workspace();
  auto&       backend     = workspace.Device();
  auto&       context     = device.CommandContext();
  const auto  print       = backend.AcquireLut(tables_data->print_key, tables_data->print_rgba,
                                               tables_data->edge, context);
  const auto  table       = backend.AcquireKeyedBuffer(
      tables_data->kernel_key, std::as_bytes(std::span(tables_data->kernel_values)), context);

  FilmPrintDeviceData uploaded{};
  film_print_kernel_tables::FillParams(bundle, film.negative, film.print_material, light,
                                       tables_data->edge, uploaded.params);
  uploaded.params.display_encoding = static_cast<std::uint32_t>(resolved.eotf_);
  const auto& params               = uploaded.params;

  const auto  negative             = bundle.Film(film.negative);
  const auto  image                = MakeFilmPrintImagePlan(
      plan.geometry, bundle.ImagePlaneConstants(), negative.layer_spread_um,
      params.inhibition_has_image != 0U || params.negative_grade != 0U, film,
      drt.Params().FilmFormat());
  // With no blur, one dispatch computes the final amounts, so FP4 and FP5 need no other image.
  const bool inhibitor_blur = image.image_plane && params.inhibition_has_image != 0U;

  uploaded.spread_x[0]      = image.spread_x[0];
  uploaded.spread_x[1]      = image.spread_x[1];
  uploaded.spread_y[0]      = image.spread_y[0];
  uploaded.spread_y[1]      = image.spread_y[1];
  uploaded.spread_narrow    = image.spread_narrow;
  uploaded.inhibitor_x      = image.inhibitor_x;
  uploaded.inhibitor_y      = image.inhibitor_y;
  uploaded.halation_x       = image.halation_x;
  uploaded.halation_y       = image.halation_y;
  uploaded.flare_x          = image.flare_x;
  uploaded.flare_y          = image.flare_y;

  // BeginRender waited for the earlier render, so an idle slab has no queued work.
  workspace.TransientBuffers().ReleaseIdleSlabsWhenNoneHolds(static_cast<std::size_t>(width) *
                                                             height * 3U * sizeof(float));
  void* data_memory = workspace.TransientBuffers().Allocate(sizeof(FilmPrintDeviceData));
  if (data_memory == nullptr) {
    throw std::runtime_error("RecordOpenClFilmPrint: transient allocation failed");
  }
  backend.UploadDeviceMemory(
      data_memory, std::as_bytes(std::span<const FilmPrintDeviceData>(&uploaded, 1)), context);
  const auto data_location = backend.ResolveDeviceMemory(data_memory, sizeof(FilmPrintDeviceData));
  const Recording recording{device, DeviceData{data_location.first, data_location.second}};
  const auto&     data          = recording.data;
  const auto      params_offset = data.Member(offsetof(FilmPrintDeviceData, params));
  const auto      w             = static_cast<cl_int>(width);
  const auto      h             = static_cast<cl_int>(height);
  cl_mem          values        = table.native;
  cl_mem          print_rgba    = print.native;
  cl_mem          unused_image  = backend.DummySceneReadImage();
  cl_mem          unused_buffer = backend.DummySceneReadBuffer();
  PassNames       names(pass_names);

  // FP1.
  const auto      scene_view = OpenClBindScene(device, scene);
  Plane           q          = AllocatePlane(device, width, height);
  {
    auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintExposureKernelName);
    Arguments(kernel, "RecordOpenClFilmPrint: exposure")
        .AddSceneView(scene_view, backend, OpenClSceneArgAccess::Read)
        .Add(q)
        .Add(values)
        .Add(data.native)
        .Add(params_offset)
        .Add(w)
        .Add(h);
    Dispatch(device, kernel, width, height, "RecordOpenClFilmPrint: exposure enqueue");
  }
  names.Mark("FP1 exposure");

  // FP2, rule 'spread_per_layer_pass'. After the spread, the first image holds the amounts.
  std::optional<Plane> scratch;
  Plane                a{};
  if (image.spread_passes != 0U) {
    const Plane spread = AllocatePlane(device, width, height);
    scratch.emplace(AllocatePlane(device, width, height));
    if (image.spread_narrow_fused && !separate_narrow_spread) {
      BlurHorizontal(recording, q, *scratch, width, height,
                     offsetof(FilmPrintDeviceData, spread_x) + sizeof(FilmBlurKernel));
      names.Mark("FP2 wide horizontal");
      auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintSpreadVerticalKernelName);
      Arguments(kernel, "RecordOpenClFilmPrint: spread vertical")
          .Add(*scratch)
          .Add(q)
          .Add(spread)
          .Add(w)
          .Add(h)
          .Add(data.native)
          .Add(data.Member(offsetof(FilmPrintDeviceData, spread_y) + sizeof(FilmBlurKernel)))
          .Add(image.spread_weight[1])
          .Add(data.Member(offsetof(FilmPrintDeviceData, spread_narrow)))
          .Add(image.spread_weight[0]);
      Dispatch(device, kernel, width, height, "RecordOpenClFilmPrint: spread vertical enqueue");
      names.Mark("FP2 vertical, with the narrow Gaussian");
    } else {
      for (std::uint32_t pass = 0; pass < image.spread_passes; ++pass) {
        BlurHorizontal(recording, q, *scratch, width, height,
                       offsetof(FilmPrintDeviceData, spread_x) + pass * sizeof(FilmBlurKernel));
        BlurVertical(recording, *scratch, spread, width, height,
                     offsetof(FilmPrintDeviceData, spread_y) + pass * sizeof(FilmBlurKernel),
                     image.spread_weight[pass], pass != 0U);
      }
      names.Mark("FP2 spread, 2 separate passes");
    }
    a = q;
    q = spread;
  } else {
    a = AllocatePlane(device, width, height);
  }

  const auto           long_edge = (std::max)(width, height);
  const auto           canvas    = image.canvas_extent;

  // FP3: the canonical image, then a read of it per layer, in one dispatch with the first amounts.
  std::optional<Lease> temporary_halation;
  cl_mem               halation = unused_image;
  if (image.halation_active) {
    halation = ReadOrBuildCanvasResult(
        device, plan, FilmHalationResultId(plan.drt.node_id), canvas, long_edge, temporary_halation,
        [&](cl_mem target) {
          const Plane reduced = AllocatePlane(device, canvas.width, canvas.height);
          const Plane middle  = AllocatePlane(device, canvas.width, canvas.height);
          CanvasReduce(recording, q, width, height, reduced, canvas, image.canvas_to_render,
                       image.canvas_samples, CanvasValue::kValue, print_rgba, params_offset);
          BlurHorizontal(recording, reduced, middle, canvas.width, canvas.height,
                         offsetof(FilmPrintDeviceData, halation_x));
          BlurVerticalToImage(recording, middle, target, canvas.width, canvas.height,
                              offsetof(FilmPrintDeviceData, halation_y));
        });
    names.Mark("FP3 canvas");
  }
  const FilmKernelRgb halation_strength{image.halation_strength[0], image.halation_strength[1],
                                        image.halation_strength[2]};
  const auto          canvas_width  = static_cast<cl_int>(canvas.width);
  const auto          canvas_height = static_cast<cl_int>(canvas.height);

  // FP4 and FP5, rules 'inhibition_two_steps' and 'inhibition_image_part'. After FP4, the image of
  // the exposure holds the log exposure that the steps read.
  if (inhibitor_blur && image.inhibitor_half_size) {
    // Only the last step writes full-size amounts. The first amounts and the first step write the
    // box mean directly.
    const auto  extent  = image.inhibitor_extent;
    const auto  columns = static_cast<cl_int>(extent.width);
    const auto  rows    = static_cast<cl_int>(extent.height);
    const Plane reduced = AllocatePlane(device, extent.width, extent.height);
    const Plane middle  = AllocatePlane(device, extent.width, extent.height);
    {
      auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintReducedFirstAmountsKernelName);
      Arguments(kernel, "RecordOpenClFilmPrint: reduced first amounts")
          .Add(q)
          .Add(w)
          .Add(h)
          .Add(reduced)
          .Add(columns)
          .Add(rows)
          .Add(halation)
          .Add(static_cast<cl_int>(image.halation_active ? 1 : 0))
          .Add(canvas_width)
          .Add(canvas_height)
          .Add(image.render_to_canvas)
          .Add(halation_strength)
          .Add(values)
          .Add(data.native)
          .Add(params_offset);
      Dispatch(device, kernel, extent.width, extent.height,
               "RecordOpenClFilmPrint: reduced first amounts enqueue");
    }
    names.Mark("FP3 mix and FP4 first amounts, to half size");
    BlurHorizontal(recording, reduced, middle, extent.width, extent.height,
                   offsetof(FilmPrintDeviceData, inhibitor_x));
    BlurVertical(recording, middle, reduced, extent.width, extent.height,
                 offsetof(FilmPrintDeviceData, inhibitor_y), 1.0f, false);
    names.Mark("FP4 inhibitor blur");
    {
      auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintStepToReducedKernelName);
      Arguments(kernel, "RecordOpenClFilmPrint: step to reduced")
          .Add(q)
          .Add(w)
          .Add(h)
          .Add(reduced)
          .Add(middle)
          .Add(columns)
          .Add(rows)
          .Add(values)
          .Add(data.native)
          .Add(params_offset);
      Dispatch(device, kernel, extent.width, extent.height,
               "RecordOpenClFilmPrint: step to reduced enqueue");
    }
    names.Mark("FP5 first step, to half size");
    BlurHorizontal(recording, middle, reduced, extent.width, extent.height,
                   offsetof(FilmPrintDeviceData, inhibitor_x));
    BlurVertical(recording, reduced, middle, extent.width, extent.height,
                 offsetof(FilmPrintDeviceData, inhibitor_y), 1.0f, false);
    names.Mark("FP4 inhibitor blur");
    {
      auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintStepHalfSizeKernelName);
      Arguments(kernel, "RecordOpenClFilmPrint: step half size")
          .Add(q)
          .Add(a)
          .Add(w)
          .Add(h)
          .Add(middle)
          .Add(columns)
          .Add(rows)
          .Add(values)
          .Add(data.native)
          .Add(params_offset);
      Dispatch(device, kernel, width, height, "RecordOpenClFilmPrint: step half size enqueue");
    }
    names.Mark("FP5 last step");
  } else {
    if (image.halation_active) {
      auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintHalationFirstAmountsKernelName);
      Arguments(kernel, "RecordOpenClFilmPrint: halation and first amounts")
          .Add(q)
          .Add(a)
          .Add(halation)
          .Add(canvas_width)
          .Add(canvas_height)
          .Add(image.render_to_canvas)
          .Add(halation_strength)
          .Add(static_cast<cl_int>(inhibitor_blur ? 1 : 0))
          .Add(values)
          .Add(data.native)
          .Add(params_offset)
          .Add(w)
          .Add(h);
      Dispatch(device, kernel, width, height,
               "RecordOpenClFilmPrint: halation and first amounts enqueue");
      names.Mark("FP3 mix and FP4 amounts");
    } else {
      auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintFirstAmountsKernelName);
      Arguments(kernel, "RecordOpenClFilmPrint: first amounts")
          .Add(q)
          .Add(a)
          .Add(static_cast<cl_int>(inhibitor_blur ? 1 : 0))
          .Add(values)
          .Add(data.native)
          .Add(params_offset)
          .Add(w)
          .Add(h);
      Dispatch(device, kernel, width, height, "RecordOpenClFilmPrint: first amounts enqueue");
      names.Mark("FP4 amounts");
    }
    if (inhibitor_blur) {
      if (!scratch.has_value()) scratch.emplace(AllocatePlane(device, width, height));
      for (int step = 0; step < 2; ++step) {
        BlurHorizontal(recording, a, *scratch, width, height,
                       offsetof(FilmPrintDeviceData, inhibitor_x));
        names.Mark("FP4 inhibitor horizontal");
        auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintStepVerticalKernelName);
        Arguments(kernel, "RecordOpenClFilmPrint: step vertical")
            .Add(*scratch)
            .Add(q)
            .Add(a)
            .Add(w)
            .Add(h)
            .Add(data.native)
            .Add(data.Member(offsetof(FilmPrintDeviceData, inhibitor_y)))
            .Add(values)
            .Add(params_offset);
        Dispatch(device, kernel, width, height, "RecordOpenClFilmPrint: step vertical enqueue");
        names.Mark("FP4 vertical and FP5 inhibited step");
      }
    }
  }

  // Rule 'negative_show_grade': the blurred amounts, before the flare of FP6.
  std::optional<Plane> soft;
  if (image.image_plane && params.negative_grade != 0U) {
    const auto extent = image.inhibitor_extent;
    Plane      source = a;
    if (image.inhibitor_half_size) {
      const Plane reduced = AllocatePlane(device, extent.width, extent.height);
      auto        kernel  = Kernel(OpenCL::GpuDag::kFilmPrintAmountsReduceKernelName);
      Arguments(kernel, "RecordOpenClFilmPrint: amounts reduce")
          .Add(a)
          .Add(w)
          .Add(h)
          .Add(reduced)
          .Add(static_cast<cl_int>(extent.width))
          .Add(static_cast<cl_int>(extent.height));
      Dispatch(device, kernel, extent.width, extent.height,
               "RecordOpenClFilmPrint: amounts reduce enqueue");
      source = reduced;
    }
    const Plane middle = AllocatePlane(device, extent.width, extent.height);
    soft.emplace(AllocatePlane(device, extent.width, extent.height));
    BlurHorizontal(recording, source, middle, extent.width, extent.height,
                   offsetof(FilmPrintDeviceData, inhibitor_x));
    BlurVertical(recording, middle, *soft, extent.width, extent.height,
                 offsetof(FilmPrintDeviceData, inhibitor_y), 1.0f, false);
    names.Mark("FP6 blurred amounts of the negative grade");
  }

  // FP6: the canonical image of the blurred transmittance, or on a light table of the blurred
  // display light, then the fused point stages.
  std::optional<Lease> temporary_flare;
  cl_mem               flare       = unused_image;
  const bool           light_table = params.light_table != 0U;
  if (image.flare_active) {
    const auto extent = image.flare_extent;
    const auto id     = light_table ? FilmLoupeFlareResultId(plan.drt.node_id)
                                    : FilmFlareResultId(plan.drt.node_id);
    flare             = ReadOrBuildCanvasResult(
        device, plan, id, extent, long_edge, temporary_flare, [&](cl_mem target) {
          const Plane reduced = AllocatePlane(device, canvas.width, canvas.height);
          CanvasReduce(recording, a, width, height, reduced, canvas, image.canvas_to_render,
                                   image.canvas_samples,
                       light_table ? CanvasValue::kDisplayLight : CanvasValue::kTransmittance,
                                   print_rgba, params_offset);
          const Plane middle = AllocatePlane(device, extent.width, extent.height);
          // A canvas with a small flare sigma has no second reduction.
          Plane       source = reduced;
          if (extent.width != canvas.width || extent.height != canvas.height) {
            source      = AllocatePlane(device, extent.width, extent.height);
            auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintBoxReduceKernelName);
            Arguments(kernel, "RecordOpenClFilmPrint: box reduce")
                .Add(reduced)
                .Add(canvas_width)
                .Add(canvas_height)
                .Add(source)
                .Add(static_cast<cl_int>(extent.width))
                .Add(static_cast<cl_int>(extent.height));
            Dispatch(device, kernel, extent.width, extent.height,
                                 "RecordOpenClFilmPrint: box reduce enqueue");
          }
          BlurHorizontal(recording, source, middle, extent.width, extent.height,
                                     offsetof(FilmPrintDeviceData, flare_x));
          BlurVerticalToImage(recording, middle, target, extent.width, extent.height,
                                          offsetof(FilmPrintDeviceData, flare_y));
        });
    names.Mark("FP6 flare canvas");
  }
  // Phase G6: the grain of the negative, the rules of RecordCudaFilmPrint. The FP6 kernel adds it
  // to the record of each pixel, before the flare amounts and the table read. The blurred amounts,
  // the flare canvas, and the prescan read the amounts with no grain: they are means over many
  // pixels, where the grain has a mean of 0, so their canonical results do not depend on it.
  std::optional<OpenClFilmGrainNegative> grain_data;
  if (grain.has_value() && grain->enabled) {
    grain_data = UploadOpenClFilmGrainNegative(
        device, *grain, plan.geometry,
        grain->reversal ? bundle.Reversal(film.negative).dye_max : std::span<const double>{}, width,
        height, "RecordOpenClFilmPrint");
  }
  // The arguments of the grain after those of the FP6 kernel without grain, and the dispatch.
  const auto add_grain = [&](Arguments& arguments) {
    arguments.Add(grain_data->data)
        .Add(grain_data->field_offset)
        .Add(grain_data->negative_offset)
        .AddLocal(grain_data->local_bytes);
  };
  const auto dispatch_grain = [&](cl_kernel kernel, const char* label) {
    const std::size_t local[2] = {16, 16};
    cl_event          event    = nullptr;
    CheckOpenCl(clEnqueueNDRangeKernel(backend.NativeQueue(), kernel, 2, nullptr,
                                       grain_data->global, local, 0, nullptr, &event),
                label);
    NoteOpenClEnqueueNdRange();
    backend.TrackKernelEvent(context, event);
    ++device.PassStats().film_grain_negative;
  };
  if (params.scan != 0U) {
    if (auto_colour) {
      // Rule 'frame_auto_colour': the prescan of the full frame, a canonical result. The host reads
      // only the prescan and computes the shift, as the lab does; the per-pixel pass applies it.
      const auto&          frame        = plan.geometry.edit_extent;
      const auto           prescan_plan = film_negative_scan::PrescanPlanOf(frame.height, frame.width,
                                                                            height, width);
      const ImageExtent    extent{static_cast<std::uint32_t>(prescan_plan.columns),
                                  static_cast<std::uint32_t>(prescan_plan.rows)};
      const auto           columns      = static_cast<cl_int>(extent.width);
      const auto           rows         = static_cast<cl_int>(extent.height);
      std::optional<Lease> temporary_prescan;
      const cl_mem         prescan      = ReadOrBuildCanvasResult(
          device, plan, FilmScanPrescanResultId(plan.drt.node_id), extent, long_edge,
          temporary_prescan, [&](cl_mem target) {
            const auto windows =
                backend.AcquireKeyedBuffer(prescan_plan.windows_key,
                                           std::as_bytes(std::span(prescan_plan.windows)), context);
            const Plane middle = AllocatePlane(device, extent.width, height);
            {
              auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintScanPrescanRowsKernelName);
              Arguments(kernel, "RecordOpenClFilmPrint: scan prescan rows")
                  .Add(a)
                  .Add(w)
                  .Add(h)
                  .Add(print_rgba)
                  .Add(windows.native)
                  .Add(columns)
                  .Add(middle)
                  .Add(data.native)
                  .Add(params_offset);
              // One work group of FILM_PRESCAN_LANES (film_print.cl) per window.
              constexpr std::size_t kLanes     = 32;
              const std::size_t     local[2]   = {kLanes, 1};
              const std::size_t     global[2]  = {kLanes * extent.width, height};
              cl_event              event      = nullptr;
              CheckOpenCl(clEnqueueNDRangeKernel(backend.NativeQueue(), kernel, 2, nullptr, global,
                                                 local, 0, nullptr, &event),
                          "RecordOpenClFilmPrint: scan prescan rows enqueue");
              NoteOpenClEnqueueNdRange();
              backend.TrackKernelEvent(context, event);
            }
            auto kernel = Kernel(OpenCL::GpuDag::kFilmPrintScanPrescanColumnsKernelName);
            Arguments(kernel, "RecordOpenClFilmPrint: scan prescan columns")
                .Add(middle)
                .Add(columns)
                .Add(h)
                .Add(windows.native)
                .Add(rows)
                .Add(target);
            Dispatch(device, kernel, extent.width, extent.height,
                     "RecordOpenClFilmPrint: scan prescan columns enqueue");
          });
      names.Mark("FP6 prescan");
      std::vector<float> texels(static_cast<std::size_t>(extent.width) * extent.height * 4U);
      const std::size_t  origin[3] = {0, 0, 0};
      const std::size_t  region[3] = {extent.width, extent.height, 1};
      CheckOpenCl(clEnqueueReadImage(backend.NativeQueue(), prescan, CL_TRUE, origin, region, 0, 0,
                                     texels.data(), 0, nullptr, nullptr),
                  "RecordOpenClFilmPrint: prescan read");
      const auto result = film_negative_scan::AutoColourOfDensityPrescan(
          texels, extent.height, extent.width, tables_data->negative_scan, film.scan);
      ++device.PassStats().film_scan_prescan_reads;
      device.PassStats().film_scan_auto_shift = result.shift;
      scan_terms =
          film_negative_scan::MakeRenderTerms(tables_data->negative_scan, film.scan, result.shift);
      names.Mark("FP6 prescan read and auto colour on the host");
    }
    FilmKernelNegativeScan terms{};
    for (std::size_t k = 0; k < 3; ++k) {
      terms.slope[k]  = static_cast<float>(scan_terms->slope[k]);
      terms.offset[k] = static_cast<float>(scan_terms->offset[k]);
      terms.scale[k]  = static_cast<float>(scan_terms->scale[k]);
      terms.floor[k]  = static_cast<float>(scan_terms->floor[k]);
    }
    const auto upload = [&](std::span<const std::byte> bytes) {
      void* memory = workspace.TransientBuffers().Allocate(bytes.size());
      if (memory == nullptr) {
        throw std::runtime_error("RecordOpenClFilmPrint: transient allocation failed");
      }
      backend.UploadDeviceMemory(memory, bytes, context);
      return backend.ResolveDeviceMemory(memory, bytes.size());
    };
    const auto terms_location =
        upload(std::as_bytes(std::span<const FilmKernelNegativeScan>(&terms, 1)));
    // The form ends in linear AP1: the OpenDRT of the document shows it, with the kernel of the DRT
    // program. The resolver gives Negative Scan the transform of method OpenDRT.
    const auto drt_params   = PackOpenClDrtParams(resolved);
    const auto drt_location = upload(std::as_bytes(
        std::span<const OpenCL::Pipeline::OpenClToOutputParams>(&drt_params, 1)));
    auto       kernel       = OpenClKernelCache::Instance().GetKernel(
        OpenCL::GpuDag::kDrtProgramName, grain_data.has_value()
                                                         ? OpenCL::GpuDag::kFilmPrintScanDisplayGrainKernelName
                                                         : OpenCL::GpuDag::kFilmPrintScanDisplayKernelName);
    const auto display_view = OpenClBindScene(device, display);
    Arguments  arguments(kernel, "RecordOpenClFilmPrint: scan display");
    arguments.Add(a)
        .AddSceneView(scene_view, backend, OpenClSceneArgAccess::Read)
        .AddSceneView(display_view, backend, OpenClSceneArgAccess::Write)
        .Add(static_cast<cl_uint>(width))
        .Add(static_cast<cl_uint>(height))
        .Add(print_rgba)
        .Add(data.native)
        .Add(params_offset)
        .Add(terms_location.first)
        .Add(static_cast<cl_uint>(terms_location.second))
        .Add(drt_location.first)
        .Add(static_cast<cl_uint>(drt_location.second))
        .Add(static_cast<cl_int>(plan.film_scan_linear_output ? 1 : 0));
    if (grain_data.has_value()) {
      add_grain(arguments);
      dispatch_grain(kernel, "RecordOpenClFilmPrint: scan display with grain enqueue");
      names.Mark("FP6 scan and display transform, with the grain of the negative");
    } else {
      Dispatch(device, kernel, width, height, "RecordOpenClFilmPrint: scan display enqueue");
      names.Mark("FP6 scan and display transform");
    }
    return;
  }
  {
    auto kernel = Kernel(grain_data.has_value() ? OpenCL::GpuDag::kFilmPrintDisplayGrainKernelName
                                                : OpenCL::GpuDag::kFilmPrintDisplayKernelName);
    const auto display_view = OpenClBindScene(device, display);
    Arguments  arguments(kernel, "RecordOpenClFilmPrint: display");
    arguments.Add(a)
        .AddSceneView(scene_view, backend, OpenClSceneArgAccess::Read)
        .AddSceneView(display_view, backend, OpenClSceneArgAccess::Write)
        .Add(w)
        .Add(h)
        .Add(print_rgba)
        .Add(flare)
        .Add(static_cast<cl_int>(image.flare_active ? 1 : 0))
        .Add(static_cast<cl_int>(image.flare_extent.width))
        .Add(static_cast<cl_int>(image.flare_extent.height))
        .Add(image.render_to_flare)
        .Add(image.flare)
        .Add(soft.has_value() ? soft->native : unused_buffer)
        .Add(soft.has_value() ? soft->offset : cl_uint{0})
        .Add(static_cast<cl_int>(soft.has_value() ? 1 : 0))
        .Add(static_cast<cl_int>(image.inhibitor_extent.width))
        .Add(static_cast<cl_int>(image.inhibitor_extent.height))
        .Add(static_cast<cl_int>(image.inhibitor_half_size ? 1 : 0))
        .Add(data.native)
        .Add(params_offset);
    if (grain_data.has_value()) {
      add_grain(arguments);
      dispatch_grain(kernel, "RecordOpenClFilmPrint: display with grain enqueue");
      names.Mark("FP6 display, with the grain of the negative");
    } else {
      Dispatch(device, kernel, width, height, "RecordOpenClFilmPrint: display enqueue");
      names.Mark("FP6 display");
    }
  }
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
